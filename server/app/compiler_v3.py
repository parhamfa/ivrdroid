from __future__ import annotations

import hashlib
from functools import lru_cache

from .crypto import canonical_json_bytes
from .flow_v2 import DTMF_ORDER
from .schemas import (
    CollectDigitBlockV2,
    EndCallBlockV2,
    FlowDefinitionV2,
    PlayPromptBlockV2,
    RecordMessageBlockV3,
    RecordingBehavior,
    ReturnToMenuBlockV2,
    ScheduleBranchBlockV2,
)


COMPILER_VERSION = "3.0.0"
MAXIMUM_SESSION_MS = 600_000
BEEP_DURATION_MS = 500


def _sha256(document: dict) -> str:
    return hashlib.sha256(canonical_json_bytes(document)).hexdigest()


def compile_flow_v3(
    flow: FlowDefinitionV2,
    recording_behavior: RecordingBehavior,
    prompt_durations_ms: dict[str, int],
) -> dict:
    if flow.root is None:
        raise ValueError("A published flow requires a root block.")

    entries: list[tuple[object, str | None]] = []
    seen: set[str] = set()

    def collect(block, owner_menu_id: str | None, parent_is_prompt: bool = False) -> None:
        if block.block_id in seen:
            raise ValueError(f"Block {block.block_id} is reused or duplicated.")
        seen.add(block.block_id)
        entries.append((block, owner_menu_id))
        if isinstance(block, PlayPromptBlockV2):
            if block.next is None:
                raise ValueError("Play prompt has no next step.")
            collect(block.next, owner_menu_id, True)
        elif isinstance(block, CollectDigitBlockV2):
            for branch in sorted(block.branches, key=lambda item: DTMF_ORDER.index(item.digit)):
                if branch.root is None:
                    raise ValueError(f"Digit {branch.digit} has no first step.")
                collect(branch.root, block.block_id)
            for name, child in (
                ("No input", block.on_timeout),
                ("Invalid", block.on_invalid),
                ("Return limit", block.on_return_limit),
            ):
                if child is None:
                    raise ValueError(f"{name} has no first step.")
                collect(child, block.block_id)
        elif isinstance(block, ScheduleBranchBlockV2):
            for name, child in (
                ("Open", block.on_open),
                ("Closed", block.on_closed),
                ("Holiday", block.on_holiday),
            ):
                if child is None:
                    raise ValueError(f"{name} has no first step.")
                collect(child, owner_menu_id)
        elif isinstance(block, RecordMessageBlockV3):
            if not parent_is_prompt:
                raise ValueError("Record message must immediately follow a Play prompt greeting.")
            if block.next is None or block.on_unavailable is None:
                raise ValueError("Record message requires recorded and unavailable paths.")
            collect(block.next, owner_menu_id)
            collect(block.on_unavailable, owner_menu_id)
        elif isinstance(block, ReturnToMenuBlockV2):
            if owner_menu_id is None:
                raise ValueError("Return to menu is outside a digit menu.")
        elif not isinstance(block, EndCallBlockV2):
            raise TypeError(f"Unsupported block {type(block)!r}")

    collect(flow.root, None)
    program_counter = {block.block_id: index for index, (block, _) in enumerate(entries)}

    def pc(block) -> int:
        return program_counter[block.block_id]

    instructions: list[dict] = []
    for index, (block, owner_menu_id) in enumerate(entries):
        instruction: dict = {"pc": index, "block_id": block.block_id, "op": block.type}
        if isinstance(block, PlayPromptBlockV2):
            instruction.update(prompt_id=block.prompt_id, next_pc=pc(block.next))
        elif isinstance(block, CollectDigitBlockV2):
            instruction.update(
                prompt_id=block.prompt_id,
                timeout_ms=block.timeout_ms,
                maximum_attempts=block.maximum_attempts,
                maximum_menu_returns=block.maximum_menu_returns,
                branches=[
                    {"digit": branch.digit, "target_pc": pc(branch.root)}
                    for branch in sorted(block.branches, key=lambda item: DTMF_ORDER.index(item.digit))
                ],
                on_timeout_pc=pc(block.on_timeout),
                on_invalid_pc=pc(block.on_invalid),
                on_return_limit_pc=pc(block.on_return_limit),
            )
        elif isinstance(block, ScheduleBranchBlockV2):
            instruction.update(
                schedule_id=block.schedule_id,
                on_open_pc=pc(block.on_open),
                on_closed_pc=pc(block.on_closed),
                on_holiday_pc=pc(block.on_holiday),
            )
        elif isinstance(block, RecordMessageBlockV3):
            instruction.update(
                maximum_duration_ms=recording_behavior.maximum_duration_seconds * 1000,
                finish_key=recording_behavior.finish_key,
                next_pc=pc(block.next),
                on_unavailable_pc=pc(block.on_unavailable),
            )
        elif isinstance(block, ReturnToMenuBlockV2):
            instruction["menu_pc"] = program_counter[owner_menu_id]
        instructions.append(instruction)

    collect_pcs = [item["pc"] for item in instructions if item["op"] == "collect_digit"]
    collect_index = {pc: index for index, pc in enumerate(collect_pcs)}
    active: set[tuple[int, tuple[int, ...]]] = set()

    @lru_cache(maxsize=None)
    def maximum_cost(pc_value: int, returns: tuple[int, ...]) -> int:
        state = (pc_value, returns)
        if state in active:
            raise ValueError("V3 program has an unbounded runtime cycle.")
        active.add(state)
        try:
            instruction = instructions[pc_value]
            operation = instruction["op"]
            if operation == "end_call":
                return 0
            if operation == "play_prompt":
                duration = prompt_durations_ms.get(instruction["prompt_id"], 0)
                return duration + maximum_cost(instruction["next_pc"], returns)
            if operation == "collect_digit":
                prompt_duration = prompt_durations_ms.get(instruction["prompt_id"], 0)
                wait = instruction["maximum_attempts"] * (
                    prompt_duration + instruction["timeout_ms"]
                )
                targets = [branch["target_pc"] for branch in instruction["branches"]]
                targets.extend(
                    [instruction["on_timeout_pc"], instruction["on_invalid_pc"]],
                )
                return wait + max(maximum_cost(target, returns) for target in targets)
            if operation == "schedule_branch":
                return max(
                    maximum_cost(instruction[key], returns)
                    for key in ("on_open_pc", "on_closed_pc", "on_holiday_pc")
                )
            if operation == "record_message":
                recorded = (
                    BEEP_DURATION_MS
                    + instruction["maximum_duration_ms"]
                    + maximum_cost(instruction["next_pc"], returns)
                )
                # Storage is checked before the beep today, but keep the same
                # bound if a late capture allocation failure must route to the
                # unavailable branch after the beep.
                unavailable = BEEP_DURATION_MS + maximum_cost(
                    instruction["on_unavailable_pc"],
                    returns,
                )
                return max(recorded, unavailable)
            if operation == "return_to_menu":
                menu_pc = instruction["menu_pc"]
                index = collect_index[menu_pc]
                menu = instructions[menu_pc]
                counts = list(returns)
                if counts[index] < menu["maximum_menu_returns"]:
                    counts[index] += 1
                    return maximum_cost(menu_pc, tuple(counts))
                return maximum_cost(menu["on_return_limit_pc"], returns)
            raise ValueError(f"Unsupported V3 operation: {operation}")
        finally:
            active.remove(state)

    initial_returns = tuple(0 for _ in collect_pcs)
    maximum_session_ms = maximum_cost(0, initial_returns)
    if maximum_session_ms > MAXIMUM_SESSION_MS:
        raise ValueError("Worst-case flow duration exceeds the ten-minute limit.")

    source = {
        "flow": flow.model_dump(mode="json"),
        "recording_behavior": recording_behavior.model_dump(mode="json"),
    }
    program = {
        "version": 3,
        "entry_pc": 0,
        "maximum_session_ms": maximum_session_ms,
        "instructions": instructions,
    }
    return {
        "compiler_version": COMPILER_VERSION,
        "source_sha256": _sha256(source),
        "program_sha256": _sha256(program),
        "program": program,
    }
