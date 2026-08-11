import { cleanup, fireEvent, render, screen, waitFor, within } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { api } from "../api";
import type { DraftConfiguration, Prompt } from "../types";
import { countBlocks, makeBlock } from "./flowEditor";
import { FlowPage } from "./FlowPage";

vi.mock("../api", () => ({
  api: {
    draft: vi.fn(),
    prompts: vi.fn(),
    revisions: vi.fn(),
    saveDraft: vi.fn(),
    validateDraft: vi.fn(),
    publishDraft: vi.fn(),
    simulateDraft: vi.fn(),
    diffDraft: vi.fn(),
  },
}));

const FIRST_PROMPT = "11111111-1111-4111-8111-111111111111";
const SECOND_PROMPT = "22222222-2222-4222-8222-222222222222";

const prompts: Prompt[] = [
  { id: FIRST_PROMPT, name: "o-1", version: 1, content_hash: "a", size_bytes: 1024, duration_ms: 1000, created_at: "2026-08-07T00:00:00Z", used_by: [], audio_url: "/one.wav" },
  { id: SECOND_PROMPT, name: "o-2", version: 1, content_hash: "b", size_bytes: 1024, duration_ms: 1200, created_at: "2026-08-07T00:00:00Z", used_by: [], audio_url: "/two.wav" },
];

function makeDraft(): DraftConfiguration {
  const menu = makeBlock("collect_digit", null, null);
  if (menu.type !== "collect_digit") throw new Error("menu missing");
  const branchPrompt = makeBlock("play_prompt", FIRST_PROMPT, null);
  if (branchPrompt.type !== "play_prompt") throw new Error("prompt missing");
  branchPrompt.next = makeBlock("end_call", null, null);
  menu.branches = [{ branch_id: crypto.randomUUID(), digit: "1", root: branchPrompt }];
  return {
    schema_version: 4,
    edit_version: 1,
    caller_policy: { mode: "ALLOWLIST_ONLY", route_unknown_callers: false, allowlist: [], blocklist: [] },
    schedules: [],
    recording_behavior: { maximum_duration_seconds: 60, finish_key: "#" },
    flow: { root: menu },
  };
}

beforeEach(() => {
  vi.mocked(api.draft).mockResolvedValue(makeDraft());
  vi.mocked(api.prompts).mockResolvedValue(prompts);
  vi.mocked(api.revisions).mockResolvedValue([]);
  vi.mocked(api.validateDraft).mockResolvedValue({ valid: true, errors: [] });
  vi.mocked(api.simulateDraft).mockResolvedValue({ status: "awaiting_input", trace: [], available_digits: ["1"] });
  vi.mocked(api.diffDraft).mockResolvedValue({
    base_revision_id: null,
    legacy_base: true,
    added: 6,
    removed: 0,
    changed: 0,
    changes: [],
    caller_policy_changes: [],
    schedule_changes: [],
    recording_changes: [],
    requires_policy_confirmation: false,
  });
});

afterEach(() => {
  cleanup();
  vi.clearAllMocks();
});

describe("Flow Studio V4", () => {
  it("enables, saves, reloads, and clears accessible prompt interruption", async () => {
    vi.mocked(api.saveDraft).mockImplementation(async (draft) => draft);
    const firstRender = render(<FlowPage />);
    await screen.findByRole("region", { name: "IVR flow canvas" });

    const toggle = screen.getByRole("checkbox", {
      name: /Allow configured digits during prompt/i,
    }) as HTMLInputElement;
    expect(toggle.checked).toBe(false);
    expect(toggle.disabled).toBe(true);

    fireEvent.change(screen.getByRole("combobox", { name: "Menu prompt" }), {
      target: { value: FIRST_PROMPT },
    });
    expect(toggle.disabled).toBe(false);
    fireEvent.click(toggle);
    expect(toggle.checked).toBe(true);
    expect(screen.getByText("1 digit branch · prompt input on", { exact: true })).toBeTruthy();
    fireEvent.click(screen.getByRole("button", { name: /Save draft/i }));
    await waitFor(() => expect(api.saveDraft).toHaveBeenCalled());
    const saved = vi.mocked(api.saveDraft).mock.calls.at(-1)?.[0];
    expect(saved?.flow.root).toMatchObject({ allow_prompt_barge_in: true });

    firstRender.unmount();
    vi.mocked(api.draft).mockResolvedValue(saved!);
    render(<FlowPage />);
    await screen.findByRole("region", { name: "IVR flow canvas" });
    const reloaded = screen.getByRole("checkbox", {
      name: /Allow configured digits during prompt/i,
    }) as HTMLInputElement;
    expect(reloaded.checked).toBe(true);

    fireEvent.change(screen.getByRole("combobox", { name: "Menu prompt" }), {
      target: { value: "" },
    });
    expect(reloaded.checked).toBe(false);
    expect(reloaded.disabled).toBe(true);
    expect(screen.queryByText("1 digit branch · prompt input on", { exact: true })).toBeNull();
  });

  it("adds a digit with its own selected uploaded prompt", async () => {
    render(<FlowPage />);
    const canvas = await screen.findByRole("region", { name: "IVR flow canvas" });
    fireEvent.click(within(canvas).getByRole("button", { name: "Add digit" }));
    const dialog = screen.getByRole("dialog", { name: "Add digit branch" });
    fireEvent.change(within(dialog).getByRole("combobox", { name: "First prompt" }), { target: { value: SECOND_PROMPT } });
    fireEvent.click(within(dialog).getByRole("button", { name: "Add branch" }));

    expect(within(canvas).getByText("Play prompt — o-1", { exact: true })).toBeTruthy();
    expect(within(canvas).getByText("Play prompt — o-2", { exact: true })).toBeTruthy();
    expect(screen.queryByText("Step ID")).toBeNull();
    expect(screen.queryByText("Next step")).toBeNull();
  });

  it("adds and saves a digit branch with no prompt", async () => {
    vi.mocked(api.saveDraft).mockImplementation(async (draft) => draft);
    render(<FlowPage />);
    const canvas = await screen.findByRole("region", { name: "IVR flow canvas" });
    fireEvent.click(within(canvas).getByRole("button", { name: "Add digit" }));
    const dialog = screen.getByRole("dialog", { name: "Add digit branch" });
    const promptSelect = within(dialog).getByRole("combobox", { name: "First prompt" });

    fireEvent.change(promptSelect, { target: { value: "" } });
    expect(within(promptSelect).getByRole("option", { name: "No prompt" })).toBeTruthy();
    expect((within(dialog).getByRole("button", { name: "Add branch" }) as HTMLButtonElement).disabled).toBe(false);
    expect(within(dialog).getByText("No prompt will play when this branch starts.")).toBeTruthy();
    fireEvent.click(within(dialog).getByRole("button", { name: "Add branch" }));

    expect(screen.getByText("7 steps · 5 branches")).toBeTruthy();
    expect(within(canvas).getAllByText("Play prompt — o-1", { exact: true })).toHaveLength(1);
    expect(screen.getByText("All good")).toBeTruthy();
    fireEvent.click(screen.getByRole("button", { name: /Save draft/i }));
    await waitFor(() => expect(api.saveDraft).toHaveBeenCalled());
    const saved = vi.mocked(api.saveDraft).mock.calls.at(-1)?.[0];
    expect(saved?.flow.root).toMatchObject({
      type: "collect_digit",
      branches: expect.arrayContaining([
        expect.objectContaining({ digit: "0", root: expect.objectContaining({ type: "end_call" }) }),
      ]),
    });
  });

  it("removes a prompt from an existing branch and keeps its next action", async () => {
    render(<FlowPage />);
    const canvas = await screen.findByRole("region", { name: "IVR flow canvas" });
    fireEvent.click(within(canvas).getByRole("button", { name: /Select Play prompt — o-1/i }));
    const promptSelect = screen.getByRole("combobox", { name: "Prompt" });
    const noPrompt = within(promptSelect).getByRole("option", { name: "No prompt" }) as HTMLOptionElement;

    expect(noPrompt.disabled).toBe(false);
    fireEvent.change(promptSelect, { target: { value: noPrompt.value } });

    expect(within(canvas).queryByText("Play prompt — o-1", { exact: true })).toBeNull();
    expect(within(canvas).getAllByRole("button", { name: /Select End call/i })).toHaveLength(4);
    expect(screen.getByText("5 steps · 4 branches")).toBeTruthy();
    expect(screen.getByText("All good")).toBeTruthy();
    expect(screen.getByText("Prompt removed; the next action was kept in this branch.")).toBeTruthy();

    fireEvent.click(screen.getByRole("button", { name: "Undo" }));
    expect(within(canvas).getByText("Play prompt — o-1", { exact: true })).toBeTruthy();
    expect(screen.getByText("6 steps · 4 branches")).toBeTruthy();
  });

  it("keeps No prompt unavailable when a recorded external call has no earlier notice", async () => {
    const draft = makeDraft();
    const menu = draft.flow.root;
    if (menu?.type !== "collect_digit" || menu.branches[0].root?.type !== "play_prompt") {
      throw new Error("menu branch missing");
    }
    const external = makeBlock("external_call", null, null);
    if (external.type !== "external_call") throw new Error("external call missing");
    external.phone_number = "03136644636";
    menu.branches[0].root.next = external;
    vi.mocked(api.draft).mockResolvedValue(draft);
    render(<FlowPage />);

    const canvas = await screen.findByRole("region", { name: "IVR flow canvas" });
    fireEvent.click(within(canvas).getByRole("button", { name: /Select Play prompt — o-1/i }));
    const promptSelect = screen.getByRole("combobox", { name: "Prompt" });
    const noPrompt = within(promptSelect).getByRole("option", { name: "No prompt — earlier notice required" }) as HTMLOptionElement;

    expect(noPrompt.disabled).toBe(true);
    expect(screen.getByText("No prompt is unavailable because a recorded action below this step has no other earlier prompt notice on its path.")).toBeTruthy();
  });

  it("removes a branch prompt before a recorded external call when the menu already has the notice", async () => {
    const draft = makeDraft();
    const menu = draft.flow.root;
    if (menu?.type !== "collect_digit" || menu.branches[0].root?.type !== "play_prompt") {
      throw new Error("menu branch missing");
    }
    menu.prompt_id = SECOND_PROMPT;
    const external = makeBlock("external_call", null, null);
    if (external.type !== "external_call") throw new Error("external call missing");
    external.phone_number = "03136644636";
    menu.branches[0].root.next = external;
    vi.mocked(api.draft).mockResolvedValue(draft);
    render(<FlowPage />);

    const canvas = await screen.findByRole("region", { name: "IVR flow canvas" });
    fireEvent.click(within(canvas).getByRole("button", { name: /Select Play prompt — o-1/i }));
    const promptSelect = screen.getByRole("combobox", { name: "Prompt" });
    const noPrompt = within(promptSelect).getByRole("option", { name: "No prompt" }) as HTMLOptionElement;

    expect(noPrompt.disabled).toBe(false);
    expect(screen.getByText("An earlier prompt exists on this path. Make sure its recording notice finishes before callers can interrupt it.")).toBeTruthy();
    fireEvent.change(promptSelect, { target: { value: noPrompt.value } });

    expect(within(canvas).queryByText("Play prompt — o-1", { exact: true })).toBeNull();
    expect(within(canvas).getByText("External call — ••••4636", { exact: true })).toBeTruthy();
    expect(screen.getByText("All good")).toBeTruthy();
  });

  it("deletes the selected root and all descendants, then undo restores them", async () => {
    const draft = makeDraft();
    vi.mocked(api.draft).mockResolvedValue(draft);
    render(<FlowPage />);
    const initialCount = countBlocks(draft.flow.root);
    const canvas = await screen.findByRole("region", { name: "IVR flow canvas" });
    fireEvent.click(within(canvas).getByRole("button", { name: /Select Collect one digit/i }));
    fireEvent.click(screen.getByRole("button", { name: "Delete subtree" }));
    fireEvent.click(screen.getByRole("button", { name: `Delete ${initialCount} steps` }));

    expect(screen.getByText("0 steps · 0 branches")).toBeTruthy();
    expect(within(canvas).getByText("Build the first call path")).toBeTruthy();
    fireEvent.click(screen.getByRole("button", { name: "Undo" }));
    expect(screen.getByText(`${initialCount} steps · 4 branches`)).toBeTruthy();
    expect(within(canvas).getByRole("button", { name: /Select Collect one digit/i })).toBeTruthy();
  });

  it("keeps the palette usable and places a prompt between branch steps", async () => {
    render(<FlowPage />);
    const canvas = await screen.findByRole("region", { name: "IVR flow canvas" });
    const palette = screen.getByRole("complementary", { name: "Add step palette" });
    const playPrompt = within(palette).getByRole("button", { name: /Play prompt/i });

    expect((playPrompt as HTMLButtonElement).disabled).toBe(false);
    fireEvent.click(playPrompt);
    expect(within(palette).getByText("Placing Play prompt. Choose a highlighted position.")).toBeTruthy();
    fireEvent.click(within(canvas).getByRole("button", { name: /Insert step between Play prompt — o-1 and End call/i }));

    expect(screen.getByText("7 steps · 4 branches")).toBeTruthy();
    expect(within(canvas).getAllByText("Play prompt — o-1", { exact: true })).toHaveLength(2);
    expect(within(canvas).getAllByRole("button", { name: /Select End call/i })).toHaveLength(4);
    fireEvent.change(screen.getByRole("combobox", { name: "Prompt" }), { target: { value: SECOND_PROMPT } });
    expect(within(canvas).getByText("Play prompt — o-2", { exact: true })).toBeTruthy();
    expect(within(canvas).getByText("Play prompt — o-1", { exact: true })).toBeTruthy();
  });

  it("selects the exact occupied edge between top-level blocks before choosing an action", async () => {
    const draft = makeDraft();
    const intro = makeBlock("play_prompt", SECOND_PROMPT, null);
    if (intro.type !== "play_prompt") throw new Error("intro missing");
    intro.next = draft.flow.root;
    draft.flow.root = intro;
    vi.mocked(api.draft).mockResolvedValue(draft);
    render(<FlowPage />);
    const canvas = await screen.findByRole("region", { name: "IVR flow canvas" });

    fireEvent.click(within(canvas).getByRole("button", { name: /Insert step between Play prompt — o-2 and Collect one digit/i }));

    const palette = screen.getByRole("complementary", { name: "Add step palette" });
    expect(within(palette).getByText("Position selected. Choose the step to insert.")).toBeTruthy();
    expect((within(palette).getByRole("button", { name: /Collect one digit/i }) as HTMLButtonElement).disabled).toBe(true);
    fireEvent.click(within(palette).getByRole("button", { name: /Play prompt/i }));
    expect(screen.getByText("8 steps · 4 branches")).toBeTruthy();
    expect(within(canvas).getByRole("button", { name: /Select Collect one digit/i })).toBeTruthy();
  });

  it("adds an explicit recording only after a greeting and renders both outcomes", async () => {
    const draft = makeDraft();
    const greeting = makeBlock("play_prompt", SECOND_PROMPT, null);
    if (greeting.type !== "play_prompt") throw new Error("greeting missing");
    greeting.next = draft.flow.root;
    draft.flow.root = greeting;
    vi.mocked(api.draft).mockResolvedValue(draft);
    render(<FlowPage />);

    const canvas = await screen.findByRole("region", { name: "IVR flow canvas" });
    const palette = screen.getByRole("complementary", { name: "Add step palette" });
    expect(within(palette).getByText("Up to 60s · # finishes")).toBeTruthy();
    fireEvent.click(within(palette).getByRole("button", { name: /Record message/i }));
    fireEvent.click(within(canvas).getByRole("button", { name: /Insert step between Play prompt — o-2 and Collect one digit/i }));

    expect(within(canvas).getByRole("button", { name: /Select Record message/i })).toBeTruthy();
    expect(within(canvas).getByRole("region", { name: "Recording outcomes" })).toBeTruthy();
    expect(within(canvas).getByText("Recorded")).toBeTruthy();
    expect(within(canvas).getByText("Unavailable")).toBeTruthy();
    expect(screen.getByText(/built-in beep, then records for up to 60 seconds/i)).toBeTruthy();
  });

  it("configures an external call after its notice and renders all three outcomes", async () => {
    const draft = makeDraft();
    const notice = makeBlock("play_prompt", SECOND_PROMPT, null);
    if (notice.type !== "play_prompt") throw new Error("notice missing");
    notice.next = draft.flow.root;
    draft.flow.root = notice;
    vi.mocked(api.draft).mockResolvedValue(draft);
    render(<FlowPage />);

    const canvas = await screen.findByRole("region", { name: "IVR flow canvas" });
    const palette = screen.getByRole("complementary", { name: "Add step palette" });
    fireEvent.click(within(palette).getByRole("button", { name: /External call/i }));
    fireEvent.click(within(canvas).getByRole("button", { name: /Insert step between Play prompt — o-2 and Collect one digit/i }));

    expect(within(canvas).getByRole("region", { name: "External call outcomes" })).toBeTruthy();
    expect(within(canvas).getByText("Completed")).toBeTruthy();
    expect(within(canvas).getByText("Not connected")).toBeTruthy();
    expect(within(canvas).getByText("System failure")).toBeTruthy();
    fireEvent.change(screen.getByRole("textbox", { name: "Operator phone number" }), { target: { value: "03136644636" } });
    fireEvent.change(screen.getByRole("spinbutton", { name: "Answer timeout" }), { target: { value: "45" } });
    expect(within(canvas).getByText("External call — ••••4636", { exact: true })).toBeTruthy();
    expect(within(canvas).getByText("Waits up to 45s · recording required", { exact: true })).toBeTruthy();
  });

  it("moves an external-call card with all owned outcomes through the inspector", async () => {
    const draft = makeDraft();
    const menu = draft.flow.root;
    if (menu?.type !== "collect_digit" || menu.branches[0].root?.type !== "play_prompt") {
      throw new Error("menu branch missing");
    }
    const external = makeBlock("external_call", null, null);
    if (external.type !== "external_call") throw new Error("external call missing");
    external.phone_number = "03136644636";
    menu.branches[0].root.next = external;
    const secondNotice = makeBlock("play_prompt", SECOND_PROMPT, null);
    if (secondNotice.type !== "play_prompt") throw new Error("notice missing");
    secondNotice.next = makeBlock("end_call", null, null);
    menu.branches.push({ branch_id: crypto.randomUUID(), digit: "2", root: secondNotice });
    vi.mocked(api.draft).mockResolvedValue(draft);
    render(<FlowPage />);

    const canvas = await screen.findByRole("region", { name: "IVR flow canvas" });
    fireEvent.click(within(canvas).getByRole("button", { name: /Select External call/i }));
    fireEvent.click(screen.getByRole("button", { name: "Move subtree" }));
    const dialog = screen.getByRole("dialog", { name: "Move subtree" });
    const target = within(dialog).getByRole("option", { name: /after Play prompt — o-2/i }) as HTMLOptionElement;
    fireEvent.change(within(dialog).getByRole("combobox", { name: "Move destination" }), { target: { value: target.value } });
    fireEvent.click(within(dialog).getByRole("button", { name: "Move subtree" }));

    expect(screen.getByText("Step moved with all of its owned outcomes.")).toBeTruthy();
    expect(within(canvas).getByRole("region", { name: "External call outcomes" })).toBeTruthy();
    expect(within(canvas).getByText("External call — ••••4636", { exact: true })).toBeTruthy();
    expect(screen.getByText("11 steps · 8 branches")).toBeTruthy();
  });

  it("simulates each named external-call outcome event", async () => {
    vi.mocked(api.simulateDraft)
      .mockResolvedValueOnce({
        status: "awaiting_external",
        trace: [],
        available_digits: [],
        available_external_outcomes: ["external_completed", "external_not_connected", "external_system_failure"],
      })
      .mockResolvedValueOnce({ status: "complete", trace: [], available_digits: [] });
    render(<FlowPage />);
    await screen.findByRole("region", { name: "IVR flow canvas" });

    fireEvent.click(screen.getByRole("button", { name: /Simulate/i }));
    const dialog = await screen.findByRole("dialog", { name: "Simulate call path" });
    expect(within(dialog).getByRole("button", { name: "Completed" })).toBeTruthy();
    expect(within(dialog).getByRole("button", { name: "System failure" })).toBeTruthy();
    fireEvent.click(within(dialog).getByRole("button", { name: "Not connected" }));

    await waitFor(() => expect(api.simulateDraft).toHaveBeenLastCalledWith(expect.anything(), ["external_not_connected"]));
  });

  it("reviews every signed configuration section and publishes the exact reviewed draft", async () => {
    vi.mocked(api.diffDraft).mockResolvedValue({
      base_revision_id: 19,
      legacy_base: false,
      added: 0,
      removed: 0,
      changed: 1,
      changes: ["Changed Collect one digit"],
      caller_policy_changes: ["Policy mode: Allowlist only → Accept all"],
      schedule_changes: ["Changed schedule: Business hours"],
      recording_changes: ["Maximum recording duration: 60 → 90 seconds"],
      requires_policy_confirmation: false,
    });
    vi.mocked(api.publishDraft).mockResolvedValue({
      id: 20,
      schema_version: 4,
      manifest_sha256: "a".repeat(64),
      signature_b64: "signed",
      source_revision_id: null,
      published_at: "2026-08-11T12:00:00Z",
      published_by: "owner@example.com",
    });
    render(<FlowPage />);
    await screen.findByRole("region", { name: "IVR flow canvas" });

    fireEvent.click(screen.getByRole("button", { name: /Review & publish/i }));
    const dialog = await screen.findByRole("dialog", { name: "Review revision" });
    expect(within(dialog).getByText("Policy mode: Allowlist only → Accept all")).toBeTruthy();
    expect(within(dialog).getByText("Changed schedule: Business hours")).toBeTruthy();
    expect(within(dialog).getByText("Maximum recording duration: 60 → 90 seconds")).toBeTruthy();
    expect(within(dialog).getByText("Changed Collect one digit")).toBeTruthy();

    fireEvent.click(within(dialog).getByRole("button", { name: /Publish signed V4 revision/i }));
    await waitFor(() => expect(api.publishDraft).toHaveBeenCalledWith({
      edit_version: 1,
      base_revision_id: 19,
    }));
    expect(await screen.findByText(/Revision 20 published/i)).toBeTruthy();
  });
});
