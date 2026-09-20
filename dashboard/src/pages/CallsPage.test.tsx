import { cleanup, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { afterEach, beforeEach, expect, it, vi } from "vitest";
import { api } from "../api";
import type { CallRecord, Recording } from "../types";
import { CallsPage } from "./CallsPage";
import { callOutcome, recordingStatus } from "./callAudit";

vi.mock("../api", () => ({ api: { calls: vi.fn(), call: vi.fn(), setRecordingListened: vi.fn(), deleteRecording: vi.fn() } }));
const recording: Recording = { id: "audio-1", call_id: "call-1", device_id: "tablet", revision_id: null, block_id: null,
  sequence: -1, kind: "session_audit", caller: null, caller_masked: "••7753", captured_at: "2026-09-12T12:00:01Z",
  duration_ms: 15000, stop_reason: "caller_hangup", status: "ready", listened_at: null, deleted_at: null,
  playback_url: "/test-audio.wav", download_url: "/test-download.wav" };
const call: CallRecord = { id: "call-1", device_id: "tablet", started_at: "2026-09-12T12:00:00Z", caller: null,
  caller_masked: "••7753", policy_decision: "IVR_HANDLED", revision_id: null, menu_path: ["builtin"], result: "REMOTE_HANGUP",
  duration_seconds: 16, events: [], recording_count: 0, pending_recording_count: 0, recordings: [],
  session_audit: { policy_version: 1, state: "ready", duration_ms: 15000, partial: true, stop_reason: "interrupted",
    events: [{ offset_ms: 0, type: "answered" }, { offset_ms: 5250, type: "digit", detail: "digit:3" }], recording } };

beforeEach(() => {
  window.history.replaceState(null, "", "/calls");
  vi.mocked(api.calls).mockResolvedValue([structuredClone(call), { ...call, id: "old-call", session_audit: null }]);
  vi.mocked(api.call).mockResolvedValue(structuredClone(call));
  vi.mocked(api.setRecordingListened).mockResolvedValue({ ...recording, listened_at: "2026-09-12T13:00:00Z" });
  vi.spyOn(HTMLMediaElement.prototype, "pause").mockImplementation(() => {});
});
afterEach(() => { cleanup(); vi.restoreAllMocks(); vi.clearAllMocks(); });

it("opens partial audio with a seekable timeline and preserves historical no-audio state", async () => {
  render(<CallsPage />);
  expect(await screen.findByRole("cell", { name: "Not recorded" })).toBeTruthy();
  fireEvent.click(screen.getByRole("button", { name: /Listen to session from/ }));
  const audio = await screen.findByLabelText("Full session recording") as HTMLAudioElement;
  Object.defineProperty(audio, "readyState", { value: 1 });
  Object.defineProperty(audio, "duration", { value: 15 });
  fireEvent.click(screen.getByRole("button", { name: "Seek to 00:05.250 Menu input" }));
  expect(audio.currentTime).toBe(5.25);
  expect(screen.getByRole("link", { name: "Download" }).getAttribute("href")).toBe("/test-download.wav");
  fireEvent.play(audio);
  await waitFor(() => expect(api.setRecordingListened).toHaveBeenCalledWith("audio-1", true));
  expect(await screen.findByRole("button", { name: "Mark unlistened" })).toBeTruthy();
});

it("loads a direct call link beyond the current history page and deletes only session audio", async () => {
  window.history.replaceState(null, "", "/calls?call=call-1");
  vi.mocked(api.calls).mockResolvedValue([]);
  vi.spyOn(window, "confirm").mockReturnValue(true);
  render(<CallsPage />);
  await screen.findByLabelText("Full session recording");
  vi.mocked(api.call).mockResolvedValue({ ...call, session_audit: { ...call.session_audit!, state: "deleted", recording: { ...recording, status: "deleted", playback_url: null, download_url: null } } });
  fireEvent.click(screen.getByRole("button", { name: "Delete session audio" }));
  await waitFor(() => expect(api.deleteRecording).toHaveBeenCalledWith("audio-1"));
  expect(await screen.findByText(/Audio was deleted/)).toBeTruthy();
  expect(screen.getByRole("button", { name: "Seek to 00:05.250 Menu input" }).hasAttribute("disabled")).toBe(true);
});

it("filters unlistened session audio without including historical calls", async () => {
  render(<CallsPage />);
  await screen.findByRole("cell", { name: "Not recorded" });
  fireEvent.change(screen.getByLabelText("Session audio filter"), { target: { value: "unlistened" } });
  expect(screen.queryByRole("cell", { name: "Not recorded" })).toBeNull();
  expect(screen.getByRole("button", { name: /Listen to session from/ })).toBeTruthy();
});

it("keeps operator transitions seekable while collapsing repeated heartbeats", async () => {
  vi.mocked(api.call).mockResolvedValue({ ...call, session_audit: { ...call.session_audit!, events: [
    { offset_ms: 1000, type: "external_call", block_id: "operator", detail: "ACK" },
    { offset_ms: 2000, type: "external_call", block_id: "operator", detail: "DIALING" },
    { offset_ms: 3000, type: "external_call", block_id: "operator", detail: "DIALING" },
    { offset_ms: 4000, type: "external_call", block_id: "operator", detail: "NOT_CONNECTED" },
    { offset_ms: 5000, type: "external_call", block_id: "operator", detail: "DIALING" },
  ] } });
  render(<CallsPage />);
  fireEvent.click(await screen.findByRole("button", { name: /Listen to session from/ }));
  const audio = await screen.findByLabelText("Full session recording") as HTMLAudioElement;
  Object.defineProperty(audio, "readyState", { value: 1 });
  expect(screen.queryByRole("button", { name: "Seek to 00:01.000 Operator call" })).toBeNull();
  expect(screen.queryByRole("button", { name: "Seek to 00:03.000 Operator call" })).toBeNull();
  expect(screen.getAllByText("Dialing operator")).toHaveLength(2);
  fireEvent.click(screen.getByRole("button", { name: "Seek to 00:04.000 Operator call" }));
  expect(audio.currentTime).toBe(4);
  expect(screen.getByText("Operator did not connect")).toBeTruthy();
});

it("shows the interruption as well as the final hangup, including outcome filtering", async () => {
  const interrupted = { ...call, events: [
    { event: "external_call", block_id: "operator", status: "CONFERENCED" },
    { event: "external_call", block_id: "operator", status: "SYSTEM_FAILURE", reason: "HELPER_CANCELLED" },
  ] };
  vi.mocked(api.calls).mockResolvedValue([interrupted, { ...call, id: "normal" }]);
  vi.mocked(api.call).mockResolvedValue(interrupted);
  const outcome = "Conversation interrupted by system failure · Caller hung up";
  render(<CallsPage />);
  await screen.findByRole("cell", { name: outcome });
  fireEvent.change(screen.getByLabelText("Call outcome filter"), { target: { value: outcome } });
  expect(screen.queryByRole("cell", { name: "Caller hung up" })).toBeNull();
  fireEvent.click(screen.getByRole("cell", { name: outcome }));
  expect(await screen.findByText("Final disconnect")).toBeTruthy();
  expect(screen.getByText("Caller hung up", { selector: "dd" })).toBeTruthy();
});

it("does not call a later dialing failure an interrupted conversation", () => {
  const events = ["CONFERENCED", "COMPLETED", "DIALING", "SYSTEM_FAILURE"].map((status) => ({
    event: "external_call", block_id: "operator", status,
  }));
  expect(callOutcome({ ...call, events })).toBe("System failure during call · Caller hung up");
});

it("keeps recording and cleanup errors separate from a normal caller hangup", async () => {
  const value: CallRecord = { ...call, cleanup_status: "failed", ended_at: "2026-09-12T12:00:16Z",
    pending_session_audio_count: 1, session_audit: { ...call.session_audit!, state: "processing",
      recording: { ...recording, status: "processing", processing_state: "retrying", processing_error: "Permission denied", playback_url: null } } };
  vi.mocked(api.calls).mockResolvedValue([value]);
  vi.mocked(api.call).mockResolvedValue(value);
  render(<CallsPage />);
  expect(await screen.findByText("0 pending recordings · 1 pending session audio")).toBeTruthy();
  expect(screen.getByRole("cell", { name: "Processing needs attention" })).toBeTruthy();
  fireEvent.click(screen.getByRole("cell", { name: "Caller hung up" }));
  expect(await screen.findByText("Resource cleanup")).toBeTruthy();
  expect(screen.getByText("failed", { selector: "dd" })).toBeTruthy();
  expect(screen.getAllByText("Caller hung up", { selector: "dd" })).toHaveLength(2);
});

it("distinguishes missing details, upload failure and capture timing review", () => {
  expect(callOutcome({ ...call, result: "END_DETAILS_UNAVAILABLE", cleanup_status: "pending" })).toBe("Details pending");
  expect(recordingStatus({ ...recording, status: "uploading", processing_error: "Upload failed" })).toBe("Upload needs attention");
  expect(recordingStatus({ ...recording, status: "failed", processing_state: "needs_attention", processing_error: "Timing mismatch" })).toBe("Recording needs review");
});
