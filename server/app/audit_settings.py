"""Versioned recording policy, independent of the published IVR flow."""
from fastapi import APIRouter, Depends, HTTPException, Request
from pydantic import Field
from sqlalchemy import select
from sqlalchemy.orm import Session

from .auth import require_admin, require_admin_write
from .database import get_session
from .models import Device, SessionAuditPolicy, DeviceAuditPolicyAcknowledgement
from .schemas import StrictModel, SessionAuditReport
from .services import audit

router = APIRouter(prefix="/api/admin/v1/audit-recording-settings", tags=["call-auditing"])


class AuditSettingsInput(StrictModel):
    enabled: bool
    local_quota_bytes: int = Field(default=1024**3, ge=64 * 1024**2, le=4 * 1024**3)


def signed_policy(request: Request, session: Session) -> dict:
    policy = session.scalar(select(SessionAuditPolicy).order_by(SessionAuditPolicy.version.desc()).limit(1))
    document = {
        "kind": "session_audit_policy",
        "version": policy.version if policy else 0,
        "enabled": policy.enabled if policy else False,
        "local_quota_bytes": policy.local_quota_bytes if policy else 1024**3,
    }
    digest, signature = request.app.state.signer.sign(document)
    return {"document": document, "sha256": digest, "signature_b64": signature}


def validate_call_report(session: Session, report: SessionAuditReport | None, result: str, device_id: str) -> None:
    if report is None or report.state == "disabled":
        return
    policy = session.get(SessionAuditPolicy, report.policy_version)
    if policy is None or not policy.enabled or result == "STOCK_DIALER" or not session.get(DeviceAuditPolicyAcknowledgement, (device_id, report.policy_version)):
        raise HTTPException(status_code=422, detail="Audit audio requires an enabled session policy and an IVR call")


def acknowledge_policy(session: Session, device: Device, status) -> None:
    if not status.session_audit_capable or status.audit_policy_version == 0:
        if status.audit_enabled:
            raise HTTPException(status_code=422, detail="Audit recording needs a verified policy and supported helper")
        return
    policy = session.get(SessionAuditPolicy, status.audit_policy_version)
    if policy is None or policy.enabled != status.audit_enabled:
        raise HTTPException(status_code=422, detail="Applied audit policy does not match a signed policy")
    identity = (device.id, policy.version)
    if session.get(DeviceAuditPolicyAcknowledgement, identity) is None:
        session.add(DeviceAuditPolicyAcknowledgement(device_id=device.id, policy_version=policy.version))


@router.get("")
def get_settings(request: Request, _: str = Depends(require_admin), session: Session = Depends(get_session)) -> dict:
    value = signed_policy(request, session)
    devices = session.scalars(select(Device).where(Device.revoked_at.is_(None))).all()
    value["devices"] = [{
        "id": device.id, "name": device.display_name,
        "capable": (device.status or {}).get("session_audit_capable", False),
        "applied_version": (device.status or {}).get("audit_policy_version", 0),
        "enabled": (device.status or {}).get("audit_enabled", False),
        "spool_bytes": (device.status or {}).get("audit_spool_bytes", 0),
        "spool_count": (device.status or {}).get("audit_spool_count", 0),
        "last_error": (device.status or {}).get("audit_last_error"),
    } for device in devices]
    value["server_quota_bytes"] = request.app.state.settings.audit_recording_quota_bytes
    return value


@router.put("")
def save_settings(body: AuditSettingsInput, request: Request, actor: str = Depends(require_admin_write),
                  session: Session = Depends(get_session)) -> dict:
    devices = session.scalars(select(Device).where(Device.revoked_at.is_(None))).all()
    if body.enabled and (not devices or any(not (d.status or {}).get("session_audit_capable") for d in devices)):
        raise HTTPException(status_code=409, detail="Update the enrolled tablet app and helper before enabling call auditing")
    policy = SessionAuditPolicy(enabled=body.enabled, local_quota_bytes=body.local_quota_bytes, created_by=actor)
    session.add(policy)
    session.flush()
    audit(session, actor, "session_audit.policy_changed", f"audit-policy:{policy.version}")
    session.commit()
    return get_settings(request, actor, session)
