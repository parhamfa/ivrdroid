from __future__ import annotations

import hashlib
import subprocess
import wave
from dataclasses import dataclass
from pathlib import Path


ALLOWED_SUFFIXES = frozenset({".wav", ".mp3", ".ogg"})


class MediaError(ValueError):
    pass


@dataclass(frozen=True)
class CanonicalPrompt:
    content_hash: str
    size_bytes: int
    duration_ms: int
    path: Path


def transcode_prompt(source: Path, destination: Path, duration_limit_seconds: int) -> CanonicalPrompt:
    if source.suffix.lower() not in ALLOWED_SUFFIXES:
        raise MediaError("Only WAV, MP3, and OGG prompt files are accepted.")
    duration = _probe_duration(source)
    if duration <= 0 or duration > duration_limit_seconds:
        raise MediaError(f"Prompt duration must be between 0 and {duration_limit_seconds} seconds.")

    destination.parent.mkdir(parents=True, exist_ok=True)
    process = subprocess.run(
        [
            "ffmpeg",
            "-nostdin",
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-i",
            str(source),
            "-vn",
            "-ac",
            "2",
            "-ar",
            "48000",
            "-c:a",
            "pcm_s16le",
            str(destination),
        ],
        capture_output=True,
        timeout=90,
        check=False,
    )
    if process.returncode != 0:
        destination.unlink(missing_ok=True)
        raise MediaError("Audio conversion failed.")

    try:
        with wave.open(str(destination), "rb") as audio:
            if audio.getnchannels() != 2 or audio.getframerate() != 48000 or audio.getsampwidth() != 2:
                raise MediaError("Converted prompt did not match 48 kHz stereo PCM16.")
            duration_ms = round(audio.getnframes() * 1000 / audio.getframerate())
    except (wave.Error, EOFError) as error:
        destination.unlink(missing_ok=True)
        raise MediaError("Converted prompt is not a valid PCM WAV file.") from error

    size = destination.stat().st_size
    digest = hashlib.sha256(destination.read_bytes()).hexdigest()
    return CanonicalPrompt(digest, size, duration_ms, destination)


def _probe_duration(source: Path) -> float:
    process = subprocess.run(
        [
            "ffprobe",
            "-v",
            "error",
            "-show_entries",
            "format=duration",
            "-of",
            "default=noprint_wrappers=1:nokey=1",
            str(source),
        ],
        capture_output=True,
        text=True,
        timeout=20,
        check=False,
    )
    if process.returncode != 0:
        raise MediaError("Audio file could not be inspected.")
    try:
        return float(process.stdout.strip())
    except ValueError as error:
        raise MediaError("Audio duration is unavailable.") from error
