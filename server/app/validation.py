from __future__ import annotations

from collections.abc import Iterable
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

from .schemas import (
    CollectDigitNode,
    DraftConfiguration,
    DraftConfigurationV2,
    DraftConfigurationV3,
    DraftConfigurationV4,
    EndCallNode,
    EndCallBlockV2,
    ExternalCallBlockV4,
    PlayPromptNode,
    PlayPromptBlockV2,
    RecordMessageBlockV3,
    RepeatMenuNode,
    ReturnToMenuBlockV2,
    ScheduleBranchNode,
    ScheduleBranchBlockV2,
    CollectDigitBlockV2,
)


def validate_configuration(
    configuration: DraftConfiguration,
    prompt_ids: set[str],
    prompt_sizes: dict[str, int],
    revision_asset_limit_bytes: int,
) -> list[str]:
    errors: list[str] = []
    flow = configuration.flow
    nodes = {node.id: node for node in flow.nodes}
    if len(nodes) != len(flow.nodes):
        errors.append("Flow node IDs must be unique.")
        return errors
    if flow.root_node not in nodes:
        errors.append("Flow root node does not exist.")

    schedules = {schedule.id: schedule for schedule in configuration.schedules}
    if len(schedules) != len(configuration.schedules):
        errors.append("Schedule IDs must be unique.")
    for schedule in configuration.schedules:
        try:
            ZoneInfo(schedule.timezone)
        except ZoneInfoNotFoundError:
            errors.append(f"Schedule {schedule.id} has an unknown timezone.")

    used_prompts: set[str] = set()
    regular_edges: dict[str, list[str]] = {}
    all_edges: dict[str, list[str]] = {}
    missing_node_reference = False

    for node in flow.nodes:
        edges: list[str] = []
        bounded_repeat_edges: list[str] = []
        if isinstance(node, PlayPromptNode):
            used_prompts.add(node.prompt_id)
            edges = [node.next]
        elif isinstance(node, CollectDigitNode):
            if node.prompt_id:
                used_prompts.add(node.prompt_id)
            edges = list(node.branches.values()) + [node.on_timeout, node.on_invalid]
        elif isinstance(node, ScheduleBranchNode):
            if node.schedule_id not in schedules:
                errors.append(f"Node {node.id} references missing schedule {node.schedule_id}.")
            edges = [node.on_open, node.on_closed, node.on_holiday]
        elif isinstance(node, RepeatMenuNode):
            bounded_repeat_edges = [node.target]
            edges = [node.on_exhausted]
        elif isinstance(node, EndCallNode):
            edges = []
        regular_edges[node.id] = edges
        all_edges[node.id] = edges + bounded_repeat_edges
        for target in all_edges[node.id]:
            if target not in nodes:
                errors.append(f"Node {node.id} references missing node {target}.")
                missing_node_reference = True

    for prompt_id in used_prompts:
        if prompt_id not in prompt_ids:
            errors.append(f"Flow references missing prompt {prompt_id}.")
    total_asset_bytes = sum(prompt_sizes.get(prompt_id, 0) for prompt_id in used_prompts)
    if total_asset_bytes > revision_asset_limit_bytes:
        errors.append("Revision prompt assets exceed the 128 MiB limit.")

    if missing_node_reference or flow.root_node not in nodes:
        return errors

    reachable = _walk(flow.root_node, all_edges)
    unreachable = sorted(set(nodes) - reachable)
    if unreachable:
        errors.append(f"Unreachable flow nodes: {', '.join(unreachable)}.")

    cycle = _find_cycle(flow.root_node, regular_edges)
    if cycle:
        errors.append(f"Unbounded flow cycle: {' -> '.join(cycle)}.")

    max_depth = _maximum_depth(flow.root_node, regular_edges)
    if max_depth > 8:
        errors.append(f"Flow depth {max_depth} exceeds the limit of 8.")

    for node_id in reachable:
        if not _can_reach_end(node_id, nodes, regular_edges, set()):
            errors.append(f"Node {node_id} has no terminating path.")
            break
    return errors


def validate_configuration_v2(
    configuration: DraftConfigurationV2,
    prompt_ids: set[str],
    prompt_sizes: dict[str, int],
    revision_asset_limit_bytes: int,
) -> list[str]:
    errors: list[str] = []
    schedules = {schedule.id: schedule for schedule in configuration.schedules}
    if len(schedules) != len(configuration.schedules):
        errors.append("Schedule IDs must be unique.")
    for schedule in configuration.schedules:
        try:
            ZoneInfo(schedule.timezone)
        except ZoneInfoNotFoundError:
            errors.append(f"Schedule {schedule.name} has an unknown timezone.")

    root = configuration.flow.root
    if root is None:
        return [*errors, "Add a first step before publishing."]

    seen_blocks: set[str] = set()
    seen_branches: set[str] = set()
    used_prompts: set[str] = set()
    maximum_depth = 0

    def visit(block, depth: int, owner_menu: str | None, forbidden_return_owner: str | None = None) -> None:
        nonlocal maximum_depth
        maximum_depth = max(maximum_depth, depth)
        if block.block_id in seen_blocks:
            errors.append("A flow block is duplicated or shared between branches.")
            return
        seen_blocks.add(block.block_id)

        if isinstance(block, PlayPromptBlockV2):
            if not block.prompt_id:
                errors.append("A Play prompt step has no prompt selected.")
            else:
                used_prompts.add(block.prompt_id)
                if block.prompt_id not in prompt_ids:
                    errors.append("A Play prompt step references a missing prompt.")
            if block.next is None:
                errors.append("A Play prompt step has no next step.")
            else:
                visit(block.next, depth + 1, owner_menu, forbidden_return_owner)
            return

        if isinstance(block, CollectDigitBlockV2):
            if block.allow_prompt_barge_in and not block.prompt_id:
                errors.append("Prompt interruption requires a menu prompt.")
            if block.prompt_id:
                used_prompts.add(block.prompt_id)
                if block.prompt_id not in prompt_ids:
                    errors.append("A Collect digit step references a missing menu prompt.")
            digits = [branch.digit for branch in block.branches]
            if not digits:
                errors.append("A Collect digit step requires at least one digit branch.")
            if len(digits) != len(set(digits)):
                errors.append("Digit branches must use unique keys.")
            for branch in block.branches:
                if branch.branch_id in seen_branches:
                    errors.append("A digit branch identifier is duplicated.")
                seen_branches.add(branch.branch_id)
                if branch.root is None:
                    errors.append(f"Digit {branch.digit} has no first step.")
                else:
                    visit(branch.root, depth + 1, block.block_id)
            for name, child, forbidden in (
                ("No input", block.on_timeout, None),
                ("Invalid input", block.on_invalid, None),
                ("Menu return limit", block.on_return_limit, block.block_id),
            ):
                if child is None:
                    errors.append(f"{name} has no first step.")
                else:
                    visit(child, depth + 1, block.block_id, forbidden)
            return

        if isinstance(block, ScheduleBranchBlockV2):
            if not block.schedule_id or block.schedule_id not in schedules:
                errors.append("A Check schedule step references a missing schedule.")
            for name, child in (
                ("Open", block.on_open),
                ("Closed", block.on_closed),
                ("Holiday", block.on_holiday),
            ):
                if child is None:
                    errors.append(f"Schedule path {name} has no first step.")
                else:
                    visit(child, depth + 1, owner_menu, forbidden_return_owner)
            return

        if isinstance(block, ReturnToMenuBlockV2):
            if owner_menu is None:
                errors.append("Return to menu can only be used inside a digit branch.")
            elif owner_menu == forbidden_return_owner:
                errors.append("The menu return-limit path cannot return to the same menu.")
            return

        if isinstance(block, EndCallBlockV2):
            return

        errors.append("The flow contains an unsupported block type.")

    visit(root, 1, None)
    if len(seen_blocks) > 64:
        errors.append(f"Flow has {len(seen_blocks)} steps; the limit is 64.")
    if maximum_depth > 8:
        errors.append(f"Flow depth {maximum_depth} exceeds the limit of 8.")
    total_asset_bytes = sum(prompt_sizes.get(prompt_id, 0) for prompt_id in used_prompts)
    if total_asset_bytes > revision_asset_limit_bytes:
        errors.append("Revision prompt assets exceed the 128 MiB limit.")
    return list(dict.fromkeys(errors))


def validate_configuration_v3(
    configuration: DraftConfigurationV3,
    prompt_ids: set[str],
    prompt_sizes: dict[str, int],
    revision_asset_limit_bytes: int,
) -> list[str]:
    errors: list[str] = []
    schedules = {schedule.id: schedule for schedule in configuration.schedules}
    if len(schedules) != len(configuration.schedules):
        errors.append("Schedule IDs must be unique.")
    for schedule in configuration.schedules:
        try:
            ZoneInfo(schedule.timezone)
        except ZoneInfoNotFoundError:
            errors.append(f"Schedule {schedule.name} has an unknown timezone.")

    root = configuration.flow.root
    if root is None:
        return [*errors, "Add a first step before publishing."]

    seen_blocks: set[str] = set()
    seen_branches: set[str] = set()
    used_prompts: set[str] = set()
    maximum_depth = 0

    def visit(
        block,
        depth: int,
        owner_menu: str | None,
        forbidden_return_owner: str | None = None,
        parent_is_prompt: bool = False,
    ) -> None:
        nonlocal maximum_depth
        maximum_depth = max(maximum_depth, depth)
        if block.block_id in seen_blocks:
            errors.append("A flow block is duplicated or shared between branches.")
            return
        seen_blocks.add(block.block_id)

        if isinstance(block, PlayPromptBlockV2):
            if not block.prompt_id:
                errors.append("A Play prompt step has no prompt selected.")
            else:
                used_prompts.add(block.prompt_id)
                if block.prompt_id not in prompt_ids:
                    errors.append("A Play prompt step references a missing prompt.")
            if block.next is None:
                errors.append("A Play prompt step has no next step.")
            else:
                visit(block.next, depth + 1, owner_menu, forbidden_return_owner, True)
            return

        if isinstance(block, RecordMessageBlockV3):
            if not parent_is_prompt:
                errors.append("Record message must immediately follow a Play prompt greeting.")
            for name, child in (("recorded", block.next), ("unavailable", block.on_unavailable)):
                if child is None:
                    errors.append(f"Record message {name} path has no first step.")
                else:
                    visit(child, depth + 1, owner_menu, forbidden_return_owner)
            return

        if isinstance(block, CollectDigitBlockV2):
            if block.allow_prompt_barge_in and not block.prompt_id:
                errors.append("Prompt interruption requires a menu prompt.")
            if block.prompt_id:
                used_prompts.add(block.prompt_id)
                if block.prompt_id not in prompt_ids:
                    errors.append("A Collect digit step references a missing menu prompt.")
            digits = [branch.digit for branch in block.branches]
            if not digits:
                errors.append("A Collect digit step requires at least one digit branch.")
            if len(digits) != len(set(digits)):
                errors.append("Digit branches must use unique keys.")
            for branch in block.branches:
                if branch.branch_id in seen_branches:
                    errors.append("A digit branch identifier is duplicated.")
                seen_branches.add(branch.branch_id)
                if branch.root is None:
                    errors.append(f"Digit {branch.digit} has no first step.")
                else:
                    visit(branch.root, depth + 1, block.block_id)
            for name, child, forbidden in (
                ("No input", block.on_timeout, None),
                ("Invalid input", block.on_invalid, None),
                ("Menu return limit", block.on_return_limit, block.block_id),
            ):
                if child is None:
                    errors.append(f"{name} has no first step.")
                else:
                    visit(child, depth + 1, block.block_id, forbidden)
            return

        if isinstance(block, ScheduleBranchBlockV2):
            if not block.schedule_id or block.schedule_id not in schedules:
                errors.append("A Check schedule step references a missing schedule.")
            for name, child in (
                ("Open", block.on_open),
                ("Closed", block.on_closed),
                ("Holiday", block.on_holiday),
            ):
                if child is None:
                    errors.append(f"Schedule path {name} has no first step.")
                else:
                    visit(child, depth + 1, owner_menu, forbidden_return_owner)
            return

        if isinstance(block, ReturnToMenuBlockV2):
            if owner_menu is None:
                errors.append("Return to menu can only be used inside a digit branch.")
            elif owner_menu == forbidden_return_owner:
                errors.append("The menu return-limit path cannot return to the same menu.")
            return
        if isinstance(block, EndCallBlockV2):
            return
        errors.append("The flow contains an unsupported block type.")

    visit(root, 1, None)
    if len(seen_blocks) > 64:
        errors.append(f"Flow has {len(seen_blocks)} steps; the limit is 64.")
    if maximum_depth > 8:
        errors.append(f"Flow depth {maximum_depth} exceeds the limit of 8.")
    total_asset_bytes = sum(prompt_sizes.get(prompt_id, 0) for prompt_id in used_prompts)
    if total_asset_bytes > revision_asset_limit_bytes:
        errors.append("Revision prompt assets exceed the 128 MiB limit.")
    return list(dict.fromkeys(errors))


def validate_configuration_v4(
    configuration: DraftConfigurationV4,
    prompt_ids: set[str],
    prompt_sizes: dict[str, int],
    revision_asset_limit_bytes: int,
) -> list[str]:
    errors: list[str] = []
    schedules = {schedule.id: schedule for schedule in configuration.schedules}
    if len(schedules) != len(configuration.schedules):
        errors.append("Schedule IDs must be unique.")
    for schedule in configuration.schedules:
        try:
            ZoneInfo(schedule.timezone)
        except ZoneInfoNotFoundError:
            errors.append(f"Schedule {schedule.name} has an unknown timezone.")

    root = configuration.flow.root
    if root is None:
        return [*errors, "Add a first step before publishing."]

    seen_blocks: set[str] = set()
    seen_branches: set[str] = set()
    used_prompts: set[str] = set()
    maximum_depth = 0

    def visit(
        block,
        depth: int,
        owner_menu: str | None,
        forbidden_return_owner: str | None = None,
        prompt_available: bool = False,
    ) -> None:
        nonlocal maximum_depth
        maximum_depth = max(maximum_depth, depth)
        if block.block_id in seen_blocks:
            errors.append("A flow block is duplicated or shared between branches.")
            return
        seen_blocks.add(block.block_id)

        if isinstance(block, PlayPromptBlockV2):
            if not block.prompt_id:
                errors.append("A Play prompt step has no prompt selected.")
            else:
                used_prompts.add(block.prompt_id)
                if block.prompt_id not in prompt_ids:
                    errors.append("A Play prompt step references a missing prompt.")
            if block.next is None:
                errors.append("A Play prompt step has no next step.")
            else:
                visit(
                    block.next,
                    depth + 1,
                    owner_menu,
                    forbidden_return_owner,
                    prompt_available or bool(block.prompt_id),
                )
            return

        if isinstance(block, RecordMessageBlockV3):
            if not prompt_available:
                errors.append(
                    "Record message requires an earlier Play prompt or menu prompt notice on this path."
                )
            for name, child in (("recorded", block.next), ("unavailable", block.on_unavailable)):
                if child is None:
                    errors.append(f"Record message {name} path has no first step.")
                else:
                    visit(child, depth + 1, owner_menu, forbidden_return_owner, prompt_available)
            return

        if isinstance(block, ExternalCallBlockV4):
            if not prompt_available:
                errors.append(
                    "External call requires an earlier Play prompt or menu prompt notice on this path."
                )
            for name, child in (
                ("completed", block.next),
                ("not connected", block.on_not_connected),
                ("system failure", block.on_system_failure),
            ):
                if child is None:
                    errors.append(f"External call {name} path has no first step.")
                else:
                    visit(child, depth + 1, owner_menu, forbidden_return_owner, prompt_available)
            return

        if isinstance(block, CollectDigitBlockV2):
            if block.allow_prompt_barge_in and not block.prompt_id:
                errors.append("Prompt interruption requires a menu prompt.")
            if block.prompt_id:
                used_prompts.add(block.prompt_id)
                if block.prompt_id not in prompt_ids:
                    errors.append("A Collect digit step references a missing menu prompt.")
            digits = [branch.digit for branch in block.branches]
            if not digits:
                errors.append("A Collect digit step requires at least one digit branch.")
            if len(digits) != len(set(digits)):
                errors.append("Digit branches must use unique keys.")
            branch_prompt_available = prompt_available or bool(block.prompt_id)
            for branch in block.branches:
                if branch.branch_id in seen_branches:
                    errors.append("A digit branch identifier is duplicated.")
                seen_branches.add(branch.branch_id)
                if branch.root is None:
                    errors.append(f"Digit {branch.digit} has no first step.")
                else:
                    visit(branch.root, depth + 1, block.block_id, None, branch_prompt_available)
            for name, child, forbidden in (
                ("No input", block.on_timeout, None),
                ("Invalid input", block.on_invalid, None),
                ("Menu return limit", block.on_return_limit, block.block_id),
            ):
                if child is None:
                    errors.append(f"{name} has no first step.")
                else:
                    visit(child, depth + 1, block.block_id, forbidden, branch_prompt_available)
            return

        if isinstance(block, ScheduleBranchBlockV2):
            if not block.schedule_id or block.schedule_id not in schedules:
                errors.append("A Check schedule step references a missing schedule.")
            for name, child in (
                ("Open", block.on_open),
                ("Closed", block.on_closed),
                ("Holiday", block.on_holiday),
            ):
                if child is None:
                    errors.append(f"Schedule path {name} has no first step.")
                else:
                    visit(child, depth + 1, owner_menu, forbidden_return_owner, prompt_available)
            return

        if isinstance(block, ReturnToMenuBlockV2):
            if owner_menu is None:
                errors.append("Return to menu can only be used inside a digit branch.")
            elif owner_menu == forbidden_return_owner:
                errors.append("The menu return-limit path cannot return to the same menu.")
            return
        if isinstance(block, EndCallBlockV2):
            return
        errors.append("The flow contains an unsupported block type.")

    visit(root, 1, None)
    if len(seen_blocks) > 64:
        errors.append(f"Flow has {len(seen_blocks)} steps; the limit is 64.")
    if maximum_depth > 8:
        errors.append(f"Flow depth {maximum_depth} exceeds the limit of 8.")
    total_asset_bytes = sum(prompt_sizes.get(prompt_id, 0) for prompt_id in used_prompts)
    if total_asset_bytes > revision_asset_limit_bytes:
        errors.append("Revision prompt assets exceed the 128 MiB limit.")
    return list(dict.fromkeys(errors))


def _walk(root: str, edges: dict[str, list[str]]) -> set[str]:
    visited: set[str] = set()
    pending = [root]
    while pending:
        node = pending.pop()
        if node in visited or node not in edges:
            continue
        visited.add(node)
        pending.extend(edges[node])
    return visited


def _find_cycle(root: str, edges: dict[str, list[str]]) -> list[str] | None:
    visited: set[str] = set()
    active: list[str] = []

    def visit(node: str) -> list[str] | None:
        if node in active:
            position = active.index(node)
            return active[position:] + [node]
        if node in visited or node not in edges:
            return None
        active.append(node)
        for target in edges[node]:
            found = visit(target)
            if found:
                return found
        active.pop()
        visited.add(node)
        return None

    return visit(root)


def _maximum_depth(root: str, edges: dict[str, list[str]]) -> int:
    memo: dict[str, int] = {}

    def depth(node: str, active: set[str]) -> int:
        if node in memo:
            return memo[node]
        if node in active or node not in edges or not edges[node]:
            return 1
        value = 1 + max(depth(target, active | {node}) for target in edges[node])
        memo[node] = value
        return value

    return depth(root, set())


def _can_reach_end(
    node_id: str,
    nodes: dict,
    edges: dict[str, list[str]],
    active: set[str],
) -> bool:
    if node_id in active or node_id not in nodes:
        return False
    if isinstance(nodes[node_id], EndCallNode):
        return True
    return any(
        _can_reach_end(target, nodes, edges, active | {node_id})
        for target in edges.get(node_id, [])
    )
