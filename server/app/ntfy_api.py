from __future__ import annotations

from fastapi import APIRouter, Depends, HTTPException, Request
from sqlalchemy.orm import Session

from .auth import require_admin, require_admin_write
from .database import get_session
from .models import utcnow
from .ntfy import decrypt_token, initialize_ntfy_settings, parse_events, send_test_notification
from .schemas import NtfySettingsRequest, NtfySettingsResponse
from .services import audit


router = APIRouter(prefix="/api/admin/v1/ntfy-settings", tags=["admin-ntfy"])


def _response(row) -> NtfySettingsResponse:
    return NtfySettingsResponse(
        enabled=row.enabled,
        server_url=row.server_url,
        topic=row.topic,
        token_configured=bool(row.token_encrypted),
        events=parse_events(row),
        updated_at=row.updated_at,
        updated_by=row.updated_by,
    )


@router.get("", response_model=NtfySettingsResponse)
def get_ntfy_settings(
    request: Request,
    _: str = Depends(require_admin),
    session: Session = Depends(get_session),
) -> NtfySettingsResponse:
    row = initialize_ntfy_settings(session, request.app.state.settings.development_admin_email)
    session.commit()
    return _response(row)


@router.put("", response_model=NtfySettingsResponse)
def put_ntfy_settings(
    body: NtfySettingsRequest,
    request: Request,
    actor: str = Depends(require_admin_write),
    session: Session = Depends(get_session),
) -> NtfySettingsResponse:
    row = initialize_ntfy_settings(session, actor, lock=True)
    row.enabled = body.enabled
    row.server_url = body.server_url
    row.topic = body.topic
    if body.token is not None:
        row.token_encrypted = request.app.state.cipher.encrypt(body.token)
    row.events = body.events.model_dump(mode="json")
    row.updated_at = utcnow()
    row.updated_by = actor
    audit(
        session,
        actor,
        "ntfy.settings_updated",
        "ntfy-settings",
        {"enabled": body.enabled, "topic": body.topic, "server_url": body.server_url},
    )
    session.commit()
    return _response(row)


@router.post("/test", response_model=NtfySettingsResponse)
def test_ntfy_settings(
    request: Request,
    actor: str = Depends(require_admin_write),
    session: Session = Depends(get_session),
) -> NtfySettingsResponse:
    row = initialize_ntfy_settings(session, actor, lock=True)
    try:
        send_test_notification(request.app, row, decrypt_token(request.app.state.cipher, row))
    except ValueError as error:
        raise HTTPException(status_code=400, detail=str(error)) from error
    audit(session, actor, "ntfy.test_sent", "ntfy-settings")
    session.commit()
    return _response(row)
