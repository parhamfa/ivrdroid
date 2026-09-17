import base64
from uuid import uuid4

import pytest
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey

from app.crypto import canonical_json_bytes, digest_device_token
from app.models import Device


def test_signed_default_and_versioned_duration(client):
    initial = client.get("/api/admin/v1/call-safety-settings").json()
    assert initial["document"]["maximum_call_duration_seconds"] == 3600
    key = Ed25519PublicKey.from_public_bytes(base64.b64decode(client.app.state.signer.public_key_b64()))
    key.verify(base64.b64decode(initial["signature_b64"]), canonical_json_bytes(initial["document"]))
    changed = client.put("/api/admin/v1/call-safety-settings", json={"maximum_call_duration_minutes": 1440}).json()
    assert changed["document"]["version"] == 1
    assert changed["document"]["maximum_call_duration_seconds"] == 86400
    key.verify(base64.b64decode(changed["signature_b64"]), canonical_json_bytes(changed["document"]))
    repeated = client.put("/api/admin/v1/call-safety-settings", json={"maximum_call_duration_minutes": 1440}).json()
    assert repeated["document"] == changed["document"]
    assert client.put("/api/admin/v1/call-safety-settings", json={"maximum_call_duration_minutes": 1}).json()["document"]["version"] == 2


@pytest.mark.parametrize("minutes", [0, 1441, -1, 1.5, True, "60"])
def test_invalid_duration_rejected(client, minutes):
    assert client.put("/api/admin/v1/call-safety-settings", json={"maximum_call_duration_minutes": minutes}).status_code == 422


def test_acknowledgment_requires_exact_applied_policy(client):
    device_id = str(uuid4())
    token = "call-safety-test-token-" + device_id
    with client.app.state.database.session() as session:
        session.add(Device(id=device_id, display_name="Tablet", token_hash=digest_device_token(token), status={}))
        session.commit()
    status = {"helper_state": "READY", "helper_result": "NONE", "call_state": "idle",
              "local_kill_switch": False, "storage_free_bytes": 1000000}
    body = {"app_version": "test", "helper_version": "test", "status": status}
    headers = {"Authorization": f"Bearer {token}"}
    # Old tablets can still sync but must not be shown as having applied version zero.
    assert client.post("/api/device/v1/sync", headers=headers, json=body).status_code == 200
    assert client.get("/api/admin/v1/call-safety-settings").json()["devices"][0]["applied_version"] is None
    status.update(call_safety_capable=True, call_safety_policy_version=0, maximum_call_duration_seconds=3600)
    assert client.post("/api/device/v1/sync", headers=headers, json=body).status_code == 200
    assert client.get("/api/admin/v1/call-safety-settings").json()["devices"][0]["applied_version"] == 0
    client.put("/api/admin/v1/call-safety-settings", json={"maximum_call_duration_minutes": 10})
    status.update(call_safety_policy_version=1, maximum_call_duration_seconds=3600)
    assert client.post("/api/device/v1/sync", headers=headers, json=body).status_code == 422
    status["maximum_call_duration_seconds"] = 600
    assert client.post("/api/device/v1/sync", headers=headers, json=body).status_code == 200
    assert client.get("/api/admin/v1/call-safety-settings").json()["devices"][0]["maximum_call_duration_seconds"] == 600
