from __future__ import annotations

import os
import tempfile
from datetime import datetime, timedelta, timezone
from pathlib import Path

from fastapi import APIRouter, Depends, Header, HTTPException, Query, Request, Response
from fastapi.responses import FileResponse
from starlette.background import BackgroundTask
from sqlalchemy import func, select
from sqlalchemy.exc import IntegrityError
from sqlalchemy.orm import Session

from .auth import require_admin, require_admin_write, require_device
from .crypto import mask_phone
from .database import get_session
from .models import (
    CallRecord,
    Device,
    Recording,
    RecordingRetentionPolicy,
    RecordingUpload,
    Revision,
    utcnow,
)
from .recording_service import (
    RecordingError,
    cleanup_abandoned_uploads,
    create_upload_directory,
    discard_quarantined_media,
    finalize_recording,
    initialize_retention_policy,
    load_media,
    materialize_media,
    purge_expired_recordings,
    quarantine_media,
    recording_used_bytes,
    remove_upload_files,
    restore_quarantined_media,
    store_chunk,
    tombstone_recording,
)
from .schemas import (
    RecordingCreateRequest,
    RecordingListenedRequest,
    RecordingListResponse,
    RecordingResponse,
    RecordingUploadResponse,
    RetentionPolicyRequest,
    RetentionPolicyResponse,
)
from .services import audit, load_manifest


device_router = APIRouter(prefix="/api/device/v1/recordings", tags=["device-recordings"])
admin_router = APIRouter(prefix="/api/admin/v1/recordings", tags=["admin-recordings"])
settings_router = APIRouter(prefix="/api/admin/v1/recording-settings", tags=["admin-recordings"])


def _as_upload_response(recording: Recording, upload: RecordingUpload | None) -> RecordingUploadResponse:
    expected = upload.expected_size_bytes if upload is not None else recording.source_size_bytes
    return RecordingUploadResponse(
        id=recording.id,
        # A completed recording can be tombstoned before the device receives
        # its final receipt. Preserve the verified acknowledgement contract so
        # that race cannot strand caller audio in the encrypted tablet spool.
        status="ready" if recording.status == "deleted" else recording.status,
        upload_offset=0 if upload is None else upload.upload_offset,
        expected_size_bytes=expected,
        source_sha256=recording.source_sha256,
        acknowledged=recording.status in {"ready", "deleted"},
    )


def _get_owned_recording(
    session: Session,
    recording_id: str,
    device: Device,
) -> Recording:
    recording = session.get(Recording, recording_id)
    if recording is None:
        raise HTTPException(status_code=404, detail="Recording not found")
    if recording.device_id != device.id:
        raise HTTPException(status_code=403, detail="Recording belongs to another device")
    return recording


def _recording_instruction_exists(manifest: dict, block_id: str) -> bool:
    return any(
        instruction.get("op") == "record_message" and instruction.get("block_id") == block_id
        for instruction in manifest.get("program", {}).get("instructions", [])
    )


def _utc(value: datetime) -> datetime:
    if value.tzinfo is None:
        return value.replace(tzinfo=timezone.utc)
    return value.astimezone(timezone.utc)


def _require_recording_capacity(
    request: Request,
    session: Session,
    required_bytes: int,
) -> None:
    # This singleton row serializes quota reservations on PostgreSQL. Counting the full declared
    # source size also prevents abandoned resumable uploads from overcommitting the media volume.
    initialize_retention_policy(
        session,
        request.app.state.settings.development_admin_email,
    )
    if (
        recording_used_bytes(session) + required_bytes
        > request.app.state.settings.recording_quota_bytes
    ):
        raise HTTPException(
            status_code=507,
            detail="Voicemail storage quota is full; retain the encrypted device spool",
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
            raise HTTPException(status_code=413, detail="Recording chunk exceeds 1 MiB")
    body = bytearray()
    async for part in request.stream():
        if len(body) + len(part) > limit:
            raise HTTPException(status_code=413, detail="Recording chunk exceeds 1 MiB")
        body.extend(part)
    return bytes(body)


@device_router.post("", response_model=RecordingUploadResponse, status_code=201)
def create_recording(
    body: RecordingCreateRequest,
    request: Request,
    device: Device = Depends(require_device),
    session: Session = Depends(get_session),
) -> RecordingUploadResponse:
    recording_id = str(body.recording_id)
    call_id = str(body.call_id)
    existing = session.get(Recording, recording_id)
    if existing is not None:
        if existing.device_id != device.id:
            raise HTTPException(status_code=409, detail="Recording ID belongs to another device")
        existing_upload = session.get(RecordingUpload, recording_id)
        same = (
            existing.call_id == call_id
            and existing.revision_id == body.revision_id
            and existing.block_id == body.block_id
            and existing.sequence == body.sequence
            and existing.duration_ms == body.duration_ms
            and existing.stop_reason == body.stop_reason
            and existing.source_size_bytes == body.expected_size_bytes
            and existing.source_sha256 == body.source_sha256
        )
        if not same:
            raise HTTPException(status_code=409, detail="Recording ID was reused with different metadata")
        if existing.status == "failed" and existing_upload is None:
            _require_recording_capacity(request, session, body.expected_size_bytes)
            storage_name = create_upload_directory(request.app.state.settings)
            existing.status = "uploading"
            existing.updated_at = utcnow()
            existing_upload = RecordingUpload(
                recording_id=existing.id,
                expected_size_bytes=body.expected_size_bytes,
                source_sha256=body.source_sha256,
                storage_name=storage_name,
            )
            session.add(existing_upload)
            session.commit()
        return _as_upload_response(existing, existing_upload)

    call = session.get(CallRecord, call_id)
    if call is None:
        raise HTTPException(status_code=409, detail="Call event must be acknowledged before its recording")
    if call.device_id != device.id:
        raise HTTPException(status_code=403, detail="Call belongs to another device")
    captured_at = _utc(body.captured_at)
    call_started_at = _utc(call.started_at)
    if (
        captured_at < call_started_at - timedelta(seconds=5)
        or captured_at > call_started_at + timedelta(minutes=10)
        or captured_at > utcnow() + timedelta(minutes=5)
    ):
        raise HTTPException(status_code=422, detail="Recording timestamp is outside its call session")
    revision = session.get(Revision, body.revision_id)
    if revision is None or revision.schema_version not in {3, 4}:
        raise HTTPException(status_code=422, detail="Recording revision is not a V3/V4 revision")
    if call.revision_id != body.revision_id:
        raise HTTPException(status_code=409, detail="Recording revision does not match the call")
    manifest = load_manifest(revision, request.app.state.cipher)
    if not _recording_instruction_exists(manifest, body.block_id):
        raise HTTPException(status_code=422, detail="Recording block is not present in the signed revision")
    behavior = manifest.get("recording_behavior", {})
    if body.stop_reason == "finish_key" and behavior.get("finish_key") is None:
        raise HTTPException(status_code=422, detail="The signed revision disables finish-key stopping")
    signed_maximum_ms = int(behavior.get("maximum_duration_seconds", 0)) * 1000
    if body.duration_ms > signed_maximum_ms:
        raise HTTPException(status_code=422, detail="Recording duration exceeds the signed limit")
    if body.stop_reason == "maximum_duration" and body.duration_ms < signed_maximum_ms - 100:
        raise HTTPException(status_code=422, detail="Maximum-duration receipt ended too early")
    maximum_wav_bytes = 44 + body.duration_ms * 192 + 4096
    if (
        body.expected_size_bytes > request.app.state.settings.recording_upload_limit_bytes
        or body.expected_size_bytes > maximum_wav_bytes
    ):
        raise HTTPException(status_code=413, detail="Recording size exceeds its duration bound")
    _require_recording_capacity(request, session, body.expected_size_bytes)

    storage_name = create_upload_directory(request.app.state.settings)
    recording = Recording(
        id=recording_id,
        device_id=device.id,
        call_id=call_id,
        revision_id=body.revision_id,
        block_id=body.block_id,
        sequence=body.sequence,
        captured_at=body.captured_at,
        duration_ms=body.duration_ms,
        stop_reason=body.stop_reason,
        source_size_bytes=body.expected_size_bytes,
        source_sha256=body.source_sha256,
        status="uploading",
    )
    upload = RecordingUpload(
        recording_id=recording_id,
        expected_size_bytes=body.expected_size_bytes,
        source_sha256=body.source_sha256,
        storage_name=storage_name,
    )
    session.add_all([recording, upload])
    try:
        session.commit()
    except IntegrityError as error:
        session.rollback()
        path = request.app.state.settings.recording_root / ".uploads" / storage_name
        path.rmdir()
        raise HTTPException(status_code=409, detail="Duplicate recording for this call step") from error
    return _as_upload_response(recording, upload)


@device_router.get("/{recording_id}/upload", response_model=RecordingUploadResponse)
def recording_upload_status(
    recording_id: str,
    device: Device = Depends(require_device),
    session: Session = Depends(get_session),
) -> RecordingUploadResponse:
    recording = _get_owned_recording(session, recording_id, device)
    return _as_upload_response(recording, session.get(RecordingUpload, recording.id))


@device_router.put("/{recording_id}/content", response_model=RecordingUploadResponse)
async def upload_recording_content(
    recording_id: str,
    request: Request,
    response: Response,
    upload_offset: int | None = Header(default=None, alias="Upload-Offset"),
    device: Device = Depends(require_device),
    session: Session = Depends(get_session),
) -> RecordingUploadResponse:
    if upload_offset is None or upload_offset < 0:
        raise HTTPException(status_code=400, detail="Upload-Offset is required")
    if request.headers.get("content-type", "").split(";", 1)[0] != "application/offset+octet-stream":
        raise HTTPException(status_code=415, detail="Use application/offset+octet-stream")
    recording = _get_owned_recording(session, recording_id, device)
    if recording.status in {"ready", "deleted"}:
        return _as_upload_response(recording, None)
    if recording.status != "uploading":
        raise HTTPException(status_code=409, detail="Recording is not accepting content")
    upload = session.scalar(
        select(RecordingUpload).where(RecordingUpload.recording_id == recording.id).with_for_update(),
    )
    if upload is None:
        raise HTTPException(status_code=409, detail="Recording upload is unavailable")
    body = await _read_bounded_chunk(
        request,
        request.app.state.settings.recording_chunk_limit_bytes,
    )
    try:
        store_chunk(
            request.app.state.settings,
            request.app.state.recording_cipher,
            recording,
            upload,
            upload_offset,
            body,
        )
    except RecordingError as error:
        raise HTTPException(status_code=error.status_code, detail=str(error)) from error
    session.commit()
    response.headers["Upload-Offset"] = str(upload.upload_offset)
    response.headers["Cache-Control"] = "no-store"
    return _as_upload_response(recording, upload)


@device_router.post("/{recording_id}/complete", response_model=RecordingUploadResponse)
def complete_recording(
    recording_id: str,
    request: Request,
    device: Device = Depends(require_device),
    session: Session = Depends(get_session),
) -> RecordingUploadResponse:
    recording = _get_owned_recording(session, recording_id, device)
    if recording.status in {"ready", "deleted"}:
        return _as_upload_response(recording, None)
    if recording.status != "uploading":
        raise HTTPException(status_code=409, detail="Recording cannot be completed")
    upload = session.scalar(
        select(RecordingUpload).where(RecordingUpload.recording_id == recording.id).with_for_update(),
    )
    if upload is None:
        raise HTTPException(status_code=409, detail="Recording upload is unavailable")
    destination = None
    try:
        destination = finalize_recording(
            session,
            request.app.state.settings,
            request.app.state.recording_cipher,
            recording,
            upload,
        )
    except RecordingError as error:
        raise HTTPException(status_code=error.status_code, detail=str(error)) from error
    session.delete(upload)
    try:
        session.commit()
    except Exception:
        session.rollback()
        if destination is not None:
            destination.unlink(missing_ok=True)
        raise
    remove_upload_files(request.app.state.settings, upload)
    return _as_upload_response(recording, None)


def _recording_response(request: Request, recording: Recording, call: CallRecord) -> RecordingResponse:
    caller = request.app.state.cipher.decrypt(call.caller_encrypted) if call.caller_encrypted else None
    operator = (
        request.app.state.cipher.decrypt(recording.operator_encrypted)
        if recording.operator_encrypted
        else None
    )
    available = recording.status == "ready"
    return RecordingResponse(
        id=recording.id,
        call_id=recording.call_id,
        device_id=recording.device_id,
        revision_id=recording.revision_id,
        block_id=recording.block_id,
        sequence=recording.sequence,
        kind=recording.kind,
        operator_masked=mask_phone(operator) if operator else "",
        segment_count=recording.segment_count,
        caller=caller,
        caller_masked=mask_phone(caller),
        captured_at=recording.captured_at,
        duration_ms=recording.duration_ms,
        stop_reason=recording.stop_reason,
        status=recording.status,
        listened_at=recording.listened_at,
        deleted_at=recording.deleted_at,
        playback_url=f"/api/admin/v1/recordings/{recording.id}/audio" if available else None,
        download_url=f"/api/admin/v1/recordings/{recording.id}/download" if available else None,
    )


def _apply_retention(request: Request, session: Session) -> None:
    policy = initialize_retention_policy(session, request.app.state.settings.development_admin_email)
    quarantined = purge_expired_recordings(session, request.app.state.settings, policy)
    try:
        session.commit()
    except Exception:
        session.rollback()
        for item in reversed(quarantined):
            restore_quarantined_media(request.app.state.settings, item)
        raise
    for item in quarantined:
        discard_quarantined_media(request.app.state.settings, item)


@admin_router.get("", response_model=RecordingListResponse)
def list_recordings(
    request: Request,
    unread: bool | None = Query(default=None),
    page: int = Query(default=1, ge=1),
    page_size: int = Query(default=50, ge=1, le=100),
    _: str = Depends(require_admin),
    session: Session = Depends(get_session),
) -> RecordingListResponse:
    _apply_retention(request, session)
    statement = select(Recording).where(Recording.status != "deleted")
    count_statement = select(func.count()).select_from(Recording).where(Recording.status != "deleted")
    if unread is True:
        statement = statement.where(Recording.listened_at.is_(None))
        count_statement = count_statement.where(Recording.listened_at.is_(None))
    statement = statement.order_by(Recording.captured_at.desc()).offset((page - 1) * page_size).limit(page_size)
    recordings = session.scalars(statement).all()
    calls = {
        call.id: call
        for call in session.scalars(
            select(CallRecord).where(CallRecord.id.in_({item.call_id for item in recordings})),
        )
    }
    return RecordingListResponse(
        items=[_recording_response(request, item, calls[item.call_id]) for item in recordings],
        page=page,
        page_size=page_size,
        total=int(session.scalar(count_statement) or 0),
    )


def _parse_range(value: str | None, size: int) -> tuple[int, int] | None:
    if value is None:
        return None
    if not value.startswith("bytes=") or "," in value:
        raise HTTPException(
            status_code=416,
            detail="Only one byte range is supported",
            headers={"Content-Range": f"bytes */{size}", "Cache-Control": "no-store, private"},
        )
    bounds = value.removeprefix("bytes=").split("-", 1)
    try:
        if not bounds[0]:
            length = int(bounds[1])
            if length <= 0:
                raise ValueError
            start = max(0, size - length)
            end = size - 1
        else:
            start = int(bounds[0])
            end = size - 1 if not bounds[1] else int(bounds[1])
    except ValueError as error:
        raise HTTPException(
            status_code=416,
            detail="Invalid byte range",
            headers={"Content-Range": f"bytes */{size}", "Cache-Control": "no-store, private"},
        ) from error
    if start < 0 or start >= size or end < start:
        raise HTTPException(
            status_code=416,
            detail="Byte range is outside the recording",
            headers={"Content-Range": f"bytes */{size}", "Cache-Control": "no-store, private"},
        )
    return start, min(end, size - 1)


def _audio_response(
    recording_id: str,
    request: Request,
    session: Session,
    download: bool,
) -> Response:
    recording = session.get(Recording, recording_id)
    if recording is None or recording.status == "deleted":
        raise HTTPException(status_code=404, detail="Recording not found")
    if recording.status != "ready":
        raise HTTPException(status_code=409, detail="Recording audio is not ready")
    if recording.kind == "conversation":
        descriptor, temporary_name = tempfile.mkstemp(
            prefix=".playback-",
            suffix=".mp3",
            dir=request.app.state.settings.recording_root,
        )
        os.close(descriptor)
        temporary = Path(temporary_name)
        try:
            materialize_media(
                request.app.state.settings,
                request.app.state.recording_cipher,
                recording,
                temporary,
            )
        except RecordingError as error:
            temporary.unlink(missing_ok=True)
            raise HTTPException(status_code=error.status_code, detail=str(error)) from error
        headers = {
            "Cache-Control": "no-store, private",
            "Pragma": "no-cache",
            "X-Content-Type-Options": "nosniff",
            "Content-Security-Policy": "default-src 'none'",
        }
        if download:
            headers["Content-Disposition"] = (
                f'attachment; filename="conversation-{recording.id}.mp3"'
            )
        return FileResponse(
            temporary,
            media_type="audio/mpeg",
            headers=headers,
            background=BackgroundTask(temporary.unlink, missing_ok=True),
        )
    try:
        media = load_media(
            request.app.state.settings,
            request.app.state.recording_cipher,
            recording,
        )
    except RecordingError as error:
        raise HTTPException(status_code=error.status_code, detail=str(error)) from error
    selected = _parse_range(request.headers.get("range"), len(media))
    headers = {
        "Accept-Ranges": "bytes",
        "Cache-Control": "no-store, private",
        "Pragma": "no-cache",
        "X-Content-Type-Options": "nosniff",
        "Content-Security-Policy": "default-src 'none'",
    }
    status_code = 200
    content = media
    if selected is not None:
        start, end = selected
        content = media[start : end + 1]
        status_code = 206
        headers["Content-Range"] = f"bytes {start}-{end}/{len(media)}"
    if download:
        label = "conversation" if recording.kind == "conversation" else "voicemail"
        headers["Content-Disposition"] = f'attachment; filename="{label}-{recording.id}.mp3"'
    return Response(content, status_code=status_code, media_type="audio/mpeg", headers=headers)


@admin_router.get("/{recording_id}/audio")
def play_recording(
    recording_id: str,
    request: Request,
    _: str = Depends(require_admin),
    session: Session = Depends(get_session),
) -> Response:
    return _audio_response(recording_id, request, session, False)


@admin_router.get("/{recording_id}/download")
def download_recording(
    recording_id: str,
    request: Request,
    _: str = Depends(require_admin),
    session: Session = Depends(get_session),
) -> Response:
    return _audio_response(recording_id, request, session, True)


@admin_router.patch("/{recording_id}/listened", response_model=RecordingResponse)
def set_recording_listened(
    recording_id: str,
    body: RecordingListenedRequest,
    request: Request,
    actor: str = Depends(require_admin_write),
    session: Session = Depends(get_session),
) -> RecordingResponse:
    recording = session.get(Recording, recording_id)
    if recording is None or recording.status == "deleted":
        raise HTTPException(status_code=404, detail="Recording not found")
    if recording.status != "ready":
        raise HTTPException(status_code=409, detail="Only completed recordings can be marked listened")
    recording.listened_at = utcnow() if body.listened else None
    recording.updated_at = utcnow()
    audit(session, actor, "recording.listened_changed", f"recording:{recording.id}", {"listened": body.listened})
    session.commit()
    call = session.get(CallRecord, recording.call_id)
    return _recording_response(request, recording, call)


@admin_router.delete("/{recording_id}", status_code=204)
def delete_recording(
    recording_id: str,
    request: Request,
    actor: str = Depends(require_admin_write),
    session: Session = Depends(get_session),
) -> None:
    recording = session.scalar(
        select(Recording).where(Recording.id == recording_id).with_for_update(),
    )
    if recording is None:
        raise HTTPException(status_code=404, detail="Recording not found")
    if recording.status == "deleted":
        return
    if recording.status != "ready":
        raise HTTPException(status_code=409, detail="Pending device recordings cannot be deleted")
    try:
        quarantined = quarantine_media(request.app.state.settings, recording)
    except RecordingError as error:
        raise HTTPException(status_code=error.status_code, detail=str(error)) from error
    tombstone_recording(recording, "manual")
    audit(session, actor, "recording.deleted", f"recording:{recording.id}")
    try:
        session.commit()
    except Exception:
        session.rollback()
        restore_quarantined_media(request.app.state.settings, quarantined)
        raise
    discard_quarantined_media(request.app.state.settings, quarantined)


def _policy_response(
    session: Session,
    request: Request,
    policy: RecordingRetentionPolicy,
) -> RetentionPolicyResponse:
    pending = session.scalar(
        select(func.count()).select_from(Recording).where(Recording.status.in_(["uploading", "processing"])),
    ) or 0
    return RetentionPolicyResponse(
        mode=policy.mode,
        days=policy.days,
        quota_bytes=request.app.state.settings.recording_quota_bytes,
        used_bytes=recording_used_bytes(session),
        pending_count=int(pending),
        updated_at=policy.updated_at,
        updated_by=policy.updated_by,
    )


@settings_router.get("", response_model=RetentionPolicyResponse)
def get_recording_settings(
    request: Request,
    _: str = Depends(require_admin),
    session: Session = Depends(get_session),
) -> RetentionPolicyResponse:
    policy = initialize_retention_policy(session, request.app.state.settings.development_admin_email)
    quarantined = purge_expired_recordings(session, request.app.state.settings, policy)
    abandoned = cleanup_abandoned_uploads(session, request.app.state.settings)
    try:
        session.commit()
    except Exception:
        session.rollback()
        for item in reversed(quarantined):
            restore_quarantined_media(request.app.state.settings, item)
        raise
    for item in quarantined:
        discard_quarantined_media(request.app.state.settings, item)
    for upload in abandoned:
        remove_upload_files(request.app.state.settings, upload)
    return _policy_response(session, request, policy)


@settings_router.put("", response_model=RetentionPolicyResponse)
def put_recording_settings(
    body: RetentionPolicyRequest,
    request: Request,
    actor: str = Depends(require_admin_write),
    session: Session = Depends(get_session),
) -> RetentionPolicyResponse:
    policy = initialize_retention_policy(session, actor)
    policy.mode = body.mode
    policy.days = body.days
    policy.updated_at = utcnow()
    policy.updated_by = actor
    quarantined = purge_expired_recordings(session, request.app.state.settings, policy)
    audit(
        session,
        actor,
        "recording.retention_updated",
        "recording-retention-policy",
        {"mode": body.mode, "days": body.days},
    )
    try:
        session.commit()
    except Exception:
        session.rollback()
        for item in reversed(quarantined):
            restore_quarantined_media(request.app.state.settings, item)
        raise
    for item in quarantined:
        discard_quarantined_media(request.app.state.settings, item)
    return _policy_response(session, request, policy)
