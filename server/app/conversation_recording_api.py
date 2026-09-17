from __future__ import annotations

import tempfile
from datetime import datetime, timedelta, timezone
from pathlib import Path

from fastapi import APIRouter, BackgroundTasks, Depends, Header, HTTPException, Request, Response
from sqlalchemy import func, select
from sqlalchemy.exc import IntegrityError
from sqlalchemy.orm import Session

from .auth import require_device
from .crypto import mask_phone
from .database import get_session
from .models import (
    CallRecord,
    ConversationRecordingSegment,
    Device,
    Recording,
    Revision,
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
    ConversationRecordingCreateRequest,
    ConversationRecordingUploadResponse,
    ConversationSegmentCreateRequest,
    ConversationSegmentUploadResponse,
    ExternalCallSubEvent,
)
from .services import load_manifest
from .ntfy import notify_storage_full, schedule_ntfy


router = APIRouter(
    prefix="/api/device/v1/conversation-recordings",
    tags=["device-conversation-recordings"],
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
        raise HTTPException(status_code=404, detail="Conversation recording not found")
    if recording.device_id != device.id:
        raise HTTPException(status_code=403, detail="Conversation recording belongs to another device")
    if recording.kind != "conversation":
        raise HTTPException(status_code=409, detail="Recording ID belongs to voicemail")
    if recording.source_format != "legacy_wav":
        raise HTTPException(status_code=409, detail="Use the continuous recording API for this recording")
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
        raise HTTPException(status_code=404, detail="Conversation segment not found")
    return segment


def _external_instruction(manifest: dict, block_id: str) -> dict | None:
    for instruction in manifest.get("program", {}).get("instructions", []):
        if instruction.get("op") == "external_call" and instruction.get("block_id") == block_id:
            return instruction
    return None


def _conference_at(call: CallRecord, block_id: str) -> datetime | None:
    timestamps: list[datetime] = []
    for document in call.events or []:
        try:
            event = ExternalCallSubEvent.model_validate(document)
        except (TypeError, ValueError):
            continue
        if event.block_id == block_id and event.status == "CONFERENCED":
            timestamps.append(_utc(event.occurred_at))
    return min(timestamps) if timestamps else None


def _require_capacity(request: Request, session: Session, required_bytes: int) -> None:
    initialize_retention_policy(
        session,
        request.app.state.settings.development_admin_email,
    )
    if recording_used_bytes(session) + required_bytes > request.app.state.settings.recording_quota_bytes:
        notify_storage_full(
            request.app,
            "Recording storage quota is full; retain the encrypted device spool",
        )
        raise HTTPException(
            status_code=507,
            detail="Recording storage quota is full; retain the encrypted device spool",
        )


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
            raise HTTPException(status_code=413, detail="Conversation chunk exceeds 1 MiB")
    body = bytearray()
    async for part in request.stream():
        if len(body) + len(part) > limit:
            raise HTTPException(status_code=413, detail="Conversation chunk exceeds 1 MiB")
        body.extend(part)
    return bytes(body)


@router.post("", response_model=ConversationRecordingUploadResponse, status_code=201)
def create_conversation_recording(
    body: ConversationRecordingCreateRequest,
    request: Request,
    device: Device = Depends(require_device),
    session: Session = Depends(get_session),
) -> ConversationRecordingUploadResponse:
    recording_id = str(body.recording_id)
    call_id = str(body.call_id)
    existing = session.get(Recording, recording_id)
    if existing is not None:
        if existing.source_format != "legacy_wav":
            raise HTTPException(status_code=409, detail="Use the continuous recording API for this recording")
        if existing.device_id != device.id:
            raise HTTPException(status_code=409, detail="Recording ID belongs to another device")
        same = (
            existing.kind == "conversation"
            and existing.call_id == call_id
            and existing.revision_id == body.revision_id
            and existing.block_id == body.block_id
            and _utc(existing.captured_at) == _utc(body.captured_at)
        )
        if not same:
            raise HTTPException(status_code=409, detail="Recording ID was reused with different metadata")
        return _recording_response(existing)

    call = session.get(CallRecord, call_id)
    if call is None:
        raise HTTPException(status_code=409, detail="Call event must be acknowledged before its conversation")
    if call.device_id != device.id:
        raise HTTPException(status_code=403, detail="Call belongs to another device")
    if call.result == "IN_PROGRESS":
        raise HTTPException(status_code=409, detail="A terminal call event is required before upload")
    revision = session.get(Revision, body.revision_id)
    if revision is None or revision.schema_version != 4:
        raise HTTPException(status_code=422, detail="Conversation revision is not a V4 revision")
    if call.revision_id != body.revision_id:
        raise HTTPException(status_code=409, detail="Conversation revision does not match the call")
    manifest = load_manifest(revision, request.app.state.cipher)
    instruction = _external_instruction(manifest, body.block_id)
    if instruction is None:
        raise HTTPException(status_code=422, detail="External-call block is absent from the signed revision")

    captured_at = _utc(body.captured_at)
    call_started_at = _utc(call.started_at)
    call_ended_at = call_started_at + timedelta(seconds=call.duration_seconds)
    conference_at = _conference_at(call, body.block_id)
    if conference_at is None:
        raise HTTPException(
            status_code=409,
            detail="A verified CONFERENCED call event is required before conversation upload",
        )
    if (
        captured_at < call_started_at - timedelta(seconds=5)
        or captured_at > call_ended_at + timedelta(seconds=5)
        or conference_at < call_started_at - timedelta(seconds=5)
        or conference_at > call_ended_at + timedelta(seconds=5)
        or captured_at < conference_at - timedelta(seconds=1)
    ):
        raise HTTPException(
            status_code=422,
            detail="Conversation timestamp is outside its verified conference session",
        )

    current_sequence = session.scalar(
        select(func.max(Recording.sequence)).where(
            Recording.device_id == device.id,
            Recording.call_id == call_id,
        ),
    )
    sequence = 0 if current_sequence is None else int(current_sequence) + 1
    phone_number = instruction["phone_number"]
    recording = Recording(
        id=recording_id,
        device_id=device.id,
        call_id=call_id,
        revision_id=body.revision_id,
        block_id=body.block_id,
        sequence=sequence,
        kind="conversation",
        operator_encrypted=request.app.state.cipher.encrypt(phone_number),
        operator_last4=phone_number[-4:],
        segment_count=0,
        captured_at=body.captured_at,
        duration_ms=0,
        stop_reason="recording_failure",
        source_size_bytes=0,
        source_sha256="0" * 64,
        status="uploading",
    )
    session.add(recording)
    try:
        session.commit()
    except IntegrityError as error:
        session.rollback()
        raise HTTPException(status_code=409, detail="Duplicate conversation recording") from error
    return _recording_response(recording)


@router.get("/{recording_id}", response_model=ConversationRecordingUploadResponse)
def conversation_recording_status(
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
def create_conversation_segment(
    recording_id: str,
    segment_index: int,
    body: ConversationSegmentCreateRequest,
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
        or body.revision_id != recording.revision_id
        or body.block_id != recording.block_id
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
        raise HTTPException(status_code=409, detail="Conversation recording is already complete")
    if recording.status != "uploading":
        raise HTTPException(status_code=409, detail="Conversation recording is not accepting segments")

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
        raise HTTPException(status_code=422, detail="Segment timestamp is outside its conversation")
    maximum_wav_bytes = 44 + body.duration_ms * 192 + 4096
    if (
        body.expected_size_bytes > request.app.state.settings.recording_upload_limit_bytes
        or body.expected_size_bytes > maximum_wav_bytes
    ):
        raise HTTPException(status_code=413, detail="Conversation segment exceeds its duration bound")
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
        raise HTTPException(status_code=409, detail="Duplicate conversation segment") from error
    return _segment_response(segment)


@router.get(
    "/{recording_id}/segments/{segment_index}/upload",
    response_model=ConversationSegmentUploadResponse,
)
def conversation_segment_upload_status(
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
async def upload_conversation_segment_content(
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
        raise HTTPException(status_code=409, detail="Conversation segment is not accepting content")
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
def complete_conversation_segment(
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
        raise HTTPException(status_code=409, detail="Conversation segment cannot be completed")
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
        raise HTTPException(status_code=error.status_code, detail=str(error)) from error
    segment.duration_ms = details.duration_ms
    segment.status = "verified"
    segment.updated_at = utcnow()
    session.commit()
    return _segment_response(segment)


@router.post("/{recording_id}/complete", response_model=ConversationRecordingUploadResponse)
def complete_conversation_recording(
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
        raise HTTPException(status_code=404, detail="Conversation recording not found")
    if recording.device_id != device.id:
        raise HTTPException(status_code=403, detail="Conversation recording belongs to another device")
    if recording.kind != "conversation":
        raise HTTPException(status_code=409, detail="Recording ID belongs to voicemail")
    if recording.source_format != "legacy_wav":
        raise HTTPException(status_code=409, detail="Use the continuous recording API for this recording")
    if recording.status in {"ready", "deleted"}:
        return _recording_response(recording)
    if recording.status != "uploading":
        raise HTTPException(status_code=409, detail="Conversation recording cannot be completed")
    segments = session.scalars(
        select(ConversationRecordingSegment)
        .where(ConversationRecordingSegment.recording_id == recording.id)
        .order_by(ConversationRecordingSegment.segment_index.asc())
        .with_for_update(),
    ).all()
    if len(segments) != body.segment_count:
        raise HTTPException(status_code=409, detail="Conversation segment count does not match")

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
    call = session.get(CallRecord, recording.call_id)
    caller = (
        request.app.state.cipher.decrypt(call.caller_encrypted)
        if call is not None and call.caller_encrypted
        else None
    )
    try:
        session.commit()
    except Exception:
        session.rollback()
        if destination is not None:
            destination.unlink(missing_ok=True)
        raise
    for segment in segments:
        remove_upload_files(request.app.state.settings, segment)
    schedule_ntfy(
        background_tasks,
        request.app,
        "conversation_ready",
        title="Conversation ready",
        message="An external-call recording is ready to play.",
        click_path=f"/voicemail?recording={recording.id}",
        tags="telephone_receiver,ivrdroid",
        caller_masked=mask_phone(caller) if caller else None,
    )
    return _recording_response(recording)
