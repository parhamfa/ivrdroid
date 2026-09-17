"""Serial, restartable PCM processing with bounded memory and durable source retention."""
from contextlib import contextmanager
from datetime import timedelta
import hashlib
import logging
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import time
from uuid import uuid4

from sqlalchemy import or_, select, text

from .models import CallRecord, ContinuousRecording, Recording, RecordingUpload, utcnow
from .ntfy import dispatch_ntfy
from .crypto import mask_phone
from .recording_service import (MEDIA_MAGIC, RecordingError, _fsync_directory, _require_safe_file,
    _upload_directory, initialize_retention_policy, recording_used_bytes, remove_upload_files)

LOGGER = logging.getLogger(__name__)
_local_lock = threading.Lock()
_LOCK_ID = 0x49565243504D


@contextmanager
def worker_lock(database):
    """Session advisory lock survives commits, releases on process/connection failure."""
    if not _local_lock.acquire(blocking=False):
        yield False
        return
    connection = None
    locked = False
    try:
        if database.engine.dialect.name == "postgresql":
            connection = database.engine.connect()
            locked = bool(connection.scalar(text("SELECT pg_try_advisory_lock(:key)"), {"key": _LOCK_ID}))
        else:
            locked = True
        yield locked
    finally:
        try:
            if connection is not None:
                if locked:
                    connection.execute(text("SELECT pg_advisory_unlock(:key)"), {"key": _LOCK_ID})
                connection.close()
        finally:
            _local_lock.release()


def _materialize(settings, cipher, recording, upload, path):
    directory = _upload_directory(settings, upload)
    offset = 0
    digest = hashlib.sha256()
    with path.open("xb") as target:
        os.chmod(path, 0o600)
        while offset < upload.expected_size_bytes:
            part = directory / f"{offset:012x}.chunk"
            _require_safe_file(part)
            # Enforce an allocation bound before reading even locally corrupted files.
            if part.stat().st_size > settings.recording_chunk_limit_bytes + 128:
                raise RecordingError("Encrypted upload record exceeds its bound")
            data = cipher.decrypt_chunk(recording.id, offset, part.read_bytes())
            if not data or offset + len(data) > upload.expected_size_bytes:
                raise RecordingError("Continuous audio contains invalid byte ranges")
            target.write(data); digest.update(data); offset += len(data)
        target.flush(); os.fsync(target.fileno())
    if offset != recording.source_size_bytes or digest.hexdigest() != recording.source_sha256:
        raise RecordingError("Continuous audio failed size or checksum verification")


def _process(session, settings, cipher, recording, receipt, upload):
    if upload is None or upload.upload_offset != recording.source_size_bytes:
        raise RecordingError("Durable upload source is missing or incomplete", 503)
    reserve = settings.audit_recording_reserve_bytes
    # Raw source plus encoded, encrypted and authenticated playback copies.
    needed = recording.source_size_bytes + recording.duration_ms * 36 + reserve
    if shutil.disk_usage(settings.recording_root).free < needed:
        raise RecordingError("Server processing workspace is full; audio is preserved for retry", 507)
    work_root = settings.recording_root.parent / (settings.recording_root.name + "-processing")
    work_root.mkdir(mode=0o700, exist_ok=True)
    if work_root.is_symlink() or work_root.stat().st_uid != os.getuid():
        raise RecordingError("Server processing workspace is unsafe", 503)
    os.chmod(work_root, 0o700)
    # Only the process holding the cross-worker lock may touch these crash remnants.
    for prior in work_root.iterdir():
        if prior.is_dir() and not prior.is_symlink() and prior.name.startswith("job-"):
            shutil.rmtree(prior)
        else:
            raise RecordingError("Unexpected processing workspace entry", 503)
    with tempfile.TemporaryDirectory(prefix="job-", dir=work_root) as name:
        workspace = Path(name)
        raw = workspace / "audio.pcm"
        encoded = workspace / "audio.mp3"
        encrypted = workspace / "audio.rec"
        _materialize(settings, cipher, recording, upload, raw)
        try:
            with (workspace / "conversion-error.log").open("w+b") as diagnostic:
                process = subprocess.run(["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
                    "-f", "s16le", "-ar", "48000", "-ac", "2", "-i", str(raw),
                    "-vn", "-c:a", "libmp3lame", "-b:a", "96k", "-f", "mp3", str(encoded)],
                    stdout=subprocess.DEVNULL, stderr=diagnostic, timeout=3600, check=False)
                diagnostic.seek(0)
                detail = diagnostic.read(1024).decode("utf-8", errors="replace").replace(str(workspace), "<workspace>")
        except (OSError, subprocess.TimeoutExpired) as error:
            raise RecordingError("Audio conversion is unavailable; durable source retained", 503) from error
        if process.returncode != 0 or not encoded.is_file():
            raise RecordingError(f"Audio conversion failed (exit {process.returncode}): {detail}; durable source retained", 503)
        os.chmod(encoded, 0o600)
        with encoded.open("rb") as source:
            if not source.read(3).startswith((b"ID3", b"\xff")):
                raise RecordingError("Audio conversion produced invalid playback media", 503)
        initialize_retention_policy(session, settings.development_admin_email)
        encrypted_size = encoded.stat().st_size + len(MEDIA_MAGIC) + 32
        if recording_used_bytes(session, exclude_upload_recording_id=recording.id) + encrypted_size > settings.recording_quota_bytes:
            raise RecordingError("Server recording quota is full; durable source retained", 507)
        media_size, media_hash = cipher.encrypt_media_file(recording.id, encoded, encrypted)
        # Authenticate the file on disk before making it authoritative and acknowledging it.
        verified = workspace / "verified.mp3"
        _, _, verified_hash = cipher.decrypt_media_file(recording.id, encrypted, verified)
        if verified_hash != media_hash:
            raise RecordingError("Stored playback media failed verification", 503)
        storage_name = uuid4().hex + ".rec"
        # Persist the target reservation before its atomic rename, so reconciliation cannot
        # erase it in the rename/commit window. A crash merely repeats this job from source.
        old_storage = recording.media_storage_name
        recording.media_storage_name = storage_name
        session.commit()
        os.replace(encrypted, settings.recording_root / storage_name)
        _fsync_directory(settings.recording_root)
        if old_storage and old_storage != storage_name:
            (settings.recording_root / old_storage).unlink(missing_ok=True)
        recording.media_size_bytes = media_size; recording.media_sha256 = media_hash
        recording.media_key_version = cipher.version; recording.status = "ready"
        recording.updated_at = utcnow(); recording.processing_error = None
        receipt.state = "ready"; receipt.accepted_at = utcnow(); receipt.retry_at = None
        session.delete(upload)
        session.commit()
        # The DB acknowledgment follows fsync. Source deletion is deliberately last.
        remove_upload_files(settings, upload)


def process_next(application) -> bool:
    database = application.state.database
    with worker_lock(database) as locked:
        if not locked:
            return False
        with database.session() as session:
            receipt = session.scalar(select(ContinuousRecording).where(
                ContinuousRecording.state.in_(["queued", "processing", "retrying"]),
                or_(ContinuousRecording.retry_at.is_(None), ContinuousRecording.retry_at <= utcnow())
            ).order_by(ContinuousRecording.updated_at, ContinuousRecording.recording_id).limit(1))
            if receipt is None:
                return False
            recording = session.get(Recording, receipt.recording_id)
            if recording.status == "deleted":
                receipt.state = "deleted"; session.commit(); return True
            receipt.state = "processing"; receipt.attempts += 1; receipt.updated_at = utcnow()
            recording.status = "processing"; session.commit()
            started = time.monotonic()
            try:
                _process(session, application.state.settings, application.state.recording_cipher,
                         recording, receipt, session.get(RecordingUpload, recording.id))
                if recording.kind == "conversation":
                    # Preserve the existing configured ready notification. Notification
                    # delivery is never part of the durable recording acknowledgment.
                    try:
                        call = session.get(CallRecord, recording.call_id)
                        caller = application.state.cipher.decrypt(call.caller_encrypted) if call and call.caller_encrypted else None
                        dispatch_ntfy(application, "conversation_ready", title="Conversation ready",
                            message="An external-call recording is ready to play.",
                            click_path=f"/voicemail?recording={recording.id}", tags="telephone_receiver,ivrdroid",
                            caller_masked=mask_phone(caller) if caller else None)
                    except Exception:
                        LOGGER.exception("Recording ready notification failed for %s", recording.id)
            except Exception as error:
                session.rollback()
                session.refresh(recording); session.refresh(receipt)
                if receipt.accepted_at is None:
                    permanent = isinstance(error, RecordingError) and error.status_code == 422
                    receipt.state = "invalid" if permanent else "retrying"
                    receipt.retry_at = None if permanent else utcnow() + timedelta(seconds=min(3600, 30 * 2 ** min(receipt.attempts, 7)))
                    recording.status = "failed" if permanent else "processing"
                    recording.processing_error = str(error)[:500]
                LOGGER.exception("Continuous recording %s processing failed", receipt.recording_id)
            finally:
                receipt.operation_duration_ms = int((time.monotonic() - started) * 1000)
                receipt.updated_at = utcnow()
                session.commit()
            return True
