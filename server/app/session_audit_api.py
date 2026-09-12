from __future__ import annotations

import tempfile
import shutil
from datetime import datetime, timedelta, timezone
from pathlib import Path

from fastapi import APIRouter, BackgroundTasks, Depends, Header, HTTPException, Request, Response
from sqlalchemy import func, select
from sqlalchemy.exc import IntegrityError
from sqlalchemy.orm import Session

from .auth import require_device
from .database import get_session
from .models import (
    CallRecord,
    ConversationRecordingSegment,
    Device,
    Recording,
    utcnow,
)
from .recording_service import (
    RecordingError,
    assemble_conversation_recording,
    create_upload_directory,
    initialize_retention_policy,
    inspect_pcm_wav_path,
    recording_used_bytes,
    remove_upload_files,
    store_chunk,
    write_uploaded_wav_to_path,
)
from .schemas import (
    ConversationRecordingCompleteRequest,
    SessionAuditRecordingCreateRequest,
    ConversationRecordingUploadResponse,
    SessionAuditSegmentCreateRequest,
    ConversationSegmentUploadResponse,
)
from .audit_settings import validate_call_report
from .ntfy import notify_storage_full


router = APIRouter(
    prefix="/api/device/v1/session-recordings",
    tags=["device-session-recordings"],
)


def _utc(value: datetime) -> datetime:
    if value.tzinfo is None:
        return value.replace(tzinfo=timezone.utc)
    return value.astimezone(timezone.utc)


def _recording_response(recording: Recording) -> ConversationRecordingUploadResponse:
    return ConversationRecordingUploadResponse(
        id=recording.id,
        status="ready" if recording.status == "deleted" else recording.status,
        segment_count=recording.segment_count,
        duration_ms=recording.duration_ms,
        acknowledged=recording.status in {"ready", "deleted"},
    )


def _segment_response(segment: ConversationRecordingSegment) -> ConversationSegmentUploadResponse:
    return ConversationSegmentUploadResponse(
        id=segment.recording_id,
        segment_index=segment.segment_index,
        status=segment.status,
        upload_offset=segment.upload_offset,
        expected_size_bytes=segment.expected_size_bytes,
        source_sha256=segment.source_sha256,
        acknowledged=segment.status in {"verified", "assembled"},
    )


def _owned_recording(session: Session, recording_id: str, device: Device) -> Recording:
    recording = session.get(Recording, recording_id)
    if recording is None:
        raise HTTPException(status_code=404, detail="Session audit recording not found")
    if recording.device_id != device.id:
        raise HTTPException(status_code=403, detail="Session audit recording belongs to another device")
    if recording.kind != "session_audit":
        raise HTTPException(status_code=409, detail="Recording ID belongs to another recording kind")
    return recording


def _owned_segment(
    session: Session,
    recording: Recording,
    segment_index: int,
    *,
    lock: bool = False,
) -> ConversationRecordingSegment:
    statement = select(ConversationRecordingSegment).where(
        ConversationRecordingSegment.recording_id == recording.id,
        ConversationRecordingSegment.segment_index == segment_index,
    )
    if lock:
        statement = statement.with_for_update()
    segment = session.scalar(statement)
    if segment is None:
        raise HTTPException(status_code=404, detail="Session audit segment not found")
    return segment


def _require_capacity(request: Request, session: Session, required_bytes: int) -> None:
    initialize_retention_policy(session, request.app.state.settings.development_admin_email)
    settings = request.app.state.settings
    ready = session.scalar(select(func.coalesce(func.sum(Recording.media_size_bytes), 0)).where(
        Recording.kind == "session_audit", Recording.status == "ready")) or 0
    pending = session.scalar(select(func.coalesce(func.sum(ConversationRecordingSegment.expected_size_bytes), 0))
        .join(Recording, Recording.id == ConversationRecordingSegment.recording_id).where(
            Recording.kind == "session_audit", ConversationRecordingSegment.status.in_(["uploading", "verified"]))) or 0
    if (ready + pending + required_bytes > settings.audit_recording_quota_bytes or
        recording_used_bytes(session) + required_bytes + settings.audit_recording_reserve_bytes > settings.recording_quota_bytes or
        shutil.disk_usage(settings.recording_root).free < required_bytes + settings.audit_recording_reserve_bytes):
        raise HTTPException(status_code=507, detail="Audit storage is full; retain the encrypted tablet spool")


async def _read_bounded_chunk(request: Request, limit: int) -> bytes:
    declared = request.headers.get("content-length")
    if declared is not None:
        try:
            declared_size = int(declared)
        except ValueError as error:
            raise HTTPException(status_code=400, detail="Content-Length is invalid") from error
        if declared_size < 0:
            raise HTTPException(status_code=400, detail="Content-Length is invalid")
        if declared_size > limit:
            raise HTTPException(status_code=413, detail="Audit chunk exceeds 1 MiB")
    body = bytearray()
    async for part in request.stream():
        if len(body) + len(part) > limit:
            raise HTTPException(status_code=413, detail="Audit chunk exceeds 1 MiB")
        body.extend(part)
    return bytes(body)


@router.post("", response_model=ConversationRecordingUploadResponse, status_code=201)
def create_session_recording(
    body: SessionAuditRecordingCreateRequest,
    request: Request,
    device: Device = Depends(require_device),
    session: Session = Depends(get_session),
) -> ConversationRecordingUploadResponse:
    recording_id, call_id = str(body.recording_id), str(body.call_id)
    call = session.get(CallRecord, call_id)
    if call is None or call.result == "IN_PROGRESS":
        raise HTTPException(status_code=409, detail="A terminal call event must be acknowledged before audit upload")
    if call.device_id != device.id:
        raise HTTPException(status_code=403, detail="Call belongs to another device")
    from .schemas import SessionAuditReport
    if call.session_audit is None:
        raise HTTPException(status_code=409, detail="The acknowledged call has no audit receipt")
    report = SessionAuditReport.model_validate(call.session_audit)
    validate_call_report(session, report, call.result, device.id)
    if (report.state != "pending_upload" or str(report.recording_id) != recording_id
        or report.policy_version != body.policy_version or report.captured_at != body.captured_at):
        raise HTTPException(status_code=409, detail="Audit receipt does not match the acknowledged call")
    captured_at = _utc(body.captured_at)
    if not (_utc(call.started_at) - timedelta(seconds=5) <= captured_at <=
            _utc(call.started_at) + timedelta(seconds=call.duration_seconds + 5)):
        raise HTTPException(status_code=422, detail="Audit timestamp is outside its call")
    existing = session.get(Recording, recording_id)
    if existing is not None:
        if existing.device_id != device.id or existing.kind != "session_audit" or existing.call_id != call_id:
            raise HTTPException(status_code=409, detail="Recording identity was reused")
        return _recording_response(existing)
    recording = Recording(
        id=recording_id, call_id=call_id, device_id=device.id,
        revision_id=call.revision_id, block_id=None, sequence=-1, kind="session_audit",
        audit_metadata=report.model_dump(mode="json"), captured_at=body.captured_at,
        duration_ms=0, stop_reason=report.stop_reason, source_size_bytes=0,
        source_sha256="0" * 64, status="uploading",
    )
    session.add(recording)
    try:
        session.commit()
    except IntegrityError as error:
        session.rollback()
        raise HTTPException(status_code=409, detail="This call already has an audit recording") from error
    return _recording_response(recording)


@router.get("/{recording_id}", response_model=ConversationRecordingUploadResponse)
def session_recording_status(
    recording_id: str,
    device: Device = Depends(require_device),
    session: Session = Depends(get_session),
) -> ConversationRecordingUploadResponse:
    return _recording_response(_owned_recording(session, recording_id, device))


@router.post(
    "/{recording_id}/segments/{segment_index}",
    response_model=ConversationSegmentUploadResponse,
    status_code=201,
)
def create_session_segment(
    recording_id: str,
    segment_index: int,
    body: SessionAuditSegmentCreateRequest,
    request: Request,
    device: Device = Depends(require_device),
    session: Session = Depends(get_session),
) -> ConversationSegmentUploadResponse:
    recording = _owned_recording(session, recording_id, device)
    if segment_index != body.segment_index:
        raise HTTPException(status_code=409, detail="Segment path and receipt indexes do not match")
    if (
        str(body.recording_id) != recording.id
        or str(body.call_id) != recording.call_id
        or body.policy_version != (recording.audit_metadata or {}).get("policy_version")
    ):
        raise HTTPException(status_code=409, detail="Segment receipt does not match its logical recording")
    existing = session.get(
        ConversationRecordingSegment,
        (recording.id, body.segment_index),
    )
    if existing is not None:
        same = (
            _utc(existing.captured_at) == _utc(body.captured_at)
            and existing.duration_ms == body.duration_ms
            and existing.stop_reason == body.stop_reason
            and existing.partial == body.partial
            and existing.expected_size_bytes == body.expected_size_bytes
            and existing.source_sha256 == body.source_sha256
        )
        if not same:
            raise HTTPException(status_code=409, detail="Segment index was reused with different metadata")
        if existing.status == "failed":
            _require_capacity(request, session, body.expected_size_bytes)
            existing.storage_name = create_upload_directory(request.app.state.settings)
            existing.upload_offset = 0
            existing.status = "uploading"
            existing.updated_at = utcnow()
            session.commit()
        return _segment_response(existing)
    if recording.status in {"ready", "deleted"}:
        raise HTTPException(status_code=409, detail="Session audit recording is already complete")
    if recording.status != "uploading":
        raise HTTPException(status_code=409, detail="Session audit recording is not accepting segments")

    captured_at = _utc(body.captured_at)
    call = session.get(CallRecord, recording.call_id)
    if call is None or call.result == "IN_PROGRESS":
        raise HTTPException(status_code=409, detail="A terminal call event is required before upload")
    call_started_at = _utc(call.started_at)
    call_ended_at = call_started_at + timedelta(seconds=call.duration_seconds)
    if (
        captured_at < max(call_started_at, _utc(recording.captured_at)) - timedelta(seconds=5)
        or captured_at > call_ended_at + timedelta(seconds=5)
    ):
        raise HTTPException(status_code=422, detail="Segment timestamp is outside its session")
    maximum_wav_bytes = 44 + body.duration_ms * 192 + 4096
    expected_at = _utc(recording.captured_at) + timedelta(seconds=body.segment_index * 15)
    if abs((captured_at - expected_at).total_seconds()) > 0.1:
        raise HTTPException(status_code=422, detail="Audit segment timing is not contiguous")
    if (
        body.expected_size_bytes > request.app.state.settings.recording_upload_limit_bytes
        or body.expected_size_bytes > maximum_wav_bytes
    ):
        raise HTTPException(status_code=413, detail="Session audit segment exceeds its duration bound")
    _require_capacity(request, session, body.expected_size_bytes)
    storage_name = create_upload_directory(request.app.state.settings)
    segment = ConversationRecordingSegment(
        recording_id=recording.id,
        segment_index=body.segment_index,
        captured_at=body.captured_at,
        duration_ms=body.duration_ms,
        stop_reason=body.stop_reason,
        partial=body.partial,
        expected_size_bytes=body.expected_size_bytes,
        source_sha256=body.source_sha256,
        upload_offset=0,
        storage_name=storage_name,
        status="uploading",
    )
    session.add(segment)
    try:
        session.commit()
    except IntegrityError as error:
        session.rollback()
        (request.app.state.settings.recording_root / ".uploads" / storage_name).rmdir()
        raise HTTPException(status_code=409, detail="Duplicate audit segment") from error
    return _segment_response(segment)


@router.get(
    "/{recording_id}/segments/{segment_index}/upload",
    response_model=ConversationSegmentUploadResponse,
)
def session_segment_upload_status(
    recording_id: str,
    segment_index: int,
    device: Device = Depends(require_device),
    session: Session = Depends(get_session),
) -> ConversationSegmentUploadResponse:
    recording = _owned_recording(session, recording_id, device)
    return _segment_response(_owned_segment(session, recording, segment_index))


@router.put(
    "/{recording_id}/segments/{segment_index}/content",
    response_model=ConversationSegmentUploadResponse,
)
async def upload_session_segment_content(
    recording_id: str,
    segment_index: int,
    request: Request,
    response: Response,
    upload_offset: int | None = Header(default=None, alias="Upload-Offset"),
    device: Device = Depends(require_device),
    session: Session = Depends(get_session),
) -> ConversationSegmentUploadResponse:
    if upload_offset is None or upload_offset < 0:
        raise HTTPException(status_code=400, detail="Upload-Offset is required")
    if request.headers.get("content-type", "").split(";", 1)[0] != "application/offset+octet-stream":
        raise HTTPException(status_code=415, detail="Use application/offset+octet-stream")
    recording = _owned_recording(session, recording_id, device)
    segment = _owned_segment(session, recording, segment_index, lock=True)
    if segment.status in {"verified", "assembled"}:
        return _segment_response(segment)
    if segment.status != "uploading":
        raise HTTPException(status_code=409, detail="Session audit segment is not accepting content")
    body = await _read_bounded_chunk(request, request.app.state.settings.recording_chunk_limit_bytes)
    try:
        store_chunk(
            request.app.state.settings,
            request.app.state.recording_cipher,
            segment,
            segment,
            upload_offset,
            body,
            cipher_identity=f"{recording.id}:{segment.segment_index}",
        )
    except RecordingError as error:
        raise HTTPException(status_code=error.status_code, detail=str(error)) from error
    session.commit()
    response.headers["Upload-Offset"] = str(segment.upload_offset)
    response.headers["Cache-Control"] = "no-store"
    return _segment_response(segment)


@router.post(
    "/{recording_id}/segments/{segment_index}/complete",
    response_model=ConversationSegmentUploadResponse,
)
def complete_session_segment(
    recording_id: str,
    segment_index: int,
    request: Request,
    device: Device = Depends(require_device),
    session: Session = Depends(get_session),
) -> ConversationSegmentUploadResponse:
    recording = _owned_recording(session, recording_id, device)
    segment = _owned_segment(session, recording, segment_index, lock=True)
    if segment.status in {"verified", "assembled"}:
        return _segment_response(segment)
    if segment.status != "uploading":
        raise HTTPException(status_code=409, detail="Session audit segment cannot be completed")
    try:
        with tempfile.TemporaryDirectory(
            prefix=".verify-segment-",
            dir=request.app.state.settings.recording_root,
        ) as directory:
            path = Path(directory) / "segment.wav"
            write_uploaded_wav_to_path(
                request.app.state.settings,
                request.app.state.recording_cipher,
                segment,
                path,
            )
            details = inspect_pcm_wav_path(path, segment.duration_ms)
    except RecordingError as error:
        # A corrupt completed upload must be restartable on the next idle sync.
        # Keep transient storage failures resumable without discarding good chunks.
        if error.status_code in {400, 422}:
            segment.status = "failed"
            segment.updated_at = utcnow()
            session.commit()
            remove_upload_files(request.app.state.settings, segment)
        raise HTTPException(status_code=error.status_code, detail=str(error)) from error
    segment.duration_ms = details.duration_ms
    segment.status = "verified"
    segment.updated_at = utcnow()
    session.commit()
    return _segment_response(segment)


@router.post("/{recording_id}/complete", response_model=ConversationRecordingUploadResponse)
def complete_session_recording(
    recording_id: str,
    body: ConversationRecordingCompleteRequest,
    request: Request,
    background_tasks: BackgroundTasks,
    device: Device = Depends(require_device),
    session: Session = Depends(get_session),
) -> ConversationRecordingUploadResponse:
    recording = session.scalar(
        select(Recording).where(Recording.id == recording_id).with_for_update(),
    )
    if recording is None:
        raise HTTPException(status_code=404, detail="Session audit recording not found")
    if recording.device_id != device.id:
        raise HTTPException(status_code=403, detail="Session audit recording belongs to another device")
    if recording.kind != "session_audit":
        raise HTTPException(status_code=409, detail="Recording ID belongs to another recording kind")
    if recording.status in {"ready", "deleted"}:
        return _recording_response(recording)
    if recording.status != "uploading":
        raise HTTPException(status_code=409, detail="Session audit recording cannot be completed")
    segments = session.scalars(
        select(ConversationRecordingSegment)
        .where(ConversationRecordingSegment.recording_id == recording.id)
        .order_by(ConversationRecordingSegment.segment_index.asc())
        .with_for_update(),
    ).all()
    if len(segments) != body.segment_count:
        raise HTTPException(status_code=409, detail="Session audit segment count does not match")

    report = recording.audit_metadata or {}
    if abs(sum(item.duration_ms for item in segments) - report.get("duration_ms", 0)) > 30:
        raise HTTPException(status_code=409, detail="Audit duration disagrees with call report")
    if any(item.duration_ms != 15000 for item in segments[:-1]):
        raise HTTPException(status_code=409, detail="Non-terminal audit segments must cover 15 seconds")
    if segments and (segments[-1].stop_reason != report.get("stop_reason") or segments[-1].partial != report.get("partial")):
        raise HTTPException(status_code=409, detail="Audit termination disagrees with call report")
    _require_capacity(request, session, 0)
    workspace_bytes = sum(item.expected_size_bytes for item in segments) + sum(item.duration_ms for item in segments) * 12 + 4 * 1024 * 1024
    if shutil.disk_usage(request.app.state.settings.recording_root).free < workspace_bytes + request.app.state.settings.audit_recording_reserve_bytes:
        raise HTTPException(status_code=507, detail="Audit assembly filesystem reserve reached; retain the tablet spool")
    destination = None
    try:
        destination = assemble_conversation_recording(
            session,
            request.app.state.settings,
            request.app.state.recording_cipher,
            recording,
            segments,
        )
    except RecordingError as error:
        if error.status_code == 507:
            notify_storage_full(request.app, str(error))
        raise HTTPException(status_code=error.status_code, detail=str(error)) from error
    try:
        session.commit()
    except Exception:
        session.rollback()
        if destination is not None:
            destination.unlink(missing_ok=True)
        raise
    for segment in segments:
        remove_upload_files(request.app.state.settings, segment)
    return _recording_response(recording)
