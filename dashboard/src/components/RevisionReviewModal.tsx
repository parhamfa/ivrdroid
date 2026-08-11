import { AlertTriangle, CalendarClock, Cloud, GitCompare, Mic2, ShieldCheck } from "lucide-react";
import { useState, type ReactNode } from "react";
import type { FlowDiff } from "../types";
import { Button, Modal } from "./ui";


function ChangeList({ changes, empty }: { changes: string[]; empty: string }) {
  return changes.length ? (
    <ul>{changes.map((change, index) => <li key={`${change}-${index}`}>{change}</li>)}</ul>
  ) : (
    <p>{empty}</p>
  );
}

function ChangeSection({
  title,
  icon,
  changes,
  empty,
}: {
  title: string;
  icon: ReactNode;
  changes: string[];
  empty: string;
}) {
  return (
    <section className="review-changes">
      <h3>{icon}{title}</h3>
      <ChangeList changes={changes} empty={empty} />
    </section>
  );
}

export function hasRevisionChanges(diff: FlowDiff): boolean {
  return Boolean(
    diff.added
    || diff.removed
    || diff.changed
    || diff.caller_policy_changes.length
    || diff.schedule_changes.length
    || diff.recording_changes.length
  );
}

export function RevisionReviewModal({
  diff,
  stepCount,
  branchCount,
  busy,
  onClose,
  onPublish,
}: {
  diff: FlowDiff;
  stepCount: number;
  branchCount: number;
  busy: boolean;
  onClose: () => void;
  onPublish: () => void;
}) {
  const [policyConfirmed, setPolicyConfirmed] = useState(false);
  const hasChanges = hasRevisionChanges(diff);
  const changeCount = diff.added
    + diff.removed
    + diff.changed
    + diff.caller_policy_changes.length
    + diff.schedule_changes.length
    + diff.recording_changes.length;
  const publishDisabled = busy
    || !hasChanges
    || (diff.requires_policy_confirmation && !policyConfirmed);

  return (
    <Modal title="Review revision" onClose={onClose} wide>
      <div className="review-summary">
        <div>
          <span>Base revision</span>
          <strong>{diff.base_revision_id ? `#${diff.base_revision_id}` : "None"}</strong>
        </div>
        <div><span>Flow size</span><strong>{stepCount} steps · {branchCount} branches</strong></div>
        <div><span>Revision changes</span><strong>{changeCount || "None"}</strong></div>
      </div>

      <div className="review-change-grid">
        <ChangeSection
          title="Caller policy"
          icon={<ShieldCheck size={18} />}
          changes={diff.caller_policy_changes}
          empty="No caller-policy changes."
        />
        <ChangeSection
          title="Schedules"
          icon={<CalendarClock size={18} />}
          changes={diff.schedule_changes}
          empty="No schedule changes."
        />
        <ChangeSection
          title="Recording behavior"
          icon={<Mic2 size={18} />}
          changes={diff.recording_changes}
          empty="No recording-setting changes."
        />
        <ChangeSection
          title={`Flow · +${diff.added} / −${diff.removed} / ${diff.changed} changed`}
          icon={<GitCompare size={18} />}
          changes={diff.changes}
          empty="No authored flow changes."
        />
      </div>

      {diff.legacy_base ? (
        <div className="legacy-note">
          <AlertTriangle size={17} />
          The base flow is a V1–V3 rollback snapshot, so its graph cannot be structurally compared with V4.
        </div>
      ) : null}

      {!hasChanges ? (
        <div className="review-noop" role="status">
          This draft already matches the latest signed revision. There is nothing to publish.
        </div>
      ) : null}

      {diff.requires_policy_confirmation ? (
        <label className="publish-confirmation">
          <input
            type="checkbox"
            checked={policyConfirmed}
            onChange={(event) => setPolicyConfirmed(event.target.checked)}
          />
          <span>
            <strong>Confirm broader caller access</strong>
            <small>I understand this revision sends more callers, hidden callers, or both to IVR.</small>
          </span>
        </label>
      ) : null}

      <div className="modal__actions">
        <Button variant="secondary" onClick={onClose}>Cancel</Button>
        <Button onClick={onPublish} disabled={publishDisabled}>
          <Cloud size={16} /> Publish signed V4 revision
        </Button>
      </div>
    </Modal>
  );
}
