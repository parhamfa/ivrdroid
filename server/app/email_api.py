from __future__ import annotations

from urllib.parse import urljoin, urlparse

from fastapi import APIRouter, Depends, HTTPException, Request
from sqlalchemy.orm import Session

from . import email_notify
from .auth import require_admin, require_admin_write
from .database import get_session
from .email_notify import (
    EmailDeliveryError,
    build_email_payload,
    decrypt_password,
    email_ready,
    initialize_email_settings,
)
from .models import utcnow
from .schemas import EmailSettingsRequest, EmailSettingsResponse
from .services import audit


router = APIRouter(prefix="/api/admin/v1/email-settings", tags=["admin-email"])


def _response(row) -> EmailSettingsResponse:
    return EmailSettingsResponse(
        enabled=row.enabled,
        smtp_host=row.smtp_host,
        smtp_port=row.smtp_port,
        smtp_security=row.smtp_security,
        smtp_username=row.smtp_username,
        password_configured=bool(row.smtp_password_encrypted),
        from_address=row.from_address,
        from_name=row.from_name,
        recipients=list(row.recipients or []),
        updated_at=row.updated_at,
        updated_by=row.updated_by,
    )


@router.get("", response_model=EmailSettingsResponse)
def get_email_settings(
    request: Request,
    _: str = Depends(require_admin),
    session: Session = Depends(get_session),
) -> EmailSettingsResponse:
    row = initialize_email_settings(session, request.app.state.settings.development_admin_email)
    session.commit()
    return _response(row)


@router.put("", response_model=EmailSettingsResponse)
def put_email_settings(
    body: EmailSettingsRequest,
    request: Request,
    actor: str = Depends(require_admin_write),
    session: Session = Depends(get_session),
) -> EmailSettingsResponse:
    row = initialize_email_settings(session, actor, lock=True)
    password_changed = False
    if not body.smtp_username:
        # A password without its username is useless and should not linger in the database.
        password_changed = row.smtp_password_encrypted is not None
        row.smtp_password_encrypted = None
    elif body.smtp_password is not None:
        row.smtp_password_encrypted = request.app.state.cipher.encrypt(body.smtp_password)
        password_changed = True
    elif not row.smtp_password_encrypted:
        raise HTTPException(status_code=400, detail="Enter the SMTP password for this username")
    row.enabled = body.enabled
    row.smtp_host = body.smtp_host
    row.smtp_port = body.smtp_port
    row.smtp_security = body.smtp_security
    row.smtp_username = body.smtp_username
    row.from_address = body.from_address
    row.from_name = body.from_name
    row.recipients = list(body.recipients)
    row.updated_at = utcnow()
    row.updated_by = actor
    audit(
        session,
        actor,
        "email.settings_updated",
        "email-settings",
        {
            "enabled": body.enabled,
            "smtp_host": body.smtp_host,
            "smtp_port": body.smtp_port,
            "smtp_security": body.smtp_security,
            "from_address": body.from_address,
            "recipient_count": len(body.recipients),
            "password_changed": password_changed,
        },
    )
    session.commit()
    return _response(row)


@router.post("/test", response_model=EmailSettingsResponse)
def test_email_settings(
    request: Request,
    actor: str = Depends(require_admin_write),
    session: Session = Depends(get_session),
) -> EmailSettingsResponse:
    row = initialize_email_settings(session, actor)
    if not email_ready(row):
        raise HTTPException(
            status_code=400,
            detail="Enable email and set the SMTP host, sender, and a recipient before sending a test",
        )
    public_base_url = request.app.state.settings.public_base_url
    payload = build_email_payload(
        row,
        decrypt_password(request.app.state.cipher, row),
        local_hostname=urlparse(public_base_url).hostname,
        subject_title="IVRdroid test",
        body="This is a test email from the IVRdroid dashboard.",
        click_url=urljoin(f"{public_base_url.rstrip('/')}/", "settings"),
    )
    response = _response(row)
    # End the transaction first so no database lock is held while the SMTP exchange runs.
    session.commit()
    try:
        email_notify.send_test_email(payload)
    except EmailDeliveryError as error:
        # Not 502: Cloudflare replaces 5xx bodies with its own page, which would hide the SMTP reason.
        raise HTTPException(status_code=400, detail=str(error)) from None
    audit(session, actor, "email.test_sent", "email-settings")
    session.commit()
    return response
