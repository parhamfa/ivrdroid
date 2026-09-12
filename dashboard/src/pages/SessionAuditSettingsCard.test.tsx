import { cleanup, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { afterEach, expect, it, vi } from "vitest";
import { api } from "../api";
import type { SessionAuditSettings } from "../types";
import { SessionAuditSettingsCard } from "./SessionAuditSettingsCard";

vi.mock("../api", () => ({ api: { sessionAuditSettings: vi.fn(), saveSessionAuditSettings: vi.fn() } }));
const settings: SessionAuditSettings = { document: { kind: "session_audit_policy", version: 0, enabled: false, local_quota_bytes: 1024 ** 3 },
  sha256: "", signature_b64: "", server_quota_bytes: 1024 ** 3,
  devices: [{ id: "tablet", name: "SM-T585", capable: true, applied_version: 0, enabled: false, spool_bytes: 0, spool_count: 0, last_error: null }] };
afterEach(() => { cleanup(); vi.restoreAllMocks(); vi.clearAllMocks(); });

it("saves independently and distinguishes desired from tablet applied state", async () => {
  vi.mocked(api.sessionAuditSettings).mockResolvedValue(settings);
  vi.mocked(api.saveSessionAuditSettings).mockResolvedValue({ ...settings, document: { ...settings.document, version: 1, enabled: true } });
  render(<SessionAuditSettingsCard />);
  fireEvent.click(await screen.findByRole("checkbox", { name: /Record full IVR sessions/ }));
  fireEvent.click(screen.getByRole("button", { name: "Save call auditing settings" }));
  await waitFor(() => expect(api.saveSessionAuditSettings).toHaveBeenCalledWith(true, 1024 ** 3));
  expect(await screen.findByText("Pending synchronization")).toBeTruthy();
  expect(screen.getByText("On")).toBeTruthy();
  expect(screen.getByText("Off")).toBeTruthy();
});

it("keeps an unsupported tablet off and displays the capability requirement", async () => {
  vi.mocked(api.sessionAuditSettings).mockResolvedValue({ ...settings, devices: [{ ...settings.devices[0], capable: false }] });
  render(<SessionAuditSettingsCard />);
  fireEvent.click(await screen.findByRole("checkbox", { name: /Record full IVR sessions/ }));
  expect(screen.getByRole("button", { name: "Save call auditing settings" }).hasAttribute("disabled")).toBe(true);
  expect(screen.getByText(/App and helper update required/)).toBeTruthy();
});
