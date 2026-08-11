import {
  AlertTriangle,
  CalendarClock,
  Check,
  CheckCircle2,
  ChevronDown,
  ChevronRight,
  Cloud,
  ListRestart,
  Mic2,
  MoveRight,
  PhoneForwarded,
  PhoneOff,
  Play,
  Plus,
  Redo2,
  RotateCcw,
  Save,
  Search,
  Trash2,
  Undo2,
  Volume2,
  X,
} from "lucide-react";
import { useCallback, useEffect, useMemo, useState } from "react";
import { api } from "../api";
import { FlowCanvas } from "../components/FlowCanvas";
import { RevisionReviewModal } from "../components/RevisionReviewModal";
import { Button, ErrorState, Field, Loading, Modal, SuccessMessage } from "../components/ui";
import { useRemote } from "../hooks";
import type {
  CollectDigitBlock,
  DraftConfiguration,
  FlowBlock,
  FlowDiff,
  FlowDefinition,
  Prompt,
  RecordingBehavior,
  Schedule,
  SimulationResult,
} from "../types";
import {
  addDigitBranch,
  allFlowSlots,
  availableDigits,
  canInsertAtSlot,
  canMoveSubtree,
  canRemovePlayPrompt,
  countBlocks,
  countBranches,
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
  type BlockKind,
  type FlowSlot,
} from "./flowEditor";

const BLOCK_LABELS: Record<FlowBlock["type"], string> = {
  play_prompt: "Play prompt",
  record_message: "Record message",
  external_call: "External call",
  collect_digit: "Collect one digit",
  schedule_branch: "Check schedule",
  return_to_menu: "Return to menu",
  end_call: "End call",
};

const BLOCK_HELP: Record<BlockKind, string> = {
  play_prompt: "Play an audio prompt",
  record_message: "Record voicemail after a greeting",
  external_call: "Dial, merge, and record an operator",
  collect_digit: "Collect DTMF input",
  schedule_branch: "Route by time or date",
  return_to_menu: "Return to the owning menu",
  end_call: "Terminate the call",
};

const DTMF_ORDER = "0123456789*#";
const NO_PROMPT_VALUE = "__no_prompt__";

type DeleteRequest = {
  blockId: string;
  label: string;
  count: number;
};

type DigitDeleteRequest = {
  collectId: string;
  digit: string;
  count: number;
};

type MoveRequest = {
  blockId: string;
  label: string;
};

function blockIcon(type: FlowBlock["type"]) {
  return {
    play_prompt: Volume2,
    record_message: Mic2,
    external_call: PhoneForwarded,
    collect_digit: ListRestart,
    schedule_branch: CalendarClock,
    return_to_menu: RotateCcw,
    end_call: PhoneOff,
  }[type];
}

function promptName(promptId: string | null, prompts: Prompt[]): string {
  if (!promptId) return "Prompt not selected";
  return prompts.find((prompt) => prompt.id === promptId)?.name ?? "Missing prompt";
}

function maskedNumber(phoneNumber: string): string {
  const suffix = phoneNumber.replace(/\D/g, "").slice(-4);
  return suffix ? `••••${suffix}` : "Number not configured";
}

function blockTitle(block: FlowBlock, prompts: Prompt[]): string {
  if (block.type === "play_prompt") return `${BLOCK_LABELS[block.type]} — ${promptName(block.prompt_id, prompts)}`;
  if (block.type === "collect_digit" && block.prompt_id) return `${BLOCK_LABELS[block.type]} — ${promptName(block.prompt_id, prompts)}`;
  if (block.type === "external_call") return `${BLOCK_LABELS[block.type]} — ${maskedNumber(block.phone_number)}`;
  return BLOCK_LABELS[block.type];
}

function sameSlot(first: FlowSlot | null, second: FlowSlot): boolean {
  return first ? JSON.stringify(first) === JSON.stringify(second) : false;
}

export function FlowPage() {
  const remote = useRemote(async () => {
    const [draft, prompts] = await Promise.all([api.draft(), api.prompts()]);
    return { draft, prompts };
  }, []);
  const [draft, setDraft] = useState<DraftConfiguration | null>(null);
  const [selectedId, setSelectedId] = useState<string | null>(null);
  const [history, setHistory] = useState<FlowDefinition[]>([]);
  const [future, setFuture] = useState<FlowDefinition[]>([]);
  const [dirty, setDirty] = useState(false);
  const [busy, setBusy] = useState(false);
  const [message, setMessage] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [validationErrors, setValidationErrors] = useState<string[]>([]);
  const [activeSlot, setActiveSlot] = useState<FlowSlot | null>(null);
  const [pendingKind, setPendingKind] = useState<BlockKind | null>(null);
  const [addDigitFor, setAddDigitFor] = useState<string | null>(null);
  const [deleteRequest, setDeleteRequest] = useState<DeleteRequest | null>(null);
  const [digitDeleteRequest, setDigitDeleteRequest] = useState<DigitDeleteRequest | null>(null);
  const [moveRequest, setMoveRequest] = useState<MoveRequest | null>(null);
  const [collapsed, setCollapsed] = useState<Set<string>>(() => new Set());
  const [simulation, setSimulation] = useState<SimulationResult | null>(null);
  const [simulationEvents, setSimulationEvents] = useState<string[]>([]);
  const [simulationOpen, setSimulationOpen] = useState(false);
  const [reviewDiff, setReviewDiff] = useState<FlowDiff | null>(null);
  const [reviewOpen, setReviewOpen] = useState(false);

  useEffect(() => {
    if (!remote.data) return;
    setDraft(remote.data.draft);
    setSelectedId(remote.data.draft.flow.root?.block_id ?? null);
    setActiveSlot(null);
    setPendingKind(null);
  }, [remote.data]);

  useEffect(() => {
    if (!message) return;
    const timer = window.setTimeout(() => setMessage(null), 4500);
    return () => window.clearTimeout(timer);
  }, [message]);

  const prompts = remote.data?.prompts ?? [];
  const schedules = draft?.schedules ?? [];
  const selected = draft && selectedId ? findBlock(draft.flow.root, selectedId) : null;
  const localErrors = useMemo(
    () => draft ? validateFlowLocally(draft.flow, prompts, draft.schedules) : [],
    [draft, prompts],
  );
  const stepCount = countBlocks(draft?.flow.root ?? null);
  const branchCount = countBranches(draft?.flow.root ?? null);

  const commitFlow = useCallback((next: FlowDefinition) => {
    if (!draft || JSON.stringify(next) === JSON.stringify(draft.flow)) return;
    setHistory((current) => [...current.slice(-49), draft.flow]);
    setFuture([]);
    setDraft({ ...draft, flow: next });
    setDirty(true);
    setMessage(null);
    setError(null);
    setValidationErrors([]);
  }, [draft]);

  const undo = useCallback(() => {
    if (!draft || !history.length) return;
    const previous = history.at(-1)!;
    setHistory((current) => current.slice(0, -1));
    setFuture((current) => [draft.flow, ...current].slice(0, 50));
    setDraft({ ...draft, flow: previous });
    setDirty(true);
    if (selectedId && !findBlock(previous.root, selectedId)) setSelectedId(null);
  }, [draft, history, selectedId]);

  const redo = useCallback(() => {
    if (!draft || !future.length) return;
    const next = future[0];
    setHistory((current) => [...current.slice(-49), draft.flow]);
    setFuture((current) => current.slice(1));
    setDraft({ ...draft, flow: next });
    setDirty(true);
  }, [draft, future]);

  useEffect(() => {
    const onKeyDown = (event: KeyboardEvent) => {
      if (!(event.metaKey || event.ctrlKey) || event.key.toLowerCase() !== "z") return;
      event.preventDefault();
      if (event.shiftKey) redo();
      else undo();
    };
    window.addEventListener("keydown", onKeyDown);
    return () => window.removeEventListener("keydown", onKeyDown);
  }, [redo, undo]);

  const updateSelected = (block: FlowBlock) => {
    if (!draft) return;
    commitFlow(updateBlock(draft.flow, block.block_id, () => block));
  };

  const removeSelectedPrompt = (block: FlowBlock) => {
    if (!draft || block.type !== "play_prompt") return;
    const next = removePlayPrompt(draft.flow, block.block_id);
    if (next === draft.flow) {
      setError("No prompt is unavailable because a recorded action below this step has no other earlier prompt notice on its path.");
      return;
    }
    commitFlow(next);
    setSelectedId(block.next?.block_id ?? null);
    setMessage("Prompt removed; the next action was kept in this branch.");
  };

  const canPlaceBlock = (kind: BlockKind, slot: FlowSlot) => Boolean(
    draft
    && canInsertAtSlot(draft.flow, slot, kind)
    && (kind !== "return_to_menu" || owningMenuForSlot(draft.flow, slot)),
  );

  const insertBlock = (kind: BlockKind, requestedSlot?: FlowSlot) => {
    if (!draft) return;
    const slot = requestedSlot ?? activeSlot ?? (draft.flow.root ? null : { kind: "root" as const });
    if (!slot) {
      setError("Choose an Add next step position on the canvas first.");
      return;
    }
    if (!canPlaceBlock(kind, slot)) {
      setError(kind === "return_to_menu"
        ? "Return to menu can only replace an ending inside a digit menu branch."
        : `${BLOCK_LABELS[kind]} cannot be inserted at this occupied position.`);
      return;
    }
    const block = makeBlock(kind, prompts[0]?.id ?? null, schedules[0]?.id ?? null);
    commitFlow(insertAtSlot(draft.flow, slot, block));
    setSelectedId(block.block_id);
    setActiveSlot(null);
    setPendingKind(null);
  };

  const chooseSlot = (slot: FlowSlot) => {
    if (pendingKind) {
      insertBlock(pendingKind, slot);
      return;
    }
    setActiveSlot((current) => sameSlot(current, slot) ? null : slot);
    setError(null);
  };

  const choosePaletteKind = (kind: BlockKind) => {
    if (!draft) return;
    const slot = activeSlot ?? (!draft.flow.root ? { kind: "root" as const } : null);
    if (slot) {
      insertBlock(kind, slot);
      return;
    }
    setPendingKind((current) => current === kind ? null : kind);
    setError(null);
  };

  const cancelPlacement = () => {
    setActiveSlot(null);
    setPendingKind(null);
  };

  const requestDelete = (block: FlowBlock) => {
    setDeleteRequest({
      blockId: block.block_id,
      label: blockTitle(block, prompts),
      count: countBlocks(block),
    });
  };

  const confirmDelete = () => {
    if (!draft || !deleteRequest) return;
    commitFlow(deleteSubtree(draft.flow, deleteRequest.blockId));
    setSelectedId(null);
    setDeleteRequest(null);
  };

  const confirmDigitDelete = () => {
    if (!draft || !digitDeleteRequest) return;
    commitFlow(removeDigitBranch(draft.flow, digitDeleteRequest.collectId, digitDeleteRequest.digit));
    setSelectedId(digitDeleteRequest.collectId);
    setDigitDeleteRequest(null);
  };

  const confirmMove = (destination: FlowSlot) => {
    if (!draft || !moveRequest) return;
    const next = moveSubtree(draft.flow, moveRequest.blockId, destination);
    if (next === draft.flow) {
      setError("That destination cannot own this subtree.");
      return;
    }
    commitFlow(next);
    setSelectedId(moveRequest.blockId);
    setMoveRequest(null);
    setMessage("Step moved with all of its owned outcomes.");
  };

  const saveDraft = useCallback(async (): Promise<DraftConfiguration | null> => {
    if (!draft) return null;
    setBusy(true);
    setError(null);
    try {
      const saved = await api.saveDraft(draft);
      setDraft(saved);
      setDirty(false);
      setMessage(`Draft version ${saved.edit_version} saved.`);
      return saved;
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : "Could not save the draft.");
      return null;
    } finally {
      setBusy(false);
    }
  }, [draft]);

  const validate = async () => {
    if (!draft) return;
    setValidationErrors(localErrors);
    if (localErrors.length) {
      setError("Resolve the highlighted flow issues before publishing.");
      return;
    }
    setBusy(true);
    setError(null);
    try {
      const validation = await api.validateDraft(draft);
      setValidationErrors(validation.errors);
      if (validation.valid) setMessage("The current flow passes server validation.");
      else setError("The server found publication issues in this flow.");
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : "Could not validate the flow.");
    } finally {
      setBusy(false);
    }
  };

  const openSimulation = async () => {
    if (!draft) return;
    setSimulationOpen(true);
    setSimulationEvents([]);
    setBusy(true);
    try {
      setSimulation(await api.simulateDraft(draft, []));
    } catch (reason) {
      setSimulation({ status: "invalid", trace: [], available_digits: [], message: reason instanceof Error ? reason.message : "Simulation failed." });
    } finally {
      setBusy(false);
    }
  };

  const simulateEvent = async (event: string) => {
    if (!draft) return;
    const events = [...simulationEvents, event];
    setSimulationEvents(events);
    setBusy(true);
    try {
      setSimulation(await api.simulateDraft(draft, events));
    } catch (reason) {
      setSimulation({ status: "invalid", trace: [], available_digits: [], message: reason instanceof Error ? reason.message : "Simulation failed." });
    } finally {
      setBusy(false);
    }
  };

  const openReview = async () => {
    if (!draft) return;
    setValidationErrors(localErrors);
    if (localErrors.length) {
      setError("Resolve the flow issues before reviewing a publication.");
      return;
    }
    setBusy(true);
    setError(null);
    try {
      const candidate = dirty ? await api.saveDraft(draft) : draft;
      if (dirty) {
        setDraft(candidate);
        setDirty(false);
      }
      const validation = await api.validateDraft(candidate);
      if (!validation.valid) {
        setValidationErrors(validation.errors);
        setError("The server rejected this draft for publication.");
        return;
      }
      setReviewDiff(await api.diffDraft(candidate));
      setReviewOpen(true);
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : "Could not prepare the revision diff.");
    } finally {
      setBusy(false);
    }
  };

  const publish = async () => {
    if (!draft || !reviewDiff) return;
    setBusy(true);
    setError(null);
    try {
      const revision = await api.publishDraft({
        edit_version: draft.edit_version,
        base_revision_id: reviewDiff.base_revision_id,
      });
      setReviewOpen(false);
      setReviewDiff(null);
      setMessage(`Revision ${revision.id} published. The tablet will activate it only while idle.`);
    } catch (reason) {
      setReviewOpen(false);
      setError(reason instanceof Error ? reason.message : "Could not publish the revision.");
    } finally {
      setBusy(false);
    }
  };

  const toggleCollapsed = (key: string) => {
    setCollapsed((current) => {
      const next = new Set(current);
      if (next.has(key)) next.delete(key);
      else next.add(key);
      return next;
    });
  };

  if (remote.loading || !draft) return <Loading label="Loading IVR flow" />;
  if (remote.error) return <ErrorState message={remote.error} retry={remote.refresh} />;

  return (
    <div className="studio-shell">
      <StudioHeader
        dirty={dirty}
        stepCount={stepCount}
        branchCount={branchCount}
        issueCount={localErrors.length}
        busy={busy}
        canUndo={history.length > 0}
        canRedo={future.length > 0}
        onUndo={undo}
        onRedo={redo}
        onValidate={() => void validate()}
        onSimulate={() => void openSimulation()}
        onSave={() => void saveDraft()}
        onPublish={() => void openReview()}
      />

      <div className="studio-body">
        <StepPalette
          activeSlot={activeSlot}
          rootEmpty={!draft.flow.root}
          pendingKind={pendingKind}
          recordingBehavior={draft.recording_behavior}
          canInsert={(kind) => activeSlot
            ? canPlaceBlock(kind, activeSlot)
            : draft.flow.root
              ? true
              : canPlaceBlock(kind, { kind: "root" })}
          onChoose={choosePaletteKind}
          onCancel={cancelPlacement}
        />

        <main className="studio-canvas-column">
          {message ? <SuccessMessage>{message}</SuccessMessage> : null}
          {error ? <ErrorState message={error} /> : null}
          {validationErrors.length ? (
            <section className="studio-validation" aria-label="Flow validation issues">
              <AlertTriangle size={17} />
              <ul>{validationErrors.map((item) => <li key={item}>{item}</li>)}</ul>
            </section>
          ) : null}
          <FlowCanvas>
            <div className="structured-flow">
              <div className="start-node">Start</div>
              {draft.flow.root ? (
                <>
                  <AddSlot
                    slot={{ kind: "root" }}
                    label="Insert before first step"
                    ariaLabel={`Insert step before ${blockTitle(draft.flow.root, prompts)}`}
                    activeSlot={activeSlot}
                    pendingKind={pendingKind}
                    canPlace={canPlaceBlock}
                    onChoose={chooseSlot}
                  />
                  <BlockPath
                    block={draft.flow.root}
                    prompts={prompts}
                    selectedId={selectedId}
                    collapsed={collapsed}
                    activeSlot={activeSlot}
                    pendingKind={pendingKind}
                    canPlace={canPlaceBlock}
                    onSelect={setSelectedId}
                    onAdd={chooseSlot}
                    onAddDigit={setAddDigitFor}
                    onToggle={toggleCollapsed}
                    onDeleteDigit={(collectId, digit, count) => setDigitDeleteRequest({ collectId, digit, count })}
                  />
                </>
              ) : (
                <EmptyFlow onAdd={() => chooseSlot({ kind: "root" })} />
              )}
            </div>
          </FlowCanvas>
        </main>

        <Inspector
          key={selected?.block_id ?? "empty"}
          block={selected}
          flow={draft.flow}
          prompts={prompts}
          schedules={schedules}
          recordingBehavior={draft.recording_behavior}
          descendantCount={selected ? countBlocks(selected) : 0}
          onChange={updateSelected}
          onRemovePrompt={removeSelectedPrompt}
          onDelete={requestDelete}
          onMove={(block) => setMoveRequest({ blockId: block.block_id, label: blockTitle(block, prompts) })}
          onAddDigit={(collectId) => setAddDigitFor(collectId)}
          onClose={() => setSelectedId(null)}
        />
      </div>

      {addDigitFor ? (
        <AddDigitModal
          collect={findBlock(draft.flow.root, addDigitFor)}
          prompts={prompts}
          onClose={() => setAddDigitFor(null)}
          onAdd={(digit, promptId) => {
            const next = addDigitBranch(draft.flow, addDigitFor, digit, promptId);
            commitFlow(next);
            const collect = findBlock(next.root, addDigitFor);
            if (collect?.type === "collect_digit") {
              setSelectedId(collect.branches.find((branch) => branch.digit === digit)?.root?.block_id ?? collect.block_id);
            }
            setAddDigitFor(null);
          }}
        />
      ) : null}

      {deleteRequest ? (
        <ConfirmDeleteModal request={deleteRequest} onCancel={() => setDeleteRequest(null)} onConfirm={confirmDelete} />
      ) : null}

      {digitDeleteRequest ? (
        <ConfirmDigitDeleteModal request={digitDeleteRequest} onCancel={() => setDigitDeleteRequest(null)} onConfirm={confirmDigitDelete} />
      ) : null}

      {moveRequest ? (
        <MoveSubtreeModal
          request={moveRequest}
          flow={draft.flow}
          prompts={prompts}
          onCancel={() => setMoveRequest(null)}
          onConfirm={confirmMove}
        />
      ) : null}

      {simulationOpen ? (
        <SimulationModal
          result={simulation}
          prompts={prompts}
          busy={busy}
          onEvent={(event) => void simulateEvent(event)}
          onReset={() => void openSimulation()}
          onClose={() => setSimulationOpen(false)}
        />
      ) : null}

      {reviewOpen && reviewDiff ? (
        <RevisionReviewModal
          diff={reviewDiff}
          stepCount={stepCount}
          branchCount={branchCount}
          busy={busy}
          onClose={() => setReviewOpen(false)}
          onPublish={() => void publish()}
        />
      ) : null}
    </div>
  );
}

function StudioHeader({
  dirty,
  stepCount,
  branchCount,
  issueCount,
  busy,
  canUndo,
  canRedo,
  onUndo,
  onRedo,
  onValidate,
  onSimulate,
  onSave,
  onPublish,
}: {
  dirty: boolean;
  stepCount: number;
  branchCount: number;
  issueCount: number;
  busy: boolean;
  canUndo: boolean;
  canRedo: boolean;
  onUndo: () => void;
  onRedo: () => void;
  onValidate: () => void;
  onSimulate: () => void;
  onSave: () => void;
  onPublish: () => void;
}) {
  return (
    <header className="studio-header">
      <div className="studio-header__identity">
        <div><h1>IVR Flow</h1><span className="draft-label">Draft</span>{dirty ? <span className="unsaved-state"><i /> Unsaved changes</span> : null}</div>
        <p><span>{stepCount} steps · {branchCount} branches</span><span className={issueCount ? "studio-health studio-health--error" : "studio-health"}>{issueCount ? <AlertTriangle size={13} /> : <Check size={13} />}{issueCount ? `${issueCount} issue${issueCount === 1 ? "" : "s"}` : "All good"}</span></p>
      </div>
      <div className="studio-header__actions" role="toolbar" aria-label="Flow commands">
        <Button variant="secondary" onClick={onUndo} disabled={!canUndo || busy}><Undo2 size={16} /> Undo</Button>
        <Button variant="secondary" onClick={onRedo} disabled={!canRedo || busy}><Redo2 size={16} /> Redo</Button>
        <Button variant="secondary" onClick={onValidate} disabled={busy}><CheckCircle2 size={16} /> Validate</Button>
        <Button variant="secondary" onClick={onSimulate} disabled={busy}><Play size={16} /> Simulate</Button>
        <Button variant="secondary" onClick={onSave} disabled={!dirty || busy}><Save size={16} /> Save draft</Button>
        <Button onClick={onPublish} disabled={busy}><Cloud size={16} /> Review &amp; publish</Button>
      </div>
    </header>
  );
}

function StepPalette({
  activeSlot,
  rootEmpty,
  pendingKind,
  recordingBehavior,
  canInsert,
  onChoose,
  onCancel,
}: {
  activeSlot: FlowSlot | null;
  rootEmpty: boolean;
  pendingKind: BlockKind | null;
  recordingBehavior: RecordingBehavior;
  canInsert: (kind: BlockKind) => boolean;
  onChoose: (kind: BlockKind) => void;
  onCancel: () => void;
}) {
  const entries: Array<{ kind: BlockKind; icon: typeof Volume2 }> = [
    { kind: "play_prompt", icon: Volume2 },
    { kind: "record_message", icon: Mic2 },
    { kind: "external_call", icon: PhoneForwarded },
    { kind: "collect_digit", icon: ListRestart },
    { kind: "schedule_branch", icon: CalendarClock },
    { kind: "return_to_menu", icon: RotateCcw },
    { kind: "end_call", icon: PhoneOff },
  ];
  const instruction = activeSlot
    ? "Position selected. Choose the step to insert."
    : pendingKind
      ? `Placing ${BLOCK_LABELS[pendingKind]}. Choose a highlighted position.`
      : rootEmpty
        ? "Choose the first step for this flow."
        : "Choose a step, then place it on the canvas.";
  return (
    <aside className="step-palette" aria-label="Add step palette">
      <header>
        <div><h2>Add step</h2>{activeSlot || pendingKind ? <button type="button" onClick={onCancel}>Cancel</button> : null}</div>
        <p>{instruction}</p>
      </header>
      <div className="step-palette__items">
        {entries.map(({ kind, icon: Icon }) => {
          const disabled = !canInsert(kind);
          return (
            <button
              key={kind}
              type="button"
              className={pendingKind === kind ? "step-palette__item--selected" : ""}
              disabled={disabled}
              aria-pressed={pendingKind === kind}
              onClick={() => onChoose(kind)}
            >
              <Icon size={20} />
              <span><strong>{BLOCK_LABELS[kind]}</strong><small>{BLOCK_HELP[kind]}</small></span>
            </button>
          );
        })}
      </div>
      <div className="recording-behavior-summary">
        <Mic2 size={16} />
        <p><strong>Shared recording behavior</strong><span>Up to {recordingBehavior.maximum_duration_seconds}s · {recordingBehavior.finish_key ? `${recordingBehavior.finish_key} finishes` : "no finish key"}</span></p>
      </div>
      <div className="step-palette__tip"><CheckCircle2 size={16} /><p>Choose an action here and then a highlighted insertion point, or select the insertion point first.</p></div>
    </aside>
  );
}

function EmptyFlow({ onAdd }: { onAdd: () => void }) {
  return (
    <section className="empty-flow">
      <div className="empty-flow__line" />
      <h2>Build the first call path</h2>
      <p>Add a prompt, operator call, digit menu, schedule, or terminal action.</p>
      <Button onClick={onAdd}><Plus size={17} /> Add first step</Button>
    </section>
  );
}

function BlockPath({
  block,
  prompts,
  selectedId,
  collapsed,
  activeSlot,
  pendingKind,
  canPlace,
  onSelect,
  onAdd,
  onAddDigit,
  onToggle,
  onDeleteDigit,
  inline = false,
}: {
  block: FlowBlock;
  prompts: Prompt[];
  selectedId: string | null;
  collapsed: Set<string>;
  activeSlot: FlowSlot | null;
  pendingKind: BlockKind | null;
  canPlace: (kind: BlockKind, slot: FlowSlot) => boolean;
  onSelect: (id: string) => void;
  onAdd: (slot: FlowSlot) => void;
  onAddDigit: (collectId: string) => void;
  onToggle: (key: string) => void;
  onDeleteDigit: (collectId: string, digit: string, count: number) => void;
  inline?: boolean;
}) {
  const common = { prompts, selectedId, collapsed, activeSlot, pendingKind, canPlace, onSelect, onAdd, onAddDigit, onToggle, onDeleteDigit };
  const nextSlot = { kind: "next", parentId: block.block_id } as const;
  return (
    <div className={`block-path ${inline ? "block-path--inline" : ""}`}>
      <NodeCard block={block} prompts={prompts} selected={selectedId === block.block_id} onSelect={() => onSelect(block.block_id)} />
      {block.type === "play_prompt" ? (
        block.next
          ? <>
              <AddSlot
                slot={nextSlot}
                label="Insert step"
                ariaLabel={`Insert step between ${blockTitle(block, prompts)} and ${blockTitle(block.next, prompts)}`}
                activeSlot={activeSlot}
                pendingKind={pendingKind}
                canPlace={canPlace}
                onChoose={onAdd}
                inline={inline}
              />
              <BlockPath block={block.next} {...common} inline={inline} />
            </>
          : <AddSlot
              slot={nextSlot}
              label="Add next step"
              ariaLabel={`Add next step after ${blockTitle(block, prompts)}`}
              activeSlot={activeSlot}
              pendingKind={pendingKind}
              canPlace={canPlace}
              onChoose={onAdd}
              inline={inline}
            />
      ) : null}
      {block.type === "collect_digit" ? (
        <CollectBranches block={block} {...common} />
      ) : null}
      {block.type === "schedule_branch" ? (
        <ScheduleBranches block={block} {...common} />
      ) : null}
      {block.type === "record_message" ? (
        <RecordingBranches block={block} {...common} />
      ) : null}
      {block.type === "external_call" ? (
        <ExternalCallBranches block={block} {...common} />
      ) : null}
    </div>
  );
}

function NodeCard({ block, prompts, selected, onSelect }: { block: FlowBlock; prompts: Prompt[]; selected: boolean; onSelect: () => void }) {
  const Icon = blockIcon(block.type);
  const detail = block.type === "play_prompt"
    ? "Plays an audio prompt"
    : block.type === "record_message"
      ? "Explicit voicemail capture"
    : block.type === "external_call"
      ? `Waits up to ${block.answer_timeout_seconds}s · recording required`
    : block.type === "collect_digit"
      ? `${block.branches.length} digit branch${block.branches.length === 1 ? "" : "es"}${block.allow_prompt_barge_in ? " · prompt input on" : ""}`
      : block.type === "schedule_branch"
        ? "Routes by schedule"
        : block.type === "return_to_menu"
          ? "Bounded menu return"
          : "Terminates the call";
  return (
    <button type="button" className={`studio-node ${selected ? "studio-node--selected" : ""}`} onClick={onSelect} aria-label={`Select ${blockTitle(block, prompts)}`}>
      <Icon size={20} />
      <span><strong>{blockTitle(block, prompts)}</strong><small>{detail}</small></span>
      {selected ? <Check size={16} /> : <ChevronRight size={16} />}
    </button>
  );
}

function AddSlot({
  slot,
  label,
  ariaLabel,
  activeSlot,
  pendingKind,
  canPlace,
  onChoose,
  inline = false,
}: {
  slot: FlowSlot;
  label: string;
  ariaLabel: string;
  activeSlot: FlowSlot | null;
  pendingKind: BlockKind | null;
  canPlace: (kind: BlockKind, slot: FlowSlot) => boolean;
  onChoose: (slot: FlowSlot) => void;
  inline?: boolean;
}) {
  const active = sameSlot(activeSlot, slot);
  const available = !pendingKind || canPlace(pendingKind, slot);
  const visibleLabel = active
    ? "Position selected"
    : pendingKind && available
      ? `Place ${BLOCK_LABELS[pendingKind]}`
      : label;
  return (
    <button
      type="button"
      className={`studio-add-slot ${inline ? "studio-add-slot--inline" : ""} ${active ? "studio-add-slot--active" : ""} ${pendingKind && available ? "studio-add-slot--ready" : ""}`}
      disabled={!available}
      aria-label={pendingKind && available ? `${visibleLabel}: ${ariaLabel}` : ariaLabel}
      aria-pressed={active}
      onClick={() => onChoose(slot)}
    >
      {active ? <Check size={14} /> : <Plus size={14} />} {visibleLabel}
    </button>
  );
}

function CollectBranches({
  block,
  prompts,
  selectedId,
  collapsed,
  activeSlot,
  pendingKind,
  canPlace,
  onSelect,
  onAdd,
  onAddDigit,
  onToggle,
  onDeleteDigit,
}: {
  block: CollectDigitBlock;
  prompts: Prompt[];
  selectedId: string | null;
  collapsed: Set<string>;
  activeSlot: FlowSlot | null;
  pendingKind: BlockKind | null;
  canPlace: (kind: BlockKind, slot: FlowSlot) => boolean;
  onSelect: (id: string) => void;
  onAdd: (slot: FlowSlot) => void;
  onAddDigit: (collectId: string) => void;
  onToggle: (key: string) => void;
  onDeleteDigit: (collectId: string, digit: string, count: number) => void;
}) {
  const common = { prompts, selectedId, collapsed, activeSlot, pendingKind, canPlace, onSelect, onAdd, onAddDigit, onToggle, onDeleteDigit };
  const branches = [...block.branches].sort((first, second) => DTMF_ORDER.indexOf(first.digit) - DTMF_ORDER.indexOf(second.digit));
  const recovery = [
    { key: "timeout", label: "No input", root: block.on_timeout, slot: { kind: "timeout", parentId: block.block_id } as FlowSlot },
    { key: "invalid", label: "Invalid", root: block.on_invalid, slot: { kind: "invalid", parentId: block.block_id } as FlowSlot },
    { key: "return-limit", label: "Return limit", root: block.on_return_limit, slot: { kind: "return_limit", parentId: block.block_id } as FlowSlot },
  ];
  return (
    <section className="branch-group" aria-label="Digit branches">
      <header><span>Digit branches</span><button type="button" onClick={() => onAddDigit(block.block_id)}><Plus size={15} /> Add digit</button></header>
      {branches.map((branch) => {
        const key = `${block.block_id}:digit:${branch.digit}`;
        return (
          <BranchRow
            key={branch.branch_id}
            label={branch.digit}
            root={branch.root}
            slot={{ kind: "digit", parentId: block.block_id, digit: branch.digit }}
            isCollapsed={collapsed.has(key)}
            onToggleRow={() => onToggle(key)}
            onRemove={() => onDeleteDigit(block.block_id, branch.digit, countBlocks(branch.root))}
            {...common}
          />
        );
      })}
      {recovery.map((branch) => {
        const key = `${block.block_id}:${branch.key}`;
        return (
          <BranchRow
            key={key}
            label={branch.label}
            root={branch.root}
            slot={branch.slot}
            isCollapsed={collapsed.has(key)}
            onToggleRow={() => onToggle(key)}
            {...common}
          />
        );
      })}
    </section>
  );
}

function ScheduleBranches({
  block,
  prompts,
  selectedId,
  collapsed,
  activeSlot,
  pendingKind,
  canPlace,
  onSelect,
  onAdd,
  onAddDigit,
  onToggle,
  onDeleteDigit,
}: {
  block: Extract<FlowBlock, { type: "schedule_branch" }>;
  prompts: Prompt[];
  selectedId: string | null;
  collapsed: Set<string>;
  activeSlot: FlowSlot | null;
  pendingKind: BlockKind | null;
  canPlace: (kind: BlockKind, slot: FlowSlot) => boolean;
  onSelect: (id: string) => void;
  onAdd: (slot: FlowSlot) => void;
  onAddDigit: (collectId: string) => void;
  onToggle: (key: string) => void;
  onDeleteDigit: (collectId: string, digit: string, count: number) => void;
}) {
  const common = { prompts, selectedId, collapsed, activeSlot, pendingKind, canPlace, onSelect, onAdd, onAddDigit, onToggle, onDeleteDigit };
  const branches = [
    { key: "open", label: "Open", root: block.on_open, slot: { kind: "open", parentId: block.block_id } as FlowSlot },
    { key: "closed", label: "Closed", root: block.on_closed, slot: { kind: "closed", parentId: block.block_id } as FlowSlot },
    { key: "holiday", label: "Holiday", root: block.on_holiday, slot: { kind: "holiday", parentId: block.block_id } as FlowSlot },
  ];
  return (
    <section className="branch-group" aria-label="Schedule branches">
      <header><span>Schedule branches</span></header>
      {branches.map((branch) => {
        const key = `${block.block_id}:${branch.key}`;
        return <BranchRow key={key} label={branch.label} root={branch.root} slot={branch.slot} isCollapsed={collapsed.has(key)} onToggleRow={() => onToggle(key)} {...common} />;
      })}
    </section>
  );
}

function RecordingBranches({
  block,
  prompts,
  selectedId,
  collapsed,
  activeSlot,
  pendingKind,
  canPlace,
  onSelect,
  onAdd,
  onAddDigit,
  onToggle,
  onDeleteDigit,
}: {
  block: Extract<FlowBlock, { type: "record_message" }>;
  prompts: Prompt[];
  selectedId: string | null;
  collapsed: Set<string>;
  activeSlot: FlowSlot | null;
  pendingKind: BlockKind | null;
  canPlace: (kind: BlockKind, slot: FlowSlot) => boolean;
  onSelect: (id: string) => void;
  onAdd: (slot: FlowSlot) => void;
  onAddDigit: (collectId: string) => void;
  onToggle: (key: string) => void;
  onDeleteDigit: (collectId: string, digit: string, count: number) => void;
}) {
  const common = { prompts, selectedId, collapsed, activeSlot, pendingKind, canPlace, onSelect, onAdd, onAddDigit, onToggle, onDeleteDigit };
  const branches = [
    { key: "recorded", label: "Recorded", root: block.next, slot: { kind: "next", parentId: block.block_id } as FlowSlot },
    { key: "unavailable", label: "Unavailable", root: block.on_unavailable, slot: { kind: "unavailable", parentId: block.block_id } as FlowSlot },
  ];
  return (
    <section className="branch-group" aria-label="Recording outcomes">
      <header><span>Recording outcomes</span></header>
      {branches.map((branch) => {
        const key = `${block.block_id}:${branch.key}`;
        return <BranchRow key={key} label={branch.label} root={branch.root} slot={branch.slot} isCollapsed={collapsed.has(key)} onToggleRow={() => onToggle(key)} {...common} />;
      })}
    </section>
  );
}

function ExternalCallBranches({
  block,
  prompts,
  selectedId,
  collapsed,
  activeSlot,
  pendingKind,
  canPlace,
  onSelect,
  onAdd,
  onAddDigit,
  onToggle,
  onDeleteDigit,
}: {
  block: Extract<FlowBlock, { type: "external_call" }>;
  prompts: Prompt[];
  selectedId: string | null;
  collapsed: Set<string>;
  activeSlot: FlowSlot | null;
  pendingKind: BlockKind | null;
  canPlace: (kind: BlockKind, slot: FlowSlot) => boolean;
  onSelect: (id: string) => void;
  onAdd: (slot: FlowSlot) => void;
  onAddDigit: (collectId: string) => void;
  onToggle: (key: string) => void;
  onDeleteDigit: (collectId: string, digit: string, count: number) => void;
}) {
  const common = { prompts, selectedId, collapsed, activeSlot, pendingKind, canPlace, onSelect, onAdd, onAddDigit, onToggle, onDeleteDigit };
  const branches = [
    { key: "completed", label: "Completed", root: block.next, slot: { kind: "next", parentId: block.block_id } as FlowSlot },
    { key: "not-connected", label: "Not connected", root: block.on_not_connected, slot: { kind: "not_connected", parentId: block.block_id } as FlowSlot },
    { key: "system-failure", label: "System failure", root: block.on_system_failure, slot: { kind: "system_failure", parentId: block.block_id } as FlowSlot },
  ];
  return (
    <section className="branch-group" aria-label="External call outcomes">
      <header><span>External call outcomes</span></header>
      {branches.map((branch) => {
        const key = `${block.block_id}:${branch.key}`;
        return <BranchRow key={key} label={branch.label} root={branch.root} slot={branch.slot} isCollapsed={collapsed.has(key)} onToggleRow={() => onToggle(key)} {...common} />;
      })}
    </section>
  );
}

function BranchRow({
  label,
  root,
  slot,
  isCollapsed,
  onToggleRow,
  onRemove,
  prompts,
  selectedId,
  collapsed,
  activeSlot,
  pendingKind,
  canPlace,
  onSelect,
  onAdd,
  onAddDigit,
  onToggle,
  onDeleteDigit,
}: {
  label: string;
  root: FlowBlock | null;
  slot: FlowSlot;
  isCollapsed: boolean;
  onToggleRow: () => void;
  onRemove?: () => void;
  prompts: Prompt[];
  selectedId: string | null;
  collapsed: Set<string>;
  activeSlot: FlowSlot | null;
  pendingKind: BlockKind | null;
  canPlace: (kind: BlockKind, slot: FlowSlot) => boolean;
  onSelect: (id: string) => void;
  onAdd: (slot: FlowSlot) => void;
  onAddDigit: (collectId: string) => void;
  onToggle: (key: string) => void;
  onDeleteDigit: (collectId: string, digit: string, count: number) => void;
}) {
  return (
    <div className="branch-row">
      <button type="button" className="branch-row__label" onClick={onToggleRow} aria-expanded={!isCollapsed}>{label}{isCollapsed ? <ChevronRight size={15} /> : <ChevronDown size={15} />}</button>
      {!isCollapsed ? (
        <div className="branch-row__path">
          {root ? (
            <div className="branch-entry-path">
              <AddSlot
                slot={slot}
                label="Insert"
                ariaLabel={`Insert step before ${blockTitle(root, prompts)} in ${label} branch`}
                activeSlot={activeSlot}
                pendingKind={pendingKind}
                canPlace={canPlace}
                onChoose={onAdd}
                inline
              />
              <BlockPath block={root} prompts={prompts} selectedId={selectedId} collapsed={collapsed} activeSlot={activeSlot} pendingKind={pendingKind} canPlace={canPlace} onSelect={onSelect} onAdd={onAdd} onAddDigit={onAddDigit} onToggle={onToggle} onDeleteDigit={onDeleteDigit} inline />
            </div>
          ) : (
            <AddSlot
              slot={slot}
              label="Add first step"
              ariaLabel={`Add first step in ${label} branch`}
              activeSlot={activeSlot}
              pendingKind={pendingKind}
              canPlace={canPlace}
              onChoose={onAdd}
              inline
            />
          )}
        </div>
      ) : <span className="branch-row__collapsed">{countBlocks(root)} hidden steps</span>}
      {onRemove ? <button type="button" className="branch-row__remove" onClick={onRemove} aria-label={`Remove digit ${label}`}><Trash2 size={15} /></button> : null}
    </div>
  );
}

function Inspector({
  block,
  flow,
  prompts,
  schedules,
  recordingBehavior,
  descendantCount,
  onChange,
  onRemovePrompt,
  onDelete,
  onMove,
  onAddDigit,
  onClose,
}: {
  block: FlowBlock | null;
  flow: FlowDefinition;
  prompts: Prompt[];
  schedules: Schedule[];
  recordingBehavior: RecordingBehavior;
  descendantCount: number;
  onChange: (block: FlowBlock) => void;
  onRemovePrompt: (block: FlowBlock) => void;
  onDelete: (block: FlowBlock) => void;
  onMove: (block: FlowBlock) => void;
  onAddDigit: (collectId: string) => void;
  onClose: () => void;
}) {
  const [search, setSearch] = useState("");
  if (!block) {
    return (
      <aside className="studio-inspector studio-inspector--empty">
        <CheckCircle2 size={25} />
        <h2>Select a step</h2>
        <p>Choose a block on the canvas to configure it. Internal block IDs stay hidden.</p>
      </aside>
    );
  }
  const filteredPrompts = prompts.filter((prompt) => prompt.name.toLowerCase().includes(search.toLowerCase()));
  const selectedPromptId = block.type === "play_prompt" || block.type === "collect_digit" ? block.prompt_id : null;
  const selectedPrompt = prompts.find((prompt) => prompt.id === selectedPromptId) ?? null;
  const promptCanBeRemoved = canRemovePlayPrompt(flow, block);
  const hasRecordedNext = block.type === "play_prompt"
    && (block.next?.type === "record_message" || block.next?.type === "external_call");
  const promptRemovalHint = !promptCanBeRemoved
    ? "No prompt is unavailable because a recorded action below this step has no other earlier prompt notice on its path."
    : hasRecordedNext
    ? "An earlier prompt exists on this path. Make sure its recording notice finishes before callers can interrupt it."
    : "Choose No prompt to remove this step while keeping its next action.";
  return (
    <aside className="studio-inspector">
      <header><div>{(() => { const Icon = blockIcon(block.type); return <Icon size={20} />; })()}<span><h2>{BLOCK_LABELS[block.type]}</h2><p>Configure the selected step</p></span></div><button type="button" onClick={onClose} aria-label="Close inspector"><X size={19} /></button></header>
      <div className="studio-inspector__body">
        {block.type === "play_prompt" ? (
          <>
            <Field label="Prompt" hint={promptRemovalHint}>
              <div className="prompt-search"><Search size={16} /><input aria-label="Search uploaded prompts" value={search} onChange={(event) => setSearch(event.target.value)} placeholder="Search prompts…" /></div>
              <select aria-label="Prompt" value={block.prompt_id ?? NO_PROMPT_VALUE} onChange={(event) => {
                if (event.target.value === NO_PROMPT_VALUE) {
                  onRemovePrompt(block);
                  return;
                }
                onChange({ ...block, prompt_id: event.target.value || null });
              }}>
                <option value={NO_PROMPT_VALUE} disabled={!promptCanBeRemoved}>
                  {promptCanBeRemoved ? "No prompt" : "No prompt — earlier notice required"}
                </option>
                {filteredPrompts.map((prompt) => <option key={prompt.id} value={prompt.id}>{prompt.name}</option>)}
              </select>
            </Field>
            <AudioPreview prompt={selectedPrompt} />
          </>
        ) : null}
        {block.type === "collect_digit" ? (
          <>
            <Field label="Menu prompt" hint="Played before every input attempt.">
              <div className="prompt-search"><Search size={16} /><input aria-label="Search uploaded prompts" value={search} onChange={(event) => setSearch(event.target.value)} placeholder="Search prompts…" /></div>
              <select aria-label="Menu prompt" value={block.prompt_id ?? ""} onChange={(event) => {
                const promptId = event.target.value || null;
                onChange({
                  ...block,
                  prompt_id: promptId,
                  allow_prompt_barge_in: promptId ? block.allow_prompt_barge_in : false,
                });
              }}>
                <option value="">No menu prompt</option>
                {filteredPrompts.map((prompt) => <option key={prompt.id} value={prompt.id}>{prompt.name}</option>)}
              </select>
            </Field>
            <AudioPreview prompt={selectedPrompt} />
            <label className={`toggle-row ${block.prompt_id ? "" : "toggle-row--disabled"}`}>
              <input
                type="checkbox"
                checked={block.allow_prompt_barge_in ?? false}
                disabled={!block.prompt_id}
                onChange={(event) => onChange({ ...block, allow_prompt_barge_in: event.target.checked })}
              />
              <span className="toggle" aria-hidden="true" />
              <span>
                <strong>Allow configured digits during prompt</strong>
                <small>Configured digits stop playback immediately; other keys are ignored until playback finishes.</small>
              </span>
            </label>
            <div className="inspector-grid">
              <Field label="Timeout"><select value={block.timeout_ms} onChange={(event) => onChange({ ...block, timeout_ms: Number(event.target.value) })}>{[3000, 5000, 8000, 10000, 15000].map((value) => <option key={value} value={value}>{value / 1000} seconds</option>)}</select></Field>
              <Field label="Attempts"><select value={block.maximum_attempts} onChange={(event) => onChange({ ...block, maximum_attempts: Number(event.target.value) })}>{[1, 2, 3].map((value) => <option key={value}>{value}</option>)}</select></Field>
            </div>
            <Field label="Menu returns" hint="Maximum Return to menu actions before the return-limit path runs."><select value={block.maximum_menu_returns} onChange={(event) => onChange({ ...block, maximum_menu_returns: Number(event.target.value) })}>{[1, 2, 3].map((value) => <option key={value}>{value}</option>)}</select></Field>
            <section className="inspector-branches"><header><strong>Digit branches</strong><button type="button" onClick={() => onAddDigit(block.block_id)}><Plus size={15} /> Add digit</button></header>{block.branches.map((branch) => <div key={branch.branch_id}><span>{branch.digit}</span><strong>{branch.root ? blockTitle(branch.root, prompts) : "Empty branch"}</strong></div>)}</section>
          </>
        ) : null}
        {block.type === "schedule_branch" ? <Field label="Schedule"><select value={block.schedule_id ?? ""} onChange={(event) => onChange({ ...block, schedule_id: event.target.value || null })}><option value="">Select a schedule</option>{schedules.map((schedule) => <option key={schedule.id} value={schedule.id}>{schedule.name}</option>)}</select></Field> : null}
        {block.type === "record_message" ? <div className="inspector-note"><Mic2 size={18} /><p>This step plays a built-in beep, then records for up to {recordingBehavior.maximum_duration_seconds} seconds. {recordingBehavior.finish_key ? `The caller can press ${recordingBehavior.finish_key} to finish.` : "Only hangup or the hard limit stops it."} These settings are shared and signed. <a href="/settings#recording-behavior">Edit recording behavior</a>.</p></div> : null}
        {block.type === "external_call" ? <>
          <Field label="Operator phone number" hint="Use 8–15 digits, optionally starting with +. Pauses, service codes, and emergency numbers are rejected."><input type="tel" aria-label="Operator phone number" inputMode="tel" maxLength={16} placeholder="03136644636" value={block.phone_number} onChange={(event) => onChange({ ...block, phone_number: event.target.value.trim() })} /></Field>
          <Field label="Answer timeout" hint="Starts when the outgoing operator leg begins dialing."><div className="input-with-suffix"><input type="number" aria-label="Answer timeout" min={5} max={120} step={1} value={block.answer_timeout_seconds} onChange={(event) => onChange({ ...block, answer_timeout_seconds: Number(event.target.value) })} /><span>seconds</span></div></Field>
          <div className="inspector-note"><PhoneForwarded size={18} /><p>The caller must hear the preceding notice first. IVRdroid then holds the caller, dials this number, merges the carrier conference, and records it. Recording is mandatory; a failure follows System failure.</p></div>
        </> : null}
        {block.type === "return_to_menu" ? <div className="inspector-note"><RotateCcw size={18} /><p>Returns to the nearest owning digit menu. The menu’s bounded return count prevents infinite loops.</p></div> : null}
        {block.type === "end_call" ? <div className="inspector-note"><PhoneOff size={18} /><p>This terminal step safely ends the cellular call.</p></div> : null}
      </div>
      <footer><div className="inspector-footer-actions"><Button variant="secondary" onClick={() => onMove(block)}><MoveRight size={16} /> Move subtree</Button><Button variant="danger" onClick={() => onDelete(block)}><Trash2 size={16} /> Delete subtree</Button></div><p>Move preserves all {descendantCount} owned step{descendantCount === 1 ? "" : "s"}. Delete removes this step and {descendantCount - 1} descendant step{descendantCount - 1 === 1 ? "" : "s"}.</p></footer>
    </aside>
  );
}

function AudioPreview({ prompt, emptyMessage = "Select a prompt to preview it." }: { prompt: Prompt | null; emptyMessage?: string }) {
  return (
    <section className="audio-preview"><strong>Audio preview</strong>{prompt ? <><audio controls preload="metadata" src={prompt.audio_url} /><small>{Math.round(prompt.duration_ms / 100) / 10} seconds · version {prompt.version}</small></> : <p>{emptyMessage}</p>}</section>
  );
}

function AddDigitModal({ collect, prompts, onClose, onAdd }: { collect: FlowBlock | null; prompts: Prompt[]; onClose: () => void; onAdd: (digit: string, promptId: string | null) => void }) {
  const menu = collect?.type === "collect_digit" ? collect : null;
  const digits = menu ? availableDigits(menu) : [];
  const [digit, setDigit] = useState(digits[0] ?? "");
  const [promptId, setPromptId] = useState(prompts[0]?.id ?? "");
  const [search, setSearch] = useState("");
  const filteredPrompts = prompts.filter((prompt) => prompt.name.toLowerCase().includes(search.toLowerCase()));
  const selectedPrompt = prompts.find((prompt) => prompt.id === promptId) ?? null;
  return <Modal title="Add digit branch" onClose={onClose}><p className="modal-copy">Start with a private prompt for this digit, or choose No prompt to begin with an End call placeholder. Recorded actions still require their greeting or notice.</p><Field label="Digit"><select value={digit} onChange={(event) => setDigit(event.target.value)}>{digits.map((item) => <option key={item}>{item}</option>)}</select></Field><Field label="First prompt (optional)"><div className="prompt-search"><Search size={16} /><input aria-label="Search uploaded prompts" value={search} onChange={(event) => setSearch(event.target.value)} placeholder="Search uploaded prompts…" /></div><select aria-label="First prompt" value={promptId} onChange={(event) => setPromptId(event.target.value)}><option value="">No prompt</option>{filteredPrompts.map((prompt) => <option key={prompt.id} value={prompt.id}>{prompt.name}</option>)}</select></Field><AudioPreview prompt={selectedPrompt} emptyMessage="No prompt will play when this branch starts." /><div className="modal__actions"><Button variant="secondary" onClick={onClose}>Cancel</Button><Button onClick={() => onAdd(digit, promptId || null)} disabled={!digit}>Add branch</Button></div></Modal>;
}

function ConfirmDeleteModal({ request, onCancel, onConfirm }: { request: DeleteRequest; onCancel: () => void; onConfirm: () => void }) {
  return <Modal title="Delete subtree?" onClose={onCancel}><div className="delete-warning"><AlertTriangle size={22} /><div><strong>{request.label}</strong><p>This permanently removes this step and {request.count - 1} descendant step{request.count - 1 === 1 ? "" : "s"} from the draft. Undo remains available until the page is reloaded.</p></div></div><div className="modal__actions"><Button variant="secondary" onClick={onCancel}>Cancel</Button><Button variant="danger" onClick={onConfirm}><Trash2 size={16} /> Delete {request.count} steps</Button></div></Modal>;
}

function ConfirmDigitDeleteModal({ request, onCancel, onConfirm }: { request: DigitDeleteRequest; onCancel: () => void; onConfirm: () => void }) {
  return <Modal title={`Remove digit ${request.digit}?`} onClose={onCancel}><div className="delete-warning"><AlertTriangle size={22} /><div><strong>Delete this branch</strong><p>This removes digit {request.digit} and all {request.count} owned step{request.count === 1 ? "" : "s"} beneath it.</p></div></div><div className="modal__actions"><Button variant="secondary" onClick={onCancel}>Cancel</Button><Button variant="danger" onClick={onConfirm}>Remove branch</Button></div></Modal>;
}

function moveSlotLabel(flow: FlowDefinition, slot: FlowSlot, prompts: Prompt[]): string {
  if (slot.kind === "root") return "Start of flow";
  const parent = findBlock(flow.root, slot.parentId);
  const parentName = parent ? blockTitle(parent, prompts) : "Unknown step";
  const edge = slot.kind === "next"
    ? "Completed / next"
    : slot.kind === "digit"
      ? `Digit ${slot.digit}`
      : ({
          unavailable: "Unavailable",
          not_connected: "Not connected",
          system_failure: "System failure",
          timeout: "No input",
          invalid: "Invalid input",
          return_limit: "Return limit",
          open: "Open",
          closed: "Closed",
          holiday: "Holiday",
        } as const)[slot.kind];
  return `${edge} after ${parentName}`;
}

function MoveSubtreeModal({
  request,
  flow,
  prompts,
  onCancel,
  onConfirm,
}: {
  request: MoveRequest;
  flow: FlowDefinition;
  prompts: Prompt[];
  onCancel: () => void;
  onConfirm: (destination: FlowSlot) => void;
}) {
  const targets = useMemo(
    () => allFlowSlots(flow.root).filter((slot) => canMoveSubtree(flow, request.blockId, slot)),
    [flow, request.blockId],
  );
  const [destination, setDestination] = useState(() => targets[0] ? JSON.stringify(targets[0]) : "");
  return <Modal title="Move subtree" onClose={onCancel}><p className="modal-copy"><strong>{request.label}</strong> and all of its owned outcomes move together. The old position receives a safe End call placeholder.</p>{targets.length ? <Field label="Destination" hint="Branching recording and external-call steps can move only directly after a prompt notice."><select aria-label="Move destination" value={destination} onChange={(event) => setDestination(event.target.value)}>{targets.map((slot) => { const value = JSON.stringify(slot); return <option key={value} value={value}>{moveSlotLabel(flow, slot, prompts)}</option>; })}</select></Field> : <div className="delete-warning"><AlertTriangle size={20} /><div><strong>No safe destination</strong><p>Add an empty or End call position outside this subtree first.</p></div></div>}<div className="modal__actions"><Button variant="secondary" onClick={onCancel}>Cancel</Button><Button onClick={() => onConfirm(JSON.parse(destination) as FlowSlot)} disabled={!destination}><MoveRight size={16} /> Move subtree</Button></div></Modal>;
}

function SimulationModal({ result, prompts, busy, onEvent, onReset, onClose }: { result: SimulationResult | null; prompts: Prompt[]; busy: boolean; onEvent: (event: string) => void; onReset: () => void; onClose: () => void }) {
  const latestPrompt = [...(result?.trace ?? [])].reverse().find((step) => step.prompt_id)?.prompt_id ?? null;
  return <Modal title="Simulate call path" onClose={onClose} wide><div className="simulation-layout"><section><header><h3>Execution trace</h3><Button variant="secondary" onClick={onReset} disabled={busy}><RotateCcw size={15} /> Reset</Button></header><ol className="simulation-trace">{result?.trace.map((step, index) => {
    const promptName = step.prompt_id ? prompts.find((prompt) => prompt.id === step.prompt_id)?.name : null;
    const detail = [promptName, step.event].filter(Boolean).join(" · ");
    return <li key={`${step.block_id}-${index}`}><span>{index + 1}</span><div><strong>{step.label}</strong>{detail ? <small>{detail}</small> : null}</div></li>;
  })}</ol>{!result?.trace.length ? <p className="simulation-empty">No steps executed.</p> : null}</section><aside><span className={`simulation-status simulation-status--${result?.status ?? "invalid"}`}>{result?.status?.replaceAll("_", " ") ?? "Loading"}</span><p>{result?.message ?? "Preparing simulation…"}</p>{latestPrompt ? <AudioPreview prompt={prompts.find((prompt) => prompt.id === latestPrompt) ?? null} /> : null}{result?.status === "awaiting_input" ? <><strong>Send input</strong><div className="dtmf-pad">{result.available_digits.map((digit) => <button type="button" key={digit} onClick={() => onEvent(digit)} disabled={busy}>{digit}</button>)}</div><div className="simulation-actions"><Button variant="secondary" onClick={() => onEvent("timeout")} disabled={busy}>No input</Button><Button variant="secondary" onClick={() => onEvent("invalid")} disabled={busy}>Invalid</Button></div></> : null}{result?.status === "awaiting_schedule" ? <><strong>Choose schedule result</strong><div className="simulation-actions">{result.available_schedule_states?.map((state) => <Button key={state} variant="secondary" onClick={() => onEvent(state)} disabled={busy}>{state[0].toUpperCase() + state.slice(1)}</Button>)}</div></> : null}{result?.status === "awaiting_recording" ? <><strong>Choose recording result</strong><div className="simulation-actions">{result.available_recording_outcomes?.map((outcome) => <Button key={outcome} variant="secondary" onClick={() => onEvent(outcome)} disabled={busy}>{outcome === "recorded" ? "Recorded" : outcome === "unavailable" ? "Storage unavailable" : "Caller hung up"}</Button>)}</div></> : null}{result?.status === "awaiting_external" ? <><strong>Choose external call result</strong><div className="simulation-actions">{result.available_external_outcomes?.map((outcome) => <Button key={outcome} variant="secondary" onClick={() => onEvent(outcome)} disabled={busy}>{outcome === "external_completed" ? "Completed" : outcome === "external_not_connected" ? "Not connected" : "System failure"}</Button>)}</div></> : null}</aside></div></Modal>;
}
