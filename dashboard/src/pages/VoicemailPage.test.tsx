import { cleanup, fireEvent, render as renderUI, screen, waitFor, within } from "@testing-library/react";
import type { ReactNode } from "react";
import { DisplaySettingsProvider } from "../displaySettings";
import { DEFAULT_DISPLAY_SETTINGS } from "../display";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { api } from "../api";
import type { Recording, RecordingList } from "../types";
import { VoicemailPage } from "./VoicemailPage";

const render = (ui: ReactNode) => renderUI(<DisplaySettingsProvider>{ui}</DisplaySettingsProvider>);

vi.mock("../api", () => ({
  api: {
    displaySettings: vi.fn(),
    recordings: vi.fn(),
    setRecordingListened: vi.fn(),
    deleteRecording: vi.fn(),
  },
}));

const message: Recording = {
  id: "11111111-1111-4111-8111-111111111111",
  call_id: "22222222-2222-4222-8222-222222222222",
  device_id: "33333333-3333-4333-8333-333333333333",
  revision_id: 85,
  block_id: "44444444-4444-4444-8444-444444444444",
  sequence: 0,
  caller: "+15551234567",
  caller_masked: "••••4567",
  captured_at: "2026-08-08T00:00:00Z",
  duration_ms: 10_000,
  stop_reason: "finish_key",
  status: "ready",
  listened_at: null,
  deleted_at: null,
  playback_url: "/api/admin/v1/recordings/one/audio",
  download_url: "/api/admin/v1/recordings/one/download",
};

const inbox: RecordingList = { items: [message], page: 1, page_size: 50, total: 1 };

beforeEach(() => {
  vi.mocked(api.displaySettings).mockResolvedValue({ ...DEFAULT_DISPLAY_SETTINGS });
  vi.mocked(api.recordings).mockResolvedValue(inbox);
  vi.mocked(api.setRecordingListened).mockImplementation(async (_id, listened) => ({
    ...message,
    listened_at: listened ? "2026-08-08T01:00:00Z" : null,
  }));
  vi.mocked(api.deleteRecording).mockResolvedValue(undefined);
});

afterEach(() => {
  cleanup();
  vi.restoreAllMocks();
  vi.clearAllMocks();
  window.history.replaceState({}, "", "/");
});

describe("voicemail inbox", () => {
  it("keeps the active player open after an unlistened message becomes listened", async () => {
    vi.mocked(api.recordings)
      .mockResolvedValueOnce(inbox)
      .mockResolvedValueOnce({ items: [], page: 1, page_size: 50, total: 0 });
    render(<VoicemailPage />);
    await screen.findByText("+15551234567");
    fireEvent.click(screen.getByRole("button", { name: "Open" }));

    const audio = screen.getByLabelText("Voicemail from +15551234567");
    fireEvent.play(audio);

    await waitFor(() => expect(api.setRecordingListened).toHaveBeenCalledWith(message.id, true));
    expect(await screen.findByRole("heading", { name: "No unlistened recordings" })).toBeTruthy();
    expect(screen.getByRole("dialog", { name: "Voicemail" })).toBeTruthy();
    expect(screen.getByLabelText("Voicemail from +15551234567")).toBe(audio);
    expect(screen.getByRole("button", { name: /Mark unlistened/i })).toBeTruthy();

    fireEvent.click(screen.getByRole("button", { name: /Mark unlistened/i }));
    await waitFor(() => expect(api.setRecordingListened).toHaveBeenLastCalledWith(message.id, false));
    expect(await screen.findByRole("row", { name: /Finish key New Open/i })).toBeTruthy();
    expect(screen.getByRole("button", { name: /Mark listened/i })).toBeTruthy();

    fireEvent.keyDown(document, { key: "Escape" });
    expect(screen.queryByRole("dialog", { name: "Voicemail" })).toBeNull();
  });

  it("filters, plays, marks listened, downloads, and links to the call trace", async () => {
    render(<VoicemailPage />);
    await screen.findByText("+15551234567");
    expect(api.recordings).toHaveBeenCalledWith(true, 1);

    fireEvent.change(screen.getByRole("combobox", { name: "Inbox filter" }), { target: { value: "all" } });
    await waitFor(() => expect(api.recordings).toHaveBeenCalledWith(false, 1));
    fireEvent.click(screen.getByRole("button", { name: "Open" }));

    const audio = screen.getByLabelText("Voicemail from +15551234567");
    expect(audio.getAttribute("src")).toBe(message.playback_url);
    expect(screen.getByRole("link", { name: /Download MP3/i }).getAttribute("href")).toBe(message.download_url);
    expect(screen.getByRole("link", { name: /Open call trace/i }).getAttribute("href")).toContain(encodeURIComponent(message.call_id));
    fireEvent.play(audio);
    await waitFor(() => expect(api.setRecordingListened).toHaveBeenCalledWith(message.id, true));
    expect(await screen.findByRole("button", { name: /Mark unlistened/i })).toBeTruthy();
  });

  it("requires confirmation before deletion and renders the empty state after refresh", async () => {
    vi.spyOn(window, "confirm").mockReturnValue(true);
    vi.mocked(api.recordings)
      .mockResolvedValueOnce(inbox)
      .mockResolvedValue({ items: [], page: 1, page_size: 50, total: 0 });
    render(<VoicemailPage />);
    await screen.findByText("+15551234567");
    fireEvent.click(screen.getByRole("button", { name: "Open" }));
    fireEvent.click(screen.getByRole("button", { name: /Delete/i }));

    await waitFor(() => expect(api.deleteRecording).toHaveBeenCalledWith(message.id));
    expect(window.confirm).toHaveBeenCalledWith(expect.stringContaining("Permanently delete"));
    expect(await screen.findByRole("heading", { name: "No unlistened recordings" })).toBeTruthy();
  });

  it("labels playable surviving audio as partial", async () => {
    vi.mocked(api.recordings).mockResolvedValue({ ...inbox, items: [{ ...message, kind: "conversation", partial: true, stop_reason: "recording_failure" }] });
    render(<VoicemailPage />);
    await screen.findByText("Partial audio");
    fireEvent.click(screen.getByRole("button", { name: "Open" }));
    expect(screen.getByText(/some of the conversation is missing/i)).toBeTruthy();
    expect(screen.getByLabelText("Conversation from +15551234567")).toBeTruthy();
  });

  it("surfaces a durable processing failure and preserves retry information", async () => {
    vi.mocked(api.recordings).mockResolvedValue({ ...inbox, items: [{ ...message, status: "processing", playback_url: null, processing_error: "Server processing workspace is full" }] });
    render(<VoicemailPage />);
    await screen.findByText("Processing needs attention");
    fireEvent.click(screen.getByRole("button", { name: "Open" }));
    expect(screen.getByText(/Server processing workspace is full.*preserved source will be retried/i)).toBeTruthy();
  });

  it("shows pending audio without exposing destructive or listened actions", async () => {
    vi.mocked(api.recordings).mockResolvedValue({
      ...inbox,
      items: [{ ...message, status: "uploading", playback_url: null, download_url: null }],
    });
    render(<VoicemailPage />);
    await screen.findByText("+15551234567");
    fireEvent.click(screen.getByRole("button", { name: "Open" }));

    expect(screen.getByText(/Check its connection and storage status/i)).toBeTruthy();
    expect(screen.queryByRole("audio")).toBeNull();
    expect((screen.getByRole("button", { name: /Delete/i }) as HTMLButtonElement).disabled).toBe(true);
    expect((screen.getByRole("button", { name: /Mark listened/i }) as HTMLButtonElement).disabled).toBe(true);
  });

  it("exposes loading, recoverable error, and keyboard-close states", async () => {
    vi.mocked(api.recordings).mockRejectedValueOnce(new Error("Inbox unavailable"));
    render(<VoicemailPage />);
    expect(screen.getByRole("status").textContent).toContain("Loading recordings");
    expect((await screen.findByRole("alert")).textContent).toContain("Inbox unavailable");

    vi.mocked(api.recordings).mockResolvedValue(inbox);
    fireEvent.click(screen.getByRole("button", { name: "Retry" }));
    await screen.findByText("+15551234567");
    fireEvent.click(screen.getByRole("button", { name: "Open" }));
    expect(screen.getByRole("dialog", { name: "Voicemail" })).toBeTruthy();
    fireEvent.keyDown(document, { key: "Escape" });
    expect(screen.queryByRole("dialog", { name: "Voicemail" })).toBeNull();
  });

  it("opens a recording from the query string", async () => {
    window.history.pushState({}, "", "/voicemail?recording=11111111-1111-4111-8111-111111111111");
    render(<VoicemailPage />);
    expect(await screen.findByRole("dialog", { name: "Voicemail" })).toBeTruthy();
  });

  it("presents a conversation as one logical recording with its masked operator", async () => {
    const conversation: Recording = {
      ...message,
      id: "55555555-5555-4555-8555-555555555555",
      kind: "conversation",
      operator_masked: "••••4636",
      stop_reason: "operator_hangup",
    };
    vi.mocked(api.recordings).mockResolvedValue({ ...inbox, items: [conversation] });
    render(<VoicemailPage />);

    await screen.findByText("Conversation");
    expect(screen.getByText("••••4636")).toBeTruthy();
    fireEvent.click(screen.getByRole("button", { name: "Open" }));
    const drawer = screen.getByRole("dialog", { name: "Conversation" });
    expect(drawer).toBeTruthy();
    expect(within(drawer).getByText("Operator disconnected")).toBeTruthy();
    expect(within(drawer).getByLabelText("Conversation from +15551234567")).toBeTruthy();
  });
});
