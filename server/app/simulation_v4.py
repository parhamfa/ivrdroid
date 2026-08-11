from __future__ import annotations

from .flow_v2 import block_label, walk_blocks
from .schemas import (
    CollectDigitBlockV2,
    DraftConfigurationV4,
    EndCallBlockV2,
    ExternalCallBlockV4,
    PlayPromptBlockV2,
    RecordMessageBlockV3,
    ReturnToMenuBlockV2,
    ScheduleBranchBlockV2,
)


EXTERNAL_OUTCOMES = [
    "external_completed",
    "external_not_connected",
    "external_system_failure",
]


def simulate_v4(configuration: DraftConfigurationV4, events: list[str]) -> dict:
    root = configuration.flow.root
    if root is None:
        return {"status": "invalid", "trace": [], "message": "The flow is empty."}

    blocks = {block.block_id: block for block, _ in walk_blocks(root)}
    return_owners: dict[str, str] = {}

    def index_owners(block, owner: str | None) -> None:
        if isinstance(block, PlayPromptBlockV2) and block.next is not None:
            index_owners(block.next, owner)
        elif isinstance(block, CollectDigitBlockV2):
            for branch in block.branches:
                if branch.root is not None:
                    index_owners(branch.root, block.block_id)
            for child in (block.on_timeout, block.on_invalid, block.on_return_limit):
                if child is not None:
                    index_owners(child, block.block_id)
        elif isinstance(block, ScheduleBranchBlockV2):
            for child in (block.on_open, block.on_closed, block.on_holiday):
                if child is not None:
                    index_owners(child, owner)
        elif isinstance(block, RecordMessageBlockV3):
            for child in (block.next, block.on_unavailable):
                if child is not None:
                    index_owners(child, owner)
        elif isinstance(block, ExternalCallBlockV4):
            for child in (block.next, block.on_not_connected, block.on_system_failure):
                if child is not None:
                    index_owners(child, owner)
        elif isinstance(block, ReturnToMenuBlockV2) and owner is not None:
            return_owners[block.block_id] = owner

    index_owners(root, None)
    trace: list[dict] = []
    event_index = 0
    menu_returns: dict[str, int] = {}
    current = root

    for _ in range(512):
        step = {
            "block_id": current.block_id,
            "type": current.type,
            "label": block_label(current),
        }
        if isinstance(current, (PlayPromptBlockV2, CollectDigitBlockV2)):
            step["prompt_id"] = current.prompt_id
        trace.append(step)

        if isinstance(current, PlayPromptBlockV2):
            if current.next is None:
                return {"status": "invalid", "trace": trace, "message": "Play prompt has no next step."}
            current = current.next
            continue
        if isinstance(current, EndCallBlockV2):
            return {"status": "complete", "trace": trace, "message": "Call ended."}
        if isinstance(current, RecordMessageBlockV3):
            if event_index >= len(events):
                return {
                    "status": "awaiting_recording",
                    "trace": trace,
                    "awaiting_block_id": current.block_id,
                    "available_recording_outcomes": ["recorded", "unavailable", "hangup"],
                    "message": "Choose how this explicit recording step finishes.",
                }
            outcome = events[event_index]
            event_index += 1
            trace[-1]["event"] = f"recording:{outcome}"
            if outcome == "hangup":
                return {
                    "status": "complete",
                    "trace": trace,
                    "message": "Caller hangup finalized the message and ended the call.",
                }
            target = current.next if outcome == "recorded" else (
                current.on_unavailable if outcome == "unavailable" else None
            )
            if target is None:
                return {
                    "status": "invalid",
                    "trace": trace,
                    "message": f"Unknown or empty recording outcome: {outcome}.",
                }
            current = target
            continue
        if isinstance(current, ExternalCallBlockV4):
            if event_index >= len(events):
                return {
                    "status": "awaiting_external",
                    "trace": trace,
                    "awaiting_block_id": current.block_id,
                    "available_external_outcomes": EXTERNAL_OUTCOMES,
                    "message": "Choose the verified external-call outcome.",
                }
            outcome = events[event_index]
            event_index += 1
            target = {
                "external_completed": current.next,
                "external_not_connected": current.on_not_connected,
                "external_system_failure": current.on_system_failure,
            }.get(outcome)
            if target is None:
                return {
                    "status": "invalid",
                    "trace": trace,
                    "message": f"Unknown or empty external-call outcome: {outcome}.",
                }
            trace[-1]["event"] = outcome
            current = target
            continue
        if isinstance(current, ReturnToMenuBlockV2):
            owner_id = return_owners.get(current.block_id)
            owner = blocks.get(owner_id or "")
            if not isinstance(owner, CollectDigitBlockV2):
                return {"status": "invalid", "trace": trace, "message": "Return to menu has no owning menu."}
            count = menu_returns.get(owner.block_id, 0)
            if count >= owner.maximum_menu_returns:
                trace[-1]["event"] = "menu-return:limit"
                if owner.on_return_limit is None:
                    return {"status": "invalid", "trace": trace, "message": "Menu return limit has no fallback."}
                current = owner.on_return_limit
            else:
                menu_returns[owner.block_id] = count + 1
                trace[-1]["event"] = f"menu-return:{count + 1}/{owner.maximum_menu_returns}"
                current = owner
            continue
        if isinstance(current, ScheduleBranchBlockV2):
            if event_index >= len(events):
                return {
                    "status": "awaiting_schedule",
                    "trace": trace,
                    "awaiting_block_id": current.block_id,
                    "available_schedule_states": ["open", "closed", "holiday"],
                    "message": "Choose the schedule result for this simulation.",
                }
            state = events[event_index]
            event_index += 1
            target = {
                "open": current.on_open,
                "closed": current.on_closed,
                "holiday": current.on_holiday,
            }.get(state)
            if target is None:
                return {"status": "invalid", "trace": trace, "message": f"Unknown schedule result: {state}."}
            trace[-1]["event"] = f"schedule:{state}"
            current = target
            continue
        if isinstance(current, CollectDigitBlockV2):
            branch_by_digit = {branch.digit: branch for branch in current.branches}
            routed = None
            for attempt in range(current.maximum_attempts):
                if event_index >= len(events):
                    return {
                        "status": "awaiting_input",
                        "trace": trace,
                        "awaiting_block_id": current.block_id,
                        "available_digits": sorted(branch_by_digit),
                        "message": f"Waiting for attempt {attempt + 1} of {current.maximum_attempts}.",
                    }
                event = events[event_index]
                event_index += 1
                if event in branch_by_digit:
                    routed = branch_by_digit[event].root
                    trace[-1]["event"] = f"digit:{event}"
                    break
                if event == "timeout":
                    trace[-1]["event"] = "timeout"
                    if attempt + 1 == current.maximum_attempts:
                        routed = current.on_timeout
                else:
                    trace[-1]["event"] = "invalid"
                    if attempt + 1 == current.maximum_attempts:
                        routed = current.on_invalid
            if routed is None:
                return {"status": "invalid", "trace": trace, "message": "The selected branch is empty."}
            current = routed
            continue
    return {"status": "invalid", "trace": trace, "message": "Simulation exceeded 512 transitions."}
