from datetime import timedelta
import hashlib
from unittest.mock import patch

from app.continuous_recording_worker import process_next
from app.models import ContinuousRecording, Recording, RecordingUpload, utcnow
from app.recording_service import cleanup_abandoned_uploads
from test_session_audit import context

ROOT = "/api/device/v1/continuous-recordings"


def create(client, duration=100, partial=False):
    ctx = context(client, duration_ms=duration, partial=partial)
    pcm = bytes([0, 1, 0, 2]) * (duration * 48)
    body = {"format_version": 1, "recording_id": ctx["recording_id"], "call_id": ctx["call_id"],
            "kind": "session_audit", "policy_version": ctx["policy_version"], "captured_at": ctx["captured_at"],
            "source_format": "pcm_s16le_48000_stereo", "frames": duration * 48, "duration_ms": duration,
            "expected_size_bytes": len(pcm), "source_sha256": hashlib.sha256(pcm).hexdigest(),
            "partial": partial, "stop_reason": "interrupted" if partial else "caller_hangup"}
    response = client.post(ROOT, headers=ctx["headers"], json=body)
    assert response.status_code == 201, response.text
    return ctx, body, pcm, ROOT + "/" + ctx["recording_id"]


def test_resume_idempotence_background_completion_and_seek(client):
    ctx, body, pcm, url = create(client, duration=6001, partial=True)
    assert client.post(ROOT, headers=ctx["headers"], json=body).json()["upload_offset"] == 0
    assert client.post(ROOT, headers=ctx["headers"], json={**body, "source_sha256": "0" * 64}).status_code == 409
    assert client.post(url + "/complete", headers=ctx["headers"]).status_code == 409
    for offset in range(0, len(pcm), 1024 * 1024):
        chunk = pcm[offset:offset + 1024 * 1024]
        headers = {**ctx["headers"], "Upload-Offset": str(offset)}
        for _ in range(2):
            result = client.put(url + "/content", headers=headers, content=chunk)
            assert result.status_code == 200, result.text
            assert result.json()["upload_offset"] == offset + len(chunk)
    completed = client.post(url + "/complete", headers=ctx["headers"])
    assert completed.status_code == 202 and not completed.json()["acknowledged"]
    assert client.post(url + "/complete", headers=ctx["headers"]).status_code == 202
    process_next(client.app)
    status = client.get(url, headers=ctx["headers"]).json()
    assert status["status"] == "ready" and status["acknowledged"] and status["partial"], status
    audio = client.get(f"/api/admin/v1/recordings/{ctx['recording_id']}/audio", headers={"Range": "bytes=10-109"})
    assert audio.status_code == 206 and len(audio.content) == 100
    with client.app.state.database.session() as session:
        rec = session.get(Recording, ctx["recording_id"])
        storage = rec.media_storage_name
        assert session.get(RecordingUpload, rec.id) is None
        assert session.get(ContinuousRecording, rec.id).attempts == 1
    assert client.post(url + "/complete", headers=ctx["headers"]).json()["acknowledged"]
    process_next(client.app)
    with client.app.state.database.session() as session:
        assert session.get(Recording, ctx["recording_id"]).media_storage_name == storage


def test_corruption_is_not_acknowledged_or_silently_discarded(client):
    ctx, body, pcm, url = create(client)
    bad = pcm[:-1] + b"\x55"
    assert client.put(url + "/content", headers={**ctx["headers"], "Upload-Offset": "0"}, content=bad).status_code == 200
    assert client.post(url + "/complete", headers=ctx["headers"]).status_code == 202
    process_next(client.app)
    result = client.get(url, headers=ctx["headers"]).json()
    assert result["processing_state"] == "invalid" and not result["acknowledged"]
    assert "checksum" in result["error"]
    with client.app.state.database.session() as session:
        assert session.get(RecordingUpload, ctx["recording_id"]) is not None


def test_capacity_fault_and_restart_preserve_queued_audio(client):
    ctx, _, pcm, url = create(client)
    client.put(url + "/content", headers={**ctx["headers"], "Upload-Offset": "0"}, content=pcm)
    client.post(url + "/complete", headers=ctx["headers"])
    usage = type("Usage", (), {"free": 0})()
    with patch("app.continuous_recording_worker.shutil.disk_usage", return_value=usage):
        process_next(client.app)
    result = client.get(url, headers=ctx["headers"]).json()
    assert result["processing_state"] == "retrying" and "workspace" in result["error"]
    with client.app.state.database.session() as session:
        receipt = session.get(ContinuousRecording, ctx["recording_id"])
        receipt.state = "processing"; receipt.retry_at = None  # Simulate death while processing.
        upload = session.get(RecordingUpload, receipt.recording_id)
        upload.updated_at = utcnow() - timedelta(days=10)
        session.commit()
        assert cleanup_abandoned_uploads(session, client.app.state.settings) == []
        session.commit()
    process_next(client.app)
    assert client.get(url, headers=ctx["headers"]).json()["acknowledged"]


def test_pcm_metadata_and_call_coverage_must_agree(client):
    ctx = context(client)
    body = {"recording_id": ctx["recording_id"], "call_id": ctx["call_id"], "kind": "session_audit",
            "policy_version": ctx["policy_version"], "captured_at": ctx["captured_at"],
            "frames": 4800, "duration_ms": 100, "expected_size_bytes": 19204, "source_sha256": "0" * 64,
            "partial": False, "stop_reason": "caller_hangup"}
    assert client.post(ROOT, headers=ctx["headers"], json=body).status_code == 422
    body.update(expected_size_bytes=19200, stop_reason="interrupted")
    assert client.post(ROOT, headers=ctx["headers"], json=body).status_code == 422


def test_rejected_upload_can_restart_without_changing_identity(client):
    ctx, body, pcm, url = create(client)
    bad = pcm[:-1] + b'\x77'
    client.put(url + '/content', headers={**ctx['headers'], 'Upload-Offset': '0'}, content=bad)
    client.post(url + '/complete', headers=ctx['headers'])
    process_next(client.app)
    reset = client.post(url + '/reset', headers=ctx['headers'])
    assert reset.status_code == 200 and reset.json()['upload_offset'] == 0, reset.text
    client.put(url + '/content', headers={**ctx['headers'], 'Upload-Offset': '0'}, content=pcm)
    client.post(url + '/complete', headers=ctx['headers'])
    process_next(client.app)
    assert client.get(url, headers=ctx['headers']).json()['acknowledged']
    assert client.post(url + '/reset', headers=ctx['headers']).status_code == 409
    legacy = '/api/device/v1/session-recordings/' + ctx['recording_id']
    assert client.get(legacy, headers=ctx['headers']).status_code == 409
    assert client.post(legacy + '/complete', headers=ctx['headers'], json={'segment_count': 1}).status_code == 409


def test_abandoned_legacy_prefix_requires_explicit_evidence_and_remains_partial(client):
    from test_conversation_recordings_api import provision_conversation_context, create_logical_recording, create_upload_verify_segment, conversation_wav
    ctx = provision_conversation_context(client)
    create_logical_recording(client, ctx)
    source = conversation_wav()
    create_upload_verify_segment(client, ctx, 0, source, 'segment_boundary')
    url = '/api/admin/v1/recordings/' + ctx['recording_id'] + '/recover-partial'
    body = {'device_source_confirmed_absent': False,
            'evidence': 'Tablet was reconciled while idle: no active writer, native receipt or encrypted spool for this identity.',
            'verified_segment_hashes': [hashlib.sha256(source).hexdigest()]}
    assert client.post(url, json=body).status_code == 409
    body['device_source_confirmed_absent'] = True
    assert client.post(url, json={**body, 'verified_segment_hashes': ['0' * 64]}).status_code == 409
    result = client.post(url, json=body)
    assert result.status_code == 200, result.text
    assert result.json()['partial'] and result.json()['duration_ms'] == 100 and result.json()['status'] == 'ready'
    assert client.get(result.json()['playback_url']).status_code == 200
    assert client.post(url, json=body).json()['id'] == ctx['recording_id']


def test_continuous_operator_audio_requires_connection_and_notifies_once(client):
    from test_conversation_recordings_api import provision_conversation_context, EXTERNAL_BLOCK
    ctx = provision_conversation_context(client)
    pcm = bytes([0, 1, 0, 2]) * 4800
    body = {"recording_id": ctx["recording_id"], "call_id": ctx["call_id"], "kind": "conversation",
            "revision_id": 404, "block_id": EXTERNAL_BLOCK, "captured_at": ctx["captured_at"],
            "frames": 4800, "duration_ms": 100, "expected_size_bytes": len(pcm),
            "source_sha256": hashlib.sha256(pcm).hexdigest(), "partial": False, "stop_reason": "operator_hangup"}
    result = client.post(ROOT, headers=ctx["headers"], json=body)
    assert result.status_code == 201, result.text
    url = ROOT + "/" + ctx["recording_id"]
    client.put(url + '/content', headers={**ctx['headers'], 'Upload-Offset': '0'}, content=pcm)
    client.post(url + '/complete', headers=ctx['headers'])
    with patch('app.continuous_recording_worker.dispatch_ntfy') as notify:
        process_next(client.app)
        process_next(client.app)
        notify.assert_called_once()
        assert notify.call_args.args[1] == 'conversation_ready'
    assert client.get(url, headers=ctx['headers']).json()['acknowledged']
    audio = client.get(f"/api/admin/v1/recordings/{ctx['recording_id']}/audio")
    assert audio.status_code == 200 and len(audio.content) > 0
