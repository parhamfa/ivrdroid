export type PolicyMode =
  | "IVR_DISABLED"
  | "ALLOWLIST_ONLY"
  | "ACCEPT_ALL"
  | "ACCEPT_ALL_EXCEPT_BLOCKLIST";

export interface CallerEntry {
  label: string;
  e164: string;
}

export interface CallerPolicy {
  mode: PolicyMode;
  route_unknown_callers: boolean;
  allowlist: CallerEntry[];
  blocklist: CallerEntry[];
}

export interface WeeklyWindow {
  weekday: number;
  start: string;
  end: string;
}

export interface ScheduleException {
  date: string;
  state: "open" | "closed" | "holiday";
}

export interface Schedule {
  id: string;
  name: string;
  timezone: string;
  weekly: WeeklyWindow[];
  exceptions: ScheduleException[];
}

export interface PlayPromptBlock {
  block_id: string;
  type: "play_prompt";
  prompt_id: string | null;
  next: FlowBlock | null;
}

export interface RecordMessageBlock {
  block_id: string;
  type: "record_message";
  next: FlowBlock | null;
  on_unavailable: FlowBlock | null;
}

export interface ExternalCallBlock {
  block_id: string;
  type: "external_call";
  phone_number: string;
  answer_timeout_seconds: number;
  next: FlowBlock | null;
  on_not_connected: FlowBlock | null;
  on_system_failure: FlowBlock | null;
}

export interface DigitBranch {
  branch_id: string;
  digit: string;
  root: FlowBlock | null;
}

export interface CollectDigitBlock {
  block_id: string;
  type: "collect_digit";
  prompt_id: string | null;
  allow_prompt_barge_in?: boolean;
  timeout_ms: number;
  maximum_attempts: number;
  maximum_menu_returns: number;
  branches: DigitBranch[];
  on_timeout: FlowBlock | null;
  on_invalid: FlowBlock | null;
  on_return_limit: FlowBlock | null;
}

export interface ScheduleBranchBlock {
  block_id: string;
  type: "schedule_branch";
  schedule_id: string | null;
  on_open: FlowBlock | null;
  on_closed: FlowBlock | null;
  on_holiday: FlowBlock | null;
}

export interface ReturnToMenuBlock {
  block_id: string;
  type: "return_to_menu";
}

export interface EndCallBlock {
  block_id: string;
  type: "end_call";
}

export type FlowBlock =
  | PlayPromptBlock
  | RecordMessageBlock
  | ExternalCallBlock
  | CollectDigitBlock
  | ScheduleBranchBlock
  | ReturnToMenuBlock
  | EndCallBlock;

export interface FlowDefinition {
  root: FlowBlock | null;
}

export interface RecordingBehavior {
  maximum_duration_seconds: number;
  finish_key: string | null;
}

export interface FlowDocumentV3 {
  schema_version: 3;
  edit_version: number;
  caller_policy: CallerPolicy;
  schedules: Schedule[];
  recording_behavior: RecordingBehavior;
  flow: FlowDefinition;
}

export interface FlowDocumentV4 {
  schema_version: 4;
  edit_version: number;
  caller_policy: CallerPolicy;
  schedules: Schedule[];
  recording_behavior: RecordingBehavior;
  flow: FlowDefinition;
}

export type DraftConfiguration = FlowDocumentV4;

export interface Prompt {
  id: string;
  name: string;
  version: number;
  content_hash: string;
  size_bytes: number;
  duration_ms: number;
  created_at: string;
  used_by: string[];
  audio_url: string;
}

export interface Device {
  id: string;
  display_name: string;
  app_version: string;
  helper_version: string;
  desired_revision_id: number | null;
  active_revision_id: number | null;
  status: Record<string, unknown>;
  last_seen_at: string | null;
  enrolled_at: string;
  revoked_at: string | null;
}

export interface Revision {
  id: number;
  schema_version: number;
  manifest_sha256: string;
  signature_b64: string;
  source_revision_id: number | null;
  published_at: string;
  published_by: string;
}

export interface CallRecord {
  id: string;
  device_id: string;
  started_at: string;
  caller: string | null;
  caller_masked: string;
  policy_decision: string;
  revision_id: number | null;
  menu_path: string[];
  result: string;
  duration_seconds: number;
  events: Record<string, unknown>[];
  recording_count: number;
  pending_recording_count: number;
}

export interface AuditRecord {
  actor: string;
  action: string;
  target: string;
  details: Record<string, unknown>;
  created_at: string;
}

export interface Overview {
  device: Device | null;
  active_revision: number | null;
  call_count: number;
  prompt_bytes: number;
  prompt_quota_bytes: number;
  recording_count: number;
  pending_recording_count: number;
  caller_policy: CallerPolicy;
}

export interface ValidationResult {
  valid: boolean;
  errors: string[];
}

export interface SimulationTraceStep {
  block_id: string;
  type: string;
  label: string;
  prompt_id?: string | null;
  event?: string | null;
}

export interface SimulationResult {
  status: "awaiting_input" | "awaiting_schedule" | "awaiting_recording" | "awaiting_external" | "complete" | "invalid";
  trace: SimulationTraceStep[];
  awaiting_block_id?: string | null;
  available_digits: string[];
  available_schedule_states?: Array<"open" | "closed" | "holiday">;
  available_recording_outcomes?: Array<"recorded" | "unavailable" | "hangup">;
  available_external_outcomes?: Array<"external_completed" | "external_not_connected" | "external_system_failure">;
  message?: string | null;
}

export interface FlowDiff {
  base_revision_id: number | null;
  legacy_base: boolean;
  added: number;
  removed: number;
  changed: number;
  changes: string[];
  caller_policy_changes: string[];
  schedule_changes: string[];
  recording_changes: string[];
  requires_policy_confirmation: boolean;
}

export interface RevisionDetail {
  revision: Revision;
  schema_version: number;
  flow: FlowDefinition | Record<string, unknown> | null;
  legacy: boolean;
}

export type RecordingStopReason = "finish_key" | "maximum_duration" | "caller_hangup" | "operator_hangup" | "recording_failure";

export interface Recording {
  id: string;
  call_id: string;
  device_id: string;
  revision_id: number;
  block_id: string;
  sequence: number;
  kind?: "voicemail" | "conversation";
  caller: string | null;
  caller_masked: string;
  operator_masked?: string | null;
  segment_count?: number | null;
  captured_at: string;
  duration_ms: number;
  stop_reason: RecordingStopReason;
  status: "uploading" | "processing" | "ready" | "failed";
  listened_at: string | null;
  deleted_at: string | null;
  playback_url: string | null;
  download_url: string | null;
}

export interface RecordingList {
  items: Recording[];
  page: number;
  page_size: number;
  total: number;
}

export interface RecordingSettings {
  mode: "automatic" | "manual";
  days: number;
  quota_bytes: number;
  used_bytes: number;
  pending_count: number;
  updated_at: string;
  updated_by: string;
}
