import { Ban, Check, Cloud, MoreVertical, Search, ShieldCheck, UserRoundCheck, UsersRound } from "lucide-react";
import { useEffect, useMemo, useState } from "react";
import { api } from "../api";
import { RevisionReviewModal } from "../components/RevisionReviewModal";
import { Button, Drawer, ErrorState, Field, Loading, SuccessMessage } from "../components/ui";
import { useRemote } from "../hooks";
import type { CallerEntry, DraftConfiguration, FlowDiff, PolicyMode } from "../types";
import { countBlocks, countBranches } from "./flowEditor";

const MODES: Array<{ mode: PolicyMode; label: string; description: string; icon: typeof Ban }> = [
  { mode: "IVR_DISABLED", label: "IVR disabled", description: "All calls bypass IVR", icon: Ban },
  { mode: "ALLOWLIST_ONLY", label: "Allowlist only", description: "Only allowed callers reach IVR", icon: UserRoundCheck },
  { mode: "ACCEPT_ALL", label: "Accept all", description: "All callers reach IVR", icon: UsersRound },
  { mode: "ACCEPT_ALL_EXCEPT_BLOCKLIST", label: "All except exclusion list", description: "Everyone except excluded callers", icon: ShieldCheck },
];

export function CallerPolicyPage() {
  const remote = useRemote(api.draft, []);
  const [draft, setDraft] = useState<DraftConfiguration | null>(null);
  const [drawerOpen, setDrawerOpen] = useState(false);
  const [label, setLabel] = useState("");
  const [phone, setPhone] = useState("");
  const [search, setSearch] = useState("");
  const [busy, setBusy] = useState(false);
  const [message, setMessage] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [reviewDiff, setReviewDiff] = useState<FlowDiff | null>(null);
  const [reviewOpen, setReviewOpen] = useState(false);

  useEffect(() => setDraft(remote.data), [remote.data]);
  const isBlocklist = draft?.caller_policy.mode === "ACCEPT_ALL_EXCEPT_BLOCKLIST";
  const entries = useMemo(() => {
    const list = isBlocklist ? draft?.caller_policy.blocklist : draft?.caller_policy.allowlist;
    const normalized = search.trim().toLowerCase();
    return (list ?? []).filter((entry) => !normalized || entry.label.toLowerCase().includes(normalized) || entry.e164.includes(normalized));
  }, [draft, isBlocklist, search]);
  const dirty = useMemo(
    () => Boolean(
      draft
      && remote.data
      && JSON.stringify(draft.caller_policy) !== JSON.stringify(remote.data.caller_policy)
    ),
    [draft?.caller_policy, remote.data?.caller_policy],
  );
  const stepCount = countBlocks(draft?.flow.root ?? null);
  const branchCount = countBranches(draft?.flow.root ?? null);

  if (remote.loading) return <Loading label="Loading caller policy" />;
  if (remote.error) return <ErrorState message={remote.error} retry={remote.refresh} />;
  if (!draft) return <Loading label="Loading caller policy" />;

  const updatePolicy = (updates: Partial<DraftConfiguration["caller_policy"]>) => {
    setDraft({ ...draft, caller_policy: { ...draft.caller_policy, ...updates } });
    setMessage(null);
  };

  const addCaller = () => {
    const trimmedLabel = label.trim();
    const trimmedPhone = phone.trim();
    if (!trimmedLabel || !/^\+[1-9][0-9]{7,14}$/.test(trimmedPhone)) {
      setError("Enter a label and a valid E.164 phone number.");
      return;
    }
    const entry: CallerEntry = { label: trimmedLabel, e164: trimmedPhone };
    const key = isBlocklist ? "blocklist" : "allowlist";
    const current = draft.caller_policy[key];
    if (current.some((item) => item.e164 === trimmedPhone)) {
      setError("That phone number is already in this list.");
      return;
    }
    updatePolicy({ [key]: [...current, entry] });
    setLabel("");
    setPhone("");
    setDrawerOpen(false);
    setError(null);
  };

  const removeCaller = (entry: CallerEntry) => {
    const key = isBlocklist ? "blocklist" : "allowlist";
    updatePolicy({ [key]: draft.caller_policy[key].filter((item) => item.e164 !== entry.e164) });
  };

  const persistDraft = async (candidate: DraftConfiguration) => {
    const saved = await api.saveDraft(candidate);
    setDraft(saved);
    remote.setData(saved);
    return saved;
  };

  const save = async () => {
    if (!dirty) return;
    setBusy(true);
    setError(null);
    try {
      await persistDraft(draft);
      setMessage("Caller policy saved to the draft. It is not active until you publish.");
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : "Could not save caller policy");
    } finally {
      setBusy(false);
    }
  };

  const openReview = async () => {
    setBusy(true);
    setError(null);
    setMessage(null);
    try {
      const candidate = dirty ? await persistDraft(draft) : draft;
      const validation = await api.validateDraft(candidate);
      if (!validation.valid) {
        setError(`This revision cannot publish: ${validation.errors.join(" ")}`);
        return;
      }
      setReviewDiff(await api.diffDraft(candidate));
      setReviewOpen(true);
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : "Could not prepare the revision review.");
    } finally {
      setBusy(false);
    }
  };

  const publish = async () => {
    if (!reviewDiff) return;
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

  const discard = () => {
    setDraft(remote.data);
    setMessage(null);
    setError(null);
  };

  return (
    <div className={`split-page ${drawerOpen ? "split-page--open" : ""}`}>
      <div className="split-page__main policy-page">
        {message ? <SuccessMessage>{message}</SuccessMessage> : null}
        {error ? <ErrorState message={error} /> : null}
        <section>
          <h2>Policy</h2>
          <div className="policy-modes">
            {MODES.map((item) => {
              const Icon = item.icon;
              const selected = draft.caller_policy.mode === item.mode;
              return (
                <button
                  key={item.mode}
                  className={`policy-mode ${selected ? "policy-mode--selected" : ""}`}
                  onClick={() => updatePolicy({ mode: item.mode })}
                >
                  {selected ? <span className="policy-mode__check"><Check size={15} /></span> : null}
                  <Icon size={38} />
                  <strong>{item.label}</strong>
                  <span>{item.description}</span>
                </button>
              );
            })}
          </div>
        </section>

        <label className="toggle-row">
          <input
            type="checkbox"
            checked={draft.caller_policy.route_unknown_callers}
            onChange={(event) => updatePolicy({ route_unknown_callers: event.target.checked })}
          />
          <span className="toggle" aria-hidden="true" />
          <span><strong>Send hidden or unknown callers to IVR</strong><small>{draft.caller_policy.route_unknown_callers ? "On · IVR handles callers without a usable number" : "Off · Stock dialer handles callers without a usable number"}</small></span>
        </label>

        <section className="list-section">
          <div className="section-toolbar">
            <h2>{isBlocklist ? "Excluded callers" : "Allowed callers"}</h2>
            <label className="search-input"><Search size={19} /><input value={search} onChange={(event) => setSearch(event.target.value)} placeholder="Search callers" /></label>
            <span>{entries.length} callers</span>
            <Button onClick={() => setDrawerOpen(true)}>Add caller</Button>
          </div>
          <div className="table-frame">
            <table>
              <thead><tr><th>Label</th><th>Phone number</th><th>List</th><th><span className="sr-only">Actions</span></th></tr></thead>
              <tbody>
                {entries.map((entry) => (
                  <tr key={entry.e164}>
                    <td>{entry.label}</td><td>{entry.e164}</td><td>{isBlocklist ? "Exclusion" : "Allowlist"}</td>
                    <td className="row-action"><button className="icon-button" onClick={() => removeCaller(entry)} aria-label={`Remove ${entry.label}`}><MoreVertical size={19} /></button></td>
                  </tr>
                ))}
                {entries.length === 0 ? <tr><td colSpan={4} className="empty-cell">No callers in this list.</td></tr> : null}
              </tbody>
            </table>
          </div>
        </section>

        <div className="page-actions">
          {dirty ? <span className="unsaved-state" role="status"><i /> Unsaved changes</span> : null}
          <Button variant="secondary" onClick={discard} disabled={!dirty || busy}>Discard changes</Button>
          <Button variant="secondary" onClick={() => void save()} disabled={!dirty || busy}>{busy && dirty ? "Saving…" : "Save draft"}</Button>
          <Button onClick={() => void openReview()} disabled={busy}><Cloud size={16} /> Review &amp; publish</Button>
        </div>
      </div>

      {drawerOpen ? (
        <Drawer
          title="Add caller"
          onClose={() => setDrawerOpen(false)}
          footer={<><Button variant="secondary" onClick={() => setDrawerOpen(false)}>Cancel</Button><Button onClick={addCaller}>Add caller</Button></>}
        >
          <Field label="Label"><input value={label} onChange={(event) => setLabel(event.target.value)} placeholder="Enter label" autoFocus /></Field>
          <Field label="Phone number" hint="Use E.164 format, for example +15551234567"><input value={phone} onChange={(event) => setPhone(event.target.value)} placeholder="+15551234567" inputMode="tel" /></Field>
        </Drawer>
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
