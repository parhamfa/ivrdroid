from __future__ import annotations

from datetime import timedelta

from sqlalchemy import select

from app.crypto import digest_device_token
from app.models import Device, utcnow
from app.ntfy import NtfyPayload, classify_call_notifications, dispatch_ntfy, evaluate_presence, evaluate_storage
from app.schemas import default_ntfy_events


def enable_ntfy(client, **changes) -> dict:
    current = client.get("/api/admin/v1/ntfy-settings").json()
    body = {
        "enabled": True,
        "server_url": "https://ntfy.sh",
        "topic": "ivrdroid-test",
        "token": "tk_test_token",
        "events": current["events"],
        **changes,
    }
    response = client.put("/api/admin/v1/ntfy-settings", json=body)
    assert response.status_code == 200, response.text
    return response.json()


def add_device(client, *, minutes_ago: int, call_state: str = "idle") -> Device:
    with client.app.state.database.session() as session:
        device = Device(
            display_name="SM-T585",
            token_hash=digest_device_token("device-token-" + "x" * 40),
            status={"call_state": call_state, "voicemail_spool_bytes": 0, "conversation_spool_bytes": 0},
            last_seen_at=utcnow() - timedelta(minutes=minutes_ago),
        )
        session.add(device)
        session.commit()
        return device


def test_ntfy_settings_round_trip_hides_token(client) -> None:
    created = client.get("/api/admin/v1/ntfy-settings")
    assert created.status_code == 200
    body = created.json()
    assert body["enabled"] is False
    assert body["token_configured"] is False
    assert "token" not in body
    assert body["events"]["voicemail_ready"]["enabled"] is True
    assert body["events"]["ivr_session_completed"]["enabled"] is False

    saved = enable_ntfy(client)
    assert saved["enabled"] is True
    assert saved["topic"] == "ivrdroid-test"
    assert saved["token_configured"] is True
    assert "token" not in saved

    again = client.get("/api/admin/v1/ntfy-settings").json()
    assert again["token_configured"] is True
    kept = client.put(
        "/api/admin/v1/ntfy-settings",
        json={
            "enabled": True,
            "server_url": "https://ntfy.sh",
            "topic": "ivrdroid-test",
            "events": again["events"],
        },
    )
    assert kept.status_code == 200
    assert kept.json()["token_configured"] is True


def test_ntfy_rejects_insecure_server_and_bad_timeouts(client) -> None:
    events = default_ntfy_events().model_dump(mode="json")
    http = client.put(
        "/api/admin/v1/ntfy-settings",
        json={"enabled": True, "server_url": "http://ntfy.sh", "topic": "topic", "events": events},
    )
    assert http.status_code == 422
    creds = client.put(
        "/api/admin/v1/ntfy-settings",
        json={"enabled": True, "server_url": "https://user:pass@ntfy.sh", "topic": "topic", "events": events},
    )
    assert creds.status_code == 422
    events["tablet_offline"]["idle_timeout_minutes"] = 40
    events["tablet_offline"]["in_call_timeout_minutes"] = 20
    timeouts = client.put(
        "/api/admin/v1/ntfy-settings",
        json={"enabled": True, "server_url": "https://ntfy.sh", "topic": "topic", "events": events},
    )
    assert timeouts.status_code == 422


def test_ntfy_test_and_audit(client, monkeypatch) -> None:
    sent: list[NtfyPayload] = []
    monkeypatch.setattr("app.ntfy.deliver_notification", sent.append)
    assert client.post("/api/admin/v1/ntfy-settings/test").status_code == 400
    enable_ntfy(client)
    response = client.post("/api/admin/v1/ntfy-settings/test")
    assert response.status_code == 200, response.text
    assert sent[0].title == "IVRdroid test"
    assert sent[0].token == "tk_test_token"
    audit = client.get("/api/admin/v1/audit").json()
    assert any(item["action"] == "ntfy.test_sent" for item in audit)
    assert any(item["action"] == "ntfy.settings_updated" for item in audit)


def test_classify_call_notifications() -> None:
    assert classify_call_notifications("IVR_HANDLED", "IN_PROGRESS", []) == []
    assert classify_call_notifications("IVR_HANDLED", "HELPER_BUSY", []) == [("ivr_session_failed", None)]
    assert classify_call_notifications("IVR_HANDLED", "SESSION_COMPLETE", []) == [("ivr_session_completed", None)]
    assert classify_call_notifications("NOT_ALLOWLISTED", "STOCK_DIALER", []) == [("stock_dialer_routing", None)]
    assert classify_call_notifications(
        "IVR_HANDLED",
        "SESSION_COMPLETE",
        [{"event": "external_call", "status": "SYSTEM_FAILURE"}],
    ) == [
        ("ivr_session_completed", None),
        ("external_call_failed", {"external_status": "SYSTEM_FAILURE"}),
    ]


def test_call_failure_notifies_once(client, monkeypatch) -> None:
    sent: list[NtfyPayload] = []
    monkeypatch.setattr("app.ntfy.deliver_notification", sent.append)
    enable_ntfy(client)
    token = "device-token-" + "x" * 40
    with client.app.state.database.session() as session:
        session.add(
            Device(
                display_name="SM-T585",
                token_hash=digest_device_token(token),
                status={"call_state": "idle"},
                last_seen_at=utcnow(),
            ),
        )
        session.commit()
    headers = {"Authorization": f"Bearer {token}"}
    payload = {
        "call_id": "call-failure-1",
        "started_at": "2026-08-17T10:00:00Z",
        "caller": "+15551234567",
        "policy_decision": "IVR_HANDLED",
        "revision_id": None,
        "menu_path": [],
        "result": "IN_PROGRESS",
        "duration_seconds": 0,
        "events": [],
    }
    assert client.post("/api/device/v1/events:batch", headers=headers, json={"calls": [payload]}).status_code == 200
    assert sent == []
    payload["result"] = "HELPER_BUSY"
    payload["duration_seconds"] = 2
    assert client.post("/api/device/v1/events:batch", headers=headers, json={"calls": [payload]}).status_code == 200
    assert [item.title for item in sent] == ["IVR session failed"]
    assert "4567" in sent[0].message
    assert client.post("/api/device/v1/events:batch", headers=headers, json={"calls": [payload]}).status_code == 200
    assert len(sent) == 1


def test_completed_session_is_opt_in(client, monkeypatch) -> None:
    sent: list[NtfyPayload] = []
    monkeypatch.setattr("app.ntfy.deliver_notification", sent.append)
    current = enable_ntfy(client)
    token = "device-token-" + "y" * 40
    with client.app.state.database.session() as session:
        session.add(
            Device(
                display_name="SM-T585",
                token_hash=digest_device_token(token),
                status={"call_state": "idle"},
                last_seen_at=utcnow(),
            ),
        )
        session.commit()
    headers = {"Authorization": f"Bearer {token}"}
    body = {
        "calls": [
            {
                "call_id": "call-complete-1",
                "started_at": "2026-08-17T10:00:00Z",
                "caller": "+15551234567",
                "policy_decision": "IVR_HANDLED",
                "menu_path": [],
                "result": "SESSION_COMPLETE",
                "duration_seconds": 12,
                "events": [],
            },
        ],
    }
    assert client.post("/api/device/v1/events:batch", headers=headers, json=body).status_code == 200
    assert sent == []
    current["events"]["ivr_session_completed"]["enabled"] = True
    enable_ntfy(client, events=current["events"], token=None)
    body["calls"][0]["call_id"] = "call-complete-2"
    assert client.post("/api/device/v1/events:batch", headers=headers, json=body).status_code == 200
    assert [item.title for item in sent] == ["IVR session completed"]


def test_external_call_subtoggle(client, monkeypatch) -> None:
    sent: list[NtfyPayload] = []
    monkeypatch.setattr("app.ntfy.deliver_notification", sent.append)
    current = enable_ntfy(client)
    current["events"]["external_call_failed"]["not_connected"] = False
    enable_ntfy(client, events=current["events"], token=None)
    token = "device-token-" + "z" * 40
    with client.app.state.database.session() as session:
        session.add(
            Device(
                display_name="SM-T585",
                token_hash=digest_device_token(token),
                status={"call_state": "idle"},
                last_seen_at=utcnow(),
            ),
        )
        session.commit()
    headers = {"Authorization": f"Bearer {token}"}
    block = "11111111-1111-4111-8111-111111111111"
    body = {
        "calls": [
            {
                "call_id": "call-external-1",
                "started_at": "2026-08-17T10:00:00Z",
                "caller": "+15551234567",
                "policy_decision": "IVR_HANDLED",
                "menu_path": [],
                "result": "SESSION_COMPLETE",
                "duration_seconds": 20,
                "events": [
                    {
                        "event": "external_call",
                        "occurred_at": "2026-08-17T10:00:05Z",
                        "status": "NOT_CONNECTED",
                        "block_id": block,
                        "reason": "BUSY",
                    },
                ],
            },
        ],
    }
    assert client.post("/api/device/v1/events:batch", headers=headers, json=body).status_code == 200
    assert sent == []
    body["calls"][0]["call_id"] = "call-external-2"
    body["calls"][0]["events"][0]["status"] = "SYSTEM_FAILURE"
    body["calls"][0]["events"][0]["reason"] = "MERGE_FAILED"
    assert client.post("/api/device/v1/events:batch", headers=headers, json=body).status_code == 200
    assert [item.title for item in sent] == ["External call failed"]


def test_tablet_offline_timeouts_and_recovery(client, monkeypatch) -> None:
    sent: list[NtfyPayload] = []
    monkeypatch.setattr("app.ntfy.deliver_notification", sent.append)
    enable_ntfy(client)
    add_device(client, minutes_ago=10, call_state="idle")
    evaluate_presence(client.app)
    assert sent == []
    with client.app.state.database.session() as session:
        device = session.scalar(select(Device))
        device.last_seen_at = utcnow() - timedelta(minutes=16)
        session.commit()
    evaluate_presence(client.app)
    assert [item.title for item in sent] == ["Tablet offline"]
    evaluate_presence(client.app)
    assert len(sent) == 1
    with client.app.state.database.session() as session:
        device = session.scalar(select(Device))
        device.last_seen_at = utcnow()
        session.commit()
    evaluate_presence(client.app)
    assert [item.title for item in sent] == ["Tablet offline", "Tablet recovered"]


def test_in_call_timeout_is_longer_and_recovery_can_be_disabled(client, monkeypatch) -> None:
    sent: list[NtfyPayload] = []
    monkeypatch.setattr("app.ntfy.deliver_notification", sent.append)
    current = enable_ntfy(client)
    current["events"]["tablet_offline"]["notify_when_recovered"] = False
    enable_ntfy(client, events=current["events"], token=None)
    add_device(client, minutes_ago=16, call_state="active")
    evaluate_presence(client.app)
    assert sent == []
    with client.app.state.database.session() as session:
        device = session.scalar(select(Device))
        device.last_seen_at = utcnow() - timedelta(minutes=46)
        session.commit()
    evaluate_presence(client.app)
    assert [item.title for item in sent] == ["Tablet offline"]
    with client.app.state.database.session() as session:
        device = session.scalar(select(Device))
        device.last_seen_at = utcnow()
        session.commit()
    evaluate_presence(client.app)
    assert [item.title for item in sent] == ["Tablet offline"]


def test_storage_warn_percent(client, monkeypatch) -> None:
    sent: list[NtfyPayload] = []
    monkeypatch.setattr("app.ntfy.deliver_notification", sent.append)
    current = enable_ntfy(client)
    current["events"]["storage_full"]["warn_at_quota_percent"] = 50
    enable_ntfy(client, events=current["events"], token=None)
    client.app.state.settings.recording_quota_bytes = 1000
    evaluate_storage(client.app)
    assert sent == []
    with client.app.state.database.session() as session:
        device = Device(
            display_name="SM-T585",
            token_hash="b" * 64,
            status={"voicemail_spool_bytes": 512 * 1024 * 1024, "conversation_spool_bytes": 0, "call_state": "idle"},
            last_seen_at=utcnow(),
        )
        session.add(device)
        session.commit()
    evaluate_storage(client.app)
    assert [item.title for item in sent] == ["Recording storage is full"]
    evaluate_storage(client.app)
    assert len(sent) == 1


def test_revision_activation_failed(client, monkeypatch) -> None:
    sent: list[NtfyPayload] = []
    monkeypatch.setattr("app.ntfy.deliver_notification", sent.append)
    enable_ntfy(client)
    token = "device-token-" + "w" * 40
    with client.app.state.database.session() as session:
        from app.models import Revision

        revision = Revision(
            schema_version=4,
            manifest_encrypted=client.app.state.cipher.encrypt("{}"),
            manifest_sha256="a" * 64,
            signature_b64="sig",
            published_by="owner@example.com",
        )
        session.add(revision)
        session.flush()
        device = Device(
            display_name="SM-T585",
            token_hash=digest_device_token(token),
            status={"call_state": "idle"},
            desired_revision_id=revision.id,
            last_seen_at=utcnow(),
        )
        session.add(device)
        session.commit()
        revision_id = revision.id
    response = client.post(
        f"/api/device/v1/revisions/{revision_id}/ack",
        headers={"Authorization": f"Bearer {token}"},
        json={"state": "failed", "error": "REVISION_REJECTED"},
    )
    assert response.status_code == 204
    assert [item.title for item in sent] == ["Revision activation failed"]
    assert "REVISION_REJECTED" in sent[0].message


def test_voicemail_ready_notifies_once(client, monkeypatch) -> None:
    from test_recordings_api import caller_wav, create_and_upload, provision_recording_context

    sent: list[NtfyPayload] = []
    monkeypatch.setattr("app.ntfy.deliver_notification", sent.append)
    enable_ntfy(client)
    context = provision_recording_context(client)
    create_and_upload(client, context, caller_wav())
    complete = client.post(
        f"/api/device/v1/recordings/{context['recording_id']}/complete",
        headers=context["headers"],
    )
    assert complete.status_code == 200, complete.text
    assert [item.title for item in sent] == ["Voicemail ready"]
    assert "4567" in sent[0].message
    assert "+15551234567" not in sent[0].message
    assert context["recording_id"] in (sent[0].click or "")
    replay = client.post(
        f"/api/device/v1/recordings/{context['recording_id']}/complete",
        headers=context["headers"],
    )
    assert replay.status_code == 200
    assert len(sent) == 1


def test_masked_caller_can_be_omitted(client, monkeypatch) -> None:
    sent: list[NtfyPayload] = []
    monkeypatch.setattr("app.ntfy.deliver_notification", sent.append)
    current = enable_ntfy(client)
    current["events"]["voicemail_ready"]["include_masked_caller"] = False
    enable_ntfy(client, events=current["events"], token=None)
    dispatch_ntfy(
        client.app,
        "voicemail_ready",
        title="Voicemail ready",
        message="A recorded message is ready to play.",
        click_path="/voicemail?recording=abc",
        tags="mailbox_with_mail,ivrdroid",
        caller_masked="••••4567",
    )
    assert sent[0].message == "A recorded message is ready to play."
