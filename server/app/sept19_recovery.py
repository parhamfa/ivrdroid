"""Evidence-pinned September 19 repair. Dry run by default; never sends notifications."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil

from sqlalchemy import select

from .call_history import merge_events
from .config import load_settings
from .continuous_recording_worker import worker_lock
from .database import Database
from .models import CallRecord, CallEventReceipt, CallHistoryCorrection, ContinuousRecording, Device, Recording, RecordingUpload
from .recording_service import RecordingCipher, _require_safe_file, _upload_directory

EVIDENCE_SHA256 = "da70b6014001a8485325edf16867682e833756ee9f5ddebb2226982dea09ffaf"
CORRECTIONS = {
    "8bece748-b775-42b4-9c2b-beaf5f7c4acf": ("2026-09-19T10:47:48.156+00:00", "recovered"),
    "93ad1fcf-9d05-474c-898f-0fdbedf897a0": ("2026-09-19T10:49:03.907+00:00", "failed"),
    "394d15f4-ad5d-4cf4-a219-ffc62cf4c991": ("2026-09-19T10:51:57.098+00:00", "recovered"),
}
SOURCES = {
    "8bece748-b775-42b4-9c2b-beaf5f7c4acf": (20246400, "851f3aa72cdf05942ce137e000d8f8fb44d38614f8840d52d326a3ed7d2f73e4"),
    "394d15f4-ad5d-4cf4-a219-ffc62cf4c991": (20059200, "1e4a9a84634ba8e368500c5d504380b79643557f2503aa6b8fd76c3703a2ffac"),
}


def evidence_bundle(directory: Path) -> dict:
    index = (directory / "SHA256SUMS").read_bytes()
    if hashlib.sha256(index).hexdigest() != EVIDENCE_SHA256:
        raise ValueError("Evidence index differs from the investigated snapshot")
    for line in index.decode().splitlines():
        digest, name = line.split(maxsplit=1)
        target = directory / name.lstrip("*")
        if target.is_symlink() or target.resolve().parent != directory.resolve():
            raise ValueError("Evidence index contains a nonlocal file")
        if hashlib.sha256(target.read_bytes()).hexdigest() != digest:
            raise ValueError(f"Evidence changed: {target.name}")
    return {"calls": json.loads((directory / "calls.json").read_text()),
            "events": json.loads((directory / "external-events.json").read_text())}


def row_document(row):
    return {column.name: (value.isoformat() if isinstance(value, datetime) else value)
            for column in row.__table__.columns if (value := getattr(row, column.name)) is not None}


def proposal(call: CallRecord, evidence: dict) -> dict:
    saved = next(item for item in evidence["calls"] if item["id"] == call.id)
    started = call.started_at.replace(tzinfo=timezone.utc) if call.started_at.tzinfo is None else call.started_at
    if started != datetime.fromisoformat(saved["started_at"]) or call.result != saved["result"]:
        raise ValueError(f"Call {call.id} changed since the investigation; review it before correction")
    ended, cleanup = CORRECTIONS[call.id]
    end = datetime.fromisoformat(ended)
    events = [{"event": "external_call", **{key: item[key] for key in ("occurred_at", "status", "block_id", "reason")}}
              for item in evidence["events"] if item["session_id"] == call.id]
    return {"result": "REMOTE_HANGUP", "ended_at": ended, "cleanup_status": cleanup,
            "duration_seconds": int((end - started).total_seconds()), "events": events}


def authenticate_source(settings, cipher, recording, upload, expected):
    if upload is None or recording.source_size_bytes != expected[0] or recording.source_sha256 != expected[1]:
        raise ValueError("Preserved upload does not match the investigated source")
    if upload.upload_offset != expected[0]:
        raise ValueError("Preserved upload is incomplete")
    directory = _upload_directory(settings, upload)
    offset = 0
    digest = hashlib.sha256()
    while offset < expected[0]:
        path = directory / f"{offset:012x}.chunk"
        _require_safe_file(path)
        if path.stat().st_size > settings.recording_chunk_limit_bytes + 128:
            raise ValueError("Upload chunk exceeds its bound")
        data = cipher.decrypt_chunk(recording.id, offset, path.read_bytes())
        if not data or offset + len(data) > expected[0]:
            raise ValueError("Unexpected upload chunk boundary")
        digest.update(data); offset += len(data)
    if digest.hexdigest() != expected[1]:
        raise ValueError("Preserved source checksum mismatch")


def durable_json(path: Path, value):
    with path.open("x", encoding="utf-8") as stream:
        os.chmod(path, 0o600)
        json.dump(value, stream, indent=2); stream.write("\n"); stream.flush(); os.fsync(stream.fileno())


def backup(session, settings, destination: Path, evidence_dir: Path):
    destination.mkdir(mode=0o700, parents=False, exist_ok=False)
    calls = session.scalars(select(CallRecord).where(CallRecord.id.in_(CORRECTIONS))).all()
    recordings = session.scalars(select(Recording).where(Recording.call_id.in_(CORRECTIONS))).all()
    ids = [row.id for row in recordings]
    uploads = session.scalars(select(RecordingUpload).where(RecordingUpload.recording_id.in_(ids))).all()
    rows = {"call_records": [row_document(row) for row in calls], "recordings": [row_document(row) for row in recordings],
            "recording_uploads": [row_document(row) for row in uploads],
            "continuous_recordings": [row_document(row) for row in session.scalars(select(ContinuousRecording).where(ContinuousRecording.recording_id.in_(ids)))],
            "call_event_receipts": [row_document(row) for row in session.scalars(select(CallEventReceipt).where(CallEventReceipt.call_id.in_(CORRECTIONS)))],
            "call_history_corrections": [row_document(row) for row in session.scalars(select(CallHistoryCorrection).where(CallHistoryCorrection.call_id.in_(CORRECTIONS)))]}
    durable_json(destination / "original-rows.json", rows)
    audio = destination / "encrypted-audio"; audio.mkdir(mode=0o700)
    for upload in uploads:
        source = _upload_directory(settings, upload)
        for path in source.iterdir():
            _require_safe_file(path)
        shutil.copytree(source, audio / upload.storage_name)
    for recording in recordings:
        if recording.media_storage_name:
            source = settings.recording_root / recording.media_storage_name
            _require_safe_file(source)
            shutil.copy2(source, audio / recording.media_storage_name)
    shutil.copytree(evidence_dir, destination / "evidence", symlinks=False)
    checksums = {}
    for path in destination.rglob("*"):
        if path.is_file():
            with path.open("rb") as stream:
                checksums[str(path.relative_to(destination))] = hashlib.file_digest(stream, "sha256").hexdigest()
                os.fsync(stream.fileno())
    durable_json(destination / "SHA256.json", checksums)
    for path in [p for p in destination.rglob("*") if p.is_dir()] + [destination, destination.parent]:
        fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try: os.fsync(fd)
        finally: os.close(fd)


def correct_call(session, call, changes):
    existing = session.get(CallHistoryCorrection, call.id)
    if existing:
        if existing.evidence_sha256 != EVIDENCE_SHA256:
            raise ValueError("A different historical correction already exists")
        return
    original = row_document(call)
    call.result = changes["result"]
    call.ended_at = datetime.fromisoformat(changes["ended_at"])
    call.duration_seconds = changes["duration_seconds"]
    call.cleanup_status = changes["cleanup_status"]
    call.terminal_notification_scheduled = True
    merge_events(session, call.device_id, call.id, changes["events"], call)
    for receipt in session.scalars(select(CallEventReceipt).where(CallEventReceipt.call_id == call.id)):
        receipt.notification_scheduled = True
    session.add(CallHistoryCorrection(call_id=call.id, evidence_sha256=EVIDENCE_SHA256,
                                     original=original, applied=changes))


def run(settings, evidence_dir: Path, *, apply=False, backup_dir: Path | None = None):
    evidence = evidence_bundle(evidence_dir)
    database = Database(settings.database_url)
    cipher = RecordingCipher(settings.recording_encryption_key_b64, settings.recording_encryption_key_version,
                             settings.recording_encryption_previous_keys_json)
    try:
        with worker_lock(database) as acquired, database.session() as session:
            if not acquired:
                raise RuntimeError("Recording worker is busy; retry while it is idle")
            # Same per-device lock as event ingestion. Nothing can overwrite the backup/correction gap.
            device_ids = session.scalars(select(CallRecord.device_id).where(CallRecord.id.in_(CORRECTIONS))).all()
            session.execute(select(Device).where(Device.id.in_(device_ids)).order_by(Device.id).with_for_update()).all()
            plan = {}; requeue = []
            for call_id in CORRECTIONS:
                call = session.get(CallRecord, call_id)
                if call is None: raise ValueError(f"Missing investigated call {call_id}")
                corrected = session.get(CallHistoryCorrection, call_id)
                if corrected and corrected.evidence_sha256 != EVIDENCE_SHA256:
                    raise ValueError("Existing correction belongs to different evidence")
                if not corrected: plan[call_id] = proposal(call, evidence)
            for call_id, expected in SOURCES.items():
                recording = session.scalar(select(Recording).where(Recording.call_id == call_id, Recording.kind == "session_audit"))
                receipt = session.get(ContinuousRecording, recording.id) if recording else None
                if recording is None or receipt is None: raise ValueError(f"Missing upload receipt {call_id}")
                if recording.status == "ready": continue
                if recording.kind != "session_audit" or receipt.validation_error:
                    raise ValueError("Recording identity or validation changed; explicit review required")
                authenticate_source(settings, cipher, recording, session.get(RecordingUpload, recording.id), expected)
                requeue.append(recording.id)
            if apply:
                if backup_dir is None: raise ValueError("--apply requires a new --backup-dir")
                backup(session, settings, backup_dir, evidence_dir)
                for call_id, changes in plan.items(): correct_call(session, session.get(CallRecord, call_id), changes)
                for call_id in requeue:
                    receipt = session.get(ContinuousRecording, call_id)
                    receipt.state = "queued"; receipt.retry_at = None
                    session.get(Recording, call_id).status = "processing"
                session.commit()
                durable_json(backup_dir / "applied.json", {"evidence_sha256": EVIDENCE_SHA256, "corrections": plan, "requeued": requeue})
            return {"applied": apply, "corrections": plan, "requeue_verified_sources": requeue,
                    "tablet_partial": "93ad1fcf-9d05-474c-898f-0fdbedf897a0 remains separate: preserve encrypted spool/Keystore; upload and inspect timing discrepancy without extending the call",
                    "notifications": "suppressed for historical corrections", "playback_verified": False}
    finally:
        database.engine.dispose()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--backup-dir", type=Path)
    args = parser.parse_args()
    print(json.dumps(run(load_settings(), args.evidence, apply=args.apply, backup_dir=args.backup_dir), indent=2))
