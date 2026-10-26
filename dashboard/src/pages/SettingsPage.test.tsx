import { cleanup, fireEvent, render as renderUI, screen, waitFor } from "@testing-library/react";
import type { ReactNode } from "react";
import { DisplaySettingsProvider } from "../displaySettings";
import { DEFAULT_DISPLAY_SETTINGS } from "../display";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { api } from "../api";
import type { DraftConfiguration, EmailSettings, NtfySettings, RecordingSettings } from "../types";
import { SettingsPage } from "./SettingsPage";

const render = (ui: ReactNode) => renderUI(<DisplaySettingsProvider>{ui}</DisplaySettingsProvider>);

vi.mock("../api", () => ({
  api: {
    displaySettings: vi.fn(),
    saveDisplaySettings: vi.fn(),
    draft: vi.fn(),
    recordingSettings: vi.fn(),
    sessionAuditSettings: vi.fn(),
    saveSessionAuditSettings: vi.fn(),
    callSafetySettings: vi.fn(),
    saveCallSafetySettings: vi.fn(),
    ntfySettings: vi.fn(),
    saveNtfySettings: vi.fn(),
    testNtfySettings: vi.fn(),
    emailSettings: vi.fn(),
    saveEmailSettings: vi.fn(),
    testEmailSettings: vi.fn(),
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

const ntfy: NtfySettings = {
  enabled: false,
  server_url: "https://ntfy.sh",
  topic: "",
  token_configured: false,
  events: {
    voicemail_ready: { enabled: true, email: false, priority: "default", include_masked_caller: true },
    conversation_ready: { enabled: true, email: false, priority: "default", include_masked_caller: true },
    ivr_session_failed: { enabled: true, email: false, priority: "high", include_masked_caller: true },
    external_call_failed: { enabled: true, email: false, priority: "high", include_masked_caller: true, not_connected: true, system_failure: true },
    revision_activation_failed: { enabled: true, email: false, priority: "high" },
    storage_full: { enabled: true, email: false, priority: "max", warn_at_quota_percent: 90 },
    tablet_offline: { enabled: true, email: false, priority: "high", idle_timeout_minutes: 15, in_call_timeout_minutes: 45, notify_when_recovered: true },
    ivr_session_completed: { enabled: false, email: false, priority: "default", include_masked_caller: true },
    stock_dialer_routing: { enabled: false, email: false, priority: "low", include_masked_caller: false },
  },
  updated_at: "2026-08-17T00:00:00Z",
  updated_by: "owner@example.com",
};

const email: EmailSettings = {
  enabled: false,
  smtp_host: "smtp.mail.me.com",
  smtp_port: 587,
  smtp_security: "starttls",
  smtp_username: "",
  password_configured: false,
  from_address: "",
  from_name: "IVRdroid",
  recipients: [],
  updated_at: "2026-08-17T00:00:00Z",
  updated_by: "owner@example.com",
};

const configuredEmail: EmailSettings = {
  ...email,
  enabled: true,
  smtp_username: "owner@icloud.com",
  password_configured: true,
  from_address: "ivrdroid@rx1.ir",
  recipients: ["owner@example.com"],
};

beforeEach(() => {
  vi.mocked(api.displaySettings).mockResolvedValue({ ...DEFAULT_DISPLAY_SETTINGS });
  vi.mocked(api.saveDisplaySettings).mockImplementation(async (value) => value);
  vi.mocked(api.callSafetySettings).mockResolvedValue({ document: { kind: "call_safety_policy", schema_version: 1, version: 0, maximum_call_duration_seconds: 3600 }, sha256: "", signature_b64: "", devices: [] });
  vi.mocked(api.sessionAuditSettings).mockResolvedValue({ document: { kind: "session_audit_policy", version: 0, enabled: false, local_quota_bytes: 1024 ** 3 }, sha256: "", signature_b64: "", server_quota_bytes: 1024 ** 3, devices: [] });
  vi.mocked(api.draft).mockResolvedValue(structuredClone(draft));
  vi.mocked(api.recordingSettings).mockResolvedValue({ ...operational });
  vi.mocked(api.ntfySettings).mockResolvedValue(structuredClone(ntfy));
  vi.mocked(api.saveNtfySettings).mockImplementation(async (body) => ({
    ...ntfy,
    ...body,
    token_configured: Boolean(body.token) || ntfy.token_configured,
    updated_at: "2026-08-17T01:00:00Z",
    updated_by: "owner@example.com",
  }));
  vi.mocked(api.testNtfySettings).mockResolvedValue({ ...ntfy, enabled: true, topic: "ivrdroid-test" });
  vi.mocked(api.emailSettings).mockResolvedValue(structuredClone(email));
  vi.mocked(api.saveEmailSettings).mockImplementation(async ({ smtp_password, ...body }) => ({
    ...email,
    ...body,
    password_configured: Boolean(smtp_password) || email.password_configured,
    updated_at: "2026-08-17T01:00:00Z",
    updated_by: "owner@example.com",
  }));
  vi.mocked(api.testEmailSettings).mockResolvedValue(structuredClone(configuredEmail));
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
  it("saves display preferences without resetting or publishing operational edits", async () => {
    render(<SettingsPage />);
    const duration = await screen.findByRole("spinbutton", { name: /Maximum duration/i });
    fireEvent.change(duration, { target: { value: "90" } });
    fireEvent.change(screen.getByLabelText(/Calendar/), { target: { value: "gregorian" } });
    fireEvent.click(screen.getByRole("button", { name: "Save display settings" }));
    await screen.findByText("Display settings saved for all administrators.");
    expect((duration as HTMLInputElement).value).toBe("90");
    expect(api.saveDraft).not.toHaveBeenCalled();
    expect(api.saveRecordingSettings).not.toHaveBeenCalled();
  });

  it("validates the whole-call limit without changing the separate voicemail duration", async () => {
    vi.mocked(api.saveCallSafetySettings).mockResolvedValue({ document: { kind: "call_safety_policy", schema_version: 1, version: 1, maximum_call_duration_seconds: 7200 }, sha256: "", signature_b64: "", devices: [] });
    render(<SettingsPage />);
    const input = await screen.findByLabelText("Maximum call duration (minutes)");
    expect((input as HTMLInputElement).value).toBe("60");
    const save = screen.getByRole("button", { name: "Save call limit" });
    for (const value of ["", "0", "1441", "1.5"]) {
      fireEvent.change(input, { target: { value } });
      expect((save as HTMLButtonElement).disabled).toBe(true);
    }
    fireEvent.change(input, { target: { value: "120" } });
    fireEvent.click(save);
    await waitFor(() => expect(api.saveCallSafetySettings).toHaveBeenCalledWith(120));
    expect(api.saveDraft).not.toHaveBeenCalled();
    expect(await screen.findByText(/this limit takes effect for subsequent calls/i)).toBeTruthy();
  });
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

describe("ntfy settings", () => {
  it("saves per-event knobs without sending a blank token", async () => {
    render(<SettingsPage />);
    await screen.findByRole("heading", { name: /^Notifications/i });
    fireEvent.click(screen.getByRole("checkbox", { name: /Enable ntfy/i }));
    fireEvent.change(screen.getByRole("textbox", { name: /Topic/i }), { target: { value: "ivrdroid-test" } });
    fireEvent.change(screen.getByRole("spinbutton", { name: /Idle timeout \(minutes\)/i }), { target: { value: "20" } });
    fireEvent.click(screen.getByRole("button", { name: "Save" }));
    await waitFor(() => expect(api.saveNtfySettings).toHaveBeenCalledWith(expect.objectContaining({
      enabled: true,
      topic: "ivrdroid-test",
      token: null,
      events: expect.objectContaining({
        tablet_offline: expect.objectContaining({ idle_timeout_minutes: 20 }),
      }),
    })));
  });

  it("sends a single test notification", async () => {
    render(<SettingsPage />);
    await screen.findByRole("heading", { name: /^Notifications/i });
    fireEvent.click(screen.getByRole("checkbox", { name: /Enable ntfy/i }));
    fireEvent.change(screen.getByRole("textbox", { name: /Topic/i }), { target: { value: "ivrdroid-test" } });
    fireEvent.click(screen.getByRole("button", { name: "Send test" }));
    await waitFor(() => expect(api.testNtfySettings).toHaveBeenCalledTimes(1));
  });

  it("disables an event's Email channel until email is enabled", async () => {
    render(<SettingsPage />);
    await screen.findByRole("heading", { name: /^Notifications/i });
    const channel = screen.getByRole("checkbox", { name: "Email: Voicemail ready" }) as HTMLInputElement;
    expect(channel.disabled).toBe(true);
    expect((screen.getByRole("checkbox", { name: "Push: Voicemail ready" }) as HTMLInputElement).disabled).toBe(true);
    fireEvent.click(screen.getByRole("checkbox", { name: /Enable email/i }));
    expect(channel.disabled).toBe(false);
  });

  it("saves email routing through the notification save while ntfy stays off", async () => {
    vi.mocked(api.emailSettings).mockResolvedValue(structuredClone(configuredEmail));
    render(<SettingsPage />);
    await screen.findByRole("heading", { name: /^Notifications/i });
    const channel = screen.getByRole("checkbox", { name: "Email: Stock-dialer routing" }) as HTMLInputElement;
    expect(channel.disabled).toBe(false);
    fireEvent.click(channel);
    fireEvent.click(screen.getByRole("button", { name: "Save" }));
    await waitFor(() => expect(api.saveNtfySettings).toHaveBeenCalledWith(expect.objectContaining({
      enabled: false,
      events: expect.objectContaining({
        stock_dialer_routing: expect.objectContaining({ enabled: false, email: true }),
        voicemail_ready: expect.objectContaining({ email: false }),
      }),
    })));
    expect(api.saveEmailSettings).not.toHaveBeenCalled();
  });
});

describe("email settings", () => {
  it("keeps the saved password when blank and parses recipients on save", async () => {
    render(<SettingsPage />);
    await screen.findByRole("heading", { name: /Email delivery/i });
    fireEvent.click(screen.getByRole("checkbox", { name: /Enable email/i }));
    fireEvent.change(screen.getByRole("textbox", { name: /Username/i }), { target: { value: "owner@icloud.com" } });
    fireEvent.change(screen.getByRole("textbox", { name: /From address/i }), { target: { value: "ivrdroid@rx1.ir" } });
    fireEvent.change(screen.getByRole("textbox", { name: /Recipients/i }), { target: { value: "a@x.ir, b@y.com\nc@z.org" } });
    fireEvent.click(screen.getByRole("button", { name: "Save email settings" }));
    await waitFor(() => expect(api.saveEmailSettings).toHaveBeenCalledWith({
      enabled: true,
      smtp_host: "smtp.mail.me.com",
      smtp_port: 587,
      smtp_security: "starttls",
      smtp_username: "owner@icloud.com",
      smtp_password: null,
      from_address: "ivrdroid@rx1.ir",
      from_name: "IVRdroid",
      recipients: ["a@x.ir", "b@y.com", "c@z.org"],
    }));
    expect(await screen.findByText("Email settings are effective immediately.")).toBeTruthy();
    expect(api.saveNtfySettings).not.toHaveBeenCalled();
  });

  it("swaps the default port with the security mode", async () => {
    render(<SettingsPage />);
    await screen.findByRole("heading", { name: /Email delivery/i });
    fireEvent.click(screen.getByRole("checkbox", { name: /Enable email/i }));
    const port = screen.getByRole("spinbutton", { name: /^Port/i }) as HTMLInputElement;
    fireEvent.change(screen.getByRole("combobox", { name: /^Security/i }), { target: { value: "ssl" } });
    expect(port.value).toBe("465");
    fireEvent.change(screen.getByRole("combobox", { name: /^Security/i }), { target: { value: "starttls" } });
    expect(port.value).toBe("587");
  });

  it("requires saving before a test email", async () => {
    vi.mocked(api.emailSettings).mockResolvedValue(structuredClone(configuredEmail));
    render(<SettingsPage />);
    const test = await screen.findByRole("button", { name: "Send test email" }) as HTMLButtonElement;
    expect(test.disabled).toBe(false);
    expect(screen.queryByText("Save before testing")).toBeNull();
    fireEvent.change(screen.getByRole("textbox", { name: /From name/i }), { target: { value: "Front desk" } });
    expect(test.disabled).toBe(true);
    expect(screen.getByText("Save before testing")).toBeTruthy();
    fireEvent.change(screen.getByRole("textbox", { name: /From name/i }), { target: { value: "IVRdroid" } });
    expect(test.disabled).toBe(false);
  });

  it("shows the server's reason when the test email fails", async () => {
    vi.mocked(api.emailSettings).mockResolvedValue(structuredClone(configuredEmail));
    vi.mocked(api.testEmailSettings).mockRejectedValue(new Error("SMTP authentication failed: check the app-specific password."));
    render(<SettingsPage />);
    fireEvent.click(await screen.findByRole("button", { name: "Send test email" }));
    expect(await screen.findByText("SMTP authentication failed: check the app-specific password.")).toBeTruthy();
    expect(api.testEmailSettings).toHaveBeenCalledTimes(1);
    expect(screen.queryByText("Test email sent.")).toBeNull();
  });
});
