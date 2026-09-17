"""Idempotent, resumable continuous PCM uploads. Completion only queues durable work."""
from datetime import datetime, timedelta
from typing import Literal
from uuid import UUID
import shutil
from types import SimpleNamespace

from fastapi import APIRouter, Depends, Header, HTTPException, Request
from pydantic import Field, model_validator
from sqlalchemy import func, select
from sqlalchemy.exc import IntegrityError
from sqlalchemy.orm import Session

from .auth import require_device
from .audit_settings import validate_call_report
from .conversation_recording_api import _conference_at, _external_instruction
from .database import get_session
from .models import CallRecord, ContinuousRecording, ConversationRecordingSegment, Device, Recording, RecordingUpload, Revision, utcnow
from .recording_api import _read_bounded_chunk, _utc
from .recording_service import RecordingError, create_upload_directory, initialize_retention_policy, recording_used_bytes, store_chunk
from .recording_service import remove_upload_files
from .schemas import SessionAuditReport, StrictModel
from .services import load_manifest

router = APIRouter(prefix="/api/device/v1/continuous-recordings", tags=["continuous-recordings"])


class ContinuousRecordingCreate(StrictModel):
    format_version: Literal[1] = 1
    recording_id: UUID
    call_id: UUID
    kind: Literal["conversation", "session_audit"]
    revision_id: int | None = Field(default=None, ge=1)
    block_id: UUID | None = None
    policy_version: int | None = Field(default=None, ge=1)
    captured_at: datetime
    source_format: Literal["pcm_s16le_48000_stereo"] = "pcm_s16le_48000_stereo"
    frames: int = Field(ge=1, le=48_000 * 86_400, strict=True)
    duration_ms: int = Field(ge=1, le=86_400_000, strict=True)
    expected_size_bytes: int = Field(ge=4, le=192_000 * 86_400, strict=True)
    source_sha256: str = Field(pattern=r"^[0-9a-f]{64}$")
    partial: bool
    stop_reason: Literal["session_complete", "caller_hangup", "operator_hangup", "max_call_duration", "completed", "preempted", "capture_failure", "storage_full", "interrupted", "writer_failure", "buffer_overrun", "recording_failure"]

    @model_validator(mode="after")
    def validate_pcm(self):
        if self.expected_size_bytes != self.frames * 4 or self.duration_ms != self.frames * 1000 // 48000:
            raise ValueError("PCM byte count, frame count and duration must agree")
        if self.captured_at.tzinfo is None:
            raise ValueError("Recording timestamp requires a timezone")
        if self.kind == "conversation" and (self.revision_id is None or self.block_id is None or self.policy_version is not None):
            raise ValueError("Conversation requires a revision and external-call block")
        if self.kind == "session_audit" and (self.policy_version is None or self.block_id is not None):
            raise ValueError("Whole-call audio requires its applied audit policy")
        if self.stop_reason not in {"session_complete", "caller_hangup", "operator_hangup", "max_call_duration", "completed"} and not self.partial:
            raise ValueError("Interrupted audio cannot be reported as complete")
        return self


def _owned(session: Session, recording_id: str, device: Device, *, lock=False):
    query = select(Recording).where(Recording.id == recording_id)
    recording = session.scalar(query.with_for_update() if lock else query)
    receipt = session.get(ContinuousRecording, recording_id)
    if recording is None or receipt is None:
        raise HTTPException(404, "Continuous recording not found")
    if recording.device_id != device.id:
        raise HTTPException(403, "Recording belongs to another device")
    return recording, receipt, session.get(RecordingUpload, recording_id)


def _response(recording, receipt, upload):
    accepted = receipt.accepted_at is not None
    return {"id": recording.id, "format_version": 1, "status": recording.status,
            "processing_state": receipt.state, "upload_offset": upload.upload_offset if upload else recording.source_size_bytes if accepted else 0,
            "expected_size_bytes": recording.source_size_bytes, "source_sha256": recording.source_sha256,
            "partial": recording.partial, "acknowledged": accepted,
            "error": recording.processing_error, "retry_at": receipt.retry_at}


def _capacity(request, session, body):
    settings = request.app.state.settings
    initialize_retention_policy(session, settings.development_admin_email)
    reserve = settings.audit_recording_reserve_bytes
    if recording_used_bytes(session) + body.expected_size_bytes + reserve > settings.recording_quota_bytes:
        raise HTTPException(507, "Server recording capacity is full; keep the encrypted tablet copy")
    if shutil.disk_usage(settings.recording_root).free < body.expected_size_bytes * 2 + reserve:
        raise HTTPException(507, "Server lacks upload and processing workspace; keep the encrypted tablet copy")
    if body.kind == "session_audit":
        ready = session.scalar(select(func.coalesce(func.sum(Recording.media_size_bytes), 0)).where(Recording.kind == "session_audit", Recording.status == "ready")) or 0
        pending = session.scalar(select(func.coalesce(func.sum(RecordingUpload.expected_size_bytes), 0)).join(Recording).where(Recording.kind == "session_audit")) or 0
        legacy = session.scalar(select(func.coalesce(func.sum(ConversationRecordingSegment.expected_size_bytes), 0)).join(Recording).where(Recording.kind == "session_audit", ConversationRecordingSegment.status.in_(["uploading", "verified"]))) or 0
        if ready + pending + legacy + body.expected_size_bytes > settings.audit_recording_quota_bytes:
            raise HTTPException(507, "Server audit quota is full; keep the encrypted tablet copy")


@router.post("", status_code=201)
def create(body: ContinuousRecordingCreate, request: Request, device: Device = Depends(require_device), session: Session = Depends(get_session)):
    recording_id, call_id = str(body.recording_id), str(body.call_id)
    manifest = body.model_dump(mode="json")
    existing = session.get(Recording, recording_id)
    if existing is not None:
        recording, receipt, upload = _owned(session, recording_id, device)
        if receipt.manifest != manifest:
            raise HTTPException(409, "Recording identity was reused with different metadata")
        return _response(recording, receipt, upload)
    call = session.get(CallRecord, call_id)
    if call is None or call.result == "IN_PROGRESS":
        raise HTTPException(409, "A terminal call event must be acknowledged before audio upload")
    if call.device_id != device.id:
        raise HTTPException(403, "Call belongs to another device")
    captured = _utc(body.captured_at)
    end = _utc(call.started_at) + timedelta(seconds=call.duration_seconds)
    if captured < _utc(call.started_at) - timedelta(seconds=5) or captured + timedelta(milliseconds=body.duration_ms) > end + timedelta(seconds=5):
        raise HTTPException(422, "Recording coverage is outside its call")
    operator = None
    if body.kind == "conversation":
        revision = session.get(Revision, body.revision_id)
        if revision is None or revision.schema_version != 4 or call.revision_id != body.revision_id:
            raise HTTPException(422, "Conversation revision does not match its call")
        instruction = _external_instruction(load_manifest(revision, request.app.state.cipher), str(body.block_id))
        conference = _conference_at(call, str(body.block_id))
        if instruction is None or conference is None or captured < conference - timedelta(seconds=1):
            raise HTTPException(422, "Conversation requires a verified operator connection in its signed flow")
        operator = instruction["phone_number"]
    else:
        if call.session_audit is None:
            raise HTTPException(409, "Call has no whole-call recording receipt")
        report = SessionAuditReport.model_validate(call.session_audit)
        validate_call_report(session, report, call.result, device.id)
        if (str(report.recording_id) != recording_id or report.policy_version != body.policy_version
            or report.captured_at != body.captured_at or report.duration_ms != body.duration_ms
            or report.partial != body.partial or report.stop_reason != body.stop_reason or report.state != "pending_upload"):
            raise HTTPException(409, "Recording differs from the acknowledged whole-call receipt")
    _capacity(request, session, body)
    maximum = session.scalar(select(func.max(Recording.sequence)).where(Recording.call_id == call_id))
    recording = Recording(id=recording_id, call_id=call_id, device_id=device.id,
        revision_id=body.revision_id if body.kind == "conversation" else call.revision_id,
        block_id=str(body.block_id) if body.block_id else None,
        sequence=-1 if body.kind == "session_audit" else max(0, (maximum or 0) + 1), kind=body.kind,
        audit_metadata=call.session_audit if body.kind == "session_audit" else None,
        operator_encrypted=request.app.state.cipher.encrypt(operator) if operator else None,
        operator_last4=operator[-4:] if operator else "", captured_at=body.captured_at,
        duration_ms=body.duration_ms, stop_reason=body.stop_reason, source_size_bytes=body.expected_size_bytes,
        source_sha256=body.source_sha256, source_format=body.source_format, partial=body.partial, status="uploading")
    receipt = ContinuousRecording(recording_id=recording_id, manifest=manifest, state="uploading")
    upload = RecordingUpload(recording_id=recording_id, expected_size_bytes=body.expected_size_bytes,
        source_sha256=body.source_sha256, upload_offset=0, storage_name=create_upload_directory(request.app.state.settings))
    try:
        session.add(recording); session.flush(); session.add_all([receipt, upload])
        session.commit()
    except IntegrityError as error:
        session.rollback()
        # A concurrent creation can reserve an orphan directory, removed by reconciliation.
        raise HTTPException(409, "Concurrent recording creation; retry the identical request") from error
    return _response(recording, receipt, upload)


@router.get("/{recording_id}")
def status(recording_id: str, device: Device = Depends(require_device), session: Session = Depends(get_session)):
    return _response(*_owned(session, recording_id, device))


@router.put("/{recording_id}/content")
async def content(recording_id: str, request: Request, upload_offset: int = Header(alias="Upload-Offset", ge=0),
                  device: Device = Depends(require_device), session: Session = Depends(get_session)):
    # Read the bounded body before taking the DB lock so a slow connection cannot hold it.
    chunk = await _read_bounded_chunk(request, request.app.state.settings.recording_chunk_limit_bytes)
    recording, receipt, upload = _owned(session, recording_id, device, lock=True)
    if receipt.state != "uploading" or upload is None:
        raise HTTPException(409, "Recording no longer accepts content; query its status")
    if shutil.disk_usage(request.app.state.settings.recording_root).free < len(chunk) + request.app.state.settings.audit_recording_reserve_bytes:
        raise HTTPException(507, "Server disk reserve reached; keep the encrypted tablet copy")
    try:
        store_chunk(request.app.state.settings, request.app.state.recording_cipher, recording, upload, upload_offset, chunk)
    except RecordingError as error:
        raise HTTPException(error.status_code, str(error)) from error
    receipt.updated_at = utcnow()
    session.commit()
    return _response(recording, receipt, upload)


@router.post("/{recording_id}/complete", status_code=202)
def complete(recording_id: str, device: Device = Depends(require_device), session: Session = Depends(get_session)):
    recording, receipt, upload = _owned(session, recording_id, device, lock=True)
    if receipt.accepted_at is not None or receipt.state in {"queued", "processing"}:
        return _response(recording, receipt, upload)
    if upload is None or upload.upload_offset != upload.expected_size_bytes:
        raise HTTPException(409, "Recording upload is incomplete")
    if receipt.state == "invalid":
        raise HTTPException(422, "Uploaded audio failed validation; the original tablet copy is still required")
    receipt.state = "queued"; receipt.retry_at = None; receipt.updated_at = utcnow()
    recording.status = "processing"; recording.processing_error = None
    session.commit()
    return _response(recording, receipt, upload)


@router.post("/{recording_id}/reset")
def reset(recording_id: str, request: Request, device: Device = Depends(require_device), session: Session = Depends(get_session)):
    recording, receipt, upload = _owned(session, recording_id, device, lock=True)
    if receipt.state != "invalid" or receipt.accepted_at is not None or upload is None:
        raise HTTPException(409, "Only a rejected upload can be restarted")
    # Preserve identity, expected size/hash and reservation. Never reset an accepted recording.
    previous = SimpleNamespace(storage_name=upload.storage_name)
    upload.storage_name = create_upload_directory(request.app.state.settings)
    upload.upload_offset = 0; upload.updated_at = utcnow()
    receipt.state = "uploading"; receipt.retry_at = None; receipt.updated_at = utcnow()
    recording.status = "uploading"; recording.processing_error = None
    session.commit()
    # An interruption before commit still leaves the old source usable. After
    # commit, reconciliation can remove the old unreferenced directory safely.
    remove_upload_files(request.app.state.settings, previous)
    return _response(recording, receipt, upload)
