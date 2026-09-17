"""Explicit recovery of a verified prefix, never an elapsed-time completion rule."""
from fastapi import APIRouter, Depends, HTTPException, Request
from pydantic import Field
from sqlalchemy import select
from sqlalchemy.orm import Session

from .auth import require_admin_write
from .database import get_session
from .models import CallRecord, ConversationRecordingSegment, Recording, utcnow
from .recording_api import _recording_response
from .recording_service import RecordingError, assemble_conversation_recording, initialize_retention_policy
from .schemas import StrictModel
from .services import audit

router = APIRouter(prefix="/api/admin/v1/recordings", tags=["recording-recovery"])


class RecoveryEvidence(StrictModel):
    device_source_confirmed_absent: bool
    evidence: str = Field(min_length=20, max_length=2000)
    # Freeze the reviewed server prefix; an in-flight transfer cannot be mistaken for it.
    verified_segment_hashes: list[str] = Field(max_length=5760)


@router.post("/{recording_id}/recover-partial")
def recover_partial(recording_id: str, body: RecoveryEvidence, request: Request,
                    actor: str = Depends(require_admin_write), session: Session = Depends(get_session)):
    if not body.device_source_confirmed_absent:
        raise HTTPException(409, "Device recovery must establish that no active writer or pending source remains")
    recording = session.scalar(select(Recording).where(Recording.id == recording_id).with_for_update())
    if recording is None:
        raise HTTPException(404, "Recording not found")
    call = session.get(CallRecord, recording.call_id)
    if call is None or call.result == "IN_PROGRESS":
        raise HTTPException(409, "An active call or transfer cannot be recovered as abandoned")
    if recording.source_format != "legacy_wav" or recording.kind != "conversation":
        raise HTTPException(409, "This recovery operation is only for legacy conversation prefixes")
    if recording.status == "ready" and recording.partial:
        return _recording_response(request, recording, call)
    if recording.status != "uploading":
        raise HTTPException(409, "Recording is not awaiting recovery")
    segments = session.scalars(select(ConversationRecordingSegment).where(
        ConversationRecordingSegment.recording_id == recording.id).order_by(
        ConversationRecordingSegment.segment_index).with_for_update()).all()
    if any(item.status != "verified" for item in segments):
        raise HTTPException(409, "Unverified or active transfer data still exists; preserve it for device reconciliation")
    if [item.source_sha256 for item in segments] != body.verified_segment_hashes:
        raise HTTPException(409, "The reviewed prefix differs from current server data")
    if segments and (segments[-1].stop_reason != "segment_boundary" or
                     [item.segment_index for item in segments] != list(range(len(segments)))):
        raise HTTPException(409, "The recording is not an incomplete contiguous prefix")
    initialize_retention_policy(session, actor)
    destination = None
    try:
        if segments:
            destination = assemble_conversation_recording(session, request.app.state.settings,
                request.app.state.recording_cipher, recording, segments, confirmed_abandoned=True)
        else:
            recording.status = "failed"; recording.partial = True
            recording.processing_error = "Confirmed abandoned after device recovery; no verified audio survives"
            recording.updated_at = utcnow()
        audit(session, actor, "recording.recovered_partial", recording.id,
              {"evidence": body.evidence, "verified_segment_hashes": body.verified_segment_hashes,
               "duration_ms": recording.duration_ms, "partial": True})
        session.commit()
    except RecordingError as error:
        session.rollback()
        if destination: destination.unlink(missing_ok=True)
        raise HTTPException(error.status_code, str(error)) from error
    except Exception:
        session.rollback()
        if destination: destination.unlink(missing_ok=True)
        raise
    # Keep the original verified encrypted chunks for the release backup and
    # reconciliation. They are not rewritten to pretend a terminal segment existed.
    return _recording_response(request, recording, call)
