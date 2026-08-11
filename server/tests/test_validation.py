from __future__ import annotations

from app.schemas import DraftConfiguration
from app.validation import validate_configuration


def validate(document: dict) -> list[str]:
    configuration = DraftConfiguration.model_validate(document)
    return validate_configuration(configuration, set(), {}, 128 * 1024 * 1024)


def test_default_end_call_flow_is_valid():
    assert validate(DraftConfiguration().model_dump(mode="json")) == []


def test_rejects_unbounded_cycle():
    errors = validate(
        {
            "schema_version": 1,
            "caller_policy": {},
            "schedules": [],
            "flow": {
                "root_node": "first",
                "nodes": [
                    {"id": "first", "type": "schedule_branch", "schedule_id": "missing", "on_open": "second", "on_closed": "end", "on_holiday": "end"},
                    {"id": "second", "type": "schedule_branch", "schedule_id": "missing", "on_open": "first", "on_closed": "end", "on_holiday": "end"},
                    {"id": "end", "type": "end_call"},
                ],
            },
        },
    )
    assert any("cycle" in error.lower() for error in errors)
    assert any("missing schedule" in error.lower() for error in errors)


def test_bounded_repeat_is_allowed():
    errors = validate(
        {
            "schema_version": 1,
            "caller_policy": {},
            "schedules": [],
            "flow": {
                "root_node": "repeat",
                "nodes": [
                    {"id": "repeat", "type": "repeat_menu", "target": "repeat", "maximum_repeats": 2, "on_exhausted": "end"},
                    {"id": "end", "type": "end_call"},
                ],
            },
        },
    )
    assert errors == []


def test_rejects_unreachable_node():
    errors = validate(
        {
            "schema_version": 1,
            "caller_policy": {},
            "schedules": [],
            "flow": {
                "root_node": "end",
                "nodes": [
                    {"id": "end", "type": "end_call"},
                    {"id": "orphan", "type": "end_call"},
                ],
            },
        },
    )
    assert errors == ["Unreachable flow nodes: orphan."]
