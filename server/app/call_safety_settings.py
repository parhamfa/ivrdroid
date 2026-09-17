"""Signed call lifetime policy, independent of IVR flow and recording policy."""
from fastapi import APIRouter, Depends, HTTPException, Request
from pydantic import Field
from sqlalchemy import select
from sqlalchemy.orm import Session

from .auth import require_admin, require_admin_write
from .database import get_session
from .models import CallSafetyPolicy, Device, DeviceCallSafetyAcknowledgement
from .schemas import StrictModel
from .services import audit

router = APIRouter(prefix="/api/admin/v1/call-safety-settings", tags=["call-safety"])


class CallSafetySettingsInput(StrictModel):
    maximum_call_duration_minutes: int = Field(default=60, ge=1, le=1440, strict=True)


def signed_call_safety_policy(request: Request, session: Session) -> dict:
    policy = session.scalar(select(CallSafetyPolicy).order_by(CallSafetyPolicy.version.desc()).limit(1))
    document = {
        "kind": "call_safety_policy", "schema_version": 1,
        "version": policy.version if policy else 0,
        "maximum_call_duration_seconds": policy.maximum_call_duration_seconds if policy else 3600,
    }
    digest, signature = request.app.state.signer.sign(document)
    return {"document": document, "sha256": digest, "signature_b64": signature}


def acknowledge_call_safety_policy(session: Session, device: Device, status) -> None:
    if not status.call_safety_capable:
        if status.call_safety_policy_version is not None or status.maximum_call_duration_seconds is not None:
            raise HTTPException(422, "Call safety acknowledgment requires a compatible app and helper")
        return
    if status.call_safety_policy_version is None:
        return  # Capable but still recovering or waiting for the helper to apply the policy.
    version = status.call_safety_policy_version
    policy = session.get(CallSafetyPolicy, version) if version else None
    expected = 3600 if version == 0 else policy.maximum_call_duration_seconds if policy else None
    if expected is None or expected != status.maximum_call_duration_seconds:
        raise HTTPException(422, "Applied call safety policy does not match a signed policy")
    # Version zero is the signed factory policy, not a missing acknowledgment.
    if session.get(DeviceCallSafetyAcknowledgement, (device.id, version)) is None:
        session.add(DeviceCallSafetyAcknowledgement(device_id=device.id, policy_version=version,
                    maximum_call_duration_seconds=expected))


@router.get("")
def get_settings(request: Request, _: str = Depends(require_admin), session: Session = Depends(get_session)) -> dict:
    value = signed_call_safety_policy(request, session)
    value["devices"] = []
    for device in session.scalars(select(Device).where(Device.revoked_at.is_(None))):
        status = device.status or {}
        version = status.get("call_safety_policy_version")
        ack = session.get(DeviceCallSafetyAcknowledgement, (device.id, version)) if version is not None else None
        value["devices"].append({
            "id": device.id, "name": device.display_name, "capable": status.get("call_safety_capable", False),
            "applied_version": ack.policy_version if ack else None,
            "maximum_call_duration_seconds": ack.maximum_call_duration_seconds if ack else None,
            "last_error": status.get("call_safety_last_error"),
        })
    return value


@router.put("")
def save_settings(body: CallSafetySettingsInput, request: Request, actor: str = Depends(require_admin_write),
                  session: Session = Depends(get_session)) -> dict:
    # Allow staging a policy before the compatible tablet is installed. Desired and
    # applied remain separate; never imply an old helper is enforcing the new value.
    seconds = body.maximum_call_duration_minutes * 60
    latest = session.scalar(select(CallSafetyPolicy).order_by(CallSafetyPolicy.version.desc()).limit(1))
    if latest is None or latest.maximum_call_duration_seconds != seconds:
        policy = CallSafetyPolicy(maximum_call_duration_seconds=seconds, created_by=actor)
        session.add(policy)
        session.flush()
        audit(session, actor, "call_safety.policy_changed", f"call-safety-policy:{policy.version}")
        session.commit()
    return get_settings(request, actor, session)
