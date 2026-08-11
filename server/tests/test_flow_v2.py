from __future__ import annotations

from copy import deepcopy

from app.compiler_v2 import compile_flow
from app.diff_v2 import diff_flows
from app.schemas import DraftConfigurationV2, FlowDefinitionV2
from app.simulation_v2 import simulate
from app.validation import validate_configuration_v2


def opaque_id(number: int) -> str:
    return f"00000000-0000-4000-8000-{number:012x}"


PROMPT_ONE = opaque_id(101)
PROMPT_TWO = opaque_id(102)


def complete_menu_document() -> dict:
    return {
        "root": {
            "block_id": opaque_id(1),
            "type": "collect_digit",
            "prompt_id": None,
            "timeout_ms": 5000,
            "maximum_attempts": 3,
            "maximum_menu_returns": 2,
            "branches": [
                {
                    "branch_id": opaque_id(20),
                    "digit": "2",
                    "root": {
                        "block_id": opaque_id(2),
                        "type": "play_prompt",
                        "prompt_id": PROMPT_TWO,
                        "next": {"block_id": opaque_id(3), "type": "end_call"},
                    },
                },
                {
                    "branch_id": opaque_id(10),
                    "digit": "1",
                    "root": {
                        "block_id": opaque_id(4),
                        "type": "play_prompt",
                        "prompt_id": PROMPT_ONE,
                        "next": {"block_id": opaque_id(5), "type": "return_to_menu"},
                    },
                },
            ],
            "on_timeout": {"block_id": opaque_id(6), "type": "end_call"},
            "on_invalid": {"block_id": opaque_id(7), "type": "end_call"},
            "on_return_limit": {"block_id": opaque_id(8), "type": "end_call"},
        },
    }


def configuration(flow: dict | None = None) -> DraftConfigurationV2:
    return DraftConfigurationV2.model_validate(
        {
            "schema_version": 2,
            "edit_version": 1,
            "flow": flow or complete_menu_document(),
        },
    )


def test_owned_branches_compile_to_independent_prompt_instructions() -> None:
    flow = configuration().flow
    first = compile_flow(flow)
    second = compile_flow(FlowDefinitionV2.model_validate(flow.model_dump(mode="json")))
    assert first == second

    instructions = first["program"]["instructions"]
    menu = instructions[0]
    assert [branch["digit"] for branch in menu["branches"]] == ["1", "2"]
    targets = [branch["target_pc"] for branch in menu["branches"]]
    assert targets[0] != targets[1]
    assert instructions[targets[0]]["prompt_id"] == PROMPT_ONE
    assert instructions[targets[1]]["prompt_id"] == PROMPT_TWO
    assert instructions[targets[0]]["block_id"] != instructions[targets[1]]["block_id"]


def test_validation_rejects_duplicate_owned_block_and_incomplete_path() -> None:
    document = complete_menu_document()
    document["root"]["branches"][1]["root"]["next"]["block_id"] = opaque_id(3)
    document["root"]["on_invalid"] = None
    errors = validate_configuration_v2(
        configuration(document),
        {PROMPT_ONE, PROMPT_TWO},
        {PROMPT_ONE: 100, PROMPT_TWO: 100},
        128 * 1024 * 1024,
    )
    assert "A flow block is duplicated or shared between branches." in errors
    assert "Invalid input has no first step." in errors


def test_bounded_menu_return_uses_fallback_after_two_returns() -> None:
    result = simulate(configuration(), ["1", "1", "1"])
    assert result["status"] == "complete"
    return_events = [step.get("event") for step in result["trace"] if step["type"] == "return_to_menu"]
    assert return_events == ["menu-return:1/2", "menu-return:2/2", "menu-return:limit"]
    assert result["trace"][-1]["block_id"] == opaque_id(8)


def test_schedule_simulation_requires_an_explicit_outcome() -> None:
    document = configuration(
        {
            "root": {
                "block_id": opaque_id(30),
                "type": "schedule_branch",
                "schedule_id": "office",
                "on_open": {"block_id": opaque_id(31), "type": "end_call"},
                "on_closed": {"block_id": opaque_id(32), "type": "end_call"},
                "on_holiday": {"block_id": opaque_id(33), "type": "end_call"},
            },
        },
    )
    waiting = simulate(document, [])
    assert waiting["status"] == "awaiting_schedule"
    assert waiting["available_schedule_states"] == ["open", "closed", "holiday"]

    expected_targets = {
        "open": opaque_id(31),
        "closed": opaque_id(32),
        "holiday": opaque_id(33),
    }
    for state, target in expected_targets.items():
        result = simulate(document, [state])
        assert result["status"] == "complete"
        assert result["trace"][0]["event"] == f"schedule:{state}"
        assert result["trace"][-1]["block_id"] == target


def test_validation_rejects_missing_and_oversized_prompt_assets() -> None:
    flow = {
        "root": {
            "block_id": opaque_id(40),
            "type": "play_prompt",
            "prompt_id": PROMPT_ONE,
            "next": {"block_id": opaque_id(41), "type": "end_call"},
        },
    }
    missing = validate_configuration_v2(configuration(flow), set(), {}, 1024)
    assert "A Play prompt step references a missing prompt." in missing

    asset_limit = 128 * 1024 * 1024
    oversized = validate_configuration_v2(
        configuration(flow),
        {PROMPT_ONE},
        {PROMPT_ONE: asset_limit + 1},
        asset_limit,
    )
    assert "Revision prompt assets exceed the 128 MiB limit." in oversized


def test_tree_diff_tracks_branch_subtree_removal_by_stable_uuid() -> None:
    base = configuration().flow
    changed_document = deepcopy(complete_menu_document())
    changed_document["root"]["branches"] = changed_document["root"]["branches"][:1]
    changed = configuration(changed_document).flow
    result = diff_flows(base, changed)
    assert result["removed"] == 2
    assert result["changed"] == 1
    assert "Removed Play prompt" in result["changes"]
    assert "Removed Return to menu" in result["changes"]
