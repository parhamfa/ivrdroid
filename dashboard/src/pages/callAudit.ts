import type { CallRecord } from "../types";

export function sessionAudioStatus(call: CallRecord): string {
  if (call.result === "STOCK_DIALER") return "Not applicable";
  const audit = call.session_audit;
  if (!audit || audit.state === "disabled") return "Not recorded";
  if (audit.state === "ready" && audit.recording?.playback_url) return audit.partial ? "Partial · Listen" : "Listen";
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
