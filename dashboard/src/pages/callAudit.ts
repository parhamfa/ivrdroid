import type { CallRecord, Recording, SessionAuditEvent } from "../types";

export function finalDisconnect(call: CallRecord): string {
  return ({ REMOTE_HANGUP: "Caller hung up", IN_PROGRESS: "In progress", COMPLETED: "Call completed",
    SYSTEM_FAILURE: "System failure", STOCK_DIALER: "Stock dialer", MAX_CALL_DURATION: "Maximum call duration reached",
    RECOVERED_OPERATOR_HANGUP: "Operator disconnected during app recovery", RECOVERED_AFTER_REBOOT: "Interrupted by tablet restart",
    RECOVERED_AND_ENDED: "Call ended during recovery" } as Record<string, string>)[call.result] ?? call.result;
}

export function recordingStatus(recording: Recording): string {
  if (recording.status === "ready") return recording.partial ? "Partial audio" : "Ready";
  if (recording.processing_error) return "Needs attention";
  if (recording.processing_state === "awaiting_device_recovery") return "Awaiting tablet recovery";
  if (recording.processing_state === "active_call") return "Call in progress";
  if (recording.status === "processing") return "Processing audio";
  if (recording.status === "uploading") return "Transferring audio";
  return recording.status === "deleted" ? "Deleted" : "Audio unavailable";
}

export function recordingProgress(recording: Recording): string {
  if (recording.processing_error) return `${recording.processing_error}. The preserved source will be retried when the problem is resolved.`;
  if (recording.processing_state === "awaiting_device_recovery") return "The call has ended, but the tablet has not confirmed the recording's final coverage. Check tablet recovery and storage status; available audio has been preserved.";
  if (recording.processing_state === "active_call") return "Recording is still associated with an active call. Final coverage is not yet known.";
  if (recording.status === "processing") return "The server is verifying and preparing the uploaded audio. The tablet retains its encrypted copy until completion is acknowledged.";
  if (recording.upload_offset != null && recording.source_size_bytes) return `${Math.floor(recording.upload_offset / recording.source_size_bytes * 100)}% received. Transfers resume automatically when the tablet is idle and connected.`;
  return "Waiting for the tablet to finish recovering and transfer the audio. Check its connection and storage status.";
}

// A final disconnect describes how the caller's leg ended, not the whole experience.
// Keep per-attempt state: a later failed dialing attempt must not relabel an earlier successful one.
export function callOutcome(call: CallRecord): string {
  const connected = new Set<string>();
  let interrupted = false;
  let systemFailure = false;
  const states = call.events.filter((event) => event.event === "external_call");
  const transitions = states.length ? states.map((event) => ({
    block: String(event.block_id ?? ""), status: String(event.status ?? "").toUpperCase(),
  })) : (call.session_audit?.events ?? []).filter((event) => event.type === "external_call").map((event) => ({
    block: event.block_id ?? "", status: (event.detail ?? "").toUpperCase(),
  }));
  for (const event of transitions) {
    if (event.status === "CONFERENCED") connected.add(event.block);
    if (event.status === "SYSTEM_FAILURE") {
      systemFailure = true;
      interrupted ||= connected.has(event.block);
    }
    if (["DIALING", "COMPLETED", "NOT_CONNECTED", "SYSTEM_FAILURE"].includes(event.status)) connected.delete(event.block);
  }
  const end = finalDisconnect(call);
  if (interrupted) return `Conversation interrupted by system failure · ${end}`;
  if (systemFailure && call.result !== "SYSTEM_FAILURE") return `System failure during call · ${end}`;
  return end;
}

export function sessionAudioStatus(call: CallRecord): string {
  if (call.result === "STOCK_DIALER") return "Not applicable";
  const audit = call.session_audit;
  if (!audit || audit.state === "disabled") return "Not recorded";
  if (audit.state === "ready" && audit.recording?.playback_url) return audit.partial ? "Partial · Listen" : "Listen";
  if (audit.recording?.processing_error) return "Needs attention";
  if (audit.state === "processing") return "Processing audio";
  if (["pending_upload", "uploading", "processing"].includes(audit.state)) return "Pending upload";
  return "Unavailable";
}

export function audioMatches(call: CallRecord, filter: string): boolean {
  const status = sessionAudioStatus(call);
  if (filter === "all") return true;
  if (filter === "unlistened") return status.includes("Listen") && !call.session_audit?.recording?.listened_at;
  if (filter === "partial") return Boolean(call.session_audit?.partial);
  if (filter === "ready") return status.includes("Listen");
  return status === filter;
}

export function eventTime(milliseconds: number): string {
  const seconds = Math.floor(milliseconds / 1000);
  return `${Math.floor(seconds / 60).toString().padStart(2, "0")}:${(seconds % 60).toString().padStart(2, "0")}.${Math.floor(milliseconds % 1000).toString().padStart(3, "0")}`;
}

// Older devices can report the same operator state on every heartbeat.
export function timelineEvents(events: SessionAuditEvent[]): SessionAuditEvent[] {
  const result: SessionAuditEvent[] = [];
  for (const event of events) {
    if (event.type === "external_call" && event.detail === "ACK") continue;
    const previous = result.at(-1);
    if (event.type === "external_call" && previous?.type === event.type &&
        previous.block_id === event.block_id && previous.detail?.toLowerCase() === event.detail?.toLowerCase()) continue;
    result.push(event);
  }
  return result;
}

export function eventDetail(event: SessionAuditEvent): string {
  if (event.type === "external_call" && event.detail?.startsWith("recording_failure:")) return "Recording problem";
  const detail = event.detail?.replaceAll("_", " ").replaceAll("-", " ").toLowerCase() ?? "";
  if (event.type !== "external_call") return detail;
  return ({ "caller held": "Caller on hold", "dialing": "Dialing operator",
    "operator answered": "Operator answered", "merging": "Connecting both calls",
    "conferenced": "Conversation started", "completed": "Conversation ended",
    "not connected": "Operator did not connect", "system failure": "System failure" } as Record<string, string>)[detail] ?? detail;
}
