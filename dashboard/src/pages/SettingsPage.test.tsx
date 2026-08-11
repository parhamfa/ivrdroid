import { cleanup, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { api } from "../api";
import type { DraftConfiguration, RecordingSettings } from "../types";
import { SettingsPage } from "./SettingsPage";

vi.mock("../api", () => ({
  api: {
    draft: vi.fn(),
    recordingSettings: vi.fn(),
    devices: vi.fn(),
    revisions: vi.fn(),
    audit: vi.fn(),
    saveDraft: vi.fn(),
    saveRecordingSettings: vi.fn(),
    createPairingCode: vi.fn(),
    rollback: vi.fn(),
    activateLegacy: vi.fn(),
    revokeDevice: vi.fn(),
  },
}));

const draft: DraftConfiguration = {
  schema_version: 4,
  edit_version: 4,
  caller_policy: { mode: "ALLOWLIST_ONLY", route_unknown_callers: false, allowlist: [], blocklist: [] },
  schedules: [],
  recording_behavior: { maximum_duration_seconds: 60, finish_key: "#" },
  flow: { root: { block_id: "11111111-1111-4111-8111-111111111111", type: "end_call" } },
};

const operational: RecordingSettings = {
  mode: "manual",
  days: 30,
  quota_bytes: 5 * 1024 ** 3,
  used_bytes: 1024,
  pending_count: 2,
  updated_at: "2026-08-08T00:00:00Z",
  updated_by: "owner@example.com",
};

beforeEach(() => {
  vi.mocked(api.draft).mockResolvedValue(structuredClone(draft));
  vi.mocked(api.recordingSettings).mockResolvedValue({ ...operational });
  vi.mocked(api.devices).mockResolvedValue([]);
  vi.mocked(api.revisions).mockResolvedValue([]);
  vi.mocked(api.audit).mockResolvedValue([]);
  vi.mocked(api.saveDraft).mockImplementation(async (value) => ({ ...value, edit_version: value.edit_version + 1 }));
  vi.mocked(api.saveRecordingSettings).mockImplementation(async (mode, days) => ({ ...operational, mode, days }));
});

afterEach(() => {
  cleanup();
  vi.restoreAllMocks();
  vi.clearAllMocks();
});

describe("recording settings", () => {
  it("shows prompt-interruption capability separately from general V4 readiness", async () => {
    vi.mocked(api.devices).mockResolvedValue([{
      id: "device-1",
      display_name: "SM-T585",
      app_version: "0.8.4-dev",
      helper_version: "0.8.2-dev",
      desired_revision_id: 16,
      active_revision_id: 16,
      status: {
        runtime_versions: [1, 2, 3, 4],
        external_call_control_capable: true,
        conversation_recording_capable: true,
        call_control_protocol_version: 1,
        prompt_barge_in_capable: true,
      },
      last_seen_at: "2026-08-10T00:00:00Z",
      enrolled_at: "2026-08-01T00:00:00Z",
      revoked_at: null,
    }]);
    render(<SettingsPage />);
    expect(await screen.findByText("Prompt interruption: ready")).toBeTruthy();
  });

  it("keeps signed recording behavior separate from immediate retention", async () => {
    vi.spyOn(window, "confirm").mockReturnValue(true);
    render(<SettingsPage />);

    await screen.findByRole("heading", { name: /Recording behavior/i });
    fireEvent.change(screen.getByRole("spinbutton", { name: /Maximum duration/i }), { target: { value: "90" } });
    fireEvent.change(screen.getByRole("combobox", { name: /DTMF finish key/i }), { target: { value: "" } });
    fireEvent.click(screen.getByRole("button", { name: "Save signed draft settings" }));
    await waitFor(() => expect(api.saveDraft).toHaveBeenCalledWith(expect.objectContaining({
      recording_behavior: { maximum_duration_seconds: 90, finish_key: null },
    })));
    expect(api.saveRecordingSettings).not.toHaveBeenCalled();

    fireEvent.change(screen.getByRole("combobox", { name: /Retention mode/i }), { target: { value: "automatic" } });
    fireEvent.change(screen.getByRole("spinbutton", { name: /Retention days/i }), { target: { value: "45" } });
    fireEvent.click(screen.getByRole("button", { name: "Confirm retention policy" }));
    await waitFor(() => expect(api.saveRecordingSettings).toHaveBeenCalledWith("automatic", 45));
    expect(window.confirm).toHaveBeenCalledWith(expect.stringContaining("irreversibly delete"));
    expect(screen.getByText(/2 pending on server/i)).toBeTruthy();
  });
});
