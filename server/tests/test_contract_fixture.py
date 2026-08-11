from __future__ import annotations

import base64
import hashlib
import json
from pathlib import Path

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey

from app.crypto import canonical_json_bytes
from app.compiler_v2 import compile_flow
from app.compiler_v3 import compile_flow_v3
from app.compiler_v4 import compile_flow_v4
from app.schemas import (
    DraftConfiguration,
    DraftConfigurationV2,
    DraftConfigurationV3,
    DraftConfigurationV4,
)
from app.validation import (
    validate_configuration,
    validate_configuration_v2,
    validate_configuration_v3,
    validate_configuration_v4,
)


FIXTURES = Path(__file__).resolve().parents[2] / "contracts" / "fixtures"


def test_configuration_revision_v1_cross_language_fixture() -> None:
    manifest = json.loads((FIXTURES / "configuration_revision_v1.json").read_text())
    metadata = json.loads((FIXTURES / "configuration_revision_v1.meta.json").read_text())
    canonical = canonical_json_bytes(manifest)
    assert hashlib.sha256(canonical).hexdigest() == metadata["manifest_sha256"]
    public_key = Ed25519PublicKey.from_public_bytes(
        base64.b64decode(metadata["signing_public_key_b64"], validate=True),
    )
    public_key.verify(
        base64.b64decode(metadata["signature_b64"], validate=True),
        canonical,
    )

    configuration = DraftConfiguration.model_validate(
        {
            "schema_version": manifest["schema_version"],
            "caller_policy": manifest["caller_policy"],
            "schedules": manifest["schedules"],
            "flow": manifest["flow"],
        },
    )
    prompt = manifest["prompts"][0]
    assert validate_configuration(
        configuration,
        {prompt["id"]},
        {prompt["id"]: prompt["size_bytes"]},
        128 * 1024 * 1024,
    ) == []


def test_configuration_revision_v2_cross_language_fixture() -> None:
    manifest = json.loads((FIXTURES / "configuration_revision_v2.json").read_text())
    metadata = json.loads((FIXTURES / "configuration_revision_v2.meta.json").read_text())
    canonical = canonical_json_bytes(manifest)
    assert hashlib.sha256(canonical).hexdigest() == metadata["manifest_sha256"]
    public_key = Ed25519PublicKey.from_public_bytes(
        base64.b64decode(metadata["signing_public_key_b64"], validate=True),
    )
    public_key.verify(base64.b64decode(metadata["signature_b64"], validate=True), canonical)

    configuration = DraftConfigurationV2.model_validate(
        {
            "schema_version": 2,
            "edit_version": 0,
            "caller_policy": manifest["caller_policy"],
            "schedules": manifest["schedules"],
            "flow": manifest["flow"],
        },
    )
    prompts = {item["id"] for item in manifest["prompts"]}
    sizes = {item["id"]: item["size_bytes"] for item in manifest["prompts"]}
    assert validate_configuration_v2(
        configuration,
        prompts,
        sizes,
        128 * 1024 * 1024,
    ) == []
    compiled = compile_flow(configuration.flow)
    assert compiled["source_sha256"] == manifest["source_sha256"]
    assert compiled["program_sha256"] == manifest["program_sha256"]
    assert compiled["program"] == manifest["program"]
    menu = compiled["program"]["instructions"][1]
    assert menu["branches"] == [
        {"digit": "1", "target_pc": 2},
        {"digit": "2", "target_pc": 4},
    ]


def test_configuration_revision_v3_cross_language_fixture() -> None:
    manifest = json.loads((FIXTURES / "configuration_revision_v3.json").read_text())
    metadata = json.loads((FIXTURES / "configuration_revision_v3.meta.json").read_text())
    canonical = canonical_json_bytes(manifest)
    assert hashlib.sha256(canonical).hexdigest() == metadata["manifest_sha256"]
    public_key = Ed25519PublicKey.from_public_bytes(
        base64.b64decode(metadata["signing_public_key_b64"], validate=True),
    )
    public_key.verify(base64.b64decode(metadata["signature_b64"], validate=True), canonical)

    configuration = DraftConfigurationV3.model_validate(
        {
            "schema_version": 3,
            "edit_version": 0,
            "caller_policy": manifest["caller_policy"],
            "schedules": manifest["schedules"],
            "recording_behavior": manifest["recording_behavior"],
            "flow": manifest["flow"],
        },
    )
    prompts = {item["id"] for item in manifest["prompts"]}
    sizes = {item["id"]: item["size_bytes"] for item in manifest["prompts"]}
    assert validate_configuration_v3(configuration, prompts, sizes, 128 * 1024 * 1024) == []
    compiled = compile_flow_v3(
        configuration.flow,
        configuration.recording_behavior,
        {item["id"]: item["duration_ms"] for item in manifest["prompts"]},
    )
    assert compiled["source_sha256"] == manifest["source_sha256"]
    assert compiled["program_sha256"] == manifest["program_sha256"]
    assert compiled["program"] == manifest["program"]


def test_configuration_revision_v4_cross_language_fixture() -> None:
    manifest = json.loads((FIXTURES / "configuration_revision_v4.json").read_text())
    metadata = json.loads((FIXTURES / "configuration_revision_v4.meta.json").read_text())
    canonical = canonical_json_bytes(manifest)
    assert hashlib.sha256(canonical).hexdigest() == metadata["manifest_sha256"]
    public_key = Ed25519PublicKey.from_public_bytes(
        base64.b64decode(metadata["signing_public_key_b64"], validate=True),
    )
    public_key.verify(base64.b64decode(metadata["signature_b64"], validate=True), canonical)

    configuration = DraftConfigurationV4.model_validate(
        {
            "schema_version": 4,
            "edit_version": 0,
            "caller_policy": manifest["caller_policy"],
            "schedules": manifest["schedules"],
            "recording_behavior": manifest["recording_behavior"],
            "flow": manifest["flow"],
        },
    )
    prompts = {item["id"] for item in manifest["prompts"]}
    sizes = {item["id"]: item["size_bytes"] for item in manifest["prompts"]}
    assert validate_configuration_v4(configuration, prompts, sizes, 128 * 1024 * 1024) == []
    compiled = compile_flow_v4(
        configuration.flow,
        configuration.recording_behavior,
        {item["id"]: item["duration_ms"] for item in manifest["prompts"]},
    )
    assert compiled["source_sha256"] == manifest["source_sha256"]
    assert compiled["program_sha256"] == manifest["program_sha256"]
    assert compiled["program"] == manifest["program"]
    assert compiled["program"]["maximum_automated_session_ms"] == 61_000
    assert "EXTERNAL_CALL 03136644636 30000 2 3 4" in (
        FIXTURES / "configuration_revision_v4.compiled.txt"
    ).read_text()


def test_configuration_revision_v41_prompt_barge_in_fixture() -> None:
    manifest = json.loads((FIXTURES / "configuration_revision_v41.json").read_text())
    metadata = json.loads((FIXTURES / "configuration_revision_v41.meta.json").read_text())
    canonical = canonical_json_bytes(manifest)
    assert hashlib.sha256(canonical).hexdigest() == metadata["manifest_sha256"]
    public_key = Ed25519PublicKey.from_public_bytes(
        base64.b64decode(metadata["signing_public_key_b64"], validate=True),
    )
    public_key.verify(base64.b64decode(metadata["signature_b64"], validate=True), canonical)

    configuration = DraftConfigurationV4.model_validate(
        {
            "schema_version": 4,
            "edit_version": 0,
            "caller_policy": manifest["caller_policy"],
            "schedules": manifest["schedules"],
            "recording_behavior": manifest["recording_behavior"],
            "flow": manifest["flow"],
        },
    )
    prompts = {item["id"] for item in manifest["prompts"]}
    sizes = {item["id"]: item["size_bytes"] for item in manifest["prompts"]}
    assert validate_configuration_v4(configuration, prompts, sizes, 128 * 1024 * 1024) == []
    compiled = compile_flow_v4(
        configuration.flow,
        configuration.recording_behavior,
        {item["id"]: item["duration_ms"] for item in manifest["prompts"]},
    )
    assert compiled["compiler_version"] == "4.1.0"
    assert compiled["source_sha256"] == manifest["source_sha256"]
    assert compiled["program_sha256"] == manifest["program_sha256"]
    assert compiled["program"] == manifest["program"]
    assert " BARGE_IN\n" in (
        FIXTURES / "configuration_revision_v41.compiled.txt"
    ).read_text()
