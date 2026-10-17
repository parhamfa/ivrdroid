import hashlib
from datetime import timedelta
from uuid import uuid4

from app.continuous_recording_worker import process_next
from app.models import CallRecord, RecordingUpload
from test_session_audit import context

ROOT = "/api/device/v1/continuous-recordings"


def manifest(ctx, pcm, *, version=2, capture_end=1100, finalized=9000):
    body = {"format_version": version, "recording_id": ctx["recording_id"], "call_id": ctx["call_id"],
            "kind": "session_audit", "policy_version": ctx["policy_version"], "captured_at": ctx["captured_at"],
            "frames": len(pcm) // 4, "duration_ms": 100, "expected_size_bytes": len(pcm),
            "source_sha256": hashlib.sha256(pcm).hexdigest(), "partial": False, "stop_reason": "caller_hangup"}
    if version == 2:
        body["capture_receipt"] = {"version": 2, "boot_id": str(uuid4()), "started_elapsed_ms": 1000,
                                   "ended_elapsed_ms": capture_end, "finalized_elapsed_ms": finalized}
    return body


def transfer(client, ctx, body, pcm):
    response = client.post(ROOT, headers=ctx["headers"], json=body)
    assert response.status_code == 201, response.text
    url = ROOT + "/" + ctx["recording_id"]
    assert client.put(url + "/content", headers={**ctx["headers"], "Upload-Offset": "0"}, content=pcm).status_code == 200
    assert client.post(url + "/complete", headers=ctx["headers"]).status_code == 202
    process_next(client.app)
    return client.get(url, headers=ctx["headers"]).json()


def test_finalization_delay_does_not_extend_capture(client):
    ctx = context(client, duration_ms=100)
    pcm = b"\0\1\0\2" * 4800
    status = transfer(client, ctx, manifest(ctx, pcm), pcm)
    assert status["acknowledged"] and status["format_version"] == 2
    detail = client.get("/api/admin/v1/calls/" + ctx["call_id"]).json()
    assert detail["duration_seconds"] == 1
    assert detail["session_audit"]["duration_ms"] == 100


def test_unexplained_capture_discrepancy_retains_source_without_rewriting_call(client):
    ctx = context(client, duration_ms=100)
    pcm = b"\0\1\0\2" * 4800
    status = transfer(client, ctx, manifest(ctx, pcm, capture_end=8000), pcm)
    assert status["processing_state"] == "needs_attention" and not status["acknowledged"]
    with client.app.state.database.session() as session:
        assert session.get(RecordingUpload, ctx["recording_id"]).upload_offset == len(pcm)
        assert session.get(CallRecord, ctx["call_id"]).duration_seconds == 1
    detail = client.get("/api/admin/v1/calls/" + ctx["call_id"]).json()
    assert detail["session_audit"]["recording"]["processing_state"] == "needs_attention"


def test_legacy_timing_discrepancy_upload_is_preserved_for_review(client):
    ctx = context(client, duration_ms=100)
    pcm = b"\0\1\0\2" * 4800
    with client.app.state.database.session() as session:
        call = session.get(CallRecord, ctx["call_id"])
        call.started_at -= timedelta(seconds=20)
        session.commit()
    status = transfer(client, ctx, manifest(ctx, pcm, version=1), pcm)
    assert status["processing_state"] == "needs_attention"
    assert not status["acknowledged"]


def test_pending_counter_includes_whole_session_upload(client):
    ctx = context(client, duration_ms=100)
    call = next(item for item in client.get("/api/admin/v1/calls").json() if item["id"] == ctx["call_id"])
    assert call["pending_recording_count"] == 0
    assert call["pending_session_audio_count"] == 1
