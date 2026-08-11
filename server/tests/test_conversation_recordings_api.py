from __future__ import annotations

import hashlib
import io
import json
import wave
from datetime import timedelta
from uuid import uuid4

from app.crypto import digest_device_token
from app.models import (
    CallRecord,
    ConversationRecordingSegment,
    Device,
    Recording,
    Revision,
    utcnow,
)
from app import recording_service


EXTERNAL_BLOCK = "00000000-0000-4000-8000-000000000402"


def conversation_wav(milliseconds: int = 100, sample: int = 0) -> bytes:
    output = io.BytesIO()
    frame = int(sample).to_bytes(2, "little", signed=True) * 2
    with wave.open(output, "wb") as audio:
        audio.setnchannels(2)
        audio.setsampwidth(2)
        audio.setframerate(48_000)
        audio.writeframes(frame * (48 * milliseconds))
    return output.getvalue()


def provision_conversation_context(client) -> dict:
    token = "conversation-device-token-" + "x" * 40
    device_id = str(uuid4())
    call_id = str(uuid4())
    recording_id = str(uuid4())
    started_at = utcnow() - timedelta(seconds=2)
    manifest = {
        "schema_version": 4,
        "revision_id": 404,
        "program": {
            "version": 4,
            "entry_pc": 0,
            "maximum_automated_session_ms": 30_000,
            "instructions": [
                {
                    "pc": 0,
                    "block_id": EXTERNAL_BLOCK,
                    "op": "external_call",
                    "phone_number": "03136644636",
                    "answer_timeout_ms": 30_000,
                    "next_pc": 1,
                    "on_not_connected_pc": 1,
                    "on_system_failure_pc": 1,
                },
                {
                    "pc": 1,
                    "block_id": "00000000-0000-4000-8000-000000000403",
                    "op": "end_call",
                },
            ],
        },
    }
    with client.app.state.database.session() as session:
        revision = Revision(
            id=404,
            schema_version=4,
            manifest_encrypted=client.app.state.cipher.encrypt(
                json.dumps(manifest, sort_keys=True, separators=(",", ":")),
            ),
            manifest_sha256="d" * 64,
            signature_b64="signed-v4",
            published_by="owner@example.com",
        )
        device = Device(
            id=device_id,
            display_name="SM-T585",
            token_hash=digest_device_token(token),
            status={
                "runtime_versions": [1, 2, 3, 4],
                "external_call_control_capable": True,
                "conversation_recording_capable": True,
                "call_control_protocol_version": 1,
            },
            active_revision_id=404,
        )
        call = CallRecord(
            id=call_id,
            device_id=device_id,
            started_at=started_at,
            caller_encrypted=client.app.state.cipher.encrypt("+989121234567"),
            caller_last4="4567",
            policy_decision="Routed to IVR",
            revision_id=404,
            result="SESSION_COMPLETE",
            duration_seconds=30,
            events=[
                {
                    "event": "external_call",
                    "occurred_at": (started_at + timedelta(milliseconds=500)).isoformat(),
                    "status": "CONFERENCED",
                    "block_id": EXTERNAL_BLOCK,
                    "reason": "-",
                },
            ],
        )
        session.add_all([revision, device, call])
        session.commit()
    return {
        "headers": {"Authorization": f"Bearer {token}"},
        "call_id": call_id,
        "recording_id": recording_id,
        "captured_at": (started_at + timedelta(seconds=1)).isoformat(),
    }


def create_logical_recording(client, context: dict) -> dict:
    response = client.post(
        "/api/device/v1/conversation-recordings",
        headers=context["headers"],
        json={
            "recording_id": context["recording_id"],
            "call_id": context["call_id"],
            "revision_id": 404,
            "block_id": EXTERNAL_BLOCK,
            "captured_at": context["captured_at"],
        },
    )
    assert response.status_code == 201, response.text
    return response.json()


def create_upload_verify_segment(
    client,
    context: dict,
    index: int,
    source: bytes,
    stop_reason: str,
    *,
    partial: bool = False,
) -> dict:
    receipt = {
        "kind": "conversation",
        "recording_id": context["recording_id"],
        "call_id": context["call_id"],
        "revision_id": 404,
        "block_id": EXTERNAL_BLOCK,
        "sequence": index,
        "segment_index": index,
        "captured_at": (
            utcnow() - timedelta(seconds=1) + timedelta(milliseconds=index * 100)
        ).isoformat(),
        "duration_ms": 100,
        "stop_reason": stop_reason,
        "expected_size_bytes": len(source),
        "source_sha256": hashlib.sha256(source).hexdigest(),
        "partial": partial,
    }
    created = client.post(
        f"/api/device/v1/conversation-recordings/{context['recording_id']}/segments/{index}",
        headers=context["headers"],
        json=receipt,
    )
    assert created.status_code == 201, created.text
    assert created.json()["id"] == context["recording_id"]
    assert created.json()["segment_index"] == index
    offset = created.json()["upload_offset"]
    while offset < len(source):
        request_offset = offset
        chunk = source[offset : offset + 1024]
        uploaded = client.put(
            f"/api/device/v1/conversation-recordings/{context['recording_id']}/segments/{index}/content",
            headers={
                **context["headers"],
                "Content-Type": "application/offset+octet-stream",
                "Upload-Offset": str(offset),
            },
            content=chunk,
        )
        assert uploaded.status_code == 200, uploaded.text
        offset = uploaded.json()["upload_offset"]
        if request_offset == 0:
            replayed = client.put(
                f"/api/device/v1/conversation-recordings/{context['recording_id']}/segments/{index}/content",
                headers={
                    **context["headers"],
                    "Content-Type": "application/offset+octet-stream",
                    "Upload-Offset": "0",
                },
                content=chunk,
            )
            assert replayed.status_code == 200, replayed.text
            assert replayed.json()["upload_offset"] == offset
    completed = client.post(
        f"/api/device/v1/conversation-recordings/{context['recording_id']}/segments/{index}/complete",
        headers=context["headers"],
    )
    assert completed.status_code == 200, completed.text
    assert completed.json()["status"] == "verified"
    assert completed.json()["acknowledged"] is True
    repeated = client.post(
        f"/api/device/v1/conversation-recordings/{context['recording_id']}/segments/{index}/complete",
        headers=context["headers"],
    )
    assert repeated.status_code == 200
    assert repeated.json() == completed.json()
    return receipt


def test_segmented_conversation_streams_assembles_and_joins_admin_retention(
    client,
    monkeypatch,
) -> None:
    context = provision_conversation_context(client)
    created = create_logical_recording(client, context)
    assert created == {
        "id": context["recording_id"],
        "status": "uploading",
        "segment_count": 0,
        "duration_ms": 0,
        "acknowledged": False,
    }

    first = conversation_wav(sample=100)
    second = conversation_wav(sample=-100)
    create_upload_verify_segment(client, context, 0, first, "segment_boundary")
    create_upload_verify_segment(client, context, 1, second, "operator_hangup")
    with client.app.state.database.session() as session:
        segments = session.query(ConversationRecordingSegment).order_by(
            ConversationRecordingSegment.segment_index,
        ).all()
        upload_paths = [
            client.app.state.settings.recording_root / ".uploads" / item.storage_name
            for item in segments
        ]

    def forbidden_join(*_args, **_kwargs):
        raise AssertionError("conversation assembly used the voicemail all-in-memory join")

    original_wave_open = recording_service.wave.open

    def forbidden_combined_riff(file, mode=None):
        if mode == "wb":
            raise AssertionError("conversation assembly reintroduced RIFF's 4 GiB ceiling")
        return original_wave_open(file, mode)

    monkeypatch.setattr(recording_service, "read_uploaded_wav", forbidden_join)
    monkeypatch.setattr(recording_service.wave, "open", forbidden_combined_riff)
    completed = client.post(
        f"/api/device/v1/conversation-recordings/{context['recording_id']}/complete",
        headers=context["headers"],
        json={"segment_count": 2},
    )
    assert completed.status_code == 200, completed.text
    assert completed.json()["status"] == "ready"
    assert completed.json()["segment_count"] == 2
    assert completed.json()["duration_ms"] == 200
    assert completed.json()["acknowledged"] is True
    assert all(not path.exists() for path in upload_paths)

    repeated = client.post(
        f"/api/device/v1/conversation-recordings/{context['recording_id']}/complete",
        headers=context["headers"],
        json={"segment_count": 2},
    )
    assert repeated.status_code == 200
    assert repeated.json() == completed.json()

    listing = client.get("/api/admin/v1/recordings").json()
    assert listing["total"] == 1
    item = listing["items"][0]
    assert item["kind"] == "conversation"
    assert item["operator_masked"] == "031-***-4636"
    assert item["segment_count"] == 2
    assert item["stop_reason"] == "operator_hangup"
    assert item["caller_masked"] == "+98-***-4567"
    playback = client.get(item["playback_url"])
    assert playback.status_code == 200
    assert playback.headers["content-type"].startswith("audio/mpeg")
    ranged = client.get(item["playback_url"], headers={"Range": "bytes=0-9"})
    assert ranged.status_code == 206
    assert len(ranged.content) == 10
    assert not list(client.app.state.settings.recording_root.glob(".playback-*"))

    with client.app.state.database.session() as session:
        logical = session.get(Recording, context["recording_id"])
        assert logical.status == "ready"
        assert logical.source_size_bytes == len(first) + len(second)
        assert {
            item.status
            for item in session.query(ConversationRecordingSegment).filter_by(
                recording_id=context["recording_id"],
            )
        } == {"assembled"}


def test_logical_conversation_requires_verified_conference_evidence(client) -> None:
    context = provision_conversation_context(client)
    with client.app.state.database.session() as session:
        call = session.get(CallRecord, context["call_id"])
        call.events = []
        session.commit()

    response = client.post(
        "/api/device/v1/conversation-recordings",
        headers=context["headers"],
        json={
            "recording_id": context["recording_id"],
            "call_id": context["call_id"],
            "revision_id": 404,
            "block_id": EXTERNAL_BLOCK,
            "captured_at": context["captured_at"],
        },
    )
    assert response.status_code == 409
    assert "CONFERENCED" in response.json()["detail"]


def test_conversation_rejects_path_mismatch_and_early_terminal_segment(client) -> None:
    context = provision_conversation_context(client)
    create_logical_recording(client, context)
    source = conversation_wav()
    receipt = {
        "kind": "conversation",
        "recording_id": context["recording_id"],
        "call_id": context["call_id"],
        "revision_id": 404,
        "block_id": EXTERNAL_BLOCK,
        "sequence": 0,
        "segment_index": 0,
        "captured_at": context["captured_at"],
        "duration_ms": 100,
        "stop_reason": "segment_boundary",
        "expected_size_bytes": len(source),
        "source_sha256": hashlib.sha256(source).hexdigest(),
        "partial": False,
    }
    missing_compound_field = client.post(
        f"/api/device/v1/conversation-recordings/{context['recording_id']}/segments/0",
        headers=context["headers"],
        json={key: value for key, value in receipt.items() if key != "call_id"},
    )
    assert missing_compound_field.status_code == 422
    unexpected_field = client.post(
        f"/api/device/v1/conversation-recordings/{context['recording_id']}/segments/0",
        headers=context["headers"],
        json={**receipt, "untrusted_extension": True},
    )
    assert unexpected_field.status_code == 422

    mismatched = client.post(
        f"/api/device/v1/conversation-recordings/{context['recording_id']}/segments/1",
        headers=context["headers"],
        json=receipt,
    )
    assert mismatched.status_code == 409

    create_upload_verify_segment(client, context, 0, source, "caller_hangup")
    create_upload_verify_segment(client, context, 1, source, "operator_hangup")
    rejected = client.post(
        f"/api/device/v1/conversation-recordings/{context['recording_id']}/complete",
        headers=context["headers"],
        json={"segment_count": 2},
    )
    assert rejected.status_code == 409
    assert "Only the final" in rejected.json()["detail"]


def test_recording_failure_is_the_only_partial_segment_reason(client) -> None:
    source = conversation_wav()
    context = provision_conversation_context(client)
    create_logical_recording(client, context)
    invalid = client.post(
        f"/api/device/v1/conversation-recordings/{context['recording_id']}/segments/0",
        headers=context["headers"],
        json={
            "kind": "conversation",
            "recording_id": context["recording_id"],
            "call_id": context["call_id"],
            "revision_id": 404,
            "block_id": EXTERNAL_BLOCK,
            "sequence": 0,
            "segment_index": 0,
            "captured_at": context["captured_at"],
            "duration_ms": 100,
            "stop_reason": "operator_hangup",
            "expected_size_bytes": len(source),
            "source_sha256": hashlib.sha256(source).hexdigest(),
            "partial": True,
        },
    )
    assert invalid.status_code == 422

    create_upload_verify_segment(
        client,
        context,
        0,
        source,
        "recording_failure",
        partial=True,
    )
    completed = client.post(
        f"/api/device/v1/conversation-recordings/{context['recording_id']}/complete",
        headers=context["headers"],
        json={"segment_count": 1},
    )
    assert completed.status_code == 200, completed.text
    assert client.get("/api/admin/v1/recordings").json()["items"][0]["stop_reason"] == (
        "recording_failure"
    )


def test_conversation_uses_the_existing_automatic_retention_pipeline(client) -> None:
    context = provision_conversation_context(client)
    create_logical_recording(client, context)
    create_upload_verify_segment(
        client,
        context,
        0,
        conversation_wav(),
        "caller_hangup",
    )
    completed = client.post(
        f"/api/device/v1/conversation-recordings/{context['recording_id']}/complete",
        headers=context["headers"],
        json={"segment_count": 1},
    )
    assert completed.status_code == 200
    with client.app.state.database.session() as session:
        recording = session.get(Recording, context["recording_id"])
        media_path = client.app.state.settings.recording_root / recording.media_storage_name
        assert media_path.is_file()
        recording.captured_at = utcnow() - timedelta(days=31)
        session.commit()

    settings = client.get("/api/admin/v1/recording-settings")
    assert settings.status_code == 200
    assert not media_path.exists()
    with client.app.state.database.session() as session:
        recording = session.get(Recording, context["recording_id"])
        assert recording.status == "deleted"
        assert recording.deletion_reason == "retention"
