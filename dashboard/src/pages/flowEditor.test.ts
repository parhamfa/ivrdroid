import { describe, expect, it } from "vitest";
import type { CollectDigitBlock, FlowDefinition } from "../types";
import {
  addDigitBranch,
  allFlowSlots,
  canMoveSubtree,
  countBlocks,
  canInsertAtSlot,
  canRemovePlayPrompt,
  deleteSubtree,
  findBlock,
  insertAtSlot,
  makeBlock,
  moveSubtree,
  owningMenuForSlot,
  removeDigitBranch,
  removePlayPrompt,
  updateBlock,
  validateFlowLocally,
} from "./flowEditor";

const FIRST_PROMPT = "11111111-1111-4111-8111-111111111111";
const SECOND_PROMPT = "22222222-2222-4222-8222-222222222222";

function flowWithMenu(): FlowDefinition {
  return { root: makeBlock("collect_digit", null, null) };
}

describe("owned flow tree", () => {
  it("defaults prompt interruption off and rejects it without a menu prompt", () => {
    const menu = makeBlock("collect_digit", null, null) as CollectDigitBlock;
    expect(menu.allow_prompt_barge_in).toBe(false);
    menu.allow_prompt_barge_in = true;
    menu.branches = [{
      branch_id: crypto.randomUUID(),
      digit: "1",
      root: makeBlock("end_call", null, null),
    }];
    expect(validateFlowLocally({ root: menu }, [], [])).toContain(
      "Prompt interruption requires a menu prompt.",
    );
  });

  it("creates independent prompt subtrees for each digit", () => {
    const menu = flowWithMenu().root as CollectDigitBlock;
    let flow = addDigitBranch({ root: menu }, menu.block_id, "1", FIRST_PROMPT);
    flow = addDigitBranch(flow, menu.block_id, "2", SECOND_PROMPT);
    const updatedMenu = findBlock(flow.root, menu.block_id);
    if (updatedMenu?.type !== "collect_digit") throw new Error("menu missing");
    const first = updatedMenu.branches.find((branch) => branch.digit === "1")?.root;
    const second = updatedMenu.branches.find((branch) => branch.digit === "2")?.root;
    expect(first?.block_id).not.toBe(second?.block_id);
    expect(first).toMatchObject({ type: "play_prompt", prompt_id: FIRST_PROMPT });
    expect(second).toMatchObject({ type: "play_prompt", prompt_id: SECOND_PROMPT });

    const changed = updateBlock(flow, second!.block_id, (block) => block.type === "play_prompt"
      ? { ...block, prompt_id: FIRST_PROMPT }
      : block);
    const changedMenu = findBlock(changed.root, menu.block_id) as CollectDigitBlock;
    expect(changedMenu.branches.find((branch) => branch.digit === "1")?.root).toMatchObject({ prompt_id: FIRST_PROMPT });
    expect(changedMenu.branches.find((branch) => branch.digit === "2")?.root).toMatchObject({ prompt_id: FIRST_PROMPT });
    expect(changedMenu.branches[0].root?.block_id).not.toBe(changedMenu.branches[1].root?.block_id);
  });

  it("creates a valid terminal placeholder when a digit branch has no prompt", () => {
    const menu = flowWithMenu().root as CollectDigitBlock;
    const flow = addDigitBranch({ root: menu }, menu.block_id, "1", null);
    const updatedMenu = findBlock(flow.root, menu.block_id) as CollectDigitBlock;

    expect(updatedMenu.branches[0].root).toMatchObject({ type: "end_call" });
    expect(validateFlowLocally(flow, [], [])).toEqual([]);
  });

  it("allows a recorded action directly in a branch covered by the menu prompt", () => {
    const menu = makeBlock("collect_digit", FIRST_PROMPT, null) as CollectDigitBlock;
    const flow = addDigitBranch({ root: menu }, menu.block_id, "1", null);
    const slot = { kind: "digit", parentId: menu.block_id, digit: "1" } as const;
    const external = makeBlock("external_call", null, null);
    if (external.type !== "external_call") throw new Error("external call missing");
    external.phone_number = "03136644636";

    expect(canInsertAtSlot(flow, slot, "external_call")).toBe(true);
    const updated = insertAtSlot(flow, slot, external);
    expect((updated.root as CollectDigitBlock).branches[0].root).toMatchObject({
      block_id: external.block_id,
      type: "external_call",
    });

    const uncoveredMenu = makeBlock("collect_digit", null, null) as CollectDigitBlock;
    const uncovered = addDigitBranch({ root: uncoveredMenu }, uncoveredMenu.block_id, "1", null);
    expect(canInsertAtSlot(
      uncovered,
      { kind: "digit", parentId: uncoveredMenu.block_id, digit: "1" },
      "external_call",
    )).toBe(false);
  });

  it("removes an existing prompt while preserving its next subtree", () => {
    const menu = flowWithMenu().root as CollectDigitBlock;
    const flow = addDigitBranch({ root: menu }, menu.block_id, "1", FIRST_PROMPT);
    const configuredMenu = findBlock(flow.root, menu.block_id) as CollectDigitBlock;
    const prompt = configuredMenu.branches[0].root;
    if (prompt?.type !== "play_prompt") throw new Error("prompt missing");
    prompt.next = makeBlock("schedule_branch", null, null);
    const nextId = prompt.next.block_id;

    expect(canRemovePlayPrompt(flow, prompt)).toBe(true);
    const updated = removePlayPrompt(flow, prompt.block_id);
    const updatedMenu = findBlock(updated.root, menu.block_id) as CollectDigitBlock;

    expect(updatedMenu.branches[0].root).toMatchObject({ block_id: nextId, type: "schedule_branch" });
    expect(findBlock(updated.root, prompt.block_id)).toBeNull();
    expect(countBlocks(updated.root)).toBe(countBlocks(flow.root) - 1);
  });

  it("replaces a prompt without a next action with a valid End call placeholder", () => {
    const prompt = makeBlock("play_prompt", FIRST_PROMPT, null);
    const updated = removePlayPrompt({ root: prompt }, prompt.block_id);

    expect(updated.root).toMatchObject({ type: "end_call" });
    expect(validateFlowLocally(updated, [], [])).toEqual([]);
  });

  it("keeps the only prompt before recorded actions", () => {
    for (const kind of ["record_message", "external_call"] as const) {
      const prompt = makeBlock("play_prompt", FIRST_PROMPT, null);
      if (prompt.type !== "play_prompt") throw new Error("prompt missing");
      prompt.next = makeBlock(kind, null, null);
      const flow = { root: prompt };

      expect(canRemovePlayPrompt(flow, prompt)).toBe(false);
      expect(removePlayPrompt(flow, prompt.block_id)).toBe(flow);
    }
  });

  it("keeps the only prompt for a recorded action deeper in its subtree", () => {
    const prompt = makeBlock("play_prompt", FIRST_PROMPT, null);
    const schedule = makeBlock("schedule_branch", null, null);
    const external = makeBlock("external_call", null, null);
    if (prompt.type !== "play_prompt" || schedule.type !== "schedule_branch" || external.type !== "external_call") {
      throw new Error("expected blocks missing");
    }
    external.phone_number = "03136644636";
    schedule.on_open = external;
    prompt.next = schedule;
    const flow = { root: prompt };

    expect(canRemovePlayPrompt(flow, prompt)).toBe(false);
    expect(removePlayPrompt(flow, prompt.block_id)).toBe(flow);
  });

  it("removes a branch prompt before recorded actions when the menu already has a prompt", () => {
    const uploadedPrompts = [FIRST_PROMPT, SECOND_PROMPT].map((id, index) => ({
      id,
      name: `notice-${index}`,
      version: 1,
      content_hash: `${index}`,
      size_bytes: 1,
      duration_ms: 1,
      created_at: "",
      used_by: [],
      audio_url: "",
    }));

    for (const kind of ["record_message", "external_call"] as const) {
      const menu = makeBlock("collect_digit", FIRST_PROMPT, null) as CollectDigitBlock;
      const branchPrompt = makeBlock("play_prompt", SECOND_PROMPT, null);
      if (branchPrompt.type !== "play_prompt") throw new Error("branch prompt missing");
      branchPrompt.next = makeBlock(kind, null, null);
      if (branchPrompt.next.type === "external_call") branchPrompt.next.phone_number = "03136644636";
      menu.branches = [{ branch_id: crypto.randomUUID(), digit: "1", root: branchPrompt }];
      const flow = { root: menu };

      expect(canRemovePlayPrompt(flow, branchPrompt)).toBe(true);
      const updated = removePlayPrompt(flow, branchPrompt.block_id);
      const updatedMenu = updated.root as CollectDigitBlock;

      expect(updatedMenu.branches[0].root?.type).toBe(kind);
      expect(validateFlowLocally(updated, uploadedPrompts, [])).toEqual([]);
    }
  });

  it("deleting the root removes every descendant and leaves zero steps", () => {
    const prompt = makeBlock("play_prompt", FIRST_PROMPT, null);
    if (prompt.type !== "play_prompt") throw new Error("prompt missing");
    prompt.next = makeBlock("collect_digit", null, null);
    const flow = { root: prompt };
    expect(countBlocks(flow.root)).toBe(5);
    const deleted = deleteSubtree(flow, prompt.block_id);
    expect(deleted.root).toBeNull();
    expect(countBlocks(deleted.root)).toBe(0);
  });

  it("removing a digit removes only that branch's owned subtree", () => {
    const menu = flowWithMenu().root as CollectDigitBlock;
    let flow = addDigitBranch({ root: menu }, menu.block_id, "1", FIRST_PROMPT);
    flow = addDigitBranch(flow, menu.block_id, "2", SECOND_PROMPT);
    const before = countBlocks(flow.root);
    const after = removeDigitBranch(flow, menu.block_id, "1");
    const updated = findBlock(after.root, menu.block_id) as CollectDigitBlock;
    expect(updated.branches.map((branch) => branch.digit)).toEqual(["2"]);
    expect(countBlocks(after.root)).toBe(before - 1);
  });

  it("rejects duplicate internal block IDs even in separate branches", () => {
    const menu = flowWithMenu().root as CollectDigitBlock;
    let flow = addDigitBranch({ root: menu }, menu.block_id, "1", FIRST_PROMPT);
    flow = addDigitBranch(flow, menu.block_id, "2", SECOND_PROMPT);
    const updated = findBlock(flow.root, menu.block_id) as CollectDigitBlock;
    updated.branches[1].root = updated.branches[0].root;
    const prompts = [
      { id: FIRST_PROMPT, name: "one", version: 1, content_hash: "a", size_bytes: 1, duration_ms: 1, created_at: "", used_by: [], audio_url: "" },
      { id: SECOND_PROMPT, name: "two", version: 1, content_hash: "b", size_bytes: 1, duration_ms: 1, created_at: "", used_by: [], audio_url: "" },
    ];
    expect(validateFlowLocally(flow, prompts, [])).toContain("A block is duplicated or shared between branches.");
  });

  it("offers return-to-menu only inside a normal branch, never its own return-limit path", () => {
    const menu = flowWithMenu().root as CollectDigitBlock;
    let flow = addDigitBranch({ root: menu }, menu.block_id, "1", FIRST_PROMPT);
    const branchRoot = (findBlock(flow.root, menu.block_id) as CollectDigitBlock).branches[0].root!;
    expect(owningMenuForSlot(flow, { kind: "next", parentId: branchRoot.block_id })).toBe(menu.block_id);
    expect(owningMenuForSlot(flow, { kind: "return_limit", parentId: menu.block_id })).toBeNull();
  });

  it("inserts a prompt on an occupied edge without losing the existing subtree", () => {
    const first = makeBlock("play_prompt", FIRST_PROMPT, null);
    if (first.type !== "play_prompt") throw new Error("prompt missing");
    const menu = makeBlock("collect_digit", null, null);
    first.next = menu;
    const flow = { root: first };
    const inserted = makeBlock("play_prompt", SECOND_PROMPT, null);

    const next = insertAtSlot(flow, { kind: "next", parentId: first.block_id }, inserted);

    const updatedFirst = findBlock(next.root, first.block_id);
    expect(updatedFirst).toMatchObject({ type: "play_prompt", next: { block_id: inserted.block_id } });
    expect(findBlock(next.root, inserted.block_id)).toMatchObject({
      type: "play_prompt",
      prompt_id: SECOND_PROMPT,
      next: { block_id: menu.block_id },
    });
    expect(countBlocks(next.root)).toBe(countBlocks(flow.root) + 1);
  });

  it("only offers safe action types on an occupied non-terminal edge", () => {
    const prompt = makeBlock("play_prompt", FIRST_PROMPT, null);
    if (prompt.type !== "play_prompt") throw new Error("prompt missing");
    prompt.next = makeBlock("collect_digit", null, null);
    const flow = { root: prompt };
    const slot = { kind: "next", parentId: prompt.block_id } as const;

    expect(canInsertAtSlot(flow, slot, "play_prompt")).toBe(true);
    expect(canInsertAtSlot(flow, slot, "record_message")).toBe(true);
    expect(canInsertAtSlot(flow, slot, "external_call")).toBe(true);
    expect(canInsertAtSlot(flow, slot, "collect_digit")).toBe(false);
    expect(canInsertAtSlot(flow, slot, "end_call")).toBe(false);
  });

  it("preserves the success path and creates a separate unavailable path for recording", () => {
    const greeting = makeBlock("play_prompt", FIRST_PROMPT, null);
    if (greeting.type !== "play_prompt") throw new Error("greeting missing");
    const existing = makeBlock("collect_digit", null, null);
    greeting.next = existing;
    const flow = { root: greeting };
    const recording = makeBlock("record_message", null, null);

    const next = insertAtSlot(flow, { kind: "next", parentId: greeting.block_id }, recording);
    const inserted = findBlock(next.root, recording.block_id);
    expect(inserted).toMatchObject({
      type: "record_message",
      next: { block_id: existing.block_id },
      on_unavailable: { type: "end_call" },
    });
    expect(countBlocks(next.root)).toBe(countBlocks(flow.root) + 2);
  });

  it("rejects a recording with no earlier prompt on its path", () => {
    const recording = makeBlock("record_message", null, null);
    const prompts = [{ id: FIRST_PROMPT, name: "one", version: 1, content_hash: "a", size_bytes: 1, duration_ms: 1, created_at: "", used_by: [], audio_url: "" }];
    expect(validateFlowLocally({ root: recording }, prompts, [])).toContain(
      "Record message requires an earlier Play prompt or menu prompt notice on this path.",
    );
    expect(canInsertAtSlot({ root: null }, { kind: "root" }, "record_message")).toBe(false);
  });

  it("preserves the completed path and owns two separate external-call fallback paths", () => {
    const notice = makeBlock("play_prompt", FIRST_PROMPT, null);
    if (notice.type !== "play_prompt") throw new Error("notice missing");
    const existing = makeBlock("collect_digit", null, null);
    notice.next = existing;
    const flow = { root: notice };
    const external = makeBlock("external_call", null, null);

    const next = insertAtSlot(flow, { kind: "next", parentId: notice.block_id }, external);
    const inserted = findBlock(next.root, external.block_id);
    expect(inserted).toMatchObject({
      type: "external_call",
      phone_number: "",
      answer_timeout_seconds: 30,
      next: { block_id: existing.block_id },
      on_not_connected: { type: "end_call" },
      on_system_failure: { type: "end_call" },
    });
    expect(countBlocks(next.root)).toBe(countBlocks(flow.root) + 3);
  });

  it("moves an external-call subtree only between notice prompts and leaves a safe source placeholder", () => {
    const menu = flowWithMenu().root as CollectDigitBlock;
    let flow = addDigitBranch({ root: menu }, menu.block_id, "1", FIRST_PROMPT);
    flow = addDigitBranch(flow, menu.block_id, "2", SECOND_PROMPT);
    const configuredMenu = findBlock(flow.root, menu.block_id) as CollectDigitBlock;
    const sourceNotice = configuredMenu.branches[0].root;
    const destinationNotice = configuredMenu.branches[1].root;
    if (sourceNotice?.type !== "play_prompt" || destinationNotice?.type !== "play_prompt") {
      throw new Error("notices missing");
    }
    sourceNotice.next = makeBlock("external_call", null, null);
    const external = sourceNotice.next;
    if (external.type !== "external_call") throw new Error("external call missing");
    external.phone_number = "03136644636";
    const destination = { kind: "next", parentId: destinationNotice.block_id } as const;

    expect(allFlowSlots(flow.root)).toContainEqual(destination);
    expect(canMoveSubtree(flow, external.block_id, destination)).toBe(true);
    const moved = moveSubtree(flow, external.block_id, destination);

    expect(findBlock(moved.root, sourceNotice.block_id)).toMatchObject({
      type: "play_prompt",
      next: { type: "end_call" },
    });
    expect(findBlock(moved.root, destinationNotice.block_id)).toMatchObject({
      type: "play_prompt",
      next: { block_id: external.block_id, phone_number: "03136644636" },
    });
    expect(canMoveSubtree(moved, external.block_id, { kind: "next", parentId: external.block_id })).toBe(false);
  });

  it("validates external-call notice, number, timeout, and all three owned outcomes", () => {
    const prompts = [{ id: FIRST_PROMPT, name: "notice", version: 1, content_hash: "a", size_bytes: 1, duration_ms: 1, created_at: "", used_by: [], audio_url: "" }];
    const external = makeBlock("external_call", null, null);
    expect(validateFlowLocally({ root: external }, prompts, [])).toEqual(expect.arrayContaining([
      "External call requires an earlier Play prompt or menu prompt notice on this path.",
      "External call needs a local or international number containing 8 to 15 digits.",
    ]));
    if (external.type !== "external_call") throw new Error("external call missing");
    external.phone_number = "+989123456789";
    external.answer_timeout_seconds = 30;
    const notice = makeBlock("play_prompt", FIRST_PROMPT, null);
    if (notice.type !== "play_prompt") throw new Error("notice missing");
    notice.next = external;
    expect(validateFlowLocally({ root: notice }, prompts, [])).toEqual([]);
    external.phone_number = "0912,3456789";
    external.answer_timeout_seconds = 121;
    expect(validateFlowLocally({ root: notice }, prompts, [])).toEqual(expect.arrayContaining([
      "External call needs a local or international number containing 8 to 15 digits.",
      "External call answer timeout must be between 5 and 120 seconds.",
    ]));
    external.phone_number = "+00980911";
    external.answer_timeout_seconds = 30;
    expect(validateFlowLocally({ root: notice }, prompts, [])).toContain(
      "External call cannot dial an emergency or public-safety number.",
    );
  });
});
