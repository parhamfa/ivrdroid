from __future__ import annotations

import base64
import io
import json
import wave

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
from fastapi.testclient import TestClient

from app.crypto import canonical_json_bytes
from app.crypto import DataCipher
from app.database import Base, Database
from app.main import create_app
from app.models import AuditLog, CallRecord, Device, Draft, Revision
from app.schemas import DraftConfigurationV3
from app.services import V4_DRAFT_ID


ROOT_BLOCK_ID = "11111111-1111-4111-8111-111111111111"
MENU_BLOCK_ID = "22222222-2222-4222-8222-222222222222"
BRANCH_ID = "33333333-3333-4333-8333-333333333333"
BRANCH_PROMPT_ID = "44444444-4444-4444-8444-444444444444"
BRANCH_END_ID = "55555555-5555-4555-8555-555555555555"
TIMEOUT_END_ID = "66666666-6666-4666-8666-666666666666"
INVALID_END_ID = "77777777-7777-4777-8777-777777777777"
RETURN_LIMIT_END_ID = "88888888-8888-4888-8888-888888888888"


def make_wav(seconds: float = 0.1) -> bytes:
    output = io.BytesIO()
    with wave.open(output, "wb") as audio:
        audio.setnchannels(2)
        audio.setsampwidth(2)
        audio.setframerate(48000)
        audio.writeframes(b"\0\0\0\0" * round(48000 * seconds))
    return output.getvalue()


def save_end_flow(client) -> dict:
    draft = client.get("/api/admin/v4/draft").json()
    draft["flow"] = {
        "root": {"block_id": ROOT_BLOCK_ID, "type": "end_call"},
    }
    response = client.put("/api/admin/v4/draft", json=draft)
    assert response.status_code == 200, response.text
    return response.json()


def save_end_flow_v3(client) -> dict:
    draft = client.get("/api/admin/v3/draft").json()
    draft["flow"] = {
        "root": {"block_id": ROOT_BLOCK_ID, "type": "end_call"},
    }
    response = client.put("/api/admin/v3/draft", json=draft)
    assert response.status_code == 200, response.text
    return response.json()


def provision_v4_device(client, display_name: str = "SM-T585") -> str:
    with client.app.state.database.session() as session:
        device = Device(
            display_name=display_name,
            token_hash=(display_name.encode("utf-8").hex() + "0" * 64)[:64],
            status={
                "runtime_versions": [1, 2, 3, 4],
                "recording_capable": True,
                "external_call_control_capable": True,
                "conversation_recording_capable": True,
                "prompt_barge_in_capable": True,
                "call_control_protocol_version": 1,
            },
        )
        session.add(device)
        session.commit()
        return device.id


def make_menu_flow(prompt_id: str) -> dict:
    return {
        "root": {
            "block_id": MENU_BLOCK_ID,
            "type": "collect_digit",
            "prompt_id": None,
            "timeout_ms": 5000,
            "maximum_attempts": 3,
            "maximum_menu_returns": 2,
            "branches": [
                {
                    "branch_id": BRANCH_ID,
                    "digit": "1",
                    "root": {
                        "block_id": BRANCH_PROMPT_ID,
                        "type": "play_prompt",
                        "prompt_id": prompt_id,
                        "next": {"block_id": BRANCH_END_ID, "type": "end_call"},
                    },
                },
            ],
            "on_timeout": {"block_id": TIMEOUT_END_ID, "type": "end_call"},
            "on_invalid": {"block_id": INVALID_END_ID, "type": "end_call"},
            "on_return_limit": {"block_id": RETURN_LIMIT_END_ID, "type": "end_call"},
        },
    }


def test_health_and_default_draft(client):
    assert client.get("/health").json()["status"] == "ok"
    response = client.get("/api/admin/v4/draft")
    assert response.status_code == 200
    assert response.json()["schema_version"] == 4
    assert response.json()["edit_version"] == 1
    assert response.json()["recording_behavior"] == {
        "maximum_duration_seconds": 60,
        "finish_key": "#",
    }
    assert response.json()["flow"] == {"root": None}
    v3 = client.get("/api/admin/v3/draft")
    assert v3.status_code == 200
    assert v3.json()["schema_version"] == 3
    assert v3.json()["edit_version"] == 1
    assert v3.json()["flow"] == {"root": None}
    assert client.get("/api/admin/v1/draft").status_code == 410
    assert client.get("/api/admin/v2/draft").status_code == 410


def test_restart_keeps_the_v4_draft_and_edit_version(settings):
    database = Database(settings.database_url)
    Base.metadata.create_all(database.engine)
    cipher = DataCipher(settings.data_encryption_key_b64)
    legacy_document = DraftConfigurationV3().model_dump(mode="json", exclude={"edit_version"})
    legacy_document["caller_policy"]["mode"] = "ACCEPT_ALL"
    with database.session() as session:
        session.add(
            Draft(
                id=1,
                document_encrypted=cipher.encrypt(
                    json.dumps(legacy_document, sort_keys=True, separators=(",", ":")),
                ),
                edit_version=7,
                updated_by="v3-editor@example.com",
            ),
        )
        session.commit()

    with TestClient(create_app(settings)) as first:
        draft = first.get("/api/admin/v4/draft").json()
        assert draft["schema_version"] == 4
        assert draft["edit_version"] == 1
        assert draft["caller_policy"]["mode"] == "ACCEPT_ALL"
        draft["caller_policy"]["mode"] = "ALLOWLIST_ONLY"
        saved = first.put("/api/admin/v4/draft", json=draft)
        assert saved.status_code == 200
        edit_version = saved.json()["edit_version"]

    with TestClient(create_app(settings)) as restarted:
        current = restarted.get("/api/admin/v4/draft").json()
        assert current["edit_version"] == edit_version
        assert current["caller_policy"]["mode"] == "ALLOWLIST_ONLY"
        v3 = restarted.get("/api/admin/v3/draft").json()
        assert v3["edit_version"] == 7
        assert v3["caller_policy"]["mode"] == "ACCEPT_ALL"
        with restarted.app.state.database.session() as session:
            assert sorted(item.id for item in session.query(Draft)) == [1, V4_DRAFT_ID]


def test_overview_uses_the_active_revision_policy_instead_of_editable_drafts(client):
    device_id = provision_v4_device(client)

    legacy = client.get("/api/admin/v3/draft").json()
    legacy["caller_policy"]["allowlist"] = [
        {"label": "Legacy only", "e164": "+15551000001"},
    ]
    assert client.put("/api/admin/v3/draft", json=legacy).status_code == 200

    active = save_end_flow(client)
    active["caller_policy"]["allowlist"] = [
        {"label": "Primary", "e164": "+15551000002"},
        {"label": "Second", "e164": "+15551000003"},
    ]
    saved = client.put("/api/admin/v4/draft", json=active)
    assert saved.status_code == 200, saved.text
    published = client.post("/api/admin/v4/draft/publish")
    assert published.status_code == 200, published.text

    with client.app.state.database.session() as session:
        device = session.get(Device, device_id)
        assert device is not None
        device.active_revision_id = published.json()["id"]
        session.commit()

    edited = client.get("/api/admin/v4/draft").json()
    edited["caller_policy"]["allowlist"].append(
        {"label": "Unpublished", "e164": "+15551000004"},
    )
    assert client.put("/api/admin/v4/draft", json=edited).status_code == 200

    overview = client.get("/api/admin/v1/overview")
    assert overview.status_code == 200, overview.text
    assert overview.json()["active_revision"] == published.json()["id"]
    assert [entry["label"] for entry in overview.json()["caller_policy"]["allowlist"]] == [
        "Primary",
        "Second",
    ]


def test_overview_falls_back_to_the_v4_draft_before_first_activation(client):
    legacy = client.get("/api/admin/v3/draft").json()
    legacy["caller_policy"]["allowlist"] = [
        {"label": "Legacy only", "e164": "+15552000001"},
    ]
    assert client.put("/api/admin/v3/draft", json=legacy).status_code == 200

    current = client.get("/api/admin/v4/draft").json()
    current["caller_policy"]["allowlist"] = [
        {"label": "Primary", "e164": "+15552000002"},
        {"label": "Second", "e164": "+15552000003"},
    ]
    assert client.put("/api/admin/v4/draft", json=current).status_code == 200

    overview = client.get("/api/admin/v1/overview")
    assert overview.status_code == 200, overview.text
    assert overview.json()["active_revision"] is None
    assert [entry["label"] for entry in overview.json()["caller_policy"]["allowlist"]] == [
        "Primary",
        "Second",
    ]


def test_v3_and_v4_authoring_publish_and_rollback_are_independent(client):
    provision_v4_device(client)
    save_end_flow_v3(client)
    v3_first = client.post("/api/admin/v3/draft/publish")
    assert v3_first.status_code == 200, v3_first.text
    assert v3_first.json()["schema_version"] == 3

    save_end_flow(client)
    v4_first = client.post("/api/admin/v4/draft/publish")
    assert v4_first.status_code == 200, v4_first.text
    assert v4_first.json()["schema_version"] == 4

    v3_changed = client.get("/api/admin/v3/draft").json()
    v3_changed["caller_policy"]["mode"] = "ACCEPT_ALL"
    assert client.put("/api/admin/v3/draft", json=v3_changed).status_code == 200
    assert client.get("/api/admin/v4/draft").json()["caller_policy"]["mode"] == (
        "ALLOWLIST_ONLY"
    )

    v4_changed = client.get("/api/admin/v4/draft").json()
    v4_changed["caller_policy"]["mode"] = "ACCEPT_ALL_EXCEPT_BLOCKLIST"
    assert client.put("/api/admin/v4/draft", json=v4_changed).status_code == 200
    assert client.get("/api/admin/v3/draft").json()["caller_policy"]["mode"] == "ACCEPT_ALL"

    v3_rollback = client.post(
        f"/api/admin/v3/revisions/{v3_first.json()['id']}/rollback",
    )
    assert v3_rollback.status_code == 200, v3_rollback.text
    assert client.get("/api/admin/v3/draft").json()["caller_policy"]["mode"] == (
        "ALLOWLIST_ONLY"
    )
    assert client.get("/api/admin/v4/draft").json()["caller_policy"]["mode"] == (
        "ACCEPT_ALL_EXCEPT_BLOCKLIST"
    )

    v4_rollback = client.post(
        f"/api/admin/v4/revisions/{v4_first.json()['id']}/rollback",
    )
    assert v4_rollback.status_code == 200, v4_rollback.text
    assert client.get("/api/admin/v4/draft").json()["caller_policy"]["mode"] == (
        "ALLOWLIST_ONLY"
    )
    assert client.get("/api/admin/v3/draft").json()["caller_policy"]["mode"] == (
        "ALLOWLIST_ONLY"
    )


def test_publish_signs_immutable_manifest(client):
    provision_v4_device(client)
    save_end_flow(client)
    publish = client.post("/api/admin/v4/draft/publish")
    assert publish.status_code == 200, publish.text
    revision = publish.json()
    assert revision["schema_version"] == 4
    manifest_response = client.get(
        f"/api/device/v1/revisions/{revision['id']}/manifest",
        headers={"Authorization": "Bearer invalid"},
    )
    assert manifest_response.status_code == 401

    me = client.get("/api/admin/v1/me").json()
    with client.app.state.database.session() as session:
        from app.models import Revision

        stored = session.get(Revision, revision["id"])
        manifest = json.loads(client.app.state.cipher.decrypt(stored.manifest_encrypted))
    public_key = Ed25519PublicKey.from_public_bytes(base64.b64decode(me["signing_public_key_b64"]))
    public_key.verify(base64.b64decode(revision["signature_b64"]), canonical_json_bytes(manifest))
    assert manifest["compiler_version"] == "4.0.0"
    assert manifest["recording_behavior"] == {
        "finish_key": "#",
        "maximum_duration_seconds": 60,
    }
    assert manifest["program"] == {
        "entry_pc": 0,
        "instructions": [{"block_id": ROOT_BLOCK_ID, "op": "end_call", "pc": 0}],
        "maximum_automated_session_ms": 0,
        "version": 4,
    }


def test_v4_diff_reviews_every_signed_configuration_section(client):
    provision_v4_device(client)
    save_end_flow(client)
    base = client.post("/api/admin/v4/draft/publish")
    assert base.status_code == 200, base.text

    candidate = client.get("/api/admin/v4/draft").json()
    candidate["caller_policy"] = {
        "mode": "ACCEPT_ALL",
        "route_unknown_callers": True,
        "allowlist": [{"label": "Primary", "e164": "+989120000000"}],
        "blocklist": [],
    }
    candidate["schedules"] = [
        {
            "id": "business_hours",
            "name": "Business hours",
            "timezone": "Asia/Tehran",
            "weekly": [{"weekday": 0, "start": "09:00", "end": "17:00"}],
            "exceptions": [],
        },
    ]
    candidate["recording_behavior"]["maximum_duration_seconds"] = 90
    saved = client.put("/api/admin/v4/draft", json=candidate)
    assert saved.status_code == 200, saved.text

    diff = client.post(
        "/api/admin/v4/draft/diff",
        json={"configuration": saved.json(), "base_revision_id": base.json()["id"]},
    )
    assert diff.status_code == 200, diff.text
    body = diff.json()
    assert body["base_revision_id"] == base.json()["id"]
    assert body["added"] == body["removed"] == body["changed"] == 0
    assert body["changes"] == []
    assert body["caller_policy_changes"] == [
        "Policy mode: Allowlist only → Accept all",
        "Hidden or unknown callers: stock dialer → IVR",
        "Added to allowlist: Primary (••••0000)",
    ]
    assert body["schedule_changes"] == ["Added schedule: Business hours"]
    assert body["recording_changes"] == [
        "Maximum recording duration: 60 → 90 seconds",
    ]
    assert body["requires_policy_confirmation"] is True


def test_v4_publish_is_bound_to_the_reviewed_draft_and_base_revision(client):
    provision_v4_device(client)
    save_end_flow(client)
    base = client.post("/api/admin/v4/draft/publish")
    assert base.status_code == 200, base.text
    reviewed = client.get("/api/admin/v4/draft").json()

    changed = json.loads(json.dumps(reviewed))
    changed["caller_policy"]["mode"] = "ACCEPT_ALL"
    saved = client.put("/api/admin/v4/draft", json=changed)
    assert saved.status_code == 200, saved.text

    stale_draft = client.post(
        "/api/admin/v4/draft/publish",
        json={
            "edit_version": reviewed["edit_version"],
            "base_revision_id": base.json()["id"],
        },
    )
    assert stale_draft.status_code == 409
    assert "draft changed after review" in stale_draft.json()["detail"].lower()

    published = client.post(
        "/api/admin/v4/draft/publish",
        json={
            "edit_version": saved.json()["edit_version"],
            "base_revision_id": base.json()["id"],
        },
    )
    assert published.status_code == 200, published.text

    stale_base = client.post(
        "/api/admin/v4/draft/publish",
        json={
            "edit_version": saved.json()["edit_version"],
            "base_revision_id": base.json()["id"],
        },
    )
    assert stale_base.status_code == 409
    assert "published revision changed after review" in stale_base.json()["detail"].lower()


def test_v4_publish_requires_the_exact_call_control_capabilities(client):
    save_end_flow(client)
    no_device = client.post("/api/admin/v4/draft/publish")
    assert no_device.status_code == 409
    assert "at least one active device" in no_device.json()["detail"]
    with client.app.state.database.session() as session:
        device = Device(
            display_name="SM-T585",
            token_hash="c" * 64,
            status={
                "runtime_versions": [1, 2, 3, 4],
                "external_call_control_capable": True,
                "conversation_recording_capable": True,
                "call_control_protocol_version": 3,
            },
        )
        session.add(device)
        session.commit()
        device_id = device.id

    rejected = client.post("/api/admin/v4/draft/publish")
    assert rejected.status_code == 409
    assert "protocol 1" in rejected.json()["detail"]

    with client.app.state.database.session() as session:
        device = session.get(Device, device_id)
        device.status = {**device.status, "call_control_protocol_version": 1}
        session.commit()
    assert client.post("/api/admin/v4/draft/publish").status_code == 200


def test_prompt_barge_in_persists_diffs_and_requires_device_capability(client):
    device_id = provision_v4_device(client)
    prompt_response = client.post(
        "/api/admin/v1/prompts",
        data={"name": "Interruptible menu"},
        files={"file": ("menu.wav", make_wav(), "audio/wav")},
    )
    assert prompt_response.status_code == 201, prompt_response.text
    prompt_id = prompt_response.json()["id"]

    draft = client.get("/api/admin/v4/draft").json()
    draft["flow"] = make_menu_flow(prompt_id)
    draft["flow"]["root"]["prompt_id"] = prompt_id
    saved = client.put("/api/admin/v4/draft", json=draft)
    assert saved.status_code == 200, saved.text
    assert "allow_prompt_barge_in" not in saved.json()["flow"]["root"]
    base = client.post("/api/admin/v4/draft/publish")
    assert base.status_code == 200, base.text

    enabled = client.get("/api/admin/v4/draft").json()
    enabled["flow"]["root"]["allow_prompt_barge_in"] = True
    saved_enabled = client.put("/api/admin/v4/draft", json=enabled)
    assert saved_enabled.status_code == 200, saved_enabled.text
    reloaded = client.get("/api/admin/v4/draft").json()
    assert reloaded["flow"]["root"]["allow_prompt_barge_in"] is True
    diff = client.post(
        "/api/admin/v4/draft/diff",
        json={"configuration": reloaded, "base_revision_id": base.json()["id"]},
    )
    assert diff.status_code == 200, diff.text
    assert diff.json()["changed"] == 1

    with client.app.state.database.session() as session:
        device = session.get(Device, device_id)
        device.status = {**device.status, "prompt_barge_in_capable": False}
        session.commit()
    rejected = client.post("/api/admin/v4/draft/publish")
    assert rejected.status_code == 409
    assert "prompt interruption" in rejected.json()["detail"]

    with client.app.state.database.session() as session:
        device = session.get(Device, device_id)
        device.status = {**device.status, "prompt_barge_in_capable": True}
        session.commit()
    published = client.post("/api/admin/v4/draft/publish")
    assert published.status_code == 200, published.text
    with client.app.state.database.session() as session:
        stored = session.get(Revision, published.json()["id"])
        manifest = json.loads(client.app.state.cipher.decrypt(stored.manifest_encrypted))
    assert manifest["compiler_version"] == "4.1.0"
    collector = next(
        item for item in manifest["program"]["instructions"]
        if item["op"] == "collect_digit"
    )
    assert collector["allow_prompt_barge_in"] is True


def test_rollback_republishes_prior_configuration_as_new_revision(client):
    provision_v4_device(client)
    save_end_flow(client)
    first = client.post("/api/admin/v4/draft/publish")
    assert first.status_code == 200
    first_id = first.json()["id"]

    changed = client.get("/api/admin/v4/draft").json()
    changed["caller_policy"]["mode"] = "ACCEPT_ALL"
    assert client.put("/api/admin/v4/draft", json=changed).status_code == 200
    second = client.post("/api/admin/v4/draft/publish")
    assert second.status_code == 200
    assert second.json()["id"] == first_id + 1

    rollback = client.post(f"/api/admin/v4/revisions/{first_id}/rollback")
    assert rollback.status_code == 200, rollback.text
    restored = rollback.json()
    assert restored["id"] == first_id + 2
    assert restored["source_revision_id"] == first_id
    assert client.get("/api/admin/v4/draft").json()["caller_policy"]["mode"] == "ALLOWLIST_ONLY"

    with client.app.state.database.session() as session:
        from app.models import Revision

        stored = session.get(Revision, restored["id"])
        manifest = json.loads(client.app.state.cipher.decrypt(stored.manifest_encrypted))
    assert manifest["rolled_back_from"] == first_id
    assert manifest["caller_policy"]["mode"] == "ALLOWLIST_ONLY"


def test_emergency_legacy_activation_reuses_signed_snapshot(client):
    with client.app.state.database.session() as session:
        legacy = Revision(
            schema_version=1,
            manifest_encrypted=client.app.state.cipher.encrypt("{}"),
            manifest_sha256="a" * 64,
            signature_b64="signed-v1",
            published_by="legacy@example.com",
        )
        session.add(legacy)
        session.flush()
        device = Device(
            display_name="SM-T585",
            token_hash="b" * 64,
            desired_revision_id=None,
            active_revision_id=None,
            status={
                "runtime_versions": [1, 2, 3, 4],
                "recording_capable": True,
                "external_call_control_capable": True,
                "conversation_recording_capable": True,
                "call_control_protocol_version": 1,
            },
        )
        session.add(device)
        session.commit()
        legacy_id = legacy.id
        device_id = device.id

    response = client.post(f"/api/admin/v4/revisions/{legacy_id}/activate-rollback")
    assert response.status_code == 200, response.text
    assert response.json()["id"] == legacy_id
    assert response.json()["schema_version"] == 1

    with client.app.state.database.session() as session:
        assert session.get(Device, device_id).desired_revision_id == legacy_id
        event = session.query(AuditLog).filter_by(
            action="revision.legacy_rollback_activated",
        ).one()
        assert event.target == f"revision:{legacy_id}"
        assert event.details == {"schema_version": 1, "affected_devices": 1}

    save_end_flow(client)
    v4 = client.post("/api/admin/v4/draft/publish").json()
    rejected = client.post(f"/api/admin/v4/revisions/{v4['id']}/activate-rollback")
    assert rejected.status_code == 409


def test_validation_can_check_unsaved_candidate(client):
    saved = save_end_flow(client)
    candidate = json.loads(json.dumps(saved))
    candidate["flow"] = {"root": None}
    validation = client.post("/api/admin/v4/draft/validate", json=candidate)
    assert validation.status_code == 200
    assert validation.json()["valid"] is False
    assert client.get("/api/admin/v4/draft").json()["flow"]["root"]["type"] == "end_call"


def test_stale_v4_draft_save_returns_conflict(client):
    first_editor = client.get("/api/admin/v4/draft").json()
    second_editor = json.loads(json.dumps(first_editor))
    first_editor["flow"] = {"root": {"block_id": ROOT_BLOCK_ID, "type": "end_call"}}
    saved = client.put("/api/admin/v4/draft", json=first_editor)
    assert saved.status_code == 200
    assert saved.json()["edit_version"] == first_editor["edit_version"] + 1

    second_editor["caller_policy"]["mode"] = "ACCEPT_ALL"
    conflict = client.put("/api/admin/v4/draft", json=second_editor)
    assert conflict.status_code == 409
    assert "changed" in conflict.json()["detail"].lower()
    current = client.get("/api/admin/v4/draft").json()
    assert current["flow"]["root"]["block_id"] == ROOT_BLOCK_ID
    assert current["caller_policy"]["mode"] == "ALLOWLIST_ONLY"


def test_prompt_upload_and_usage_validation(client):
    blank = client.post(
        "/api/admin/v1/prompts",
        data={"name": "   "},
        files={"file": ("blank.wav", make_wav(), "audio/wav")},
    )
    assert blank.status_code == 422

    response = client.post(
        "/api/admin/v1/prompts",
        data={"name": "Welcome"},
        files={"file": ("welcome.wav", make_wav(), "audio/wav")},
    )
    assert response.status_code == 201, response.text
    prompt = response.json()
    assert prompt["size_bytes"] > 0
    assert prompt["duration_ms"] == 100
    assert prompt["version"] == 1
    assert len(prompt["content_hash"]) == 64
    assert client.get(prompt["audio_url"]).status_code == 200

    second = client.post(
        "/api/admin/v1/prompts",
        data={"name": "Welcome"},
        files={"file": ("welcome-v2.wav", make_wav(0.2), "audio/wav")},
    )
    assert second.status_code == 201, second.text
    assert second.json()["version"] == 2

    draft = client.get("/api/admin/v4/draft").json()
    draft["flow"] = make_menu_flow(prompt["id"])
    assert client.put("/api/admin/v4/draft", json=draft).status_code == 200
    validation = client.post("/api/admin/v4/draft/validate").json()
    assert validation == {"valid": True, "errors": []}
    v3 = client.get("/api/admin/v3/draft").json()
    v3["flow"] = make_menu_flow(prompt["id"])
    assert client.put("/api/admin/v3/draft", json=v3).status_code == 200
    usage = next(
        item for item in client.get("/api/admin/v1/prompts").json() if item["id"] == prompt["id"]
    )
    assert len(usage["used_by"]) == 2

    v4_without_prompt = client.get("/api/admin/v4/draft").json()
    v4_without_prompt["flow"] = {"root": {"block_id": ROOT_BLOCK_ID, "type": "end_call"}}
    assert client.put("/api/admin/v4/draft", json=v4_without_prompt).status_code == 200
    assert client.delete(f"/api/admin/v1/prompts/{prompt['id']}").status_code == 409


def test_pairing_enrollment_sync_ack_and_encrypted_call(client):
    pairing = client.post(
        "/api/admin/v1/pairing-codes",
        json={"display_name": "SM-T585"},
    )
    assert pairing.status_code == 201
    code = pairing.json()["code"]

    enrollment = client.post(
        "/api/device/v1/enroll",
        json={
            "code": code,
            "device_name": "tablet",
            "app_version": "0.4.0",
            "helper_version": "0.5.0",
        },
    )
    assert enrollment.status_code == 200, enrollment.text
    credentials = enrollment.json()
    assert credentials["service_client_id"] == "service-id"
    assert client.post("/api/device/v1/enroll", json={"code": code, "device_name": "tablet", "app_version": "0.4.0", "helper_version": "0.5.0"}).status_code == 401
    second_code = client.post(
        "/api/admin/v1/pairing-codes",
        json={"display_name": "second tablet"},
    ).json()["code"]
    assert client.post(
        "/api/device/v1/enroll",
        json={"code": second_code, "device_name": "second", "app_version": "0.4.0", "helper_version": "0.5.0"},
    ).status_code == 409

    headers = {"Authorization": f"Bearer {credentials['device_token']}"}
    sync = client.post(
        "/api/device/v1/sync",
        headers=headers,
        json={
            "app_version": "0.4.0",
            "helper_version": "0.5.0",
            "active_revision_id": None,
            "status": {
                "helper_state": "READY",
                "helper_result": "NONE",
                "call_state": "idle",
                "local_kill_switch": False,
                "storage_free_bytes": 1000000,
                "last_error": None,
                "runtime_versions": [1, 2, 3, 4],
                "recording_capable": True,
                "external_call_control_capable": True,
                "conversation_recording_capable": True,
                "call_control_protocol_version": 1,
                "recording_spool_bytes": 0,
                "recording_spool_count": 0,
                "voicemail_spool_bytes": 1024,
                "voicemail_spool_count": 2,
                "conversation_spool_bytes": 4096,
                "conversation_spool_count": 3,
                "recording_filesystem_free_bytes": 999999,
                "boot_wifi_recovery": {
                    "outcome": "reconnect_recovered",
                    "completed_at": "2026-08-05T10:00:30Z",
                    "elapsed_since_boot_ms": 3931744,
                    "internet_validated": True,
                    "reconnect_attempts": 1,
                    "wifi_enable_attempts": 0,
                },
            },
        },
    )
    assert sync.status_code == 200, sync.text
    device_status = client.get("/api/admin/v1/devices").json()[0]["status"]
    assert device_status["boot_wifi_recovery"] == {
        "outcome": "reconnect_recovered",
        "completed_at": "2026-08-05T10:00:30Z",
        "elapsed_since_boot_ms": 3931744,
        "internet_validated": True,
        "reconnect_attempts": 1,
        "wifi_enable_attempts": 0,
    }
    assert device_status["recording_spool_bytes"] == 0
    assert device_status["recording_spool_count"] == 0
    assert device_status["voicemail_spool_bytes"] == 1024
    assert device_status["voicemail_spool_count"] == 2
    assert device_status["conversation_spool_bytes"] == 4096
    assert device_status["conversation_spool_count"] == 3
    assert device_status["recording_filesystem_free_bytes"] == 999999

    save_end_flow(client)
    publish = client.post("/api/admin/v4/draft/publish")
    assert publish.status_code == 200
    revision_id = publish.json()["id"]
    second_publish = client.post("/api/admin/v4/draft/publish")
    assert second_publish.status_code == 200
    assert client.post(
        f"/api/device/v1/revisions/{revision_id}/ack",
        headers=headers,
        json={"state": "activated", "error": None},
    ).status_code == 409
    assert client.post(
        f"/api/device/v1/revisions/{revision_id + 100}/ack",
        headers=headers,
        json={"state": "activated", "error": None},
    ).status_code == 404

    caller = "+15551234567"
    initial_event = {
        "call_id": "call-12345678",
        "started_at": "2026-08-05T10:00:00Z",
        "caller": caller,
        "policy_decision": "Routed to IVR",
        "revision_id": None,
        "menu_path": [],
        "result": "IN_PROGRESS",
        "duration_seconds": 0,
        "events": [{"event": "Incoming call"}],
    }
    external_subevent = {
        "event": "external_call",
        "occurred_at": "2026-08-05T10:00:03Z",
        "status": "COMPLETED",
        "block_id": ROOT_BLOCK_ID,
        "reason": "OPERATOR_DISCONNECTED",
    }
    leaked_external = client.post(
        "/api/device/v1/events:batch",
        headers=headers,
        json={
            "calls": [
                {
                    **initial_event,
                    "call_id": "call-leak-12345",
                    "events": [{**external_subevent, "phone_number": "03136644636"}],
                },
            ],
        },
    )
    assert leaked_external.status_code == 422
    leaked_reason = client.post(
        "/api/device/v1/events:batch",
        headers=headers,
        json={
            "calls": [
                {
                    **initial_event,
                    "call_id": "call-leak-24680",
                    "events": [
                        {**external_subevent, "reason": "OPERATOR_03136644636"},
                    ],
                },
            ],
        },
    )
    assert leaked_reason.status_code == 422
    leaked_legacy = client.post(
        "/api/device/v1/events:batch",
        headers=headers,
        json={
            "calls": [
                {
                    **initial_event,
                    "call_id": "call-leak-67890",
                    "events": [{"event": "Operator 03136644636"}],
                },
            ],
        },
    )
    assert leaked_legacy.status_code == 422

    events = client.post(
        "/api/device/v1/events:batch",
        headers=headers,
        json={"calls": [initial_event]},
    )
    assert events.status_code == 200, events.text
    with client.app.state.database.session() as session:
        stored = session.get(CallRecord, "call-12345678")
        encrypted_caller = stored.caller_encrypted
        assert stored.result == "IN_PROGRESS"

    completion_event = {
        **initial_event,
        "menu_path": ["prompt", "end"],
        "result": "SESSION_COMPLETE",
        "duration_seconds": 4,
        "events": [external_subevent],
    }
    completion = client.post(
        "/api/device/v1/events:batch",
        headers=headers,
        json={"calls": [completion_event]},
    )
    assert completion.status_code == 200, completion.text

    stale_retry = client.post(
        "/api/device/v1/events:batch",
        headers=headers,
        json={"calls": [initial_event]},
    )
    assert stale_retry.status_code == 200, stale_retry.text

    calls = client.get("/api/admin/v1/calls").json()
    assert calls[0]["caller"] == caller
    assert calls[0]["menu_path"] == ["prompt", "end"]
    assert calls[0]["result"] == "SESSION_COMPLETE"
    assert calls[0]["duration_seconds"] == 4
    assert calls[0]["events"] == [{"event": "Incoming call"}, external_subevent]
    with client.app.state.database.session() as session:
        stored = session.get(CallRecord, "call-12345678")
        assert caller not in stored.caller_encrypted
        assert stored.caller_encrypted == encrypted_caller


def test_draft_phone_numbers_are_encrypted_at_rest(client):
    number = "+15557654321"
    draft = client.get("/api/admin/v4/draft").json()
    draft["caller_policy"]["allowlist"] = [{"label": "Private", "e164": number}]
    assert client.put("/api/admin/v4/draft", json=draft).status_code == 200
    with client.app.state.database.session() as session:
        stored = session.get(Draft, V4_DRAFT_ID)
        assert number not in stored.document_encrypted
