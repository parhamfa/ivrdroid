from __future__ import annotations

import base64
import hashlib
import io
import json
import os
import stat
import struct
import subprocess
import tempfile
import wave
from dataclasses import dataclass
from datetime import timedelta
from pathlib import Path
from uuid import uuid4

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from sqlalchemy import func, select
from sqlalchemy.orm import Session

from .config import Settings
from .models import (
    ContinuousRecording,
    ConversationRecordingSegment,
    Recording,
    RecordingRetentionPolicy,
    RecordingUpload,
    utcnow,
)


UPLOAD_MAGIC = b"IVRUP1\0"
MEDIA_MAGIC = b"IVREC1\0"
VOICEMAIL_MONO_FILTER = "pan=mono|c0=c0"
VOICEMAIL_AUDIO_FILTER = f"{VOICEMAIL_MONO_FILTER},loudnorm=I=-18:TP=-1.5:LRA=7"


class RecordingError(ValueError):
    def __init__(self, message: str, status_code: int = 422):
        super().__init__(message)
        self.status_code = status_code


@dataclass(frozen=True)
class WavDetails:
    duration_ms: int
    frames: int


@dataclass(frozen=True)
class QuarantinedMedia:
    original: Path
    quarantine: Path


class RecordingCipher:
    def __init__(self, key_b64: str, version: int, previous_keys_json: str = "{}"):
        if version < 1:
            raise RuntimeError("recording encryption key version is invalid")
        self._master_keys: dict[int, bytes] = {}
        try:
            previous = json.loads(previous_keys_json)
        except (TypeError, json.JSONDecodeError) as error:
            raise RuntimeError("previous recording encryption keys are not valid JSON") from error
        if not isinstance(previous, dict):
            raise RuntimeError("previous recording encryption keys must be a JSON object")
        for raw_version, raw_key in previous.items():
            try:
                previous_version = int(raw_version)
                previous_key = base64.b64decode(raw_key, validate=True)
            except Exception as error:
                raise RuntimeError("a previous recording encryption key is invalid") from error
            if previous_version < 1 or previous_version == version or len(previous_key) != 32:
                raise RuntimeError("a previous recording encryption key version is invalid")
            if previous_version in self._master_keys:
                raise RuntimeError("a previous recording encryption key version is duplicated")
            self._master_keys[previous_version] = previous_key
        try:
            key = base64.b64decode(key_b64, validate=True)
        except Exception as error:
            raise RuntimeError("recording encryption key is not valid base64") from error
        if len(key) != 32:
            raise RuntimeError("recording encryption key must decode to 32 bytes")
        self._master_keys[version] = key
        self.version = version

    def _key(self, recording_id: str, version: int) -> bytes:
        master_key = self._master_keys.get(version)
        if master_key is None:
            raise RecordingError("Recording key version is unavailable.", 503)
        return HKDF(
            algorithm=hashes.SHA256(),
            length=32,
            salt=None,
            info=b"ivrdroid-recording:" + recording_id.encode("ascii"),
        ).derive(master_key)

    def encrypt_chunk(self, recording_id: str, offset: int, plaintext: bytes) -> bytes:
        nonce = os.urandom(12)
        aad = f"upload:{self.version}:{recording_id}:{offset}".encode("ascii")
        ciphertext = AESGCM(self._key(recording_id, self.version)).encrypt(nonce, plaintext, aad)
        return UPLOAD_MAGIC + self.version.to_bytes(4, "big") + nonce + ciphertext

    def decrypt_chunk(self, recording_id: str, offset: int, payload: bytes) -> bytes:
        minimum = len(UPLOAD_MAGIC) + 4 + 12 + 16
        if len(payload) < minimum or not payload.startswith(UPLOAD_MAGIC):
            raise RecordingError("Encrypted upload chunk is malformed.", 503)
        position = len(UPLOAD_MAGIC)
        version = int.from_bytes(payload[position : position + 4], "big")
        nonce = payload[position + 4 : position + 16]
        ciphertext = payload[position + 16 :]
        aad = f"upload:{version}:{recording_id}:{offset}".encode("ascii")
        try:
            return AESGCM(self._key(recording_id, version)).decrypt(nonce, ciphertext, aad)
        except RecordingError:
            raise
        except Exception as error:
            raise RecordingError("Encrypted upload chunk failed authentication.", 503) from error

    def encrypt_media(self, recording_id: str, plaintext: bytes) -> bytes:
        nonce = os.urandom(12)
        aad = f"media:{self.version}:{recording_id}".encode("ascii")
        ciphertext = AESGCM(self._key(recording_id, self.version)).encrypt(nonce, plaintext, aad)
        return MEDIA_MAGIC + self.version.to_bytes(4, "big") + nonce + ciphertext

    def encrypt_media_file(
        self,
        recording_id: str,
        source: Path,
        destination: Path,
    ) -> tuple[int, str]:
        """Encrypt a media file incrementally using the on-disk AES-GCM wire format."""
        nonce = os.urandom(12)
        aad = f"media:{self.version}:{recording_id}".encode("ascii")
        encryptor = Cipher(
            algorithms.AES(self._key(recording_id, self.version)),
            modes.GCM(nonce),
        ).encryptor()
        encryptor.authenticate_additional_data(aad)
        digest = hashlib.sha256()
        with source.open("rb") as incoming, destination.open("wb") as outgoing:
            os.chmod(destination, 0o600)
            outgoing.write(MEDIA_MAGIC)
            outgoing.write(self.version.to_bytes(4, "big"))
            outgoing.write(nonce)
            while chunk := incoming.read(1024 * 1024):
                digest.update(chunk)
                outgoing.write(encryptor.update(chunk))
            outgoing.write(encryptor.finalize())
            outgoing.write(encryptor.tag)
            outgoing.flush()
            os.fsync(outgoing.fileno())
        return destination.stat().st_size, digest.hexdigest()

    def decrypt_media(self, recording_id: str, payload: bytes) -> bytes:
        minimum = len(MEDIA_MAGIC) + 4 + 12 + 16
        if len(payload) < minimum or not payload.startswith(MEDIA_MAGIC):
            raise RecordingError("Encrypted recording is malformed.", 503)
        position = len(MEDIA_MAGIC)
        version = int.from_bytes(payload[position : position + 4], "big")
        nonce = payload[position + 4 : position + 16]
        aad = f"media:{version}:{recording_id}".encode("ascii")
        try:
            return AESGCM(self._key(recording_id, version)).decrypt(nonce, payload[position + 16 :], aad)
        except RecordingError:
            raise
        except Exception as error:
            raise RecordingError("Encrypted recording failed authentication.", 503) from error

    def decrypt_media_file(
        self,
        recording_id: str,
        source: Path,
        destination: Path,
    ) -> tuple[int, int, str]:
        """Authenticate and decrypt media incrementally into a private temporary file."""
        size = source.stat().st_size
        header_size = len(MEDIA_MAGIC) + 4 + 12
        if size < header_size + 16:
            raise RecordingError("Encrypted recording is malformed.", 503)
        with source.open("rb") as incoming:
            if incoming.read(len(MEDIA_MAGIC)) != MEDIA_MAGIC:
                raise RecordingError("Encrypted recording is malformed.", 503)
            version = int.from_bytes(incoming.read(4), "big")
            nonce = incoming.read(12)
            incoming.seek(-16, os.SEEK_END)
            tag = incoming.read(16)
            ciphertext_size = size - header_size - 16
            incoming.seek(header_size)
            decryptor = Cipher(
                algorithms.AES(self._key(recording_id, version)),
                modes.GCM(nonce, tag),
            ).decryptor()
            decryptor.authenticate_additional_data(
                f"media:{version}:{recording_id}".encode("ascii"),
            )
            digest = hashlib.sha256()
            written = 0
            try:
                with destination.open("wb") as outgoing:
                    os.chmod(destination, 0o600)
                    remaining = ciphertext_size
                    while remaining:
                        chunk = incoming.read(min(1024 * 1024, remaining))
                        if not chunk:
                            raise RecordingError("Encrypted recording is truncated.", 503)
                        plaintext = decryptor.update(chunk)
                        digest.update(plaintext)
                        outgoing.write(plaintext)
                        written += len(plaintext)
                        remaining -= len(chunk)
                    tail = decryptor.finalize()
                    digest.update(tail)
                    outgoing.write(tail)
                    written += len(tail)
                    outgoing.flush()
                    os.fsync(outgoing.fileno())
            except RecordingError:
                destination.unlink(missing_ok=True)
                raise
            except Exception as error:
                destination.unlink(missing_ok=True)
                raise RecordingError("Encrypted recording failed authentication.", 503) from error
        return version, written, digest.hexdigest()

    @staticmethod
    def media_version(payload: bytes) -> int:
        if len(payload) < len(MEDIA_MAGIC) + 4 or not payload.startswith(MEDIA_MAGIC):
            raise RecordingError("Encrypted recording is malformed.", 503)
        return int.from_bytes(payload[len(MEDIA_MAGIC) : len(MEDIA_MAGIC) + 4], "big")


def ensure_recording_directories(settings: Settings) -> None:
    settings.recording_root.mkdir(parents=True, exist_ok=True, mode=0o700)
    os.chmod(settings.recording_root, 0o700)
    uploads = settings.recording_root / ".uploads"
    uploads.mkdir(mode=0o700, exist_ok=True)
    os.chmod(uploads, 0o700)


def _fsync_directory(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def initialize_retention_policy(session: Session, actor: str) -> RecordingRetentionPolicy:
    policy = session.scalar(
        select(RecordingRetentionPolicy)
        .where(RecordingRetentionPolicy.id == 1)
        .with_for_update(),
    )
    if policy is None:
        policy = RecordingRetentionPolicy(id=1, mode="automatic", days=30, updated_by=actor)
        session.add(policy)
        session.flush()
    return policy


def create_upload_directory(settings: Settings) -> str:
    ensure_recording_directories(settings)
    name = uuid4().hex
    path = settings.recording_root / ".uploads" / name
    path.mkdir(mode=0o700)
    return name


def _upload_directory(settings: Settings, upload: RecordingUpload) -> Path:
    if len(upload.storage_name) != 32 or any(c not in "0123456789abcdef" for c in upload.storage_name):
        raise RecordingError("Upload storage metadata is invalid.", 503)
    path = settings.recording_root / ".uploads" / upload.storage_name
    _require_safe_directory(path)
    return path


def _require_safe_directory(path: Path) -> None:
    try:
        details = path.lstat()
    except FileNotFoundError as error:
        raise RecordingError("Upload storage is unavailable.", 503) from error
    if not stat.S_ISDIR(details.st_mode) or stat.S_ISLNK(details.st_mode):
        raise RecordingError("Upload storage is not a private directory.", 503)
    if details.st_uid != os.geteuid() or details.st_mode & 0o077:
        raise RecordingError("Upload storage ownership or permissions are unsafe.", 503)


def _require_safe_file(path: Path) -> None:
    try:
        details = path.lstat()
    except FileNotFoundError as error:
        raise RecordingError("Recording storage is unavailable.", 503) from error
    if not stat.S_ISREG(details.st_mode) or stat.S_ISLNK(details.st_mode):
        raise RecordingError("Recording storage is not a regular file.", 503)
    if details.st_uid != os.geteuid() or details.st_mode & 0o077:
        raise RecordingError("Recording storage ownership or permissions are unsafe.", 503)


def store_chunk(
    settings: Settings,
    cipher: RecordingCipher,
    recording: Recording,
    upload: RecordingUpload,
    offset: int,
    plaintext: bytes,
    *,
    cipher_identity: str | None = None,
) -> int:
    if not plaintext:
        raise RecordingError("Recording chunk is empty.")
    if len(plaintext) > settings.recording_chunk_limit_bytes:
        raise RecordingError("Recording chunk exceeds 1 MiB.", 413)
    if offset + len(plaintext) > upload.expected_size_bytes:
        raise RecordingError("Recording chunk exceeds the declared size.", 413)

    directory = _upload_directory(settings, upload)
    destination = directory / f"{offset:012x}.chunk"
    identity = cipher_identity or recording.id
    if offset < upload.upload_offset:
        if not destination.exists():
            raise RecordingError("Upload replay does not match stored content.", 409)
        _require_safe_file(destination)
        existing = cipher.decrypt_chunk(identity, offset, destination.read_bytes())
        if existing != plaintext or offset + len(plaintext) > upload.upload_offset:
            raise RecordingError("Upload replay does not match stored content.", 409)
        return upload.upload_offset
    if offset != upload.upload_offset:
        raise RecordingError("Upload offset does not match the server offset.", 409)
    encrypted = cipher.encrypt_chunk(identity, offset, plaintext)
    if destination.exists():
        _require_safe_file(destination)
        existing = cipher.decrypt_chunk(identity, offset, destination.read_bytes())
        if existing != plaintext:
            raise RecordingError("A different chunk already exists at this offset.", 409)
    else:
        file_descriptor, temporary_name = tempfile.mkstemp(prefix=".chunk-", dir=directory)
        temporary = Path(temporary_name)
        try:
            os.fchmod(file_descriptor, 0o600)
            with os.fdopen(file_descriptor, "wb") as handle:
                handle.write(encrypted)
                handle.flush()
                os.fsync(handle.fileno())
            os.replace(temporary, destination)
            _fsync_directory(directory)
        finally:
            temporary.unlink(missing_ok=True)
    upload.upload_offset = offset + len(plaintext)
    upload.updated_at = utcnow()
    recording.updated_at = utcnow()
    return upload.upload_offset


def read_uploaded_wav(
    settings: Settings,
    cipher: RecordingCipher,
    recording: Recording,
    upload: RecordingUpload,
    *,
    cipher_identity: str | None = None,
) -> bytes:
    if upload.upload_offset != upload.expected_size_bytes:
        raise RecordingError("Recording upload is incomplete.", 409)
    directory = _upload_directory(settings, upload)
    chunks: list[bytes] = []
    offset = 0
    while offset < upload.expected_size_bytes:
        path = directory / f"{offset:012x}.chunk"
        _require_safe_file(path)
        plaintext = cipher.decrypt_chunk(cipher_identity or recording.id, offset, path.read_bytes())
        if not plaintext or offset + len(plaintext) > upload.expected_size_bytes:
            raise RecordingError("Encrypted upload chunks are inconsistent.", 422)
        chunks.append(plaintext)
        offset += len(plaintext)
    source = b"".join(chunks)
    if len(source) != upload.expected_size_bytes:
        raise RecordingError("Recording size does not match its receipt.")
    if hashlib.sha256(source).hexdigest() != upload.source_sha256:
        raise RecordingError("Recording hash does not match its receipt.")
    return source


def write_uploaded_wav_to_path(
    settings: Settings,
    cipher: RecordingCipher,
    segment: ConversationRecordingSegment,
    destination: Path,
) -> None:
    """Decrypt one segment into a bounded private temp file without joining chunks in memory."""
    if segment.upload_offset != segment.expected_size_bytes:
        raise RecordingError("Conversation segment upload is incomplete.", 409)
    directory = _upload_directory(settings, segment)
    identity = f"{segment.recording_id}:{segment.segment_index}"
    digest = hashlib.sha256()
    written = 0
    descriptor = os.open(destination, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(descriptor, "wb") as outgoing:
            while written < segment.expected_size_bytes:
                path = directory / f"{written:012x}.chunk"
                _require_safe_file(path)
                plaintext = cipher.decrypt_chunk(identity, written, path.read_bytes())
                if not plaintext or written + len(plaintext) > segment.expected_size_bytes:
                    raise RecordingError("Encrypted conversation chunks are inconsistent.")
                digest.update(plaintext)
                outgoing.write(plaintext)
                written += len(plaintext)
            outgoing.flush()
            os.fsync(outgoing.fileno())
    except Exception:
        destination.unlink(missing_ok=True)
        raise
    if written != segment.expected_size_bytes:
        destination.unlink(missing_ok=True)
        raise RecordingError("Conversation segment size does not match its receipt.")
    if digest.hexdigest() != segment.source_sha256:
        destination.unlink(missing_ok=True)
        raise RecordingError("Conversation segment hash does not match its receipt.")


def inspect_pcm_wav_path(path: Path, expected_duration_ms: int) -> WavDetails:
    _require_safe_file(path)
    details = path.stat()
    with path.open("rb") as handle:
        if handle.read(4) != b"RIFF":
            raise RecordingError("Conversation segment is not a RIFF/WAVE file.")
        declared = struct.unpack("<I", handle.read(4))[0]
        if handle.read(4) != b"WAVE" or declared + 8 != details.st_size:
            raise RecordingError("Conversation WAV length header does not match its content.")
    try:
        with wave.open(str(path), "rb") as audio:
            if audio.getcomptype() != "NONE":
                raise RecordingError("Conversation segments must be uncompressed PCM.")
            if (
                audio.getnchannels() != 2
                or audio.getsampwidth() != 2
                or audio.getframerate() != 48_000
            ):
                raise RecordingError("Conversation segments must be 48 kHz stereo PCM16 WAV.")
            frames = audio.getnframes()
            duration_ms = round(frames * 1000 / 48_000)
            remaining = frames
            while remaining:
                count = min(48_000, remaining)
                payload = audio.readframes(count)
                if len(payload) != count * 4:
                    raise RecordingError("Conversation WAV audio is truncated.")
                remaining -= count
    except RecordingError:
        raise
    except (EOFError, wave.Error) as error:
        raise RecordingError("Conversation segment is not a valid WAV file.") from error
    if duration_ms <= 0 or duration_ms > 180_000:
        raise RecordingError("Conversation segment duration is outside the allowed range.")
    if abs(duration_ms - expected_duration_ms) > 100:
        raise RecordingError("Conversation segment duration does not match its receipt.")
    return WavDetails(duration_ms=duration_ms, frames=frames)


def inspect_caller_wav(source: bytes, expected_duration_ms: int) -> WavDetails:
    if len(source) < 44 or source[:4] != b"RIFF" or source[8:12] != b"WAVE":
        raise RecordingError("Caller recording is not a RIFF/WAVE file.")
    if struct.unpack_from("<I", source, 4)[0] + 8 != len(source):
        raise RecordingError("Caller WAV length header does not match its content.")
    try:
        with wave.open(io.BytesIO(source), "rb") as audio:
            if audio.getcomptype() != "NONE":
                raise RecordingError("Caller recording must be uncompressed PCM.")
            if (
                audio.getnchannels() != 2
                or audio.getsampwidth() != 2
                or audio.getframerate() != 48_000
            ):
                raise RecordingError("Caller recording must be 48 kHz stereo PCM16 WAV.")
            frames = audio.getnframes()
            duration_ms = round(frames * 1000 / 48_000)
            if len(audio.readframes(frames)) != frames * 4:
                raise RecordingError("Caller WAV audio is truncated.")
    except RecordingError:
        raise
    except (EOFError, wave.Error) as error:
        raise RecordingError("Caller recording is not a valid WAV file.") from error
    if duration_ms <= 0 or duration_ms > 180_000:
        raise RecordingError("Caller recording duration is outside the allowed range.")
    if abs(duration_ms - expected_duration_ms) > 100:
        raise RecordingError("Caller recording duration does not match its receipt.")
    return WavDetails(duration_ms=duration_ms, frames=frames)


def _voicemail_filter(source: bytes) -> str:
    """Avoid undefined short/silent loudness measurements and stereo phase cancellation."""
    with wave.open(io.BytesIO(source), "rb") as audio:
        if audio.getnframes() < 24_000:
            return VOICEMAIL_MONO_FILTER
        while frames := audio.readframes(48_000):
            if any(frames):
                return VOICEMAIL_AUDIO_FILTER
    return VOICEMAIL_MONO_FILTER


def transcode_voicemail(source: bytes) -> bytes:
    try:
        process = subprocess.run(
            [
                "ffmpeg",
                "-nostdin",
                "-hide_banner",
                "-loglevel",
                "error",
                "-f",
                "wav",
                "-i",
                "pipe:0",
                "-vn",
                "-af",
                _voicemail_filter(source),
                "-ac",
                "1",
                "-ar",
                "16000",
                "-c:a",
                "libmp3lame",
                "-b:a",
                "48k",
                "-f",
                "mp3",
                "pipe:1",
            ],
            input=source,
            capture_output=True,
            timeout=240,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise RecordingError("Voicemail audio conversion is unavailable.", 503) from error
    if process.returncode != 0 or not process.stdout.startswith((b"ID3", b"\xff")):
        raise RecordingError("Voicemail audio conversion failed.", 422)
    return process.stdout


def recording_used_bytes(
    session: Session,
    *,
    exclude_upload_recording_id: str | None = None,
    exclude_conversation_recording_id: str | None = None,
) -> int:
    """Return committed recording bytes, including incomplete upload reservations.

    Reserving the declared source size prevents many concurrent/incomplete uploads from
    overcommitting the dedicated recording volume. Finalization replaces the current reservation
    with the exact encrypted MP3 size under the retention-policy lock and may still return 507 if
    encoding overhead or another committed item consumes the remaining space.
    """
    ready = int(
        session.scalar(
            select(func.coalesce(func.sum(Recording.media_size_bytes), 0)).where(
                Recording.status == "ready",
            ),
        )
        or 0
    )
    pending_statement = select(
        func.coalesce(func.sum(RecordingUpload.expected_size_bytes), 0),
    )
    if exclude_upload_recording_id is not None:
        pending_statement = pending_statement.where(
            RecordingUpload.recording_id != exclude_upload_recording_id,
        )
    pending = int(session.scalar(pending_statement) or 0)
    segment_statement = select(
        func.coalesce(func.sum(ConversationRecordingSegment.expected_size_bytes), 0),
    ).where(ConversationRecordingSegment.status.in_(["uploading", "verified"]))
    if exclude_conversation_recording_id is not None:
        segment_statement = segment_statement.where(
            ConversationRecordingSegment.recording_id != exclude_conversation_recording_id,
        )
    segments = int(session.scalar(segment_statement) or 0)
    return ready + pending + segments


def assemble_conversation_recording(
    session: Session,
    settings: Settings,
    cipher: RecordingCipher,
    recording: Recording,
    segments: list[ConversationRecordingSegment],
    *, confirmed_abandoned: bool = False,
) -> Path:
    """Validate and assemble ordered PCM segments through private bounded temp files."""
    if not segments:
        raise RecordingError("Conversation recording has no segments.", 409)
    if [item.segment_index for item in segments] != list(range(len(segments))):
        raise RecordingError("Conversation segment indexes are not contiguous.", 409)
    if any(item.status != "verified" for item in segments):
        raise RecordingError("Conversation recording has unverified segments.", 409)
    if any(item.stop_reason != "segment_boundary" for item in segments[:-1]):
        raise RecordingError("Only the final conversation segment may have a terminal stop reason.", 409)
    if segments[-1].stop_reason == "segment_boundary" and not confirmed_abandoned:
        raise RecordingError("Final conversation segment must have a terminal stop reason.", 409)

    total_duration_ms = 0
    pcm_digest = hashlib.sha256()
    with tempfile.TemporaryDirectory(prefix=".conversation-", dir=settings.recording_root) as name:
        workspace = Path(name)
        os.chmod(workspace, 0o700)
        # Raw PCM avoids RIFF's 32-bit file-size ceiling. Conversation duration is intentionally
        # not flow-limited; the storage quota and segment-count cap remain the operational bounds.
        combined_path = workspace / "combined.pcm"
        descriptor = os.open(combined_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "wb") as combined:
            previous_captured_at = None
            for segment in segments:
                if previous_captured_at is not None and segment.captured_at < previous_captured_at:
                    raise RecordingError("Conversation segment timestamps are out of order.")
                previous_captured_at = segment.captured_at
                segment_path = workspace / f"{segment.segment_index:05d}.wav"
                write_uploaded_wav_to_path(settings, cipher, segment, segment_path)
                details = inspect_pcm_wav_path(segment_path, segment.duration_ms)
                total_duration_ms += details.duration_ms
                with wave.open(str(segment_path), "rb") as source:
                    remaining = source.getnframes()
                    while remaining:
                        count = min(48_000, remaining)
                        frames = source.readframes(count)
                        if len(frames) != count * 4:
                            raise RecordingError("Conversation segment changed during assembly.", 503)
                        pcm_digest.update(frames)
                        combined.write(frames)
                        remaining -= count
                segment_path.unlink()
            combined.flush()
            os.fsync(combined.fileno())
        _require_safe_file(combined_path)

        encoded_path = workspace / "conversation.mp3"
        try:
            process = subprocess.run(
                [
                    "ffmpeg",
                    "-nostdin",
                    "-hide_banner",
                    "-loglevel",
                    "error",
                    "-f",
                    "s16le",
                    "-ar",
                    "48000",
                    "-ac",
                    "2",
                    "-i",
                    str(combined_path),
                    "-vn",
                    "-ac",
                    "2",
                    "-ar",
                    "48000",
                    "-c:a",
                    "libmp3lame",
                    "-b:a",
                    "96k",
                    "-f",
                    "mp3",
                    str(encoded_path),
                ],
                capture_output=True,
                timeout=900,
                check=False,
            )
        except (OSError, subprocess.TimeoutExpired) as error:
            raise RecordingError("Conversation audio assembly is unavailable.", 503) from error
        if process.returncode != 0 or not encoded_path.is_file():
            raise RecordingError("Conversation audio assembly failed.")
        os.chmod(encoded_path, 0o600)
        _require_safe_file(encoded_path)
        with encoded_path.open("rb") as media:
            prefix = media.read(3)
            if prefix != b"ID3" and not prefix.startswith(b"\xff"):
                raise RecordingError("Conversation audio assembly produced invalid media.")

        policy = session.scalar(
            select(RecordingRetentionPolicy)
            .where(RecordingRetentionPolicy.id == 1)
            .with_for_update(),
        )
        if policy is None:
            raise RecordingError("Recording retention state is unavailable.", 503)
        encrypted_size = encoded_path.stat().st_size + len(MEDIA_MAGIC) + 4 + 12 + 16
        if (
            recording_used_bytes(
                session,
                exclude_conversation_recording_id=recording.id,
            )
            + encrypted_size
            > settings.recording_quota_bytes
        ):
            raise RecordingError(
                "Recording storage quota is full; the device must retain its spool.",
                507,
            )

        storage_name = f"{uuid4().hex}.rec"
        destination = settings.recording_root / storage_name
        descriptor, temporary_name = tempfile.mkstemp(prefix=".media-", dir=settings.recording_root)
        os.close(descriptor)
        temporary = Path(temporary_name)
        try:
            media_size, media_sha256 = cipher.encrypt_media_file(
                recording.id,
                encoded_path,
                temporary,
            )
            os.replace(temporary, destination)
            _fsync_directory(settings.recording_root)
        finally:
            temporary.unlink(missing_ok=True)

    recording.duration_ms = total_duration_ms
    recording.stop_reason = "recording_failure" if confirmed_abandoned else segments[-1].stop_reason
    recording.partial = confirmed_abandoned or any(item.partial for item in segments)
    recording.source_size_bytes = sum(item.expected_size_bytes for item in segments)
    recording.source_sha256 = pcm_digest.hexdigest()
    recording.segment_count = len(segments)
    recording.media_key_version = cipher.version
    recording.media_storage_name = storage_name
    recording.media_size_bytes = media_size
    recording.media_sha256 = media_sha256
    recording.status = "ready"
    recording.updated_at = utcnow()
    for segment in segments:
        segment.status = "assembled"
        segment.updated_at = utcnow()
    return destination


def finalize_recording(
    session: Session,
    settings: Settings,
    cipher: RecordingCipher,
    recording: Recording,
    upload: RecordingUpload,
) -> Path:
    source = read_uploaded_wav(settings, cipher, recording, upload)
    details = inspect_caller_wav(source, recording.duration_ms)
    encoded = transcode_voicemail(source)
    encrypted = cipher.encrypt_media(recording.id, encoded)
    policy = session.scalar(
        select(RecordingRetentionPolicy)
        .where(RecordingRetentionPolicy.id == 1)
        .with_for_update(),
    )
    if policy is None:
        raise RecordingError("Voicemail retention state is unavailable.", 503)
    if (
        recording_used_bytes(
            session,
            exclude_upload_recording_id=recording.id,
        )
        + len(encrypted)
        > settings.recording_quota_bytes
    ):
        raise RecordingError("Voicemail storage quota is full; the device must retain its spool.", 507)

    storage_name = f"{uuid4().hex}.rec"
    destination = settings.recording_root / storage_name
    file_descriptor, temporary_name = tempfile.mkstemp(prefix=".media-", dir=settings.recording_root)
    temporary = Path(temporary_name)
    try:
        os.fchmod(file_descriptor, 0o600)
        with os.fdopen(file_descriptor, "wb") as handle:
            handle.write(encrypted)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, destination)
        _fsync_directory(settings.recording_root)
    finally:
        temporary.unlink(missing_ok=True)

    recording.duration_ms = details.duration_ms
    recording.media_key_version = cipher.version
    recording.media_storage_name = storage_name
    recording.media_size_bytes = len(encrypted)
    recording.media_sha256 = hashlib.sha256(encoded).hexdigest()
    recording.status = "ready"
    recording.updated_at = utcnow()
    return destination


def load_media(
    settings: Settings,
    cipher: RecordingCipher,
    recording: Recording,
) -> bytes:
    if recording.status != "ready" or not recording.media_storage_name:
        raise RecordingError("Voicemail audio is unavailable.", 404)
    path = _media_path(settings, recording)
    details = path.lstat()
    if recording.media_size_bytes is None or details.st_size != recording.media_size_bytes:
        raise RecordingError("Recording media size verification failed.", 503)
    payload = path.read_bytes()
    if (
        recording.media_key_version is None
        or cipher.media_version(payload) != recording.media_key_version
    ):
        raise RecordingError("Recording media key-version verification failed.", 503)
    plaintext = cipher.decrypt_media(recording.id, payload)
    if recording.media_sha256 and hashlib.sha256(plaintext).hexdigest() != recording.media_sha256:
        raise RecordingError("Recording media hash verification failed.", 503)
    return plaintext


def materialize_media(
    settings: Settings,
    cipher: RecordingCipher,
    recording: Recording,
    destination: Path,
) -> int:
    if recording.status != "ready" or not recording.media_storage_name:
        raise RecordingError("Recording audio is unavailable.", 404)
    path = _media_path(settings, recording)
    details = path.lstat()
    if recording.media_size_bytes is None or details.st_size != recording.media_size_bytes:
        raise RecordingError("Recording media size verification failed.", 503)
    version, size, digest = cipher.decrypt_media_file(recording.id, path, destination)
    if recording.media_key_version is None or version != recording.media_key_version:
        destination.unlink(missing_ok=True)
        raise RecordingError("Recording media key-version verification failed.", 503)
    if recording.media_sha256 and digest != recording.media_sha256:
        destination.unlink(missing_ok=True)
        raise RecordingError("Recording media hash verification failed.", 503)
    return size


def remove_upload_files(settings: Settings, upload: RecordingUpload) -> None:
    try:
        directory = _upload_directory(settings, upload)
    except RecordingError:
        return
    for path in directory.iterdir():
        if path.is_file() and not path.is_symlink():
            path.unlink(missing_ok=True)
    directory.rmdir()


def delete_media_file(settings: Settings, recording: Recording) -> None:
    if not recording.media_storage_name:
        return
    path = settings.recording_root / recording.media_storage_name
    try:
        _require_safe_file(path)
    except RecordingError:
        return
    path.unlink(missing_ok=True)


def _media_path(settings: Settings, recording: Recording) -> Path:
    name = recording.media_storage_name
    if (
        name is None
        or len(name) != 36
        or not name.endswith(".rec")
        or any(c not in "0123456789abcdef" for c in name[:-4])
    ):
        raise RecordingError("Recording storage metadata is invalid.", 503)
    path = settings.recording_root / name
    _require_safe_file(path)
    return path


def quarantine_media(settings: Settings, recording: Recording) -> QuarantinedMedia:
    original = _media_path(settings, recording)
    quarantine = settings.recording_root / f".delete-{original.name}"
    if quarantine.exists() or quarantine.is_symlink():
        raise RecordingError("Recording deletion is already in progress.", 409)
    os.replace(original, quarantine)
    _fsync_directory(settings.recording_root)
    return QuarantinedMedia(original=original, quarantine=quarantine)


def restore_quarantined_media(settings: Settings, item: QuarantinedMedia) -> None:
    if item.quarantine.exists() and not item.original.exists():
        _require_safe_file(item.quarantine)
        os.replace(item.quarantine, item.original)
        _fsync_directory(settings.recording_root)


def discard_quarantined_media(settings: Settings, item: QuarantinedMedia) -> None:
    if item.quarantine.exists():
        _require_safe_file(item.quarantine)
        item.quarantine.unlink()
        _fsync_directory(settings.recording_root)


def reconcile_quarantined_media(session: Session, settings: Settings) -> None:
    ensure_recording_directories(settings)
    for path in settings.recording_root.glob(".delete-*.rec"):
        _require_safe_file(path)
        storage_name = path.name.removeprefix(".delete-")
        recording = session.scalar(
            select(Recording).where(
                Recording.media_storage_name == storage_name,
                Recording.status == "ready",
            ),
        )
        original = settings.recording_root / storage_name
        if recording is not None:
            if original.exists() or original.is_symlink():
                raise RecordingError("Recording deletion recovery found conflicting media.", 503)
            os.replace(path, original)
        else:
            path.unlink()
        _fsync_directory(settings.recording_root)


def reconcile_recording_storage(session: Session, settings: Settings) -> None:
    """Remove only private crash artifacts that no committed row owns."""
    ensure_recording_directories(settings)
    changed_root = False
    for path in settings.recording_root.iterdir():
        if path.name == ".processing":
            state = path.lstat()
            if not stat.S_ISDIR(state.st_mode) or state.st_uid != os.getuid() or state.st_mode & 0o077:
                raise RecordingError("Recording processing directory is unsafe.", 503)
            # Only the worker holding its processing lock may remove job remnants.
            continue
        if path.name == ".uploads" or path.name.startswith(".delete-"):
            continue
        if path.name.startswith((".media-", ".playback-")):
            _require_safe_file(path)
            path.unlink()
            changed_root = True
            continue
        if path.name.endswith(".rec"):
            if (
                len(path.name) != 36
                or any(character not in "0123456789abcdef" for character in path.name[:-4])
            ):
                raise RecordingError("Recording storage contains an unknown media file.", 503)
            _require_safe_file(path)
            owner = session.scalar(
                select(Recording.id).where(
                    Recording.media_storage_name == path.name,
                    Recording.status.in_(["ready", "processing"]),
                ),
            )
            if owner is None:
                path.unlink()
                changed_root = True
            continue
        raise RecordingError("Recording storage contains an unknown entry.", 503)
    if changed_root:
        _fsync_directory(settings.recording_root)

    uploads_root = settings.recording_root / ".uploads"
    active_names = set(session.scalars(select(RecordingUpload.storage_name)))
    active_names.update(
        session.scalars(
            select(ConversationRecordingSegment.storage_name).where(
                ConversationRecordingSegment.status.in_(["uploading", "verified"]),
            ),
        ),
    )
    changed_uploads = False
    for path in uploads_root.iterdir():
        if (
            len(path.name) != 32
            or any(character not in "0123456789abcdef" for character in path.name)
        ):
            raise RecordingError("Recording upload storage contains an unknown entry.", 503)
        _require_safe_directory(path)
        if path.name in active_names:
            continue
        for child in path.iterdir():
            _require_safe_file(child)
            child.unlink()
        path.rmdir()
        changed_uploads = True
    if changed_uploads:
        _fsync_directory(uploads_root)


def tombstone_recording(recording: Recording, reason: str) -> None:
    recording.status = "deleted"
    recording.deleted_at = utcnow()
    recording.deletion_reason = reason
    recording.media_storage_name = None
    recording.media_key_version = None
    recording.media_size_bytes = None
    recording.media_sha256 = None
    recording.updated_at = utcnow()


def purge_expired_recordings(
    session: Session,
    settings: Settings,
    policy: RecordingRetentionPolicy,
) -> list[QuarantinedMedia]:
    if policy.mode != "automatic":
        return []
    cutoff = utcnow() - timedelta(days=policy.days)
    expired = session.scalars(
        select(Recording).where(
            Recording.status == "ready",
            Recording.captured_at < cutoff,
        ),
    ).all()
    quarantined: list[QuarantinedMedia] = []
    try:
        for recording in expired:
            quarantined.append(quarantine_media(settings, recording))
            tombstone_recording(recording, "retention")
    except Exception:
        for item in reversed(quarantined):
            restore_quarantined_media(settings, item)
        raise
    return quarantined


def cleanup_abandoned_uploads(
    session: Session,
    settings: Settings,
) -> list[RecordingUpload | ConversationRecordingSegment]:
    cutoff = utcnow() - timedelta(hours=settings.recording_abandoned_upload_hours)
    uploads = session.scalars(select(RecordingUpload).where(RecordingUpload.updated_at < cutoff,
        ~RecordingUpload.recording_id.in_(select(ContinuousRecording.recording_id)))).all()
    # Continuous sources remain owned until explicit completion or device reconciliation.
    # Elapsed time alone cannot distinguish an offline tablet from abandoned audio.
    for upload in uploads:
        recording = session.get(Recording, upload.recording_id)
        session.delete(upload)
        if recording is not None and recording.status == "uploading":
            recording.status = "failed"
            recording.updated_at = utcnow()
    segments = session.scalars(
        select(ConversationRecordingSegment).where(
            ConversationRecordingSegment.status == "uploading",
            ConversationRecordingSegment.updated_at < cutoff,
        ),
    ).all()
    for segment in segments:
        segment.status = "failed"
        segment.updated_at = utcnow()
    return [*uploads, *segments]
