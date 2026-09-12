import base64
import hashlib
from datetime import timedelta
from uuid import uuid4

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey

from app.crypto import canonical_json_bytes, digest_device_token
from app.models import CallRecord, Device, Recording, utcnow
from test_conversation_recordings_api import conversation_wav


def context(client, partial=False, duration_ms=100):
    device_id, call_id, recording_id = (str(uuid4()) for _ in range(3))
    token = "audit-test-token-" + device_id + "x" * 40
    now = utcnow() - timedelta(milliseconds=duration_ms + 1000)
    with client.app.state.database.session() as session:
        session.add(Device(id=device_id, display_name="Tablet", token_hash=digest_device_token(token),
                           status={"session_audit_capable": True}))
        session.commit()
    policy = client.put("/api/admin/v1/audit-recording-settings", json={"enabled": True})
    assert policy.status_code == 200, policy.text
    version = policy.json()["document"]["version"]
    report = {"recording_id": recording_id, "policy_version": version, "state": "pending_upload",
              "captured_at": now.isoformat(), "duration_ms": duration_ms, "partial": partial,
              "stop_reason": "interrupted" if partial else "caller_hangup",
              "events": [{"offset_ms": 0, "type": "answered", "detail": ""},
                         {"offset_ms": duration_ms, "type": "ended", "detail": "caller_hangup"}]}
    headers = {"Authorization": f"Bearer {token}"}
    acknowledgement = client.post("/api/device/v1/sync", headers=headers, json={
        "app_version": "0.9.0", "helper_version": "0.9.0", "status": {
            "helper_state": "READY", "helper_result": "NONE", "call_state": "idle",
            "local_kill_switch": False, "storage_free_bytes": 5 * 1024**3,
            "session_audit_capable": True, "audit_policy_version": version, "audit_enabled": True,
        },
    })
    assert acknowledgement.status_code == 200, acknowledgement.text
    call = {"call_id": call_id, "started_at": now.isoformat(), "policy_decision": "IVR_HANDLED",
            "result": "REMOTE_HANGUP", "duration_seconds": (duration_ms + 999) // 1000, "session_audit": report}
    result = client.post("/api/device/v1/events:batch", headers=headers, json={"calls": [call]})
    assert result.status_code == 200, result.text
    return {"headers": headers, "call": call, "report": report, "recording_id": recording_id,
            "call_id": call_id, "policy_version": version, "captured_at": now.isoformat()}


def upload(client, ctx, corrupt_first=False):
    identity = {key: ctx[key] for key in ("recording_id", "call_id", "policy_version", "captured_at")}
    root = "/api/device/v1/session-recordings"
    created = client.post(root, headers=ctx["headers"], json=identity)
    assert created.status_code == 201, created.text
    duration = ctx["report"]["duration_ms"]
    count = (duration + 14999) // 15000
    from datetime import datetime
    for index in range(count):
        length = min(15000, duration - index * 15000)
        wav = conversation_wav(length, 400)
        segment = {**identity, "kind": "session_audit", "segment_index": index, "duration_ms": length,
                   "captured_at": (datetime.fromisoformat(ctx["captured_at"]) + timedelta(seconds=index * 15)).isoformat(),
                   "stop_reason": ctx["report"]["stop_reason"] if index == count - 1 else "segment_boundary",
                   "partial": ctx["report"]["partial"] if index == count - 1 else False,
                   "expected_size_bytes": len(wav), "source_sha256": hashlib.sha256(wav).hexdigest()}
        url = f"{root}/{ctx['recording_id']}/segments/{index}"
        result = client.post(url, headers=ctx["headers"], json=segment)
        assert result.status_code == 201, result.text
        for attempt in range(2 if corrupt_first and index == 0 else 1):
            payload = wav[:-1] + bytes([wav[-1] ^ 1]) if corrupt_first and index == 0 and attempt == 0 else wav
            for offset in range(0, len(payload), 1024 * 1024):
                chunk = payload[offset:offset + 1024 * 1024]
                result = client.put(url + "/content", headers={**ctx["headers"], "Upload-Offset": str(offset),
                                     "Content-Type": "application/offset+octet-stream"}, content=chunk)
                assert result.status_code == 200, result.text
                assert result.json()["upload_offset"] == offset + len(chunk)
                assert client.get(url + "/upload", headers=ctx["headers"]).json()["upload_offset"] == offset + len(chunk)
            result = client.post(url + "/complete", headers=ctx["headers"])
            if payload != wav:
                assert result.status_code == 422, result.text
                reset = client.post(url, headers=ctx["headers"], json=segment)
                assert reset.status_code == 201 and reset.json()["upload_offset"] == 0, reset.text
            else:
                assert result.status_code == 200 and result.json()["acknowledged"], result.text
    result = client.post(f"{root}/{ctx['recording_id']}/complete", headers=ctx["headers"], json={"segment_count": count})
    assert result.status_code == 200, result.text
    assert result.json()["acknowledged"] is True
    return identity, segment


def test_segment_boundaries_resume_and_corruption_retry(client):
    for duration in (15000, 15100, 30000):
        ctx = context(client, duration_ms=duration)
        upload(client, ctx, corrupt_first=duration == 15100)
        detail = client.get(f"/api/admin/v1/calls/{ctx['call_id']}").json()
        assert detail["session_audit"]["recording"]["duration_ms"] == duration
        assert detail["session_audit"]["recording"]["segment_count"] == (duration + 14999) // 15000


def test_shared_retention_preserves_call_and_timeline(client):
    from app.recording_service import initialize_retention_policy, purge_expired_recordings, discard_quarantined_media
    ctx = context(client)
    upload(client, ctx)
    with client.app.state.database.session() as session:
        recording = session.get(Recording, ctx["recording_id"])
        recording.captured_at = utcnow() - timedelta(days=31)
        session.flush()
        policy = initialize_retention_policy(session, "test")
        removed = purge_expired_recordings(session, client.app.state.settings, policy)
        session.commit()
        for item in removed:
            discard_quarantined_media(client.app.state.settings, item)
    detail = client.get(f"/api/admin/v1/calls/{ctx['call_id']}").json()
    assert detail["session_audit"]["state"] == "deleted"
    assert [(event["type"], event["offset_ms"]) for event in detail["session_audit"]["events"]] == [("answered", 0), ("ended", 100)]


def test_policy_is_off_signed_and_requires_capability(client):
    response = client.get("/api/admin/v1/audit-recording-settings").json()
    assert response["document"]["enabled"] is False
    public = Ed25519PublicKey.from_public_bytes(base64.b64decode(client.app.state.signer.public_key_b64()))
    public.verify(base64.b64decode(response["signature_b64"]), canonical_json_bytes(response["document"]))
    assert client.put("/api/admin/v1/audit-recording-settings", json={"enabled": True}).status_code == 409


def test_builtin_session_end_to_end_playback_retention_and_legacy_counts(client):
    ctx = context(client)
    identity, segment = upload(client, ctx)
    detail = client.get(f"/api/admin/v1/calls/{ctx['call_id']}").json()
    audit = detail["session_audit"]
    assert detail["revision_id"] is None
    assert detail["recording_count"] == 0
    assert audit["state"] == "ready"
    assert audit["events"][1]["offset_ms"] == 100
    assert client.get("/api/admin/v1/recordings").json()["total"] == 0
    assert client.get("/api/admin/v1/recordings?kind=session_audit").json()["total"] == 1
    audio = client.get(audit["recording"]["playback_url"], headers={"Range": "bytes=0-99"})
    assert audio.status_code == 206 and len(audio.content) == 100
    assert client.patch(f"/api/admin/v1/recordings/{ctx['recording_id']}/listened", json={"listened": True}).status_code == 200
    assert client.delete(f"/api/admin/v1/recordings/{ctx['recording_id']}").status_code == 204
    assert client.get(f"/api/admin/v1/calls/{ctx['call_id']}").json()["session_audit"]["state"] == "deleted"
    assert client.post("/api/device/v1/session-recordings", headers=ctx["headers"], json=identity).json()["acknowledged"] is True


def test_partial_audio_is_playable_and_original_conversation_api_rejects_it(client):
    ctx = context(client, partial=True)
    upload(client, ctx)
    detail = client.get(f"/api/admin/v1/calls/{ctx['call_id']}").json()
    assert detail["session_audit"]["partial"] is True
    assert client.get(f"/api/device/v1/conversation-recordings/{ctx['recording_id']}", headers=ctx["headers"]).status_code == 409


def test_report_cannot_be_replaced_or_assigned_to_stock_dialer(client):
    ctx = context(client)
    changed = {**ctx["call"], "session_audit": {**ctx["report"], "duration_ms": 200}}
    assert client.post("/api/device/v1/events:batch", headers=ctx["headers"], json={"calls": [changed]}).status_code == 409
    changed = {**ctx["call"], "call_id": str(uuid4()), "result": "STOCK_DIALER"}
    assert client.post("/api/device/v1/events:batch", headers=ctx["headers"], json={"calls": [changed]}).status_code == 422
    stock = {**changed, "session_audit": None}
    assert client.post("/api/device/v1/events:batch", headers=ctx["headers"], json={"calls": [stock]}).status_code == 200
    late = {**changed, "result": "REMOTE_HANGUP"}
    assert client.post("/api/device/v1/events:batch", headers=ctx["headers"], json={"calls": [late]}).status_code == 422


def test_disabled_policy_remains_valid_for_previously_recorded_session(client):
    ctx = context(client)
    assert client.put("/api/admin/v1/audit-recording-settings", json={"enabled": False}).status_code == 200
    upload(client, ctx)


def test_audit_capacity_failure_does_not_reserve_or_mutate_calls(client):
    ctx = context(client)
    client.app.state.settings.audit_recording_quota_bytes = 100
    identity = {key: ctx[key] for key in ("recording_id", "call_id", "policy_version", "captured_at")}
    root = "/api/device/v1/session-recordings"
    assert client.post(root, headers=ctx["headers"], json=identity).status_code == 201
    wav = conversation_wav(100)
    segment = {**identity, "kind": "session_audit", "segment_index": 0, "duration_ms": 100,
               "stop_reason": "caller_hangup", "expected_size_bytes": len(wav), "source_sha256": hashlib.sha256(wav).hexdigest()}
    assert client.post(f"{root}/{ctx['recording_id']}/segments/0", headers=ctx["headers"], json=segment).status_code == 507
    assert client.get(f"/api/admin/v1/calls/{ctx['call_id']}").json()["result"] == "REMOTE_HANGUP"
