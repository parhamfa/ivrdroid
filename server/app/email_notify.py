from __future__ import annotations

import logging
import re
import smtplib
import socket
import ssl
from dataclasses import dataclass, field
from email.headerregistry import Address
from email.message import EmailMessage
from email.utils import formatdate, make_msgid

from sqlalchemy import select
from sqlalchemy.orm import Session

from .crypto import DataCipher
from .models import EmailSettings

LOGGER = logging.getLogger("ivrdroid.email")

SMTP_TIMEOUT_SECONDS = 10
SUBJECT_PREFIX = "[IVRdroid]"
ERROR_LIMIT = 200


class EmailDeliveryError(Exception):
    pass


@dataclass(frozen=True)
class EmailPayload:
    host: str
    port: int
    security: str
    username: str
    password: str | None = field(repr=False)
    from_address: str
    from_name: str
    recipients: tuple[str, ...]
    subject: str
    body: str
    local_hostname: str | None


def initialize_email_settings(session: Session, actor: str, *, lock: bool = False) -> EmailSettings:
    statement = select(EmailSettings).where(EmailSettings.id == 1)
    if lock:
        statement = statement.with_for_update()
    row = session.scalar(statement)
    if row is None:
        row = EmailSettings(
            id=1,
            enabled=False,
            smtp_host="smtp.mail.me.com",
            smtp_port=587,
            smtp_security="starttls",
            smtp_username="",
            from_address="",
            from_name="IVRdroid",
            recipients=[],
            updated_by=actor,
        )
        session.add(row)
        session.flush()
    return row


def email_ready(row: EmailSettings | None) -> bool:
    return bool(row is not None and row.enabled and row.smtp_host and row.from_address and row.recipients)


def decrypt_password(cipher: DataCipher, row: EmailSettings) -> str | None:
    if not row.smtp_password_encrypted:
        return None
    return cipher.decrypt(row.smtp_password_encrypted)


def build_email_payload(
    row: EmailSettings,
    password: str | None,
    *,
    local_hostname: str | None,
    subject_title: str,
    body: str,
    click_url: str | None,
) -> EmailPayload:
    title = " ".join(subject_title.split())[:120]
    return EmailPayload(
        host=row.smtp_host,
        port=row.smtp_port,
        security=row.smtp_security,
        username=row.smtp_username,
        password=password,
        from_address=row.from_address,
        from_name=row.from_name,
        recipients=tuple(row.recipients),
        subject=f"{SUBJECT_PREFIX} {title}",
        body=f"{body}\n\n{click_url}\n" if click_url else body,
        local_hostname=local_hostname,
    )


def compose_message(payload: EmailPayload) -> EmailMessage:
    message = EmailMessage()
    message["From"] = Address(display_name=payload.from_name, addr_spec=payload.from_address)
    message["To"] = tuple(Address(addr_spec=recipient) for recipient in payload.recipients)
    message["Subject"] = payload.subject
    message["Date"] = formatdate(localtime=False, usegmt=True)
    # An explicit domain avoids a reverse lookup of this host and keeps its name out of the headers.
    message["Message-ID"] = make_msgid(domain=payload.from_address.rpartition("@")[2])
    message["Auto-Submitted"] = "auto-generated"
    message.set_content(payload.body)
    return message


def _send(payload: EmailPayload) -> None:
    message = compose_message(payload)
    # smtplib's implicit context skips certificate checks, so always pass a verifying one.
    context = ssl.create_default_context()
    if payload.security == "ssl":
        client = smtplib.SMTP_SSL(
            payload.host,
            payload.port,
            local_hostname=payload.local_hostname,
            timeout=SMTP_TIMEOUT_SECONDS,
            context=context,
        )
    else:
        client = smtplib.SMTP(
            payload.host,
            payload.port,
            local_hostname=payload.local_hostname,
            timeout=SMTP_TIMEOUT_SECONDS,
        )
    try:
        if payload.security != "ssl":
            client.ehlo()
            # Unconditional: a server without STARTTLS fails here instead of receiving the login in plaintext.
            client.starttls(context=context)
            client.ehlo()
        if payload.username:
            client.login(payload.username, payload.password or "")
        refused = client.send_message(
            message,
            from_addr=payload.from_address,
            to_addrs=list(payload.recipients),
        )
        if refused:
            LOGGER.warning("SMTP server refused %d of %d email recipients", len(refused), len(payload.recipients))
    finally:
        try:
            client.quit()
        except Exception:
            client.close()


def _server_reply(code: int | None, text: bytes | str | None) -> str:
    if isinstance(text, bytes):
        text = text.decode("utf-8", "replace")
    return f"{code} {text or ''}".strip()


def describe_smtp_error(error: BaseException, payload: EmailPayload) -> str:
    endpoint = f"{payload.host}:{payload.port}"
    mismatch = "check that the port matches the security setting (465 uses SSL, 587 uses STARTTLS)"
    if isinstance(error, smtplib.SMTPAuthenticationError):
        text = f"SMTP login failed ({_server_reply(error.smtp_code, error.smtp_error)})"
    elif isinstance(error, smtplib.SMTPSenderRefused):
        text = f"SMTP server refused the sender address ({_server_reply(error.smtp_code, error.smtp_error)})"
    elif isinstance(error, smtplib.SMTPRecipientsRefused):
        replies = list(error.recipients.values())
        first = _server_reply(*replies[0]) if replies else "no reply"
        text = f"SMTP server refused all {len(replies)} recipients ({first})"
    elif isinstance(error, smtplib.SMTPNotSupportedError):
        text = f"SMTP server does not support a required feature: {error}"
    elif isinstance(error, smtplib.SMTPServerDisconnected):
        text = f"SMTP server closed the connection ({error}); {mismatch}"
    elif isinstance(error, ssl.SSLCertVerificationError):
        reason = getattr(error, "verify_message", None) or "verification failed"
        text = f"TLS certificate for {payload.host} could not be verified ({reason})"
    elif isinstance(error, ssl.SSLError):
        text = f"TLS handshake with {endpoint} failed ({getattr(error, 'reason', None) or 'SSL error'}); {mismatch}"
    elif isinstance(error, socket.gaierror):
        text = f"SMTP host {payload.host} was not found"
    elif isinstance(error, TimeoutError):
        text = f"Connection to {endpoint} timed out; the port may be blocked"
    elif isinstance(error, ConnectionRefusedError):
        text = f"Connection to {endpoint} was refused"
    elif isinstance(error, smtplib.SMTPResponseException):
        text = f"SMTP error ({_server_reply(error.smtp_code, error.smtp_error)})"
    elif isinstance(error, smtplib.SMTPException):
        text = f"SMTP error: {error}"
    elif isinstance(error, OSError):
        text = f"Could not reach {endpoint} ({error.strerror or type(error).__name__})"
    else:
        text = f"Email delivery failed ({type(error).__name__})"
    text = " ".join(text.split())
    for secret in (payload.password, payload.username):
        if secret:
            text = re.sub(re.escape(secret), "***", text, flags=re.IGNORECASE)
    return text[:ERROR_LIMIT]


def deliver_email(payload: EmailPayload) -> None:
    try:
        _send(payload)
    except Exception as error:
        LOGGER.warning("Email notification failed: %s", describe_smtp_error(error, payload))


def send_test_email(payload: EmailPayload) -> None:
    try:
        _send(payload)
    except Exception as error:
        raise EmailDeliveryError(describe_smtp_error(error, payload)) from None
