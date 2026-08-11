from __future__ import annotations

import hashlib

from .crypto import canonical_json_bytes
from .flow_v2 import DTMF_ORDER
from .schemas import (
    CollectDigitBlockV2,
    EndCallBlockV2,
    FlowDefinitionV2,
    PlayPromptBlockV2,
    ReturnToMenuBlockV2,
    ScheduleBranchBlockV2,
)


COMPILER_VERSION = "2.0.0"


def _sha256(document: dict) -> str:
    return hashlib.sha256(canonical_json_bytes(document)).hexdigest()


def compile_flow(flow: FlowDefinitionV2) -> dict:
    if flow.root is None:
        raise ValueError("A published flow requires a root block.")

    entries: list[tuple[object, str | None]] = []
    seen: set[str] = set()

    def collect(block, owner_menu_id: str | None) -> None:
        if block.block_id in seen:
            raise ValueError(f"Block {block.block_id} is reused or duplicated.")
        seen.add(block.block_id)
        entries.append((block, owner_menu_id))
        if isinstance(block, PlayPromptBlockV2):
            if block.next is None:
                raise ValueError("Play prompt has no next step.")
            collect(block.next, owner_menu_id)
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
        instruction: dict = {
            "pc": index,
            "block_id": block.block_id,
            "op": block.type,
        }
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
        elif isinstance(block, ReturnToMenuBlockV2):
            instruction["menu_pc"] = program_counter[owner_menu_id]
        instructions.append(instruction)

    source = flow.model_dump(mode="json")
    program = {"version": 2, "entry_pc": 0, "instructions": instructions}
    return {
        "compiler_version": COMPILER_VERSION,
        "source_sha256": _sha256(source),
        "program_sha256": _sha256(program),
        "program": program,
    }
