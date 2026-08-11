import { cleanup, fireEvent, render, screen, waitFor, within } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { api } from "../api";
import type { DraftConfiguration, FlowDiff } from "../types";
import { CallerPolicyPage } from "./CallerPolicyPage";


vi.mock("../api", () => ({
  api: {
    draft: vi.fn(),
    saveDraft: vi.fn(),
    validateDraft: vi.fn(),
    diffDraft: vi.fn(),
    publishDraft: vi.fn(),
  },
}));

function makeDraft(): DraftConfiguration {
  return {
    schema_version: 4,
    edit_version: 1,
    caller_policy: {
      mode: "ALLOWLIST_ONLY",
      route_unknown_callers: false,
      allowlist: [{ label: "Primary", e164: "+989120000000" }],
      blocklist: [],
    },
    schedules: [],
    recording_behavior: { maximum_duration_seconds: 60, finish_key: "#" },
    flow: {
      root: {
        block_id: "11111111-1111-4111-8111-111111111111",
        type: "end_call",
      },
    },
  };
}

function makeDiff(overrides: Partial<FlowDiff> = {}): FlowDiff {
  return {
    base_revision_id: 19,
    legacy_base: false,
    added: 0,
    removed: 0,
    changed: 0,
    changes: [],
    caller_policy_changes: [],
    schedule_changes: [],
    recording_changes: [],
    requires_policy_confirmation: false,
    ...overrides,
  };
}

beforeEach(() => {
  vi.mocked(api.draft).mockResolvedValue(makeDraft());
  vi.mocked(api.saveDraft).mockImplementation(async (draft) => ({
    ...draft,
    edit_version: draft.edit_version + 1,
  }));
  vi.mocked(api.validateDraft).mockResolvedValue({ valid: true, errors: [] });
  vi.mocked(api.diffDraft).mockResolvedValue(makeDiff());
  vi.mocked(api.publishDraft).mockResolvedValue({
    id: 20,
    schema_version: 4,
    manifest_sha256: "a".repeat(64),
    signature_b64: "signed",
    source_revision_id: null,
    published_at: "2026-08-11T12:00:00Z",
    published_by: "owner@example.com",
  });
});

afterEach(() => {
  cleanup();
  vi.clearAllMocks();
});

describe("Caller Policy publishing", () => {
  it("saves, reviews, confirms, and publishes a broader caller policy", async () => {
    vi.mocked(api.diffDraft).mockResolvedValue(makeDiff({
      caller_policy_changes: ["Policy mode: Allowlist only → Accept all"],
      requires_policy_confirmation: true,
    }));
    render(<CallerPolicyPage />);
    await screen.findByRole("heading", { name: "Policy" });

    expect((screen.getByRole("button", { name: "Save draft" }) as HTMLButtonElement).disabled).toBe(true);
    expect(screen.getByRole("button", { name: /Review & publish/i })).toBeTruthy();

    fireEvent.click(screen.getByRole("button", { name: /^Accept all/ }));
    expect(screen.getByText("Unsaved changes")).toBeTruthy();
    fireEvent.click(screen.getByRole("button", { name: /Review & publish/i }));

    const dialog = await screen.findByRole("dialog", { name: "Review revision" });
    expect(api.saveDraft).toHaveBeenCalledWith(expect.objectContaining({
      caller_policy: expect.objectContaining({ mode: "ACCEPT_ALL" }),
    }));
    expect(within(dialog).getByText("Policy mode: Allowlist only → Accept all")).toBeTruthy();
    const publish = within(dialog).getByRole("button", { name: /Publish signed V4 revision/i }) as HTMLButtonElement;
    expect(publish.disabled).toBe(true);

    fireEvent.click(within(dialog).getByRole("checkbox", { name: /Confirm broader caller access/i }));
    expect(publish.disabled).toBe(false);
    fireEvent.click(publish);

    await waitFor(() => expect(api.publishDraft).toHaveBeenCalledWith({
      edit_version: 2,
      base_revision_id: 19,
    }));
    expect(await screen.findByText(/Revision 20 published/i)).toBeTruthy();
  });

  it("shows a complete no-op review and refuses a duplicate revision", async () => {
    render(<CallerPolicyPage />);
    await screen.findByRole("heading", { name: "Policy" });

    fireEvent.click(screen.getByRole("button", { name: /Review & publish/i }));
    const dialog = await screen.findByRole("dialog", { name: "Review revision" });

    expect(within(dialog).getByText("No caller-policy changes.")).toBeTruthy();
    expect(within(dialog).getByText("No schedule changes.")).toBeTruthy();
    expect(within(dialog).getByText("No recording-setting changes.")).toBeTruthy();
    expect(within(dialog).getByText("No authored flow changes.")).toBeTruthy();
    expect(within(dialog).getByText(/nothing to publish/i)).toBeTruthy();
    expect((within(dialog).getByRole("button", { name: /Publish signed V4 revision/i }) as HTMLButtonElement).disabled).toBe(true);
    expect(api.saveDraft).not.toHaveBeenCalled();
  });

  it("keeps validation failures out of the publish dialog", async () => {
    vi.mocked(api.validateDraft).mockResolvedValue({
      valid: false,
      errors: ["Add a first step before publishing."],
    });
    render(<CallerPolicyPage />);
    await screen.findByRole("heading", { name: "Policy" });

    fireEvent.click(screen.getByRole("button", { name: /Review & publish/i }));

    expect((await screen.findByRole("alert")).textContent).toContain("Add a first step before publishing.");
    expect(screen.queryByRole("dialog", { name: "Review revision" })).toBeNull();
    expect(api.diffDraft).not.toHaveBeenCalled();
  });
});
