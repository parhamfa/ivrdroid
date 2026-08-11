from __future__ import annotations

from app.compiler_v3 import compile_flow_v3
from app.schemas import DraftConfigurationV2, DraftConfigurationV3
from app.simulation_v3 import simulate_v3
from app.validation import validate_configuration_v3


def opaque_id(number: int) -> str:
    return f"00000000-0000-4000-8000-{number:012x}"


PROMPT = opaque_id(900)


def recording_flow(start: int = 1) -> dict:
    return {
        "root": {
            "block_id": opaque_id(start),
            "type": "play_prompt",
            "prompt_id": PROMPT,
            "next": {
                "block_id": opaque_id(start + 1),
                "type": "record_message",
                "next": {"block_id": opaque_id(start + 2), "type": "end_call"},
                "on_unavailable": {"block_id": opaque_id(start + 3), "type": "end_call"},
            },
        },
    }


def configuration(flow: dict | None = None, finish_key: str | None = "#") -> DraftConfigurationV3:
    return DraftConfigurationV3.model_validate(
        {
            "schema_version": 3,
            "edit_version": 1,
            "recording_behavior": {
                "maximum_duration_seconds": 60,
                "finish_key": finish_key,
            },
            "flow": flow or recording_flow(),
        },
    )


def test_record_message_requires_a_separate_immediate_greeting() -> None:
    valid = validate_configuration_v3(
        configuration(),
        {PROMPT},
        {PROMPT: 4096},
        128 * 1024 * 1024,
    )
    assert valid == []

    root = recording_flow()["root"]["next"]
    invalid = configuration({"root": root})
    errors = validate_configuration_v3(invalid, {PROMPT}, {PROMPT: 4096}, 128 * 1024 * 1024)
    assert "Record message must immediately follow a Play prompt greeting." in errors


def test_simulation_exposes_recorded_unavailable_and_hangup_outcomes() -> None:
    waiting = simulate_v3(configuration(), [])
    assert waiting["status"] == "awaiting_recording"
    assert waiting["available_recording_outcomes"] == ["recorded", "unavailable", "hangup"]

    recorded = simulate_v3(configuration(), ["recorded"])
    assert recorded["status"] == "complete"
    assert recorded["trace"][1]["event"] == "recording:recorded"
    assert recorded["trace"][-1]["block_id"] == opaque_id(3)

    unavailable = simulate_v3(configuration(), ["unavailable"])
    assert unavailable["trace"][-1]["block_id"] == opaque_id(4)
    hangup = simulate_v3(configuration(), ["hangup"])
    assert hangup["status"] == "complete"
    assert "finalized" in hangup["message"]


def test_compiler_is_deterministic_with_enabled_or_disabled_finish_key() -> None:
    enabled = configuration()
    first = compile_flow_v3(enabled.flow, enabled.recording_behavior, {PROMPT: 1000})
    second = compile_flow_v3(enabled.flow, enabled.recording_behavior, {PROMPT: 1000})
    assert first == second
    record = first["program"]["instructions"][1]
    assert record["finish_key"] == "#"
    assert first["program"]["maximum_session_ms"] == 61_500

    disabled = configuration(finish_key=None)
    compiled = compile_flow_v3(disabled.flow, disabled.recording_behavior, {PROMPT: 1000})
    assert compiled["program"]["instructions"][1]["finish_key"] is None
    assert compiled["source_sha256"] != first["source_sha256"]


def test_compiler_rejects_a_worst_case_session_over_ten_minutes() -> None:
    root = recording_flow()["root"]
    current_record = root["next"]
    for index in range(3):
        following = recording_flow(10 + index * 10)["root"]
        current_record["next"] = following
        current_record = following["next"]
    document = configuration({"root": root})
    long_behavior = document.recording_behavior.model_copy(
        update={"maximum_duration_seconds": 180},
    )
    try:
        compile_flow_v3(document.flow, long_behavior, {PROMPT: 1000})
    except ValueError as error:
        assert "ten-minute" in str(error)
    else:  # pragma: no cover - guards a safety regression
        raise AssertionError("unbounded recording flow compiled")


def test_v2_contract_rejects_record_message_but_remains_parseable() -> None:
    try:
        DraftConfigurationV2.model_validate(
            {"schema_version": 2, "edit_version": 0, "flow": recording_flow()},
        )
    except ValueError as error:
        assert "cannot contain record_message" in str(error)
    else:  # pragma: no cover
        raise AssertionError("V2 accepted a V3 recording block")
