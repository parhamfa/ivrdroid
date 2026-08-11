from __future__ import annotations

from datetime import date, datetime
from enum import Enum
from typing import Annotated, Literal
from uuid import UUID

from pydantic import BaseModel, ConfigDict, Field, field_validator, model_validator


NODE_ID_PATTERN = r"^[a-z][a-z0-9_-]{0,31}$"
PROMPT_ID_PATTERN = r"^[0-9a-f-]{36}$"
BLOCK_ID_PATTERN = r"^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"
DTMF_KEYS = frozenset("0123456789*#")
EMERGENCY_SERVICE_NUMBERS = frozenset({"110", "112", "115", "125", "911", "999"})


def is_obvious_emergency_number(value: str) -> bool:
    digits = value.removeprefix("+")
    candidates = {digits, digits.lstrip("0")}
    for prefix in ("00", "98", "0098", "1", "44"):
        if digits.startswith(prefix):
            remainder = digits[len(prefix) :]
            candidates.update({remainder, remainder.lstrip("0")})
    return bool(candidates & EMERGENCY_SERVICE_NUMBERS)


class StrictModel(BaseModel):
    model_config = ConfigDict(extra="forbid")


class CallerPolicyMode(str, Enum):
    IVR_DISABLED = "IVR_DISABLED"
    ALLOWLIST_ONLY = "ALLOWLIST_ONLY"
    ACCEPT_ALL = "ACCEPT_ALL"
    ACCEPT_ALL_EXCEPT_BLOCKLIST = "ACCEPT_ALL_EXCEPT_BLOCKLIST"


class CallerEntry(StrictModel):
    label: str = Field(min_length=1, max_length=120)
    e164: str = Field(pattern=r"^\+[1-9][0-9]{7,14}$")


class CallerPolicy(StrictModel):
    mode: CallerPolicyMode = CallerPolicyMode.ALLOWLIST_ONLY
    route_unknown_callers: bool = False
    allowlist: list[CallerEntry] = Field(default_factory=list, max_length=1000)
    blocklist: list[CallerEntry] = Field(default_factory=list, max_length=1000)

    @model_validator(mode="after")
    def unique_numbers(self):
        for name, entries in (("allowlist", self.allowlist), ("blocklist", self.blocklist)):
            numbers = [entry.e164 for entry in entries]
            if len(numbers) != len(set(numbers)):
                raise ValueError(f"{name} contains duplicate phone numbers")
        return self


class WeeklyWindow(StrictModel):
    weekday: int = Field(ge=0, le=6, description="Monday is 0")
    start: str = Field(pattern=r"^(?:[01][0-9]|2[0-3]):[0-5][0-9]$")
    end: str = Field(pattern=r"^(?:[01][0-9]|2[0-3]):[0-5][0-9]$")

    @model_validator(mode="after")
    def ordered(self):
        if self.start >= self.end:
            raise ValueError("weekly window end must be later than start")
        return self


class ScheduleException(StrictModel):
    date: date
    state: Literal["open", "closed", "holiday"]


class Schedule(StrictModel):
    id: str = Field(pattern=NODE_ID_PATTERN)
    name: str = Field(min_length=1, max_length=120)
    timezone: str = Field(default="Asia/Tehran", min_length=1, max_length=80)
    weekly: list[WeeklyWindow] = Field(default_factory=list, max_length=64)
    exceptions: list[ScheduleException] = Field(default_factory=list, max_length=366)

    @model_validator(mode="after")
    def no_overlaps_or_duplicate_exceptions(self):
        grouped: dict[int, list[tuple[str, str]]] = {}
        for window in self.weekly:
            grouped.setdefault(window.weekday, []).append((window.start, window.end))
        for windows in grouped.values():
            ordered = sorted(windows)
            if any(current[0] < previous[1] for previous, current in zip(ordered, ordered[1:])):
                raise ValueError("weekly schedule windows overlap")
        dates = [item.date for item in self.exceptions]
        if len(dates) != len(set(dates)):
            raise ValueError("schedule exception dates must be unique")
        return self


class PlayPromptNode(StrictModel):
    id: str = Field(pattern=NODE_ID_PATTERN)
    type: Literal["play_prompt"]
    prompt_id: str = Field(pattern=PROMPT_ID_PATTERN)
    next: str = Field(pattern=NODE_ID_PATTERN)


class CollectDigitNode(StrictModel):
    id: str = Field(pattern=NODE_ID_PATTERN)
    type: Literal["collect_digit"]
    prompt_id: str | None = Field(default=None, pattern=PROMPT_ID_PATTERN)
    timeout_ms: int = Field(default=5000, ge=1000, le=15000)
    maximum_attempts: int = Field(default=3, ge=1, le=3)
    branches: dict[str, str] = Field(default_factory=dict, max_length=12)
    on_timeout: str = Field(pattern=NODE_ID_PATTERN)
    on_invalid: str = Field(pattern=NODE_ID_PATTERN)

    @field_validator("branches")
    @classmethod
    def validate_branches(cls, value: dict[str, str]) -> dict[str, str]:
        if not value:
            raise ValueError("collect_digit requires at least one digit branch")
        for digit, target in value.items():
            if digit not in DTMF_KEYS:
                raise ValueError(f"unsupported DTMF key: {digit}")
            if not target or len(target) > 32:
                raise ValueError("invalid DTMF branch target")
        return value


class ScheduleBranchNode(StrictModel):
    id: str = Field(pattern=NODE_ID_PATTERN)
    type: Literal["schedule_branch"]
    schedule_id: str = Field(pattern=NODE_ID_PATTERN)
    on_open: str = Field(pattern=NODE_ID_PATTERN)
    on_closed: str = Field(pattern=NODE_ID_PATTERN)
    on_holiday: str = Field(pattern=NODE_ID_PATTERN)


class RepeatMenuNode(StrictModel):
    id: str = Field(pattern=NODE_ID_PATTERN)
    type: Literal["repeat_menu"]
    target: str = Field(pattern=NODE_ID_PATTERN)
    maximum_repeats: int = Field(default=2, ge=1, le=3)
    on_exhausted: str = Field(pattern=NODE_ID_PATTERN)


class EndCallNode(StrictModel):
    id: str = Field(pattern=NODE_ID_PATTERN)
    type: Literal["end_call"]


FlowNode = Annotated[
    PlayPromptNode | CollectDigitNode | ScheduleBranchNode | RepeatMenuNode | EndCallNode,
    Field(discriminator="type"),
]


class FlowDefinition(StrictModel):
    root_node: str = Field(pattern=NODE_ID_PATTERN)
    nodes: list[FlowNode] = Field(min_length=1, max_length=64)


class DraftConfiguration(StrictModel):
    schema_version: Literal[1] = 1
    caller_policy: CallerPolicy = Field(default_factory=CallerPolicy)
    schedules: list[Schedule] = Field(default_factory=list, max_length=32)
    flow: FlowDefinition = Field(
        default_factory=lambda: FlowDefinition(
            root_node="end",
            nodes=[EndCallNode(id="end", type="end_call")],
        ),
    )


class PlayPromptBlockV2(StrictModel):
    block_id: str = Field(pattern=BLOCK_ID_PATTERN)
    type: Literal["play_prompt"]
    prompt_id: str | None = Field(default=None, pattern=PROMPT_ID_PATTERN)
    next: "FlowBlockV2 | None" = None


class DigitBranchV2(StrictModel):
    branch_id: str = Field(pattern=BLOCK_ID_PATTERN)
    digit: str = Field(min_length=1, max_length=1)
    root: "FlowBlockV2 | None" = None

    @field_validator("digit")
    @classmethod
    def supported_digit(cls, value: str) -> str:
        if value not in DTMF_KEYS:
            raise ValueError(f"unsupported DTMF key: {value}")
        return value


class CollectDigitBlockV2(StrictModel):
    block_id: str = Field(pattern=BLOCK_ID_PATTERN)
    type: Literal["collect_digit"]
    prompt_id: str | None = Field(default=None, pattern=PROMPT_ID_PATTERN)
    allow_prompt_barge_in: bool = Field(default=False, exclude_if=lambda value: not value)
    timeout_ms: int = Field(default=5000, ge=1000, le=15000)
    maximum_attempts: int = Field(default=3, ge=1, le=3)
    maximum_menu_returns: int = Field(default=2, ge=1, le=3)
    branches: list[DigitBranchV2] = Field(default_factory=list, max_length=12)
    on_timeout: "FlowBlockV2 | None" = None
    on_invalid: "FlowBlockV2 | None" = None
    on_return_limit: "FlowBlockV2 | None" = None

    @model_validator(mode="after")
    def barge_in_requires_prompt(self):
        if self.allow_prompt_barge_in and self.prompt_id is None:
            raise ValueError("Prompt interruption requires a menu prompt")
        return self


class ScheduleBranchBlockV2(StrictModel):
    block_id: str = Field(pattern=BLOCK_ID_PATTERN)
    type: Literal["schedule_branch"]
    schedule_id: str | None = Field(default=None, pattern=NODE_ID_PATTERN)
    on_open: "FlowBlockV2 | None" = None
    on_closed: "FlowBlockV2 | None" = None
    on_holiday: "FlowBlockV2 | None" = None


class ReturnToMenuBlockV2(StrictModel):
    block_id: str = Field(pattern=BLOCK_ID_PATTERN)
    type: Literal["return_to_menu"]


class EndCallBlockV2(StrictModel):
    block_id: str = Field(pattern=BLOCK_ID_PATTERN)
    type: Literal["end_call"]


class RecordMessageBlockV3(StrictModel):
    block_id: str = Field(pattern=BLOCK_ID_PATTERN)
    type: Literal["record_message"]
    next: "FlowBlockV2 | None" = None
    on_unavailable: "FlowBlockV2 | None" = None


class ExternalCallBlockV4(StrictModel):
    block_id: str = Field(pattern=BLOCK_ID_PATTERN)
    type: Literal["external_call"]
    phone_number: str = Field(pattern=r"^\+?[0-9]{8,15}$")
    answer_timeout_seconds: int = Field(default=30, ge=5, le=120)
    next: "FlowBlockV2 | None" = None
    on_not_connected: "FlowBlockV2 | None" = None
    on_system_failure: "FlowBlockV2 | None" = None

    @field_validator("phone_number")
    @classmethod
    def reject_emergency_services(cls, value: str) -> str:
        if is_obvious_emergency_number(value):
            raise ValueError("Emergency and public-safety service numbers are not allowed")
        return value


FlowBlockV2 = Annotated[
    PlayPromptBlockV2
    | CollectDigitBlockV2
    | ScheduleBranchBlockV2
    | ReturnToMenuBlockV2
    | EndCallBlockV2
    | RecordMessageBlockV3
    | ExternalCallBlockV4,
    Field(discriminator="type"),
]

for recursive_model in (
    PlayPromptBlockV2,
    DigitBranchV2,
    CollectDigitBlockV2,
    ScheduleBranchBlockV2,
    RecordMessageBlockV3,
    ExternalCallBlockV4,
):
    recursive_model.model_rebuild(_types_namespace={"FlowBlockV2": FlowBlockV2})


class FlowDefinitionV2(StrictModel):
    root: FlowBlockV2 | None = None


class FlowDocumentV2(StrictModel):
    schema_version: Literal[2] = 2
    edit_version: int = Field(default=0, ge=0)
    caller_policy: CallerPolicy = Field(default_factory=CallerPolicy)
    schedules: list[Schedule] = Field(default_factory=list, max_length=32)
    flow: FlowDefinitionV2 = Field(default_factory=FlowDefinitionV2)

    @model_validator(mode="after")
    def release_two_has_no_recording(self):
        pending = [self.flow.root] if self.flow.root is not None else []
        while pending:
            block = pending.pop()
            if isinstance(block, RecordMessageBlockV3):
                raise ValueError("FlowDocumentV2 cannot contain record_message blocks")
            if isinstance(block, ExternalCallBlockV4):
                raise ValueError("FlowDocumentV2 cannot contain external_call blocks")
            if isinstance(block, CollectDigitBlockV2) and block.allow_prompt_barge_in:
                raise ValueError("FlowDocumentV2 cannot enable prompt interruption")
            if isinstance(block, PlayPromptBlockV2) and block.next is not None:
                pending.append(block.next)
            elif isinstance(block, CollectDigitBlockV2):
                pending.extend(branch.root for branch in block.branches if branch.root is not None)
                pending.extend(
                    child for child in (block.on_timeout, block.on_invalid, block.on_return_limit)
                    if child is not None
                )
            elif isinstance(block, ScheduleBranchBlockV2):
                pending.extend(
                    child for child in (block.on_open, block.on_closed, block.on_holiday)
                    if child is not None
                )
            elif isinstance(block, ExternalCallBlockV4):
                pending.extend(
                    child
                    for child in (block.next, block.on_not_connected, block.on_system_failure)
                    if child is not None
                )
        return self


class RecordingBehavior(StrictModel):
    maximum_duration_seconds: int = Field(default=60, ge=10, le=180)
    finish_key: str | None = "#"

    @field_validator("finish_key")
    @classmethod
    def supported_finish_key(cls, value: str | None) -> str | None:
        if value is not None and (len(value) != 1 or value not in DTMF_KEYS):
            raise ValueError("finish_key must be one DTMF key or null")
        return value


class FlowDocumentV3(StrictModel):
    schema_version: Literal[3] = 3
    edit_version: int = Field(default=0, ge=0)
    caller_policy: CallerPolicy = Field(default_factory=CallerPolicy)
    schedules: list[Schedule] = Field(default_factory=list, max_length=32)
    recording_behavior: RecordingBehavior = Field(default_factory=RecordingBehavior)
    flow: FlowDefinitionV2 = Field(default_factory=FlowDefinitionV2)

    @model_validator(mode="after")
    def release_three_has_no_external_calls(self):
        pending = [self.flow.root] if self.flow.root is not None else []
        while pending:
            block = pending.pop()
            if isinstance(block, ExternalCallBlockV4):
                raise ValueError("FlowDocumentV3 cannot contain external_call blocks")
            if isinstance(block, CollectDigitBlockV2) and block.allow_prompt_barge_in:
                raise ValueError("FlowDocumentV3 cannot enable prompt interruption")
            if isinstance(block, PlayPromptBlockV2) and block.next is not None:
                pending.append(block.next)
            elif isinstance(block, CollectDigitBlockV2):
                pending.extend(branch.root for branch in block.branches if branch.root is not None)
                pending.extend(
                    child
                    for child in (block.on_timeout, block.on_invalid, block.on_return_limit)
                    if child is not None
                )
            elif isinstance(block, ScheduleBranchBlockV2):
                pending.extend(
                    child for child in (block.on_open, block.on_closed, block.on_holiday)
                    if child is not None
                )
            elif isinstance(block, RecordMessageBlockV3):
                pending.extend(
                    child for child in (block.next, block.on_unavailable) if child is not None
                )
        return self


class FlowDocumentV4(StrictModel):
    schema_version: Literal[4] = 4
    edit_version: int = Field(default=0, ge=0)
    caller_policy: CallerPolicy = Field(default_factory=CallerPolicy)
    schedules: list[Schedule] = Field(default_factory=list, max_length=32)
    recording_behavior: RecordingBehavior = Field(default_factory=RecordingBehavior)
    flow: FlowDefinitionV2 = Field(default_factory=FlowDefinitionV2)


# Kept as a source-compatible name for the existing API/service call sites.
DraftConfigurationV2 = FlowDocumentV2
DraftConfigurationV3 = FlowDocumentV3
DraftConfigurationV4 = FlowDocumentV4


class ValidationResult(StrictModel):
    valid: bool
    errors: list[str] = Field(default_factory=list)


class PromptResponse(StrictModel):
    id: str
    name: str
    version: int
    content_hash: str
    size_bytes: int
    duration_ms: int
    created_at: datetime
    used_by: list[str] = Field(default_factory=list)
    audio_url: str


class RevisionResponse(StrictModel):
    id: int
    schema_version: int = 1
    manifest_sha256: str
    signature_b64: str
    source_revision_id: int | None
    published_at: datetime
    published_by: str


class SimulationRequest(StrictModel):
    configuration: DraftConfigurationV2
    events: list[str] = Field(default_factory=list, max_length=64)


class SimulationRequestV3(StrictModel):
    configuration: DraftConfigurationV3
    events: list[str] = Field(default_factory=list, max_length=64)


class SimulationRequestV4(StrictModel):
    configuration: DraftConfigurationV4
    events: list[str] = Field(default_factory=list, max_length=64)


class SimulationTraceStep(StrictModel):
    block_id: str
    type: str
    label: str
    prompt_id: str | None = None
    event: str | None = None


class SimulationResult(StrictModel):
    status: Literal[
        "awaiting_input",
        "awaiting_schedule",
        "awaiting_recording",
        "awaiting_external",
        "complete",
        "invalid",
    ]
    trace: list[SimulationTraceStep] = Field(default_factory=list, max_length=512)
    awaiting_block_id: str | None = None
    available_digits: list[str] = Field(default_factory=list, max_length=12)
    available_schedule_states: list[Literal["open", "closed", "holiday"]] = Field(
        default_factory=list,
        max_length=3,
    )
    available_recording_outcomes: list[
        Literal["recorded", "unavailable", "hangup"]
    ] = Field(default_factory=list, max_length=3)
    available_external_outcomes: list[
        Literal[
            "external_completed",
            "external_not_connected",
            "external_system_failure",
        ]
    ] = Field(default_factory=list, max_length=3)
    message: str | None = None


class FlowDiffRequest(StrictModel):
    configuration: DraftConfigurationV2
    base_revision_id: int | None = Field(default=None, ge=1)


class FlowDiffRequestV3(StrictModel):
    configuration: DraftConfigurationV3
    base_revision_id: int | None = Field(default=None, ge=1)


class FlowDiffRequestV4(StrictModel):
    configuration: DraftConfigurationV4
    base_revision_id: int | None = Field(default=None, ge=1)


class PublishDraftRequestV4(StrictModel):
    edit_version: int = Field(ge=0)
    base_revision_id: int | None = Field(default=None, ge=1)


class FlowDiffResult(StrictModel):
    base_revision_id: int | None
    legacy_base: bool = False
    added: int = 0
    removed: int = 0
    changed: int = 0
    changes: list[str] = Field(default_factory=list, max_length=256)
    caller_policy_changes: list[str] = Field(default_factory=list, max_length=4096)
    schedule_changes: list[str] = Field(default_factory=list, max_length=64)
    recording_changes: list[str] = Field(default_factory=list, max_length=2)
    requires_policy_confirmation: bool = False


class RevisionDetail(StrictModel):
    revision: RevisionResponse
    schema_version: int
    flow: dict | None = None
    legacy: bool = False


class PairingCodeRequest(StrictModel):
    display_name: str = Field(default="SM-T585", min_length=1, max_length=120)


class PairingCodeResponse(StrictModel):
    code: str
    expires_at: datetime


class DeviceEnrollmentRequest(StrictModel):
    code: str = Field(pattern=r"^[0-9]{8}$")
    device_name: str = Field(min_length=1, max_length=120)
    app_version: str = Field(min_length=1, max_length=80)
    helper_version: str = Field(min_length=1, max_length=80)


class DeviceEnrollmentResponse(StrictModel):
    device_id: str
    device_token: str
    service_client_id: str
    service_client_secret: str
    server_url: str
    poll_interval_seconds: int = 60


class BootWifiRecoveryStatus(StrictModel):
    outcome: Literal[
        "already_connected",
        "reconnect_recovered",
        "wifi_link_unvalidated",
        "gave_up",
        "gave_up_active_call",
    ]
    completed_at: datetime
    # BOOT_COMPLETED normally invokes the guardian within minutes, but a supervised diagnostic
    # may invoke it later in a long-running boot. Keep the value bounded without rejecting that
    # safe diagnostic path.
    elapsed_since_boot_ms: int = Field(ge=0, le=31_536_000_000)
    internet_validated: bool
    reconnect_attempts: int = Field(ge=0, le=3)
    wifi_enable_attempts: int = Field(ge=0, le=3)


class DeviceStatus(StrictModel):
    helper_state: str = Field(max_length=80)
    helper_result: str = Field(max_length=80)
    call_state: Literal["idle", "ringing", "active", "unknown"]
    local_kill_switch: bool
    storage_free_bytes: int = Field(ge=0)
    last_error: str | None = Field(default=None, max_length=500)
    boot_wifi_recovery: BootWifiRecoveryStatus | None = None
    runtime_versions: list[int] = Field(default_factory=lambda: [1, 2], max_length=8)
    recording_capable: bool = False
    external_call_control_capable: bool = False
    conversation_recording_capable: bool = False
    prompt_barge_in_capable: bool = False
    call_control_protocol_version: int | None = Field(default=None, ge=1, le=2_147_483_647)
    call_control_state: str | None = Field(default=None, pattern=r"^[A-Z][A-Z0-9_]{0,79}$")
    call_control_recovery_pending: bool = False
    recording_spool_bytes: int = Field(default=0, ge=0)
    recording_spool_count: int = Field(default=0, ge=0)
    voicemail_spool_bytes: int = Field(
        default=0,
        ge=0,
        le=9_223_372_036_854_775_807,
    )
    voicemail_spool_count: int = Field(default=0, ge=0, le=2_147_483_647)
    conversation_spool_bytes: int = Field(
        default=0,
        ge=0,
        le=9_223_372_036_854_775_807,
    )
    conversation_spool_count: int = Field(default=0, ge=0, le=2_147_483_647)
    recording_filesystem_free_bytes: int = Field(
        default=0,
        ge=0,
        le=9_223_372_036_854_775_807,
    )


class DeviceSyncRequest(StrictModel):
    app_version: str = Field(max_length=80)
    helper_version: str = Field(max_length=80)
    active_revision_id: int | None = Field(default=None, ge=1)
    status: DeviceStatus


class DeviceSyncResponse(StrictModel):
    server_time: datetime
    desired_revision_id: int | None
    active_revision_id: int | None
    manifest_sha256: str | None
    signature_b64: str | None
    poll_interval_seconds: int = 60


class RevisionAckRequest(StrictModel):
    state: Literal["staged", "activated", "failed"]
    error: str | None = Field(default=None, max_length=500)


class ExternalCallSubEvent(StrictModel):
    event: Literal["external_call"]
    occurred_at: datetime
    status: Literal[
        "ACK",
        "CALLER_HELD",
        "DIALING",
        "OPERATOR_ANSWERED",
        "MERGING",
        "CONFERENCED",
        "COMPLETED",
        "NOT_CONNECTED",
        "SYSTEM_FAILURE",
    ]
    block_id: str = Field(pattern=BLOCK_ID_PATTERN)
    reason: str = Field(pattern=r"^(?:-|[A-Z][A-Z0-9_]{0,63})$")

    @field_validator("reason")
    @classmethod
    def reason_contains_no_phone_number(cls, value: str) -> str:
        if len("".join(character for character in value if character.isdigit())) >= 8:
            raise ValueError("external-call reasons cannot contain phone numbers")
        return value


class LegacyCallSubEvent(StrictModel):
    # Older runtimes emitted only a short label. Keep that one-field contract during rollout,
    # while forbidding number-bearing text and every arbitrary metadata field.
    event: str = Field(pattern=r"^[A-Za-z][A-Za-z0-9 _-]{0,63}$")

    @field_validator("event")
    @classmethod
    def contains_no_phone_number(cls, value: str) -> str:
        if value == "external_call":
            raise ValueError("external_call events require the strict external-call schema")
        if len("".join(character for character in value if character.isdigit())) >= 8:
            raise ValueError("legacy call-event labels cannot contain phone numbers")
        return value


CallSubEvent = ExternalCallSubEvent | LegacyCallSubEvent


class CallEventInput(StrictModel):
    call_id: str = Field(min_length=8, max_length=80)
    started_at: datetime
    caller: str | None = Field(default=None, pattern=r"^\+[1-9][0-9]{7,14}$")
    policy_decision: str = Field(min_length=1, max_length=64)
    revision_id: int | None = Field(default=None, ge=1)
    menu_path: list[Annotated[str, Field(max_length=160)]] = Field(default_factory=list, max_length=64)
    result: str = Field(min_length=1, max_length=64)
    duration_seconds: int = Field(default=0, ge=0, le=86400)
    events: list[CallSubEvent] = Field(default_factory=list, max_length=128)


class EventBatchRequest(StrictModel):
    calls: list[CallEventInput] = Field(default_factory=list, max_length=100)


class DeviceResponse(StrictModel):
    id: str
    display_name: str
    app_version: str
    helper_version: str
    desired_revision_id: int | None
    active_revision_id: int | None
    status: dict
    last_seen_at: datetime | None
    enrolled_at: datetime
    revoked_at: datetime | None


class CallResponse(StrictModel):
    id: str
    device_id: str
    started_at: datetime
    caller: str | None
    caller_masked: str
    policy_decision: str
    revision_id: int | None
    menu_path: list[str]
    result: str
    duration_seconds: int
    events: list[dict]
    recording_count: int = 0
    pending_recording_count: int = 0


class RecordingCreateRequest(StrictModel):
    recording_id: UUID
    call_id: UUID
    revision_id: int = Field(ge=1)
    block_id: str = Field(pattern=BLOCK_ID_PATTERN)
    sequence: int = Field(ge=0, le=63)
    captured_at: datetime
    duration_ms: int = Field(gt=0, le=180_000)
    stop_reason: Literal["finish_key", "maximum_duration", "caller_hangup"]
    expected_size_bytes: int = Field(gt=44, le=40 * 1024 * 1024)
    source_sha256: str = Field(pattern=r"^[0-9a-f]{64}$")


class ConversationRecordingCreateRequest(StrictModel):
    recording_id: UUID
    call_id: UUID
    revision_id: int = Field(ge=1)
    block_id: str = Field(pattern=BLOCK_ID_PATTERN)
    captured_at: datetime


class ConversationSegmentCreateRequest(StrictModel):
    kind: Literal["conversation"]
    recording_id: UUID
    call_id: UUID
    revision_id: int = Field(ge=1)
    block_id: str = Field(pattern=BLOCK_ID_PATTERN)
    sequence: int = Field(ge=0, le=65_535)
    segment_index: int = Field(ge=0, le=65_535)
    captured_at: datetime
    duration_ms: int = Field(gt=0, le=180_000)
    stop_reason: Literal[
        "segment_boundary",
        "operator_hangup",
        "caller_hangup",
        "recording_failure",
    ]
    expected_size_bytes: int = Field(gt=44, le=40 * 1024 * 1024)
    source_sha256: str = Field(pattern=r"^[0-9a-f]{64}$")
    partial: bool = False

    @model_validator(mode="after")
    def partial_matches_stop_reason(self):
        if self.sequence != self.segment_index:
            raise ValueError("conversation sequence must equal segment_index")
        should_be_partial = self.stop_reason == "recording_failure"
        if self.partial != should_be_partial:
            raise ValueError(
                "partial is true only for a retained recording_failure prefix",
            )
        return self


class ConversationRecordingCompleteRequest(StrictModel):
    segment_count: int = Field(ge=1, le=65_536)


class ConversationRecordingUploadResponse(StrictModel):
    id: str
    status: Literal["uploading", "processing", "ready", "deleted", "failed"]
    segment_count: int = 0
    duration_ms: int = 0
    acknowledged: bool = False


class ConversationSegmentUploadResponse(StrictModel):
    id: str
    segment_index: int
    status: Literal["uploading", "verified", "assembled", "failed"]
    upload_offset: int
    expected_size_bytes: int
    source_sha256: str
    acknowledged: bool = False


class RecordingUploadResponse(StrictModel):
    id: str
    status: Literal["uploading", "processing", "ready", "deleted", "failed"]
    upload_offset: int = 0
    expected_size_bytes: int
    source_sha256: str
    acknowledged: bool = False


class RecordingResponse(StrictModel):
    id: str
    call_id: str
    device_id: str
    revision_id: int
    block_id: str
    sequence: int
    kind: Literal["voicemail", "conversation"] = "voicemail"
    operator_masked: str = ""
    segment_count: int = 0
    caller: str | None
    caller_masked: str
    captured_at: datetime
    duration_ms: int
    stop_reason: str
    status: str
    listened_at: datetime | None
    deleted_at: datetime | None
    playback_url: str | None
    download_url: str | None


class RecordingListResponse(StrictModel):
    items: list[RecordingResponse]
    page: int
    page_size: int
    total: int


class RecordingListenedRequest(StrictModel):
    listened: bool


class RetentionPolicyRequest(StrictModel):
    mode: Literal["automatic", "manual"]
    days: int = Field(default=30, ge=1, le=365)


class RetentionPolicyResponse(StrictModel):
    mode: Literal["automatic", "manual"]
    days: int
    quota_bytes: int
    used_bytes: int
    pending_count: int
    updated_at: datetime
    updated_by: str


class AuditResponse(StrictModel):
    actor: str
    action: str
    target: str
    details: dict
    created_at: datetime
