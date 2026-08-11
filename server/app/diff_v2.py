from __future__ import annotations

from .flow_v2 import block_label, walk_blocks
from .schemas import FlowDefinitionV2


CHILD_FIELDS = {
    "next",
    "branches",
    "on_timeout",
    "on_invalid",
    "on_return_limit",
    "on_open",
    "on_closed",
    "on_holiday",
    "on_unavailable",
    "on_not_connected",
    "on_system_failure",
}


def _block_documents(flow: FlowDefinitionV2) -> dict[str, tuple[str, dict]]:
    result: dict[str, tuple[str, dict]] = {}
    for block, _ in walk_blocks(flow.root):
        document = block.model_dump(mode="json", exclude=CHILD_FIELDS)
        if hasattr(block, "branches"):
            document["digits"] = sorted(branch.digit for branch in block.branches)
        result[block.block_id] = (block_label(block), document)
    return result


def diff_flows(base: FlowDefinitionV2 | None, current: FlowDefinitionV2) -> dict:
    before = {} if base is None else _block_documents(base)
    after = _block_documents(current)
    added_ids = sorted(set(after) - set(before))
    removed_ids = sorted(set(before) - set(after))
    changed_ids = sorted(
        block_id for block_id in set(before) & set(after)
        if before[block_id][1] != after[block_id][1]
    )
    changes = [*(f"Added {after[item][0]}" for item in added_ids)]
    changes.extend(f"Removed {before[item][0]}" for item in removed_ids)
    changes.extend(f"Changed {after[item][0]}" for item in changed_ids)
    return {
        "added": len(added_ids),
        "removed": len(removed_ids),
        "changed": len(changed_ids),
        "changes": changes[:256],
    }
