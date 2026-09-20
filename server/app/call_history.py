"""Call evidence and notification identities survive independently of parent uploads."""
from __future__ import annotations

import hashlib
import json
from datetime import datetime, timezone

from fastapi import HTTPException
from sqlalchemy import select
from sqlalchemy.orm import Session

from .models import CallEventReceipt, CallRecord
from .ntfy import classify_call_notifications


def event_identity(document: dict) -> str:
    value = dict(document)
    if "occurred_at" in value:
        value["occurred_at"] = datetime.fromisoformat(value["occurred_at"].replace("Z", "+00:00")).astimezone(timezone.utc).isoformat()
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def merge_events(session: Session, device_id: str, call_id: str, documents: list[dict], call: CallRecord | None) -> None:
    if call is not None and call.device_id != device_id:
        raise HTTPException(409, "Call ID belongs to another device")
    session.flush()
    receipts = {row.identity: row for row in session.scalars(select(CallEventReceipt).where(
        CallEventReceipt.device_id == device_id, CallEventReceipt.call_id == call_id))}
    # Seed legacy events as already notified if the parent was terminal. New late
    # events still get their own identity; a retry cannot regenerate an alert.
    for document in (call.events or []) if call is not None else []:
        identity = event_identity(document)
        if identity not in receipts:
            receipts[identity] = CallEventReceipt(device_id=device_id, call_id=call_id, identity=identity,
                document=document, notification_scheduled=call.result != "IN_PROGRESS")
            session.add(receipts[identity])
    for document in documents:
        identity = event_identity(document)
        if identity not in receipts:
            receipts[identity] = CallEventReceipt(device_id=device_id, call_id=call_id, identity=identity,
                document=document, notification_scheduled=False)
            session.add(receipts[identity])
    session.flush()
    if call is not None:
        call.events = sorted((row.document for row in receipts.values()), key=lambda d: (d.get("occurred_at", ""), event_identity(d)))


def claim_notifications(session: Session, call: CallRecord) -> list[tuple[str, dict | None]]:
    if call.result in {"IN_PROGRESS", "END_DETAILS_UNAVAILABLE"}:
        return []
    result = []
    if not call.terminal_notification_scheduled:
        result.extend(classify_call_notifications(call.policy_decision, call.result, []))
        call.terminal_notification_scheduled = True
    session.flush()
    for row in session.scalars(select(CallEventReceipt).where(CallEventReceipt.device_id == call.device_id,
            CallEventReceipt.call_id == call.id, CallEventReceipt.notification_scheduled.is_(False))):
        result.extend((key, extra) for key, extra in classify_call_notifications(
            call.policy_decision, call.result, [row.document]) if key == "external_call_failed")
        row.notification_scheduled = True
    return result
