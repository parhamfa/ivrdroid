import type {
  CollectDigitBlock,
  FlowBlock,
  FlowDefinition,
  Prompt,
  Schedule,
} from "../types";

export const DTMF_KEYS = "0123456789*#".split("");
const EMERGENCY_SERVICE_NUMBERS = new Set(["110", "112", "115", "125", "911", "999"]);

function isObviousEmergencyNumber(value: string): boolean {
  const digits = value.startsWith("+") ? value.slice(1) : value;
  const candidates = new Set([digits, digits.replace(/^0+/, "")]);
  for (const prefix of ["00", "98", "0098", "1", "44"]) {
    if (!digits.startsWith(prefix)) continue;
    const remainder = digits.slice(prefix.length);
    candidates.add(remainder);
    candidates.add(remainder.replace(/^0+/, ""));
  }
  return [...candidates].some((candidate) => EMERGENCY_SERVICE_NUMBERS.has(candidate));
}

export type BlockKind =
  | "play_prompt"
  | "record_message"
  | "external_call"
  | "collect_digit"
  | "schedule_branch"
  | "return_to_menu"
  | "end_call";

export type FlowSlot =
  | { kind: "root" }
  | { kind: "next"; parentId: string }
  | { kind: "unavailable"; parentId: string }
  | { kind: "not_connected"; parentId: string }
  | { kind: "system_failure"; parentId: string }
  | { kind: "digit"; parentId: string; digit: string }
  | { kind: "timeout"; parentId: string }
  | { kind: "invalid"; parentId: string }
  | { kind: "return_limit"; parentId: string }
  | { kind: "open"; parentId: string }
  | { kind: "closed"; parentId: string }
  | { kind: "holiday"; parentId: string };

function uuid(): string {
  return globalThis.crypto.randomUUID();
}

export function makeBlock(
  kind: BlockKind,
  promptId: string | null,
  scheduleId: string | null,
): FlowBlock {
  if (kind === "play_prompt") {
    return { block_id: uuid(), type: "play_prompt", prompt_id: promptId, next: null };
  }
  if (kind === "record_message") {
    return {
      block_id: uuid(),
      type: "record_message",
      next: makeBlock("end_call", null, null),
      on_unavailable: makeBlock("end_call", null, null),
    };
  }
  if (kind === "external_call") {
    return {
      block_id: uuid(),
      type: "external_call",
      phone_number: "",
      answer_timeout_seconds: 30,
      next: makeBlock("end_call", null, null),
      on_not_connected: makeBlock("end_call", null, null),
      on_system_failure: makeBlock("end_call", null, null),
    };
  }
  if (kind === "collect_digit") {
    return {
      block_id: uuid(),
      type: "collect_digit",
      prompt_id: promptId,
      allow_prompt_barge_in: false,
      timeout_ms: 5000,
      maximum_attempts: 3,
      maximum_menu_returns: 2,
      branches: [],
      on_timeout: makeBlock("end_call", null, null),
      on_invalid: makeBlock("end_call", null, null),
      on_return_limit: makeBlock("end_call", null, null),
    };
  }
  if (kind === "schedule_branch") {
    return {
      block_id: uuid(),
      type: "schedule_branch",
      schedule_id: scheduleId,
      on_open: makeBlock("end_call", null, null),
      on_closed: makeBlock("end_call", null, null),
      on_holiday: makeBlock("end_call", null, null),
    };
  }
  if (kind === "return_to_menu") {
    return { block_id: uuid(), type: "return_to_menu" };
  }
  return { block_id: uuid(), type: "end_call" };
}

export function ownedChildren(block: FlowBlock): FlowBlock[] {
  if (block.type === "play_prompt") return block.next ? [block.next] : [];
  if (block.type === "record_message") {
    return [block.next, block.on_unavailable].flatMap((item) => item ? [item] : []);
  }
  if (block.type === "external_call") {
    return [block.next, block.on_not_connected, block.on_system_failure].flatMap((item) => item ? [item] : []);
  }
  if (block.type === "collect_digit") {
    return [
      ...block.branches.flatMap((branch) => branch.root ? [branch.root] : []),
      ...[block.on_timeout, block.on_invalid, block.on_return_limit].flatMap((item) => item ? [item] : []),
    ];
  }
  if (block.type === "schedule_branch") {
    return [block.on_open, block.on_closed, block.on_holiday].flatMap((item) => item ? [item] : []);
  }
  return [];
}

export function countBlocks(root: FlowBlock | null): number {
  if (!root) return 0;
  return 1 + ownedChildren(root).reduce((total, child) => total + countBlocks(child), 0);
}

export function countBranches(root: FlowBlock | null): number {
  if (!root) return 0;
  const own = root.type === "collect_digit"
    ? root.branches.length + 3
    : root.type === "schedule_branch"
      ? 3
      : root.type === "record_message"
        ? 2
        : root.type === "external_call"
          ? 3
      : 0;
  return own + ownedChildren(root).reduce((total, child) => total + countBranches(child), 0);
}

export function findBlock(root: FlowBlock | null, blockId: string): FlowBlock | null {
  if (!root) return null;
  if (root.block_id === blockId) return root;
  for (const child of ownedChildren(root)) {
    const found = findBlock(child, blockId);
    if (found) return found;
  }
  return null;
}

function mapBlock(
  block: FlowBlock | null,
  blockId: string,
  update: (current: FlowBlock) => FlowBlock,
): FlowBlock | null {
  if (!block) return null;
  if (block.block_id === blockId) return update(block);
  if (block.type === "play_prompt") {
    return { ...block, next: mapBlock(block.next, blockId, update) };
  }
  if (block.type === "record_message") {
    return {
      ...block,
      next: mapBlock(block.next, blockId, update),
      on_unavailable: mapBlock(block.on_unavailable, blockId, update),
    };
  }
  if (block.type === "external_call") {
    return {
      ...block,
      next: mapBlock(block.next, blockId, update),
      on_not_connected: mapBlock(block.on_not_connected, blockId, update),
      on_system_failure: mapBlock(block.on_system_failure, blockId, update),
    };
  }
  if (block.type === "collect_digit") {
    return {
      ...block,
      branches: block.branches.map((branch) => ({
        ...branch,
        root: mapBlock(branch.root, blockId, update),
      })),
      on_timeout: mapBlock(block.on_timeout, blockId, update),
      on_invalid: mapBlock(block.on_invalid, blockId, update),
      on_return_limit: mapBlock(block.on_return_limit, blockId, update),
    };
  }
  if (block.type === "schedule_branch") {
    return {
      ...block,
      on_open: mapBlock(block.on_open, blockId, update),
      on_closed: mapBlock(block.on_closed, blockId, update),
      on_holiday: mapBlock(block.on_holiday, blockId, update),
    };
  }
  return block;
}

export function updateBlock(
  flow: FlowDefinition,
  blockId: string,
  update: (current: FlowBlock) => FlowBlock,
): FlowDefinition {
  return { root: mapBlock(flow.root, blockId, update) };
}

function removeBlock(block: FlowBlock | null, blockId: string): FlowBlock | null {
  if (!block || block.block_id === blockId) return null;
  if (block.type === "play_prompt") return { ...block, next: removeBlock(block.next, blockId) };
  if (block.type === "record_message") {
    return {
      ...block,
      next: removeBlock(block.next, blockId),
      on_unavailable: removeBlock(block.on_unavailable, blockId),
    };
  }
  if (block.type === "external_call") {
    return {
      ...block,
      next: removeBlock(block.next, blockId),
      on_not_connected: removeBlock(block.on_not_connected, blockId),
      on_system_failure: removeBlock(block.on_system_failure, blockId),
    };
  }
  if (block.type === "collect_digit") {
    return {
      ...block,
      branches: block.branches.map((branch) => ({ ...branch, root: removeBlock(branch.root, blockId) })),
      on_timeout: removeBlock(block.on_timeout, blockId),
      on_invalid: removeBlock(block.on_invalid, blockId),
      on_return_limit: removeBlock(block.on_return_limit, blockId),
    };
  }
  if (block.type === "schedule_branch") {
    return {
      ...block,
      on_open: removeBlock(block.on_open, blockId),
      on_closed: removeBlock(block.on_closed, blockId),
      on_holiday: removeBlock(block.on_holiday, blockId),
    };
  }
  return block;
}

export function deleteSubtree(flow: FlowDefinition, blockId: string): FlowDefinition {
  return { root: removeBlock(flow.root, blockId) };
}

export function setSlot(flow: FlowDefinition, slot: FlowSlot, value: FlowBlock | null): FlowDefinition {
  if (slot.kind === "root") return { root: value };
  return updateBlock(flow, slot.parentId, (block) => {
    if (slot.kind === "next" && (block.type === "play_prompt" || block.type === "record_message" || block.type === "external_call")) {
      return { ...block, next: value };
    }
    if (slot.kind === "unavailable" && block.type === "record_message") {
      return { ...block, on_unavailable: value };
    }
    if (slot.kind === "not_connected" && block.type === "external_call") {
      return { ...block, on_not_connected: value };
    }
    if (slot.kind === "system_failure" && block.type === "external_call") {
      return { ...block, on_system_failure: value };
    }
    if (block.type === "collect_digit") {
      if (slot.kind === "digit") {
        return {
          ...block,
          branches: block.branches.map((branch) => branch.digit === slot.digit ? { ...branch, root: value } : branch),
        };
      }
      if (slot.kind === "timeout") return { ...block, on_timeout: value };
      if (slot.kind === "invalid") return { ...block, on_invalid: value };
      if (slot.kind === "return_limit") return { ...block, on_return_limit: value };
    }
    if (block.type === "schedule_branch") {
      if (slot.kind === "open") return { ...block, on_open: value };
      if (slot.kind === "closed") return { ...block, on_closed: value };
      if (slot.kind === "holiday") return { ...block, on_holiday: value };
    }
    return block;
  });
}

export function getSlot(flow: FlowDefinition, slot: FlowSlot): FlowBlock | null {
  if (slot.kind === "root") return flow.root;
  const parent = findBlock(flow.root, slot.parentId);
  if (!parent) return null;
  if (slot.kind === "next" && (parent.type === "play_prompt" || parent.type === "record_message" || parent.type === "external_call")) return parent.next;
  if (slot.kind === "unavailable" && parent.type === "record_message") return parent.on_unavailable;
  if (slot.kind === "not_connected" && parent.type === "external_call") return parent.on_not_connected;
  if (slot.kind === "system_failure" && parent.type === "external_call") return parent.on_system_failure;
  if (parent.type === "collect_digit") {
    if (slot.kind === "digit") return parent.branches.find((branch) => branch.digit === slot.digit)?.root ?? null;
    if (slot.kind === "timeout") return parent.on_timeout;
    if (slot.kind === "invalid") return parent.on_invalid;
    if (slot.kind === "return_limit") return parent.on_return_limit;
  }
  if (parent.type === "schedule_branch") {
    if (slot.kind === "open") return parent.on_open;
    if (slot.kind === "closed") return parent.on_closed;
    if (slot.kind === "holiday") return parent.on_holiday;
  }
  return null;
}

export function allFlowSlots(root: FlowBlock | null): FlowSlot[] {
  const slots: FlowSlot[] = [{ kind: "root" }];
  const visit = (block: FlowBlock) => {
    if (block.type === "play_prompt") {
      slots.push({ kind: "next", parentId: block.block_id });
      if (block.next) visit(block.next);
      return;
    }
    if (block.type === "record_message") {
      slots.push(
        { kind: "next", parentId: block.block_id },
        { kind: "unavailable", parentId: block.block_id },
      );
      if (block.next) visit(block.next);
      if (block.on_unavailable) visit(block.on_unavailable);
      return;
    }
    if (block.type === "external_call") {
      slots.push(
        { kind: "next", parentId: block.block_id },
        { kind: "not_connected", parentId: block.block_id },
        { kind: "system_failure", parentId: block.block_id },
      );
      if (block.next) visit(block.next);
      if (block.on_not_connected) visit(block.on_not_connected);
      if (block.on_system_failure) visit(block.on_system_failure);
      return;
    }
    if (block.type === "collect_digit") {
      block.branches.forEach((branch) => {
        slots.push({ kind: "digit", parentId: block.block_id, digit: branch.digit });
        if (branch.root) visit(branch.root);
      });
      slots.push(
        { kind: "timeout", parentId: block.block_id },
        { kind: "invalid", parentId: block.block_id },
        { kind: "return_limit", parentId: block.block_id },
      );
      if (block.on_timeout) visit(block.on_timeout);
      if (block.on_invalid) visit(block.on_invalid);
      if (block.on_return_limit) visit(block.on_return_limit);
      return;
    }
    if (block.type === "schedule_branch") {
      slots.push(
        { kind: "open", parentId: block.block_id },
        { kind: "closed", parentId: block.block_id },
        { kind: "holiday", parentId: block.block_id },
      );
      if (block.on_open) visit(block.on_open);
      if (block.on_closed) visit(block.on_closed);
      if (block.on_holiday) visit(block.on_holiday);
    }
  };
  if (root) visit(root);
  return slots;
}

function findBlockSlot(root: FlowBlock | null, blockId: string): FlowSlot | null {
  if (!root) return null;
  if (root.block_id === blockId) return { kind: "root" };
  for (const slot of allFlowSlots(root)) {
    if (slot.kind === "root") continue;
    if (getSlot({ root }, slot)?.block_id === blockId) return slot;
  }
  return null;
}

function blockProvidesPrompt(block: FlowBlock): boolean {
  return (block.type === "play_prompt" || block.type === "collect_digit")
    && Boolean(block.prompt_id);
}

export function hasEarlierPrompt(flow: FlowDefinition, blockId: string): boolean {
  let result = false;
  let found = false;
  const visit = (block: FlowBlock | null, promptAvailable: boolean) => {
    if (!block || found) return;
    if (block.block_id === blockId) {
      result = promptAvailable;
      found = true;
      return;
    }
    const nextPromptAvailable = promptAvailable || blockProvidesPrompt(block);
    if (block.type === "play_prompt") {
      visit(block.next, nextPromptAvailable);
    } else if (block.type === "record_message") {
      visit(block.next, nextPromptAvailable);
      visit(block.on_unavailable, nextPromptAvailable);
    } else if (block.type === "external_call") {
      visit(block.next, nextPromptAvailable);
      visit(block.on_not_connected, nextPromptAvailable);
      visit(block.on_system_failure, nextPromptAvailable);
    } else if (block.type === "collect_digit") {
      block.branches.forEach((branch) => visit(branch.root, nextPromptAvailable));
      visit(block.on_timeout, nextPromptAvailable);
      visit(block.on_invalid, nextPromptAvailable);
      visit(block.on_return_limit, nextPromptAvailable);
    } else if (block.type === "schedule_branch") {
      visit(block.on_open, nextPromptAvailable);
      visit(block.on_closed, nextPromptAvailable);
      visit(block.on_holiday, nextPromptAvailable);
    }
  };
  visit(flow.root, false);
  return found && result;
}

function hasPromptAtSlot(flow: FlowDefinition, slot: FlowSlot): boolean {
  if (slot.kind === "root") return false;
  const current = getSlot(flow, slot);
  if (current) return hasEarlierPrompt(flow, current.block_id);
  const parent = findBlock(flow.root, slot.parentId);
  return Boolean(parent && (hasEarlierPrompt(flow, parent.block_id) || blockProvidesPrompt(parent)));
}

function recordedActionsHavePrompt(block: FlowBlock | null, promptAvailable: boolean): boolean {
  if (!block) return true;
  if ((block.type === "record_message" || block.type === "external_call") && !promptAvailable) {
    return false;
  }
  const nextPromptAvailable = promptAvailable || blockProvidesPrompt(block);
  if (block.type === "play_prompt") {
    return recordedActionsHavePrompt(block.next, nextPromptAvailable);
  }
  if (block.type === "record_message") {
    return recordedActionsHavePrompt(block.next, nextPromptAvailable)
      && recordedActionsHavePrompt(block.on_unavailable, nextPromptAvailable);
  }
  if (block.type === "external_call") {
    return recordedActionsHavePrompt(block.next, nextPromptAvailable)
      && recordedActionsHavePrompt(block.on_not_connected, nextPromptAvailable)
      && recordedActionsHavePrompt(block.on_system_failure, nextPromptAvailable);
  }
  if (block.type === "collect_digit") {
    return block.branches.every((branch) => recordedActionsHavePrompt(branch.root, nextPromptAvailable))
      && recordedActionsHavePrompt(block.on_timeout, nextPromptAvailable)
      && recordedActionsHavePrompt(block.on_invalid, nextPromptAvailable)
      && recordedActionsHavePrompt(block.on_return_limit, nextPromptAvailable);
  }
  if (block.type === "schedule_branch") {
    return recordedActionsHavePrompt(block.on_open, nextPromptAvailable)
      && recordedActionsHavePrompt(block.on_closed, nextPromptAvailable)
      && recordedActionsHavePrompt(block.on_holiday, nextPromptAvailable);
  }
  return true;
}

export function canRemovePlayPrompt(flow: FlowDefinition, block: FlowBlock): boolean {
  if (block.type !== "play_prompt") return false;
  return recordedActionsHavePrompt(block.next, hasEarlierPrompt(flow, block.block_id));
}

export function removePlayPrompt(flow: FlowDefinition, blockId: string): FlowDefinition {
  const prompt = findBlock(flow.root, blockId);
  const source = findBlockSlot(flow.root, blockId);
  if (!prompt || prompt.type !== "play_prompt" || !source || !canRemovePlayPrompt(flow, prompt)) return flow;
  return setSlot(flow, source, prompt.next ?? makeBlock("end_call", null, null));
}

export function canMoveSubtree(
  flow: FlowDefinition,
  blockId: string,
  destination: FlowSlot,
): boolean {
  const moving = findBlock(flow.root, blockId);
  const source = findBlockSlot(flow.root, blockId);
  if (!moving || !source || sameFlowSlot(source, destination)) return false;
  if (destination.kind !== "root" && findBlock(moving, destination.parentId)) return false;
  const occupied = getSlot(flow, destination);
  if (occupied && occupied.type !== "end_call") return false;
  if (moving.type === "record_message" || moving.type === "external_call") {
    if (!hasPromptAtSlot(flow, destination)) return false;
  }
  if (moving.type === "return_to_menu" && !owningMenuForSlot(flow, destination)) return false;
  return true;
}

export function moveSubtree(
  flow: FlowDefinition,
  blockId: string,
  destination: FlowSlot,
): FlowDefinition {
  if (!canMoveSubtree(flow, blockId, destination)) return flow;
  const moving = findBlock(flow.root, blockId);
  const source = findBlockSlot(flow.root, blockId);
  if (!moving || !source) return flow;
  const vacated = source.kind === "root" ? null : makeBlock("end_call", null, null);
  const withoutMoving = setSlot(flow, source, vacated);
  return setSlot(withoutMoving, destination, moving);
}

function sameFlowSlot(first: FlowSlot, second: FlowSlot): boolean {
  return JSON.stringify(first) === JSON.stringify(second);
}

export function canInsertAtSlot(flow: FlowDefinition, slot: FlowSlot, kind: BlockKind): boolean {
  if (slot.kind !== "root" && !findBlock(flow.root, slot.parentId)) return false;
  const current = getSlot(flow, slot);
  if (kind === "record_message" || kind === "external_call") {
    if (!hasPromptAtSlot(flow, slot)) return false;
    return current?.type !== "record_message" && current?.type !== "external_call";
  }
  if (!current) return true;
  if (kind === "play_prompt") return true;
  // Terminal End call blocks are safe path placeholders. Extending that path
  // replaces the placeholder with the new terminal or branching action.
  return current.type === "end_call" && kind !== "end_call";
}

export function insertAtSlot(
  flow: FlowDefinition,
  slot: FlowSlot,
  block: FlowBlock,
): FlowDefinition {
  if (!canInsertAtSlot(flow, slot, block.type)) return flow;
  const current = getSlot(flow, slot);
  if (block.type === "play_prompt") {
    return setSlot(flow, slot, { ...block, next: current });
  }
  if (block.type === "record_message" || block.type === "external_call") {
    return setSlot(flow, slot, { ...block, next: current ?? block.next });
  }
  return setSlot(flow, slot, block);
}

export function addDigitBranch(
  flow: FlowDefinition,
  collectId: string,
  digit: string,
  promptId: string | null,
): FlowDefinition {
  return updateBlock(flow, collectId, (block) => {
    if (block.type !== "collect_digit" || block.branches.some((branch) => branch.digit === digit)) return block;
    return {
      ...block,
      branches: [
        ...block.branches,
        {
          branch_id: uuid(),
          digit,
          root: promptId
            ? makeBlock("play_prompt", promptId, null)
            : makeBlock("end_call", null, null),
        },
      ].sort((first, second) => DTMF_KEYS.indexOf(first.digit) - DTMF_KEYS.indexOf(second.digit)),
    };
  });
}

export function removeDigitBranch(flow: FlowDefinition, collectId: string, digit: string): FlowDefinition {
  return updateBlock(flow, collectId, (block) => block.type === "collect_digit"
    ? { ...block, branches: block.branches.filter((branch) => branch.digit !== digit) }
    : block);
}

export function availableDigits(block: CollectDigitBlock): string[] {
  const used = new Set(block.branches.map((branch) => branch.digit));
  return DTMF_KEYS.filter((digit) => !used.has(digit));
}

export function owningMenuForSlot(flow: FlowDefinition, target: FlowSlot): string | null {
  if (target.kind === "root") return null;
  let result: string | null = null;
  let found = false;
  const visit = (block: FlowBlock | null, owner: string | null, forbiddenOwner: string | null = null) => {
    if (!block || found) return;
    if (block.block_id === target.parentId) {
      found = true;
      let candidate = owner;
      if (target.kind === "digit" || target.kind === "timeout" || target.kind === "invalid" || target.kind === "return_limit") {
        candidate = block.type === "collect_digit" ? block.block_id : owner;
        if (target.kind === "return_limit" && block.type === "collect_digit") forbiddenOwner = block.block_id;
      } else {
        candidate = owner;
      }
      result = candidate === forbiddenOwner ? null : candidate;
      return;
    }
    if (block.type === "play_prompt") visit(block.next, owner, forbiddenOwner);
    if (block.type === "record_message") {
      visit(block.next, owner, forbiddenOwner);
      visit(block.on_unavailable, owner, forbiddenOwner);
    }
    if (block.type === "external_call") {
      visit(block.next, owner, forbiddenOwner);
      visit(block.on_not_connected, owner, forbiddenOwner);
      visit(block.on_system_failure, owner, forbiddenOwner);
    }
    if (block.type === "collect_digit") {
      block.branches.forEach((branch) => visit(branch.root, block.block_id, null));
      visit(block.on_timeout, block.block_id, null);
      visit(block.on_invalid, block.block_id, null);
      visit(block.on_return_limit, block.block_id, block.block_id);
    }
    if (block.type === "schedule_branch") {
      visit(block.on_open, owner, forbiddenOwner);
      visit(block.on_closed, owner, forbiddenOwner);
      visit(block.on_holiday, owner, forbiddenOwner);
    }
  };
  visit(flow.root, null);
  return result;
}

export function validateFlowLocally(
  flow: FlowDefinition,
  prompts: Prompt[],
  schedules: Schedule[],
): string[] {
  if (!flow.root) return ["Add a first step before publishing."];
  const promptIds = new Set(prompts.map((prompt) => prompt.id));
  const scheduleIds = new Set(schedules.map((schedule) => schedule.id));
  const seen = new Set<string>();
  const errors: string[] = [];
  let maximumDepth = 0;
  const visit = (
    block: FlowBlock,
    depth: number,
    ownerMenu: string | null,
    returnForbidden = false,
    promptAvailable = false,
  ) => {
    maximumDepth = Math.max(maximumDepth, depth);
    if (seen.has(block.block_id)) {
      errors.push("A block is duplicated or shared between branches.");
      return;
    }
    seen.add(block.block_id);
    if (block.type === "play_prompt") {
      if (!block.prompt_id || !promptIds.has(block.prompt_id)) errors.push("A Play prompt step needs an uploaded prompt.");
      if (!block.next) errors.push("A Play prompt step has no next step.");
      else visit(block.next, depth + 1, ownerMenu, returnForbidden, promptAvailable || Boolean(block.prompt_id));
    } else if (block.type === "record_message") {
      if (!promptAvailable) errors.push("Record message requires an earlier Play prompt or menu prompt notice on this path.");
      if (block.next) visit(block.next, depth + 1, ownerMenu, returnForbidden, promptAvailable);
      else errors.push("Record message success has no first step.");
      if (block.on_unavailable) visit(block.on_unavailable, depth + 1, ownerMenu, returnForbidden, promptAvailable);
      else errors.push("Record message unavailable has no first step.");
    } else if (block.type === "external_call") {
      if (!promptAvailable) errors.push("External call requires an earlier Play prompt or menu prompt notice on this path.");
      if (!/^\+?[0-9]{8,15}$/.test(block.phone_number)) {
        errors.push("External call needs a local or international number containing 8 to 15 digits.");
      } else if (isObviousEmergencyNumber(block.phone_number)) {
        errors.push("External call cannot dial an emergency or public-safety number.");
      }
      if (!Number.isInteger(block.answer_timeout_seconds) || block.answer_timeout_seconds < 5 || block.answer_timeout_seconds > 120) {
        errors.push("External call answer timeout must be between 5 and 120 seconds.");
      }
      if (block.next) visit(block.next, depth + 1, ownerMenu, returnForbidden, promptAvailable);
      else errors.push("External call Completed has no first step.");
      if (block.on_not_connected) visit(block.on_not_connected, depth + 1, ownerMenu, returnForbidden, promptAvailable);
      else errors.push("External call Not connected has no first step.");
      if (block.on_system_failure) visit(block.on_system_failure, depth + 1, ownerMenu, returnForbidden, promptAvailable);
      else errors.push("External call System failure has no first step.");
    } else if (block.type === "collect_digit") {
      if (block.allow_prompt_barge_in && !block.prompt_id) {
        errors.push("Prompt interruption requires a menu prompt.");
      }
      if (block.prompt_id && !promptIds.has(block.prompt_id)) errors.push("A menu prompt is missing.");
      if (!block.branches.length) errors.push("A Collect digit step needs at least one digit branch.");
      if (new Set(block.branches.map((branch) => branch.digit)).size !== block.branches.length) errors.push("Digit branches must be unique.");
      const branchPromptAvailable = promptAvailable || Boolean(block.prompt_id);
      block.branches.forEach((branch) => branch.root
        ? visit(branch.root, depth + 1, block.block_id, false, branchPromptAvailable)
        : errors.push(`Digit ${branch.digit} has no first step.`));
      if (block.on_timeout) visit(block.on_timeout, depth + 1, block.block_id, false, branchPromptAvailable);
      else errors.push("No input has no first step.");
      if (block.on_invalid) visit(block.on_invalid, depth + 1, block.block_id, false, branchPromptAvailable);
      else errors.push("Invalid input has no first step.");
      if (block.on_return_limit) visit(block.on_return_limit, depth + 1, block.block_id, true, branchPromptAvailable);
      else errors.push("Menu return limit has no first step.");
    } else if (block.type === "schedule_branch") {
      if (!block.schedule_id || !scheduleIds.has(block.schedule_id)) errors.push("A Check schedule step needs a schedule.");
      ([block.on_open, block.on_closed, block.on_holiday] as const).forEach((child) => child
        ? visit(child, depth + 1, ownerMenu, returnForbidden, promptAvailable)
        : errors.push("A schedule branch has no first step."));
    } else if (block.type === "return_to_menu") {
      if (!ownerMenu) errors.push("Return to menu must be inside a digit menu.");
      if (returnForbidden) errors.push("The return-limit path cannot return to the same menu.");
    }
  };
  visit(flow.root, 1, null);
  if (seen.size > 64) errors.push(`Flow has ${seen.size} steps; the limit is 64.`);
  if (maximumDepth > 8) errors.push(`Flow depth ${maximumDepth} exceeds the limit of 8.`);
  return [...new Set(errors)];
}
