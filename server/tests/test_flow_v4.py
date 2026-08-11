from __future__ import annotations

import pytest
from pydantic import ValidationError

from app.compiler_v4 import compile_flow_v4
from app.schemas import (
    DeviceStatus,
    DraftConfigurationV2,
    DraftConfigurationV3,
    DraftConfigurationV4,
)
from app.simulation_v4 import simulate_v4
from app.validation import validate_configuration_v4


def opaque_id(number: int) -> str:
    return f"00000000-0000-4000-8000-{number:012x}"


PROMPT = opaque_id(900)


def test_split_recording_spool_status_fields_are_bounded() -> None:
    required = {
        "helper_state": "READY",
        "helper_result": "NONE",
        "call_state": "idle",
        "local_kill_switch": False,
        "storage_free_bytes": 1,
    }
    fields = (
        "voicemail_spool_bytes",
        "voicemail_spool_count",
        "conversation_spool_bytes",
        "conversation_spool_count",
        "recording_filesystem_free_bytes",
    )
    for field in fields:
        with pytest.raises(ValidationError):
            DeviceStatus.model_validate({**required, field: -1})

    for field in (
        "voicemail_spool_bytes",
        "conversation_spool_bytes",
        "recording_filesystem_free_bytes",
    ):
        with pytest.raises(ValidationError):
            DeviceStatus.model_validate({**required, field: 9_223_372_036_854_775_808})
    for field in ("voicemail_spool_count", "conversation_spool_count"):
        with pytest.raises(ValidationError):
            DeviceStatus.model_validate({**required, field: 2_147_483_648})


def external_flow() -> dict:
    return {
        "root": {
            "block_id": opaque_id(1),
            "type": "play_prompt",
            "prompt_id": PROMPT,
            "next": {
                "block_id": opaque_id(2),
                "type": "external_call",
                "phone_number": "03136644636",
                "answer_timeout_seconds": 30,
                "next": {"block_id": opaque_id(3), "type": "end_call"},
                "on_not_connected": {"block_id": opaque_id(4), "type": "end_call"},
                "on_system_failure": {"block_id": opaque_id(5), "type": "end_call"},
            },
        },
    }


def collect_flow(allow_prompt_barge_in: bool = False, prompt_id: str | None = PROMPT) -> dict:
    collector = {
        "block_id": opaque_id(20),
        "type": "collect_digit",
        "prompt_id": prompt_id,
        "timeout_ms": 5000,
        "maximum_attempts": 3,
        "maximum_menu_returns": 2,
        "branches": [
            {
                "branch_id": opaque_id(21),
                "digit": "1",
                "root": {"block_id": opaque_id(22), "type": "end_call"},
            },
        ],
        "on_timeout": {"block_id": opaque_id(23), "type": "end_call"},
        "on_invalid": {"block_id": opaque_id(24), "type": "end_call"},
        "on_return_limit": {"block_id": opaque_id(25), "type": "end_call"},
    }
    if allow_prompt_barge_in:
        collector["allow_prompt_barge_in"] = True
    return {"root": collector}


def collect_recorded_flow(kind: str, prompt_id: str | None = PROMPT) -> dict:
    flow = collect_flow(prompt_id=prompt_id)
    if kind == "record_message":
        recorded = {
            "block_id": opaque_id(30),
            "type": "record_message",
            "next": {"block_id": opaque_id(31), "type": "end_call"},
            "on_unavailable": {"block_id": opaque_id(32), "type": "end_call"},
        }
    else:
        recorded = {
            "block_id": opaque_id(30),
            "type": "external_call",
            "phone_number": "03136644636",
            "answer_timeout_seconds": 30,
            "next": {"block_id": opaque_id(31), "type": "end_call"},
            "on_not_connected": {"block_id": opaque_id(32), "type": "end_call"},
            "on_system_failure": {"block_id": opaque_id(33), "type": "end_call"},
        }
    flow["root"]["branches"][0]["root"] = recorded
    return flow


def configuration(flow: dict | None = None) -> DraftConfigurationV4:
    return DraftConfigurationV4.model_validate(
        {
            "schema_version": 4,
            "edit_version": 1,
            "flow": flow or external_flow(),
        },
    )


def test_external_call_contract_validation_and_v3_isolation() -> None:
    document = configuration()
    assert validate_configuration_v4(
        document,
        {PROMPT},
        {PROMPT: 4096},
        128 * 1024 * 1024,
    ) == []

    external = external_flow()["root"]["next"]
    errors = validate_configuration_v4(
        configuration({"root": external}),
        {PROMPT},
        {PROMPT: 4096},
        128 * 1024 * 1024,
    )
    assert "External call requires an earlier Play prompt or menu prompt notice on this path." in errors

    with pytest.raises(ValueError, match="FlowDocumentV3 cannot contain external_call"):
        DraftConfigurationV3.model_validate(
            {"schema_version": 3, "edit_version": 1, "flow": external_flow()},
        )


@pytest.mark.parametrize("phone_number", ["+989121234567", "03136644636"])
def test_external_call_number_contract_accepts_local_or_plus_digits(phone_number: str) -> None:
    flow = external_flow()
    flow["root"]["next"]["phone_number"] = phone_number
    assert configuration(flow).flow.root.next.phone_number == phone_number


@pytest.mark.parametrize(
    "phone_number",
    [
        "031-3664-4636",
        "1234567",
        "++989121234567",
        "00000110",
        "009800110",
        "10000911",
    ],
)
def test_external_call_number_contract_rejects_ambiguous_values(phone_number: str) -> None:
    flow = external_flow()
    flow["root"]["next"]["phone_number"] = phone_number
    with pytest.raises(ValueError):
        configuration(flow)


def test_v4_compiler_is_deterministic_and_emits_locked_instruction_shape() -> None:
    document = configuration()
    first = compile_flow_v4(document.flow, document.recording_behavior, {PROMPT: 1000})
    second = compile_flow_v4(document.flow, document.recording_behavior, {PROMPT: 1000})
    assert first == second
    assert first["compiler_version"] == "4.0.0"
    assert first["program"]["version"] == 4
    assert first["program"]["maximum_automated_session_ms"] == 61_000
    assert first["program"]["instructions"][1] == {
        "pc": 1,
        "block_id": opaque_id(2),
        "op": "external_call",
        "phone_number": "03136644636",
        "answer_timeout_ms": 30_000,
        "next_pc": 2,
        "on_not_connected_pc": 3,
        "on_system_failure_pc": 4,
    }


def test_prompt_barge_in_defaults_off_and_v40_output_remains_unchanged() -> None:
    document = configuration(collect_flow())
    serialized = document.model_dump(mode="json")
    assert "allow_prompt_barge_in" not in serialized["flow"]["root"]
    compiled = compile_flow_v4(document.flow, document.recording_behavior, {PROMPT: 1000})
    assert compiled["compiler_version"] == "4.0.0"
    collector = compiled["program"]["instructions"][0]
    assert "allow_prompt_barge_in" not in collector


def test_promptless_digit_branch_compiles_and_simulates_directly_to_its_action() -> None:
    document = configuration(collect_flow(prompt_id=None))
    assert validate_configuration_v4(document, set(), {}, 128 * 1024 * 1024) == []

    compiled = compile_flow_v4(document.flow, document.recording_behavior, {})
    collector = compiled["program"]["instructions"][0]
    branch_target = collector["branches"][0]["target_pc"]
    assert compiled["program"]["instructions"][branch_target]["op"] == "end_call"

    simulated = simulate_v4(document, ["1"])
    assert simulated["status"] == "complete"
    assert [step["type"] for step in simulated["trace"]] == ["collect_digit", "end_call"]


@pytest.mark.parametrize(
    ("kind", "expected_status"),
    [("record_message", "awaiting_recording"), ("external_call", "awaiting_external")],
)
def test_menu_prompt_covers_direct_recorded_branch_action(kind: str, expected_status: str) -> None:
    document = configuration(collect_recorded_flow(kind))
    assert validate_configuration_v4(
        document,
        {PROMPT},
        {PROMPT: 4096},
        128 * 1024 * 1024,
    ) == []

    compiled = compile_flow_v4(document.flow, document.recording_behavior, {PROMPT: 1000})
    collector = compiled["program"]["instructions"][0]
    branch_target = collector["branches"][0]["target_pc"]
    assert compiled["program"]["instructions"][branch_target]["op"] == kind

    simulated = simulate_v4(document, ["1"])
    assert simulated["status"] == expected_status
    assert [step["type"] for step in simulated["trace"]] == ["collect_digit", kind]


@pytest.mark.parametrize(
    ("kind", "message"),
    [
        ("record_message", "Record message requires an earlier Play prompt or menu prompt notice on this path."),
        ("external_call", "External call requires an earlier Play prompt or menu prompt notice on this path."),
    ],
)
def test_recorded_branch_action_still_requires_an_earlier_prompt(kind: str, message: str) -> None:
    document = configuration(collect_recorded_flow(kind, prompt_id=None))
    assert message in validate_configuration_v4(document, set(), {}, 128 * 1024 * 1024)
    with pytest.raises(ValueError, match="requires an earlier Play prompt or menu prompt notice"):
        compile_flow_v4(document.flow, document.recording_behavior, {})


def test_prompt_barge_in_emits_deterministic_v41_and_is_v4_only() -> None:
    document = configuration(collect_flow(True))
    first = compile_flow_v4(document.flow, document.recording_behavior, {PROMPT: 1000})
    second = compile_flow_v4(document.flow, document.recording_behavior, {PROMPT: 1000})
    assert first == second
    assert first["compiler_version"] == "4.1.0"
    assert first["program"]["instructions"][0]["allow_prompt_barge_in"] is True

    with pytest.raises(ValueError, match="Prompt interruption requires a menu prompt"):
        configuration(collect_flow(True, None))
    with pytest.raises(ValueError, match="FlowDocumentV2 cannot enable prompt interruption"):
        DraftConfigurationV2.model_validate(
            {"schema_version": 2, "edit_version": 1, "flow": collect_flow(True)},
        )
    with pytest.raises(ValueError, match="FlowDocumentV3 cannot enable prompt interruption"):
        DraftConfigurationV3.model_validate(
            {"schema_version": 3, "edit_version": 1, "flow": collect_flow(True)},
        )


def test_v4_compiler_bounds_setup_recorder_answer_and_merge_watchdogs() -> None:
    tail: dict = {"block_id": opaque_id(500), "type": "end_call"}
    for index in reversed(range(10)):
        tail = {
            "block_id": opaque_id(100 + index * 4),
            "type": "play_prompt",
            "prompt_id": PROMPT,
            "next": {
                "block_id": opaque_id(101 + index * 4),
                "type": "external_call",
                "phone_number": "03136644636",
                "answer_timeout_seconds": 30,
                "next": tail,
                "on_not_connected": {
                    "block_id": opaque_id(102 + index * 4),
                    "type": "end_call",
                },
                "on_system_failure": {
                    "block_id": opaque_id(103 + index * 4),
                    "type": "end_call",
                },
            },
        }
    document = configuration({"root": tail})
    with pytest.raises(ValueError, match="ten-minute"):
        compile_flow_v4(document.flow, document.recording_behavior, {PROMPT: 1000})


@pytest.mark.parametrize(
    ("outcome", "terminal_block"),
    [
        ("external_completed", opaque_id(3)),
        ("external_not_connected", opaque_id(4)),
        ("external_system_failure", opaque_id(5)),
    ],
)
def test_v4_simulation_exposes_all_external_outcomes(outcome: str, terminal_block: str) -> None:
    waiting = simulate_v4(configuration(), [])
    assert waiting["status"] == "awaiting_external"
    assert waiting["available_external_outcomes"] == [
        "external_completed",
        "external_not_connected",
        "external_system_failure",
    ]
    completed = simulate_v4(configuration(), [outcome])
    assert completed["status"] == "complete"
    assert completed["trace"][1]["event"] == outcome
    assert completed["trace"][-1]["block_id"] == terminal_block
