from copy import deepcopy
from datetime import datetime, timezone
from uuid import uuid4

from sqlalchemy import select

from app.crypto import digest_device_token
from app.models import CallEventReceipt, CallRecord, Device
from test_ntfy import enable_ntfy


def fixture(client):
    token = "recovery-test-" + "x" * 40
    with client.app.state.database.session() as session:
        session.add(Device(display_name="test", token_hash=digest_device_token(token)))
        session.commit()
    return {"Authorization": f"Bearer {token}"}, {
        "call_id": str(uuid4()), "started_at": "2026-09-19T10:00:00Z", "policy_decision": "IVR_HANDLED",
        "result": "IN_PROGRESS", "duration_seconds": 0, "events": []}


def event(status="NOT_CONNECTED", at="2026-09-19T10:00:15Z"):
    return {"event": "external_call", "occurred_at": at, "status": status,
            "block_id": "22222222-2222-4222-8222-222222222222", "reason": "ANSWER_TIMEOUT"}


def test_late_events_survive_acknowledged_parent_and_notify_once(client, monkeypatch):
    sent = []
    monkeypatch.setattr("app.ntfy.deliver_notification", sent.append)
    enable_ntfy(client)
    headers, call = fixture(client)
    parent = lambda: client.post("/api/device/v1/events:batch", headers=headers, json={"calls": [call]})
    assert parent().status_code == 200
    call.update(result="REMOTE_HANGUP", duration_seconds=30, ended_at="2026-09-19T10:00:30Z", cleanup_status="failed")
    assert parent().status_code == 200
    payload = {"calls": [{"call_id": call["call_id"], "events": [event()]}]}
    for _ in range(3):
        assert client.post("/api/device/v1/call-events:batch", headers=headers, json=payload).status_code == 200
    # A stale parent snapshot cannot delete late events or overwrite the disconnect.
    assert parent().status_code == 200
    details = client.get("/api/admin/v1/calls/" + call["call_id"]).json()
    assert details["result"] == "REMOTE_HANGUP" and details["cleanup_status"] == "failed"
    assert len(details["events"]) == 1
    assert [item.title for item in sent] == ["External call failed"]


def test_orphan_events_merge_when_parent_arrives_and_duplicate_batch_items_are_safe(client, monkeypatch):
    sent = []
    monkeypatch.setattr("app.ntfy.deliver_notification", sent.append)
    enable_ntfy(client)
    headers, call = fixture(client)
    item = {"call_id": call["call_id"], "events": [event()]}
    assert client.post("/api/device/v1/call-events:batch", headers=headers, json={"calls": [item, item]}).status_code == 200
    assert sent == []
    call.update(result="REMOTE_HANGUP", duration_seconds=30)
    assert client.post("/api/device/v1/events:batch", headers=headers, json={"calls": [call]}).status_code == 200
    alternate = deepcopy(item)
    alternate["events"][0]["occurred_at"] = "2026-09-19T13:00:15+03:00"
    assert client.post("/api/device/v1/call-events:batch", headers=headers, json={"calls": [alternate]}).status_code == 200
    with client.app.state.database.session() as session:
        assert len(session.scalars(select(CallEventReceipt)).all()) == 1
        assert len(session.get(CallRecord, call["call_id"]).events) == 1
    assert len(sent) == 1


def test_incomplete_details_can_be_completed_without_rewriting_known_outcome(client):
    headers, call = fixture(client)
    call.update(result="END_DETAILS_UNAVAILABLE", cleanup_status="pending", ended_at="2026-09-19T10:00:30Z", duration_seconds=30)
    send = lambda: client.post("/api/device/v1/events:batch", headers=headers, json={"calls": [call]})
    assert send().status_code == 200
    call.update(result="REMOTE_HANGUP", cleanup_status="complete")
    assert send().status_code == 200
    call.update(result="RECOVERED_AND_ENDED", duration_seconds=100)
    assert send().status_code == 200  # Compatible retry accepted, proven terminal evidence kept.
    with client.app.state.database.session() as session:
        stored = session.get(CallRecord, call["call_id"])
        assert stored.result == "REMOTE_HANGUP" and stored.duration_seconds == 30
        assert stored.ended_at.replace(tzinfo=timezone.utc) == datetime(2026, 9, 19, 10, 0, 30, tzinfo=timezone.utc)


def test_recording_failure_does_not_become_a_call_failure_alert(client, monkeypatch):
    sent = []
    monkeypatch.setattr("app.ntfy.deliver_notification", sent.append)
    enable_ntfy(client)
    headers, call = fixture(client)
    call.update(result="REMOTE_HANGUP", events=[{**event("RECORDING_FAILURE"), "event": "recording_failure", "reason": "WRITER_FAILURE"}])
    assert client.post("/api/device/v1/events:batch", headers=headers, json={"calls": [call]}).status_code == 200
    assert sent == []
