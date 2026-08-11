from __future__ import annotations

from collections.abc import Iterator

from .schemas import (
    CollectDigitBlockV2,
    DraftConfigurationV2,
    EndCallBlockV2,
    ExternalCallBlockV4,
    FlowBlockV2,
    PlayPromptBlockV2,
    RecordMessageBlockV3,
    ReturnToMenuBlockV2,
    ScheduleBranchBlockV2,
)


DTMF_ORDER = "0123456789*#"


def owned_children(block: FlowBlockV2) -> list[tuple[str, FlowBlockV2 | None]]:
    if isinstance(block, PlayPromptBlockV2):
        return [("next", block.next)]
    if isinstance(block, CollectDigitBlockV2):
        branches = sorted(block.branches, key=lambda item: DTMF_ORDER.index(item.digit))
        return [
            *((f"digit {branch.digit}", branch.root) for branch in branches),
            ("no input", block.on_timeout),
            ("invalid", block.on_invalid),
            ("return limit", block.on_return_limit),
        ]
    if isinstance(block, ScheduleBranchBlockV2):
        return [
            ("open", block.on_open),
            ("closed", block.on_closed),
            ("holiday", block.on_holiday),
        ]
    if isinstance(block, RecordMessageBlockV3):
        return [
            ("recorded", block.next),
            ("unavailable", block.on_unavailable),
        ]
    if isinstance(block, ExternalCallBlockV4):
        return [
            ("completed", block.next),
            ("not connected", block.on_not_connected),
            ("system failure", block.on_system_failure),
        ]
    if isinstance(block, (ReturnToMenuBlockV2, EndCallBlockV2)):
        return []
    raise TypeError(f"Unsupported V2 block: {type(block)!r}")


def walk_blocks(root: FlowBlockV2 | None) -> Iterator[tuple[FlowBlockV2, int]]:
    if root is None:
        return
    pending: list[tuple[FlowBlockV2, int]] = [(root, 1)]
    while pending:
        block, depth = pending.pop()
        yield block, depth
        children = [child for _, child in owned_children(block) if child is not None]
        pending.extend((child, depth + 1) for child in reversed(children))


def prompt_ids(configuration: DraftConfigurationV2) -> set[str]:
    result: set[str] = set()
    for block, _ in walk_blocks(configuration.flow.root):
        if isinstance(block, (PlayPromptBlockV2, CollectDigitBlockV2)) and block.prompt_id:
            result.add(block.prompt_id)
    return result


def block_label(block: FlowBlockV2) -> str:
    if isinstance(block, PlayPromptBlockV2):
        return "Play prompt"
    if isinstance(block, CollectDigitBlockV2):
        return "Collect one digit"
    if isinstance(block, ScheduleBranchBlockV2):
        return "Check schedule"
    if isinstance(block, ReturnToMenuBlockV2):
        return "Return to menu"
    if isinstance(block, RecordMessageBlockV3):
        return "Record message"
    if isinstance(block, ExternalCallBlockV4):
        return "External call"
    if isinstance(block, EndCallBlockV2):
        return "End call"
    raise TypeError(f"Unsupported V2 block: {type(block)!r}")


def source_document(configuration: DraftConfigurationV2) -> dict:
    return configuration.model_dump(mode="json", exclude={"edit_version"})
