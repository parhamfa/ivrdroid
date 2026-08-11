from __future__ import annotations

import base64
import hashlib
import io
import json
import math
import re
import struct
import subprocess
import wave
from datetime import datetime, timedelta, timezone
from pathlib import Path
from uuid import uuid4

from app.crypto import digest_device_token
from app.models import (
    AuditLog,
    CallRecord,
    Device,
    Recording,
    RecordingUpload,
    Revision,
    utcnow,
)
from app.recording_service import RecordingCipher, RecordingError, transcode_voicemail


FIXTURES = Path(__file__).resolve().parents[2] / "contracts" / "fixtures"
RECORD_BLOCK = "00000000-0000-4000-8000-000000000102"


def caller_wav(milliseconds: int = 100) -> bytes:
    output = io.BytesIO()
    with wave.open(output, "wb") as audio:
        audio.setnchannels(2)
        audio.setsampwidth(2)
        audio.setframerate(48_000)
        audio.writeframes(b"\0\0\0\0" * (48 * milliseconds))
    return output.getvalue()


def quiet_antiphase_caller_wav(milliseconds: int = 2_000) -> bytes:
    """Model the low-level caller path without allowing stereo fold-down cancellation."""
    frames = bytearray()
    for frame in range(48 * milliseconds):
        sample = round(256 * math.sin(2 * math.pi * 440 * frame / 48_000))
        frames.extend(struct.pack("<hh", sample, -sample))
    output = io.BytesIO()
    with wave.open(output, "wb") as audio:
        audio.setnchannels(2)
        audio.setsampwidth(2)
        audio.setframerate(48_000)
        audio.writeframes(frames)
    return output.getvalue()


def measure_loudness(encoded: bytes) -> tuple[float, float]:
    measured = subprocess.run(
        [
            "ffmpeg",
            "-nostdin",
            "-hide_banner",
            "-i",
            "pipe:0",
            "-af",
            "ebur128=peak=true",
            "-f",
            "null",
            "-",
        ],
        input=encoded,
        capture_output=True,
        check=True,
        timeout=30,
    ).stderr.decode("utf-8", "replace")
    integrated = re.findall(r"^\s+I:\s+(-?[0-9.]+) LUFS$", measured, re.MULTILINE)
    peaks = re.findall(r"^\s+Peak:\s+(-?[0-9.]+) dBFS$", measured, re.MULTILINE)
    assert integrated and peaks
    return float(integrated[-1]), float(peaks[-1])


def test_voicemail_transcode_normalizes_quiet_speech_without_stereo_cancellation() -> None:
    encoded = transcode_voicemail(quiet_antiphase_caller_wav())
    integrated_lufs, true_peak_dbfs = measure_loudness(encoded)

    assert -19.5 <= integrated_lufs <= -17.0
    assert -20.0 <= true_peak_dbfs <= -1.0


def provision_recording_context(client, *, captured_at: datetime | None = None) -> dict:
    manifest = json.loads((FIXTURES / "configuration_revision_v3.json").read_text())
    metadata = json.loads((FIXTURES / "configuration_revision_v3.meta.json").read_text())
    token = "device-token-" + "x" * 40
    device_id = str(uuid4())
    call_id = str(uuid4())
    recording_id = str(uuid4())
    with client.app.state.database.session() as session:
        revision = Revision(
            id=85,
            schema_version=3,
            manifest_encrypted=client.app.state.cipher.encrypt(
                json.dumps(manifest, sort_keys=True, separators=(",", ":")),
            ),
            manifest_sha256=metadata["manifest_sha256"],
            signature_b64=metadata["signature_b64"],
            published_by="owner@example.com",
        )
        device = Device(
            id=device_id,
            display_name="SM-T585",
            token_hash=digest_device_token(token),
            status={"runtime_versions": [1, 2, 3], "recording_capable": True},
            active_revision_id=85,
        )
        call = CallRecord(
            id=call_id,
            device_id=device_id,
            started_at=captured_at or utcnow(),
            caller_encrypted=client.app.state.cipher.encrypt("+15551234567"),
            caller_last4="4567",
            policy_decision="Routed to IVR",
            revision_id=85,
            result="SESSION_COMPLETE",
        )
        session.add_all([revision, device, call])
        session.commit()
    return {
        "headers": {"Authorization": f"Bearer {token}"},
        "device_id": device_id,
        "call_id": call_id,
        "recording_id": recording_id,
        "captured_at": (captured_at or utcnow()).isoformat(),
    }


def recording_request(context: dict, source: bytes, **changes) -> dict:
    body = {
        "recording_id": context["recording_id"],
        "call_id": context["call_id"],
        "revision_id": 85,
        "block_id": RECORD_BLOCK,
        "sequence": context.get("sequence", 0),
        "captured_at": context["captured_at"],
        "duration_ms": 100,
        "stop_reason": "finish_key",
        "expected_size_bytes": len(source),
        "source_sha256": hashlib.sha256(source).hexdigest(),
    }
    body.update(changes)
    return body


def create_and_upload(client, context: dict, source: bytes, *, chunk_size: int = 1024) -> dict:
    body = recording_request(context, source)
    created = client.post(
        "/api/device/v1/recordings",
        headers=context["headers"],
        json=body,
    )
    assert created.status_code == 201, created.text
    offset = created.json()["upload_offset"]
    while offset < len(source):
        chunk = source[offset : offset + chunk_size]
        response = client.put(
            f"/api/device/v1/recordings/{context['recording_id']}/content",
            headers={
                **context["headers"],
                "Content-Type": "application/offset+octet-stream",
                "Upload-Offset": str(offset),
            },
            content=chunk,
        )
        assert response.status_code == 200, response.text
        offset = response.json()["upload_offset"]
        assert response.headers["Upload-Offset"] == str(offset)
    return body


def test_resumable_upload_encryption_playback_listened_and_tombstone(client) -> None:
    context = provision_recording_context(client)
    source = caller_wav()
    body = recording_request(context, source)
    assert client.post("/api/device/v1/recordings", json=body).status_code == 401

    created = client.post(
        "/api/device/v1/recordings",
        headers=context["headers"],
        json=body,
    )
    assert created.status_code == 201, created.text
    assert created.json() == {
        "id": context["recording_id"],
        "status": "uploading",
        "upload_offset": 0,
        "expected_size_bytes": len(source),
        "source_sha256": body["source_sha256"],
        "acknowledged": False,
    }
    assert client.patch(
        f"/api/admin/v1/recordings/{context['recording_id']}/listened",
        json={"listened": True},
    ).status_code == 409
    assert client.delete(f"/api/admin/v1/recordings/{context['recording_id']}").status_code == 409
    duplicate = client.post(
        "/api/device/v1/recordings",
        headers=context["headers"],
        json=body,
    )
    assert duplicate.status_code == 201
    changed = client.post(
        "/api/device/v1/recordings",
        headers=context["headers"],
        json={**body, "duration_ms": 101},
    )
    assert changed.status_code == 409

    wrong_offset = client.put(
        f"/api/device/v1/recordings/{context['recording_id']}/content",
        headers={
            **context["headers"],
            "Content-Type": "application/offset+octet-stream",
            "Upload-Offset": "1",
        },
        content=source[:100],
    )
    assert wrong_offset.status_code == 409
    unsupported = client.put(
        f"/api/device/v1/recordings/{context['recording_id']}/content",
        headers={**context["headers"], "Upload-Offset": "0"},
        content=source[:100],
    )
    assert unsupported.status_code == 415

    offset = 0
    for chunk in (source[:777], source[777:]):
        response = client.put(
            f"/api/device/v1/recordings/{context['recording_id']}/content",
            headers={
                **context["headers"],
                "Content-Type": "application/offset+octet-stream",
                "Upload-Offset": str(offset),
            },
            content=chunk,
        )
        assert response.status_code == 200, response.text
        offset += len(chunk)
        assert response.json()["upload_offset"] == offset
    status = client.get(
        f"/api/device/v1/recordings/{context['recording_id']}/upload",
        headers=context["headers"],
    )
    assert status.json()["upload_offset"] == len(source)

    completed = client.post(
        f"/api/device/v1/recordings/{context['recording_id']}/complete",
        headers=context["headers"],
    )
    assert completed.status_code == 200, completed.text
    receipt = completed.json()
    assert receipt["status"] == "ready"
    assert receipt["acknowledged"] is True
    assert receipt["source_sha256"] == hashlib.sha256(source).hexdigest()
    assert client.post(
        f"/api/device/v1/recordings/{context['recording_id']}/complete",
        headers=context["headers"],
    ).json()["acknowledged"] is True

    with client.app.state.database.session() as session:
        stored = session.get(Recording, context["recording_id"])
        assert stored.status == "ready"
        assert stored.media_key_version == 1
        assert session.get(RecordingUpload, stored.id) is None
        encrypted_path = client.app.state.settings.recording_root / stored.media_storage_name
        encrypted = encrypted_path.read_bytes()
        assert encrypted.startswith(b"IVREC1\0")
        assert source[:44] not in encrypted
        assert stored.media_sha256 not in encrypted_path.name

    inbox = client.get("/api/admin/v1/recordings")
    assert inbox.status_code == 200
    assert inbox.json()["total"] == 1
    item = inbox.json()["items"][0]
    assert item["caller"] == "+15551234567"
    assert item["caller_masked"].endswith("4567")
    assert item["call_id"] == context["call_id"]
    assert item["listened_at"] is None

    audio = client.get(item["playback_url"])
    assert audio.status_code == 200
    assert audio.headers["cache-control"] == "no-store, private"
    assert audio.headers["accept-ranges"] == "bytes"
    assert audio.content.startswith((b"ID3", b"\xff"))
    probe = subprocess.run(
        [
            "ffprobe",
            "-v",
            "error",
            "-show_entries",
            "stream=codec_name,sample_rate,channels,bit_rate",
            "-of",
            "json",
            "-i",
            "pipe:0",
        ],
        input=audio.content,
        capture_output=True,
        check=True,
    )
    stream = json.loads(probe.stdout)["streams"][0]
    assert stream["codec_name"] == "mp3"
    assert stream["sample_rate"] == "16000"
    assert stream["channels"] == 1
    assert int(stream["bit_rate"]) == 48_000

    ranged = client.get(item["playback_url"], headers={"Range": "bytes=0-31"})
    assert ranged.status_code == 206
    assert ranged.content == audio.content[:32]
    assert ranged.headers["content-range"] == f"bytes 0-31/{len(audio.content)}"
    unsatisfied = client.get(item["playback_url"], headers={"Range": "bytes=999999-"})
    assert unsatisfied.status_code == 416
    assert unsatisfied.headers["content-range"] == f"bytes */{len(audio.content)}"
    assert unsatisfied.headers["cache-control"] == "no-store, private"
    download = client.get(item["download_url"])
    assert download.headers["content-disposition"].startswith("attachment;")

    listened = client.patch(
        f"/api/admin/v1/recordings/{context['recording_id']}/listened",
        json={"listened": True},
    )
    assert listened.status_code == 200
    assert listened.json()["listened_at"] is not None
    assert client.get("/api/admin/v1/recordings?unread=true").json()["total"] == 0
    calls = client.get("/api/admin/v1/calls").json()
    assert calls[0]["recording_count"] == 1
    assert calls[0]["pending_recording_count"] == 0
    overview = client.get("/api/admin/v1/overview").json()
    assert overview["recording_count"] == 1

    deleted = client.delete(f"/api/admin/v1/recordings/{context['recording_id']}")
    assert deleted.status_code == 204
    assert client.get(item["playback_url"]).status_code == 404
    late_receipt = client.get(
        f"/api/device/v1/recordings/{context['recording_id']}/upload",
        headers=context["headers"],
    )
    assert late_receipt.status_code == 200
    assert late_receipt.json()["status"] == "ready"
    assert late_receipt.json()["acknowledged"] is True
    assert client.post(
        "/api/device/v1/recordings",
        headers=context["headers"],
        json=body,
    ).json()["acknowledged"] is True
    with client.app.state.database.session() as session:
        tombstone = session.get(Recording, context["recording_id"])
        assert tombstone.status == "deleted"
        assert tombstone.deleted_at is not None
        assert tombstone.media_storage_name is None
        assert session.query(AuditLog).filter_by(action="recording.deleted").count() == 1


def test_v4_revision_preserves_the_existing_voicemail_upload_path(client) -> None:
    context = provision_recording_context(client)
    source = caller_wav()
    with client.app.state.database.session() as session:
        revision = session.get(Revision, 85)
        manifest = json.loads(client.app.state.cipher.decrypt(revision.manifest_encrypted))
        revision.schema_version = 4
        manifest["schema_version"] = 4
        manifest["program"]["version"] = 4
        revision.manifest_encrypted = client.app.state.cipher.encrypt(
            json.dumps(manifest, sort_keys=True, separators=(",", ":")),
        )
        session.commit()

    created = client.post(
        "/api/device/v1/recordings",
        headers=context["headers"],
        json=recording_request(context, source),
    )
    assert created.status_code == 201, created.text
    assert created.json()["status"] == "uploading"


def test_foreign_ids_malformed_wav_hash_and_size_are_rejected(client) -> None:
    context = provision_recording_context(client)
    source = caller_wav()
    body = recording_request(context, source)
    second_token = "second-token-" + "y" * 40
    with client.app.state.database.session() as session:
        session.add(
            Device(
                id=str(uuid4()),
                display_name="foreign",
                token_hash=digest_device_token(second_token),
            ),
        )
        session.commit()
    foreign = {"Authorization": f"Bearer {second_token}"}
    assert client.post("/api/device/v1/recordings", headers=foreign, json=body).status_code == 403

    create_and_upload(client, context, source)
    assert client.get(
        f"/api/device/v1/recordings/{context['recording_id']}/upload",
        headers=foreign,
    ).status_code == 403

    malformed_context = {**context, "recording_id": str(uuid4()), "sequence": 1}
    malformed = b"x" * 100
    create_and_upload(client, malformed_context, malformed)
    complete = client.post(
        f"/api/device/v1/recordings/{malformed_context['recording_id']}/complete",
        headers=context["headers"],
    )
    assert complete.status_code == 422
    assert "RIFF/WAVE" in complete.json()["detail"]

    hash_context = {**context, "recording_id": str(uuid4()), "call_id": str(uuid4())}
    with client.app.state.database.session() as session:
        original = session.get(CallRecord, context["call_id"])
        session.add(
            CallRecord(
                id=hash_context["call_id"],
                device_id=context["device_id"],
                started_at=utcnow(),
                policy_decision=original.policy_decision,
                revision_id=85,
                result="SESSION_COMPLETE",
            ),
        )
        session.commit()
    wrong_hash_body = recording_request(
        hash_context,
        source,
        source_sha256="0" * 64,
    )
    assert client.post(
        "/api/device/v1/recordings",
        headers=context["headers"],
        json=wrong_hash_body,
    ).status_code == 201
    response = client.put(
        f"/api/device/v1/recordings/{hash_context['recording_id']}/content",
        headers={
            **context["headers"],
            "Content-Type": "application/offset+octet-stream",
            "Upload-Offset": "0",
        },
        content=source,
    )
    assert response.status_code == 200
    assert client.post(
        f"/api/device/v1/recordings/{hash_context['recording_id']}/complete",
        headers=context["headers"],
    ).status_code == 422

    too_large = client.post(
        "/api/device/v1/recordings",
        headers=context["headers"],
        json={
            **body,
            "recording_id": str(uuid4()),
            "expected_size_bytes": 100_000,
            "source_sha256": "a" * 64,
        },
    )
    assert too_large.status_code == 413


def test_finish_key_contract_and_streamed_chunk_limit_are_enforced(client) -> None:
    context = provision_recording_context(client)
    source = caller_wav()
    with client.app.state.database.session() as session:
        revision = session.get(Revision, 85)
        manifest = json.loads(client.app.state.cipher.decrypt(revision.manifest_encrypted))
        manifest["recording_behavior"]["finish_key"] = None
        revision.manifest_encrypted = client.app.state.cipher.encrypt(
            json.dumps(manifest, sort_keys=True, separators=(",", ":")),
        )
        session.commit()
    rejected = client.post(
        "/api/device/v1/recordings",
        headers=context["headers"],
        json=recording_request(context, source),
    )
    assert rejected.status_code == 422
    assert "disables finish-key" in rejected.json()["detail"]

    early_limit = client.post(
        "/api/device/v1/recordings",
        headers=context["headers"],
        json=recording_request(context, source, stop_reason="maximum_duration"),
    )
    assert early_limit.status_code == 422
    assert "ended too early" in early_limit.json()["detail"]
    wrong_session = client.post(
        "/api/device/v1/recordings",
        headers=context["headers"],
        json=recording_request(
            context,
            source,
            stop_reason="caller_hangup",
            captured_at="2000-01-01T00:00:00Z",
        ),
    )
    assert wrong_session.status_code == 422
    assert "call session" in wrong_session.json()["detail"]

    accepted_body = recording_request(context, source, stop_reason="caller_hangup")
    assert client.post(
        "/api/device/v1/recordings",
        headers=context["headers"],
        json=accepted_body,
    ).status_code == 201
    client.app.state.settings.recording_chunk_limit_bytes = 16
    oversized = client.put(
        f"/api/device/v1/recordings/{context['recording_id']}/content",
        headers={
            **context["headers"],
            "Content-Type": "application/offset+octet-stream",
            "Upload-Offset": "0",
        },
        content=source[:17],
    )
    assert oversized.status_code == 413
    with client.app.state.database.session() as session:
        assert session.get(RecordingUpload, context["recording_id"]).upload_offset == 0


def test_quota_returns_507_and_retains_the_resumable_upload(client) -> None:
    context = provision_recording_context(client)
    source = caller_wav()
    client.app.state.settings.recording_quota_bytes = len(source) - 1
    refused = client.post(
        "/api/device/v1/recordings",
        headers=context["headers"],
        json=recording_request(context, source),
    )
    assert refused.status_code == 507
    assert "retain" in refused.json()["detail"].lower()
    with client.app.state.database.session() as session:
        assert session.get(Recording, context["recording_id"]) is None

    client.app.state.settings.recording_quota_bytes = len(source) * 2
    create_and_upload(client, context, source)
    settings = client.get("/api/admin/v1/recording-settings").json()
    assert settings["used_bytes"] == len(source)
    client.app.state.settings.recording_quota_bytes = 10
    response = client.post(
        f"/api/device/v1/recordings/{context['recording_id']}/complete",
        headers=context["headers"],
    )
    assert response.status_code == 507
    assert "retain" in response.json()["detail"].lower()
    with client.app.state.database.session() as session:
        recording = session.get(Recording, context["recording_id"])
        upload = session.get(RecordingUpload, context["recording_id"])
        assert recording.status == "uploading"
        assert upload.upload_offset == len(source)
    settings = client.get("/api/admin/v1/recording-settings").json()
    assert settings["pending_count"] == 1
    assert settings["quota_bytes"] == 10
    assert settings["used_bytes"] == len(source)
    calls = client.get("/api/admin/v1/calls").json()
    assert calls[0]["recording_count"] == 0
    assert calls[0]["pending_recording_count"] == 1
    overview = client.get("/api/admin/v1/overview").json()
    assert overview["recording_count"] == 0
    assert overview["pending_recording_count"] == 1


def test_abandoned_upload_cleanup_keeps_device_retry_idempotent(client) -> None:
    context = provision_recording_context(client)
    source = caller_wav()
    created = client.post(
        "/api/device/v1/recordings",
        headers=context["headers"],
        json=recording_request(context, source),
    )
    assert created.status_code == 201
    with client.app.state.database.session() as session:
        upload = session.get(RecordingUpload, context["recording_id"])
        directory = client.app.state.settings.recording_root / ".uploads" / upload.storage_name
        upload.updated_at = utcnow() - timedelta(hours=25)
        session.commit()
    assert directory.is_dir()

    assert client.get("/api/admin/v1/recording-settings").status_code == 200
    assert not directory.exists()
    with client.app.state.database.session() as session:
        assert session.get(Recording, context["recording_id"]).status == "failed"
        assert session.get(RecordingUpload, context["recording_id"]) is None

    restarted = client.post(
        "/api/device/v1/recordings",
        headers=context["headers"],
        json=recording_request(context, source),
    )
    assert restarted.status_code == 201
    assert restarted.json()["status"] == "uploading"
    assert restarted.json()["upload_offset"] == 0


def test_manual_and_automatic_retention_preserve_audit_tombstones(client) -> None:
    old = datetime.now(timezone.utc) - timedelta(days=40)
    context = provision_recording_context(client, captured_at=old)
    source = caller_wav()
    create_and_upload(client, context, source)
    assert client.post(
        f"/api/device/v1/recordings/{context['recording_id']}/complete",
        headers=context["headers"],
    ).status_code == 200

    manual = client.put(
        "/api/admin/v1/recording-settings",
        json={"mode": "manual", "days": 1},
    )
    assert manual.status_code == 200
    assert client.get("/api/admin/v1/recordings").json()["total"] == 1
    automatic = client.put(
        "/api/admin/v1/recording-settings",
        json={"mode": "automatic", "days": 30},
    )
    assert automatic.status_code == 200
    assert client.get("/api/admin/v1/recordings").json()["total"] == 0
    with client.app.state.database.session() as session:
        tombstone = session.get(Recording, context["recording_id"])
        assert tombstone.status == "deleted"
        assert tombstone.deletion_reason == "retention"
        assert tombstone.deleted_at is not None


def test_versioned_recording_keyring_preserves_old_media_during_rotation() -> None:
    recording_id = str(uuid4())
    first_key = base64.b64encode(bytes(range(32))).decode("ascii")
    second_key = base64.b64encode(bytes(range(32, 64))).decode("ascii")
    old_cipher = RecordingCipher(first_key, 1)
    encrypted = old_cipher.encrypt_media(recording_id, b"private voicemail")

    rotated = RecordingCipher(second_key, 2, json.dumps({"1": first_key}))
    assert rotated.media_version(encrypted) == 1
    assert rotated.decrypt_media(recording_id, encrypted) == b"private voicemail"
    try:
        RecordingCipher(second_key, 2).decrypt_media(recording_id, encrypted)
    except RecordingError as error:
        assert error.status_code == 503
        assert "version" in str(error).lower()
    else:  # pragma: no cover - protects destructive key rotation
        raise AssertionError("old voicemail decrypted without its retained key version")
