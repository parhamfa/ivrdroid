from __future__ import annotations

from typing import Any

from .schemas import DraftConfigurationV4


POLICY_MODE_LABELS = {
    "IVR_DISABLED": "IVR disabled",
    "ALLOWLIST_ONLY": "Allowlist only",
    "ACCEPT_ALL": "Accept all",
    "ACCEPT_ALL_EXCEPT_BLOCKLIST": "All except exclusion list",
}
BROAD_POLICY_MODES = {"ACCEPT_ALL", "ACCEPT_ALL_EXCEPT_BLOCKLIST"}


def _policy_mode_label(value: object) -> str:
    return POLICY_MODE_LABELS.get(str(value), str(value).replace("_", " ").title())


def _masked_number(value: str) -> str:
    digits = "".join(character for character in value if character.isdigit())
    return f"••••{digits[-4:]}" if digits else "number unavailable"


def _entry_map(entries: object) -> dict[str, str]:
    if not isinstance(entries, list):
        return {}
    result: dict[str, str] = {}
    for entry in entries:
        if not isinstance(entry, dict):
            continue
        number = entry.get("e164")
        label = entry.get("label")
        if isinstance(number, str) and isinstance(label, str):
            result[number] = label
    return result


def _caller_list_changes(
    before: object,
    after: object,
    list_label: str,
) -> list[str]:
    previous = _entry_map(before)
    current = _entry_map(after)
    changes = [
        f"Added to {list_label}: {current[number]} ({_masked_number(number)})"
        for number in sorted(set(current) - set(previous))
    ]
    changes.extend(
        f"Removed from {list_label}: {previous[number]} ({_masked_number(number)})"
        for number in sorted(set(previous) - set(current))
    )
    changes.extend(
        f"Renamed in {list_label}: {previous[number]} → {current[number]} ({_masked_number(number)})"
        for number in sorted(set(previous) & set(current))
        if previous[number] != current[number]
    )
    return changes


def caller_policy_diff(
    base_policy: object,
    configuration: DraftConfigurationV4,
) -> tuple[list[str], bool]:
    current = configuration.caller_policy.model_dump(mode="json")
    previous = base_policy if isinstance(base_policy, dict) else None
    if previous is None:
        changes = [f"Initial policy mode: {_policy_mode_label(current['mode'])}"]
        changes.append(
            "Hidden or unknown callers: IVR"
            if current["route_unknown_callers"]
            else "Hidden or unknown callers: stock dialer"
        )
        changes.extend(_caller_list_changes([], current["allowlist"], "allowlist"))
        changes.extend(_caller_list_changes([], current["blocklist"], "exclusion list"))
        requires_confirmation = (
            current["mode"] in BROAD_POLICY_MODES
            or current["route_unknown_callers"] is True
        )
        return changes, requires_confirmation

    changes: list[str] = []
    if previous.get("mode") != current["mode"]:
        changes.append(
            f"Policy mode: {_policy_mode_label(previous.get('mode'))} → "
            f"{_policy_mode_label(current['mode'])}"
        )
    if previous.get("route_unknown_callers", False) != current["route_unknown_callers"]:
        before = "IVR" if previous.get("route_unknown_callers", False) else "stock dialer"
        after = "IVR" if current["route_unknown_callers"] else "stock dialer"
        changes.append(f"Hidden or unknown callers: {before} → {after}")
    changes.extend(
        _caller_list_changes(previous.get("allowlist"), current["allowlist"], "allowlist")
    )
    changes.extend(
        _caller_list_changes(
            previous.get("blocklist"),
            current["blocklist"],
            "exclusion list",
        )
    )
    requires_confirmation = (
        previous.get("mode") != current["mode"]
        and current["mode"] in BROAD_POLICY_MODES
    ) or (
        previous.get("route_unknown_callers", False) is not True
        and current["route_unknown_callers"] is True
    )
    return changes, requires_confirmation


def schedule_diff(
    base_schedules: object,
    configuration: DraftConfigurationV4,
) -> list[str]:
    previous_items = base_schedules if isinstance(base_schedules, list) else []
    previous = {
        item["id"]: item
        for item in previous_items
        if isinstance(item, dict) and isinstance(item.get("id"), str)
    }
    current_items = [item.model_dump(mode="json") for item in configuration.schedules]
    current = {item["id"]: item for item in current_items}
    changes = [
        f"Added schedule: {current[item]['name']}"
        for item in sorted(set(current) - set(previous))
    ]
    changes.extend(
        f"Removed schedule: {previous[item].get('name', item)}"
        for item in sorted(set(previous) - set(current))
    )
    for item in sorted(set(previous) & set(current)):
        if previous[item] == current[item]:
            continue
        before_name = previous[item].get("name", item)
        after_name = current[item]["name"]
        label = after_name if before_name == after_name else f"{before_name} → {after_name}"
        changes.append(f"Changed schedule: {label}")
    return changes


def _finish_key_label(value: Any) -> str:
    return "disabled" if value is None else str(value)


def recording_behavior_diff(
    base_behavior: object,
    configuration: DraftConfigurationV4,
) -> list[str]:
    current = configuration.recording_behavior.model_dump(mode="json")
    previous = base_behavior if isinstance(base_behavior, dict) else None
    if previous is None:
        return [
            f"Initial maximum recording duration: {current['maximum_duration_seconds']} seconds",
            f"Initial recording finish key: {_finish_key_label(current['finish_key'])}",
        ]
    changes: list[str] = []
    if previous.get("maximum_duration_seconds") != current["maximum_duration_seconds"]:
        changes.append(
            "Maximum recording duration: "
            f"{previous.get('maximum_duration_seconds')} → "
            f"{current['maximum_duration_seconds']} seconds"
        )
    if previous.get("finish_key") != current["finish_key"]:
        changes.append(
            "Recording finish key: "
            f"{_finish_key_label(previous.get('finish_key'))} → "
            f"{_finish_key_label(current['finish_key'])}"
        )
    return changes


def revision_configuration_diff(
    base_manifest: dict[str, Any] | None,
    configuration: DraftConfigurationV4,
) -> dict[str, object]:
    manifest = base_manifest or {}
    policy_changes, requires_confirmation = caller_policy_diff(
        manifest.get("caller_policy"),
        configuration,
    )
    return {
        "caller_policy_changes": policy_changes,
        "schedule_changes": schedule_diff(manifest.get("schedules"), configuration),
        "recording_changes": recording_behavior_diff(
            manifest.get("recording_behavior"),
            configuration,
        ),
        "requires_policy_confirmation": requires_confirmation,
    }
