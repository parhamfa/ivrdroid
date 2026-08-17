from __future__ import annotations

import logging
from dataclasses import dataclass
from datetime import timedelta, timezone
from typing import Any
from urllib.parse import quote, urljoin
from fastapi import BackgroundTasks, FastAPI
from sqlalchemy import select
from sqlalchemy.orm import Session

import httpx

from .crypto import DataCipher
from .models import Device, NtfySettings, utcnow
from .recording_service import recording_used_bytes
from .schemas import NtfyEvents, NtfyPriority, default_ntfy_events

LOGGER = logging.getLogger("ivrdroid.ntfy")

VOICEMAIL_SPOOL_LIMIT_BYTES = 512 * 1024 * 1024
CONVERSATION_SPOOL_LIMIT_BYTES = 4 * 1024 * 1024 * 1024
NTFY_PRIORITIES: dict[NtfyPriority, int] = {
    "min": 1,
    "low": 2,
    "default": 3,
    "high": 4,
    "max": 5,
}
IVR_FAILURE_RESULTS = frozenset(
    {
        "HELPER_BUSY",
        "HELPER_CLAIM_TIMEOUT",
        "HELPER_REQUEST_FAILED",
        "ANSWER_PERMISSION_MISSING",
        "FAILED_RESTORE",
        "FAILED_AUDIO",
        "FAILED_CAPTURE",
        "FAILED_END_CALL",
        "REJECTED_REQUEST",
        "REJECTED_BUSY",
        "REVISION_REJECTED",
        "INCOMPATIBLE_DEVICE",
        "RECOVERY_HANGUP_SKIPPED",
    },
)
IVR_COMPLETED_RESULTS = frozenset({"SESSION_COMPLETE", "REMOTE_HANGUP", "RECOVERED_AND_ENDED"})
STOCK_DIALER_POLICIES = frozenset(
    {
        "NOT_ALLOWLISTED",
        "IVR_DISABLED",
        "EXCLUDED_CALLER",
        "UNKNOWN_TO_STOCK_DIALER",
        "LOCAL_KILL_SWITCH",
        "NO_ACTIVE_CONFIGURATION",
        "INVALID_POLICY",
    },
)


@dataclass(frozen=True)
class NtfyPayload:
    url: str
    token: str | None
    title: str
    message: str
    priority: int
    tags: str
    click: str | None


def initialize_ntfy_settings(session: Session, actor: str, *, lock: bool = False) -> NtfySettings:
    statement = select(NtfySettings).where(NtfySettings.id == 1)
    if lock:
        statement = statement.with_for_update()
    row = session.scalar(statement)
    if row is None:
        row = NtfySettings(
            id=1,
            enabled=False,
            server_url="https://ntfy.sh",
            topic="",
            events=default_ntfy_events().model_dump(mode="json"),
            updated_by=actor,
        )
        session.add(row)
        session.flush()
    return row


def parse_events(row: NtfySettings) -> NtfyEvents:
    defaults = default_ntfy_events().model_dump(mode="json")
    stored = row.events if isinstance(row.events, dict) else {}
    merged = {key: {**defaults[key], **stored.get(key, {})} for key in defaults}
    return NtfyEvents.model_validate(merged)


def decrypt_token(cipher: DataCipher, row: NtfySettings) -> str | None:
    if not row.token_encrypted:
        return None
    return cipher.decrypt(row.token_encrypted)


def deliver_notification(payload: NtfyPayload) -> None:
    headers = {
        "Title": payload.title[:120],
        "Priority": str(payload.priority),
        "Tags": payload.tags,
        "User-Agent": "ivrdroid",
    }
    if payload.click:
        headers["Click"] = payload.click
    if payload.token:
        headers["Authorization"] = f"Bearer {payload.token}"
    try:
        response = httpx.post(
            payload.url,
            content=payload.message.encode("utf-8"),
            headers=headers,
            timeout=5.0,
        )
        if response.status_code >= 400:
            LOGGER.warning("ntfy rejected a notification (%s)", response.status_code)
    except Exception:
        LOGGER.exception("ntfy notification failed")


def send_test_notification(app: FastAPI, row: NtfySettings, token: str | None) -> None:
    if not row.enabled or not row.topic:
        raise ValueError("Enable ntfy and set a topic before sending a test")
    deliver_notification(
        NtfyPayload(
            url=f"{row.server_url.rstrip('/')}/{quote(row.topic, safe='')}",
            token=token,
            title="IVRdroid test",
            message="This is a test notification from the IVRdroid dashboard.",
            priority=3,
            tags="white_check_mark,ivrdroid",
            click=urljoin(f"{app.state.settings.public_base_url.rstrip('/')}/", "settings"),
        ),
    )


def _event_config(events: NtfyEvents, event_key: str):
    return getattr(events, event_key)


def _build_payload(
    *,
    row: NtfySettings,
    token: str | None,
    public_base_url: str,
    event_key: str,
    title: str,
    message: str,
    click_path: str,
    tags: str,
    caller_masked: str | None,
    extra: dict[str, Any] | None,
) -> NtfyPayload | None:
    if not row.enabled or not row.topic:
        return None
    events = parse_events(row)
    config = _event_config(events, event_key)
    if not config.enabled:
        return None
    if extra and extra.get("external_status") == "NOT_CONNECTED" and not getattr(config, "not_connected", True):
        return None
    if extra and extra.get("external_status") == "SYSTEM_FAILURE" and not getattr(config, "system_failure", True):
        return None
    body = message
    if caller_masked and getattr(config, "include_masked_caller", False):
        body = f"{message}\nCaller {caller_masked}"
    return NtfyPayload(
        url=f"{row.server_url.rstrip('/')}/{quote(row.topic, safe='')}",
        token=token,
        title=title,
        message=body[:1000],
        priority=NTFY_PRIORITIES[config.priority],
        tags=tags,
        click=urljoin(f"{public_base_url.rstrip('/')}/", click_path.lstrip("/")),
    )


def dispatch_ntfy(
    app: FastAPI,
    event_key: str,
    *,
    title: str,
    message: str,
    click_path: str = "/",
    tags: str = "ivrdroid",
    caller_masked: str | None = None,
    extra: dict[str, Any] | None = None,
) -> None:
    cipher: DataCipher = app.state.cipher
    with app.state.database.session() as session:
        row = initialize_ntfy_settings(session, app.state.settings.development_admin_email, lock=True)
        token = decrypt_token(cipher, row)
        payload = _build_payload(
            row=row,
            token=token,
            public_base_url=app.state.settings.public_base_url,
            event_key=event_key,
            title=title,
            message=message,
            click_path=click_path,
            tags=tags,
            caller_masked=caller_masked,
            extra=extra,
        )
        session.commit()
    if payload is not None:
        deliver_notification(payload)


def schedule_ntfy(
    background_tasks: BackgroundTasks | None,
    app: FastAPI,
    event_key: str,
    **kwargs,
) -> None:
    if background_tasks is None:
        dispatch_ntfy(app, event_key, **kwargs)
        return
    background_tasks.add_task(dispatch_ntfy, app, event_key, **kwargs)


def notify_storage_full(app: FastAPI, detail: str = "Recording storage is full.") -> None:
    cipher: DataCipher = app.state.cipher
    payload: NtfyPayload | None = None
    with app.state.database.session() as session:
        row = initialize_ntfy_settings(session, app.state.settings.development_admin_email, lock=True)
        if row.last_storage_notified_at is None:
            payload = _build_payload(
                row=row,
                token=decrypt_token(cipher, row),
                public_base_url=app.state.settings.public_base_url,
                event_key="storage_full",
                title="Recording storage is full",
                message=detail,
                click_path="/settings#voicemail-retention",
                tags="warning,ivrdroid",
                caller_masked=None,
                extra=None,
            )
            if payload is not None:
                row.last_storage_notified_at = utcnow()
        session.commit()
    if payload is not None:
        deliver_notification(payload)


def _spool_full(status: dict[str, Any]) -> bool:
    voicemail = int(status.get("voicemail_spool_bytes") or status.get("recording_spool_bytes") or 0)
    conversation = int(status.get("conversation_spool_bytes") or 0)
    return voicemail >= VOICEMAIL_SPOOL_LIMIT_BYTES or conversation >= CONVERSATION_SPOOL_LIMIT_BYTES


def _quota_warning(session: Session, app: FastAPI, events: NtfyEvents) -> bool:
    quota = app.state.settings.recording_quota_bytes
    if quota <= 0:
        return False
    used = recording_used_bytes(session)
    return used * 100 >= quota * events.storage_full.warn_at_quota_percent


def evaluate_storage(app: FastAPI) -> None:
    cipher: DataCipher = app.state.cipher
    payload: NtfyPayload | None = None
    with app.state.database.session() as session:
        row = initialize_ntfy_settings(session, app.state.settings.development_admin_email, lock=True)
        events = parse_events(row)
        devices = session.scalars(select(Device).where(Device.revoked_at.is_(None))).all()
        spool = any(_spool_full(device.status or {}) for device in devices)
        warning = _quota_warning(session, app, events)
        saturated = spool or warning
        if saturated and row.last_storage_notified_at is None:
            payload = _build_payload(
                row=row,
                token=decrypt_token(cipher, row),
                public_base_url=app.state.settings.public_base_url,
                event_key="storage_full",
                title="Recording storage is full",
                message="Server quota or a tablet spool has reached its warning threshold.",
                click_path="/settings#voicemail-retention",
                tags="warning,ivrdroid",
                caller_masked=None,
                extra=None,
            )
            if payload is not None:
                row.last_storage_notified_at = utcnow()
        elif not saturated:
            row.last_storage_notified_at = None
        session.commit()
    if payload is not None:
        deliver_notification(payload)


def _offline_timeout(events: NtfyEvents, call_state: str) -> timedelta:
    minutes = (
        events.tablet_offline.idle_timeout_minutes
        if call_state == "idle"
        else events.tablet_offline.in_call_timeout_minutes
    )
    return timedelta(minutes=minutes)


def evaluate_presence(app: FastAPI) -> None:
    cipher: DataCipher = app.state.cipher
    payloads: list[NtfyPayload] = []
    with app.state.database.session() as session:
        row = initialize_ntfy_settings(session, app.state.settings.development_admin_email, lock=True)
        events = parse_events(row)
        now = utcnow()
        devices = list(session.scalars(select(Device).where(Device.revoked_at.is_(None))))
        offline = False
        for device in devices:
            if device.last_seen_at is None:
                offline = True
                continue
            seen = device.last_seen_at
            if seen.tzinfo is None:
                seen = seen.replace(tzinfo=timezone.utc)
            call_state = str((device.status or {}).get("call_state") or "unknown")
            if now - seen >= _offline_timeout(events, call_state):
                offline = True
        token = decrypt_token(cipher, row)
        if offline:
            if row.last_offline_notified_at is None:
                payload = _build_payload(
                    row=row,
                    token=token,
                    public_base_url=app.state.settings.public_base_url,
                    event_key="tablet_offline",
                    title="Tablet offline",
                    message="The enrolled tablet has not synchronized within the configured timeout.",
                    click_path="/settings",
                    tags="warning,ivrdroid",
                    caller_masked=None,
                    extra=None,
                )
                if payload is not None:
                    row.last_offline_notified_at = now
                    payloads.append(payload)
        elif row.last_offline_notified_at is not None:
            row.last_offline_notified_at = None
            if events.tablet_offline.notify_when_recovered:
                payload = _build_payload(
                    row=row,
                    token=token,
                    public_base_url=app.state.settings.public_base_url,
                    event_key="tablet_offline",
                    title="Tablet recovered",
                    message="The enrolled tablet is synchronizing again.",
                    click_path="/settings",
                    tags="white_check_mark,ivrdroid",
                    caller_masked=None,
                    extra=None,
                )
                if payload is not None:
                    payloads.append(payload)
        session.commit()
    for payload in payloads:
        deliver_notification(payload)


def run_ntfy_watch(app: FastAPI) -> None:
    evaluate_presence(app)
    evaluate_storage(app)


def classify_call_notifications(
    policy_decision: str,
    result: str,
    events: list[dict[str, Any]],
) -> list[tuple[str, dict[str, Any] | None]]:
    if result == "IN_PROGRESS":
        return []
    keys: list[tuple[str, dict[str, Any] | None]] = []
    if result in IVR_FAILURE_RESULTS:
        keys.append(("ivr_session_failed", None))
    if result in IVR_COMPLETED_RESULTS:
        keys.append(("ivr_session_completed", None))
    if result == "STOCK_DIALER" or (
        policy_decision in STOCK_DIALER_POLICIES
        and result not in IVR_FAILURE_RESULTS
        and result not in IVR_COMPLETED_RESULTS
    ):
        keys.append(("stock_dialer_routing", None))
    for document in events:
        if not isinstance(document, dict) or document.get("event") != "external_call":
            continue
        status = document.get("status")
        if status in {"NOT_CONNECTED", "SYSTEM_FAILURE"}:
            keys.append(("external_call_failed", {"external_status": status}))
    return keys


def call_notification_kwargs(
    event_key: str,
    *,
    call_id: str,
    result: str,
    policy_decision: str,
    extra: dict[str, Any] | None,
) -> dict[str, Any]:
    titles = {
        "ivr_session_failed": "IVR session failed",
        "ivr_session_completed": "IVR session completed",
        "stock_dialer_routing": "Call stayed with the stock dialer",
        "external_call_failed": "External call failed",
    }
    messages = {
        "ivr_session_failed": f"The tablet reported {result}.",
        "ivr_session_completed": f"The IVR session ended with {result}.",
        "stock_dialer_routing": f"Caller policy {policy_decision} left the call with Android.",
        "external_call_failed": (
            f"The operator step ended with {extra.get('external_status') if extra else 'failure'}."
        ),
    }
    return {
        "title": titles[event_key],
        "message": messages[event_key],
        "click_path": f"/calls?call={quote(call_id, safe='')}",
        "tags": "telephone,ivrdroid",
        "extra": extra,
    }
