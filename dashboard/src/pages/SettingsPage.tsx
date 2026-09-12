import { Archive, Bell, Clipboard, HardDrive, History, KeyRound, Laptop, Mic2, Plus, RotateCcw, ShieldOff, Trash2 } from "lucide-react";
import { useEffect, useState } from "react";
import { api } from "../api";
import { Button, ErrorState, Field, Loading, SuccessMessage, formatBytes, formatDate } from "../components/ui";
import { useRemote } from "../hooks";
import { RELEASE_VERSION, SOURCE_COMMIT } from "../release";
import { SessionAuditSettingsCard } from "./SessionAuditSettingsCard";
import type { DraftConfiguration, NtfyEvents, NtfyPriority, NtfySettings, RecordingSettings, Schedule, ScheduleException, WeeklyWindow } from "../types";

const DAYS = ["Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"];
const NTFY_PRIORITIES: NtfyPriority[] = ["min", "low", "default", "high", "max"];
const NTFY_EVENTS: Array<{ key: keyof NtfyEvents; title: string; description: string }> = [
  { key: "voicemail_ready", title: "Voicemail ready", description: "When a recorded message is playable." },
  { key: "conversation_ready", title: "Conversation ready", description: "When an external-call recording is playable." },
  { key: "ivr_session_failed", title: "IVR session failed", description: "Helper or answer failures after a matched caller." },
  { key: "external_call_failed", title: "External call failed", description: "Operator step ended not connected or in system failure." },
  { key: "revision_activation_failed", title: "Revision activation failed", description: "The tablet rejected or failed a published revision." },
  { key: "storage_full", title: "Storage full", description: "Server quota warning or a tablet spool at its hard limit." },
  { key: "tablet_offline", title: "Tablet offline", description: "Uses last call state, not the 3-minute Online pill." },
  { key: "ivr_session_completed", title: "IVR session completed", description: "Successful or remote-hangup IVR sessions." },
  { key: "stock_dialer_routing", title: "Stock-dialer routing", description: "Unmatched callers left with the Android dialer." },
];

function isExternalCallReady(status: Record<string, unknown>): boolean {
  return Array.isArray(status.runtime_versions)
    && status.runtime_versions.includes(4)
    && status.external_call_control_capable === true
    && status.conversation_recording_capable === true
    && status.call_control_protocol_version === 1;
}

function isPromptBargeInReady(status: Record<string, unknown>): boolean {
  return Array.isArray(status.runtime_versions)
    && status.runtime_versions.includes(4)
    && status.prompt_barge_in_capable === true;
}

export function SettingsPage() {
  const remote = useRemote(async () => {
    const [draft, recordingSettings, ntfySettings, devices, revisions, audit] = await Promise.all([
      api.draft(),
      api.recordingSettings(),
      api.ntfySettings(),
      api.devices(),
      api.revisions(),
      api.audit(),
    ]);
    return { draft, recordingSettings, ntfySettings, devices, revisions, audit };
  }, []);
  const [draft, setDraft] = useState<DraftConfiguration | null>(null);
  const [retention, setRetention] = useState<RecordingSettings | null>(null);
  const [ntfy, setNtfy] = useState<NtfySettings | null>(null);
  const [ntfyToken, setNtfyToken] = useState("");
  const [pairingName, setPairingName] = useState("SM-T585");
  const [pairing, setPairing] = useState<{ code: string; expires_at: string } | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [message, setMessage] = useState<string | null>(null);
  useEffect(() => {
    setDraft(remote.data?.draft ?? null);
    setRetention(remote.data?.recordingSettings ?? null);
    setNtfy(remote.data?.ntfySettings ?? null);
    setNtfyToken("");
  }, [remote.data]);

  if (remote.error) return <ErrorState message={remote.error} retry={remote.refresh} />;
  if (remote.loading || !draft || !retention || !ntfy) return <Loading label="Loading settings" />;
  if (!remote.data) return <ErrorState message={remote.error ?? "Settings unavailable"} retry={remote.refresh} />;
  const tabletSpoolBytes = remote.data.devices.reduce((total, device) => total + Number(device.status.voicemail_spool_bytes ?? device.status.recording_spool_bytes ?? 0), 0);
  const tabletSpoolCount = remote.data.devices.reduce((total, device) => total + Number(device.status.voicemail_spool_count ?? device.status.recording_spool_count ?? 0), 0);
  const conversationSpoolBytes = remote.data.devices.reduce((total, device) => total + Number(device.status.conversation_spool_bytes ?? 0), 0);
  const conversationSpoolCount = remote.data.devices.reduce((total, device) => total + Number(device.status.conversation_spool_count ?? 0), 0);
  const tabletSpoolFull = tabletSpoolBytes >= 512 * 1024 ** 2;
  const conversationSpoolFull = conversationSpoolBytes >= 4 * 1024 ** 3;

  const updateSchedule = (index: number, value: Schedule) => {
    const schedules = [...draft.schedules]; schedules[index] = value; setDraft({ ...draft, schedules }); setMessage(null);
  };
  const addSchedule = () => {
    const used = new Set(draft.schedules.map((item) => item.id));
    let index = 1; while (used.has(`schedule_${index}`)) index += 1;
    setDraft({ ...draft, schedules: [...draft.schedules, { id: `schedule_${index}`, name: "Business hours", timezone: "Asia/Tehran", weekly: [], exceptions: [] }] });
  };
  const saveSchedules = async () => {
    setBusy(true); setError(null);
    try { setDraft(await api.saveDraft(draft)); setMessage("Schedules saved to the draft."); }
    catch (reason) { setError(reason instanceof Error ? reason.message : "Could not save schedules"); }
    finally { setBusy(false); }
  };
  const saveRecordingBehavior = async () => {
    setBusy(true); setError(null);
    try { setDraft(await api.saveDraft(draft)); setMessage("Recording behavior saved to the signed V4 draft. It is not active until publish."); }
    catch (reason) { setError(reason instanceof Error ? reason.message : "Could not save recording behavior"); }
    finally { setBusy(false); }
  };
  const saveRetention = async () => {
    const explanation = retention.mode === "automatic"
      ? `Automatically and irreversibly delete session audit, voicemail and conversation audio older than ${retention.days} days? Audit tombstones remain.`
      : "Switch to manual retention? Existing recordings remain until you delete them.";
    if (!window.confirm(explanation)) return;
    setBusy(true); setError(null);
    try { setRetention(await api.saveRecordingSettings(retention.mode, retention.days)); setMessage("Retention policy is effective immediately."); }
    catch (reason) { setError(reason instanceof Error ? reason.message : "Could not save retention policy"); }
    finally { setBusy(false); }
  };
  const saveNtfy = async () => {
    if (ntfy.events.tablet_offline.in_call_timeout_minutes < ntfy.events.tablet_offline.idle_timeout_minutes) {
      setError("In-call timeout must be at least the idle timeout.");
      return;
    }
    setBusy(true); setError(null);
    try {
      const saved = await api.saveNtfySettings({
        enabled: ntfy.enabled,
        server_url: ntfy.server_url,
        topic: ntfy.topic,
        token: ntfyToken.trim() ? ntfyToken.trim() : null,
        events: ntfy.events,
      });
      setNtfy(saved);
      setNtfyToken("");
      setMessage("Push notification settings are effective immediately.");
    }
    catch (reason) { setError(reason instanceof Error ? reason.message : "Could not save notification settings"); }
    finally { setBusy(false); }
  };
  const testNtfy = async () => {
    setBusy(true); setError(null);
    try {
      await api.testNtfySettings();
      setMessage("Test notification sent.");
    }
    catch (reason) { setError(reason instanceof Error ? reason.message : "Could not send a test notification"); }
    finally { setBusy(false); }
  };
  const createPairing = async () => {
    setBusy(true); setError(null);
    try { setPairing(await api.createPairingCode(pairingName.trim() || "SM-T585")); }
    catch (reason) { setError(reason instanceof Error ? reason.message : "Could not create pairing code"); }
    finally { setBusy(false); }
  };
  const rollback = async (id: number) => {
    if (!window.confirm(`Republish revision ${id} as a new immutable revision?`)) return;
    setBusy(true); setError(null);
    try { const revision = await api.rollback(id); setMessage(`Revision ${id} republished as revision ${revision.id}.`); await remote.refresh(); }
    catch (reason) { setError(reason instanceof Error ? reason.message : "Rollback failed"); }
    finally { setBusy(false); }
  };
  const activateLegacy = async (id: number) => {
    if (!window.confirm(`Emergency rollback: activate signed legacy revision ${id} on enrolled tablets?`)) return;
    setBusy(true); setError(null);
    try { await api.activateLegacy(id); setMessage(`Legacy revision ${id} selected for cutover rollback.`); await remote.refresh(); }
    catch (reason) { setError(reason instanceof Error ? reason.message : "Legacy rollback failed"); }
    finally { setBusy(false); }
  };
  const revoke = async (id: string) => {
    if (!window.confirm("Revoke this tablet credential? The tablet must be paired again.")) return;
    setBusy(true); setError(null);
    try { await api.revokeDevice(id); setMessage("Tablet credential revoked."); await remote.refresh(); }
    catch (reason) { setError(reason instanceof Error ? reason.message : "Revocation failed"); }
    finally { setBusy(false); }
  };

  return <div className="settings-page">
    {message ? <SuccessMessage>{message}</SuccessMessage> : null}{error ? <ErrorState message={error} /> : null}
    <p className="inspector-note">Dashboard {RELEASE_VERSION} · Source {SOURCE_COMMIT}</p>
    <SessionAuditSettingsCard />
    <div className="settings-grid settings-grid--recording">
      <section className="surface settings-section" id="recording-behavior"><header><div><h2>Recording behavior <span className="draft-label">Signed draft</span></h2><p>Shared by every Record message step. Saving does not activate it; publication creates a signed V4 revision.</p></div><Mic2 size={23} /></header>
        <div className="settings-form-grid"><Field label="Maximum duration" hint="Hard limit: 10–180 seconds. Caller hangup always stops sooner."><input type="number" min={10} max={180} value={draft.recording_behavior.maximum_duration_seconds} onChange={(event) => setDraft({ ...draft, recording_behavior: { ...draft.recording_behavior, maximum_duration_seconds: Number(event.target.value) } })} /></Field><Field label="DTMF finish key" hint="No silence detector is used."><select value={draft.recording_behavior.finish_key ?? ""} onChange={(event) => setDraft({ ...draft, recording_behavior: { ...draft.recording_behavior, finish_key: event.target.value || null } })}><option value="">Disabled</option>{"0123456789*#".split("").map((key) => <option key={key} value={key}>{key}</option>)}</select></Field></div>
        <div className="settings-actions"><Button onClick={() => void saveRecordingBehavior()} disabled={busy || draft.recording_behavior.maximum_duration_seconds < 10 || draft.recording_behavior.maximum_duration_seconds > 180}>Save signed draft settings</Button></div>
      </section>

      <section className="surface settings-section" id="voicemail-retention"><header><div><h2>Recording retention <span className="status-pill status-pill--muted">Effective immediately</span></h2><p>Shared by session audits, voicemail and conversations. Automatic deletion is irreversible, but audit tombstones remain.</p></div><Archive size={23} /></header>
        <div className="settings-form-grid"><Field label="Retention mode"><select value={retention.mode} onChange={(event) => setRetention({ ...retention, mode: event.target.value as RecordingSettings["mode"] })}><option value="manual">Manual deletion only</option><option value="automatic">Automatic deletion</option></select></Field><Field label="Retention days" hint="Used only in automatic mode; range 1–365 days."><input type="number" min={1} max={365} value={retention.days} disabled={retention.mode === "manual"} onChange={(event) => setRetention({ ...retention, days: Number(event.target.value) })} /></Field></div>
        <div className={`recording-capacity ${retention.used_bytes >= retention.quota_bytes || tabletSpoolFull || conversationSpoolFull ? "recording-capacity--critical" : ""}`}><HardDrive size={18} /><div><strong>{formatBytes(retention.used_bytes)} of {formatBytes(retention.quota_bytes)} reserved on server</strong><span>Includes incomplete uploads · {retention.pending_count} pending on server · voicemail spool: {tabletSpoolCount} ({formatBytes(tabletSpoolBytes)}) · conversation spool: {conversationSpoolCount} ({formatBytes(conversationSpoolBytes)})</span></div></div>
        {retention.used_bytes >= retention.quota_bytes ? <ErrorState message="Recording storage is full. New uploads return 507 and remain encrypted on the tablet." /> : null}
        {tabletSpoolFull ? <ErrorState message="The encrypted tablet voicemail spool is full. Record message steps use their unavailable path until space is acknowledged and released." /> : null}
        {conversationSpoolFull ? <ErrorState message="The encrypted tablet conversation spool is full. External call steps fail closed through System failure until space is released." /> : null}
        <div className="settings-actions"><Button onClick={() => void saveRetention()} disabled={busy || retention.days < 1 || retention.days > 365}>Confirm retention policy</Button></div>
      </section>
    </div>

    <NtfySettingsCard
      ntfy={ntfy}
      token={ntfyToken}
      busy={busy}
      onToken={setNtfyToken}
      onChange={setNtfy}
      onSave={() => void saveNtfy()}
      onTest={() => void testNtfy()}
    />

    <section className="surface settings-section"><header><div><h2>Schedules</h2><p>Exceptions override weekly hours. Every unmatched time is closed.</p></div><Button variant="secondary" onClick={addSchedule}><Plus size={17} /> Add schedule</Button></header>
      {draft.schedules.length === 0 ? <div className="settings-empty">No schedules yet. Add one before using a schedule branch in the IVR flow.</div> : draft.schedules.map((schedule, index) => <ScheduleEditor key={schedule.id} schedule={schedule} onChange={(value) => updateSchedule(index, value)} onDelete={() => setDraft({ ...draft, schedules: draft.schedules.filter((_, itemIndex) => itemIndex !== index) })} />)}
      <div className="settings-actions"><Button onClick={() => void saveSchedules()} disabled={busy}>Save schedules</Button></div>
    </section>

    <div className="settings-grid">
      <section className="surface settings-section"><header><div><h2>Tablet enrollment</h2><p>Create a single-use eight-digit code. It expires after ten minutes.</p></div><KeyRound size={23} /></header>
        <Field label="Tablet name"><input value={pairingName} onChange={(event) => setPairingName(event.target.value)} maxLength={120} /></Field>
        <Button onClick={() => void createPairing()} disabled={busy}>Create pairing code</Button>
        {pairing ? <div className="pairing-code"><small>Pairing code</small><strong>{pairing.code.slice(0, 4)} {pairing.code.slice(4)}</strong><span>Expires {formatDate(pairing.expires_at)}</span><Button variant="ghost" onClick={() => void navigator.clipboard.writeText(pairing.code)}><Clipboard size={16} /> Copy</Button></div> : null}
      </section>
      <section className="surface settings-section"><header><div><h2>Enrolled tablet</h2><p>Release one permits one tablet while retaining stable device identifiers.</p></div><Laptop size={23} /></header>
        {remote.data.devices.length === 0 ? <div className="settings-empty">No tablet enrolled.</div> : remote.data.devices.map((device) => {
          const wifi = device.status.boot_wifi_recovery as Record<string, unknown> | undefined;
          const wifiOutcome = typeof wifi?.outcome === "string" ? wifi.outcome.replaceAll("_", " ") : "not reported";
          const wifiTime = typeof wifi?.completed_at === "string" ? formatDate(wifi.completed_at) : "Never";
          const externalReady = isExternalCallReady(device.status);
          const promptBargeInReady = isPromptBargeInReady(device.status);
          return <div className="device-card" key={device.id}><div><strong>{device.display_name}</strong><span>{device.revoked_at ? "Revoked" : `App ${device.app_version} · Helper ${device.helper_version}`}</span><small>Last seen {formatDate(device.last_seen_at)}</small><small>App source: {String(device.status.source_commit ?? "Not reported")}</small><small>Helper source: {String(device.status.helper_source_commit ?? "Not reported")}</small><small>Boot Wi-Fi: {wifiOutcome} · {wifiTime}</small>{device.revoked_at ? null : <><small>External call V4: {externalReady ? "ready" : "not ready — V4 publication is blocked"}</small><small>Prompt interruption: {promptBargeInReady ? "ready" : "not ready — enabled flows cannot publish"}</small></>}</div>{device.revoked_at ? null : <Button variant="danger" onClick={() => void revoke(device.id)} disabled={busy}><ShieldOff size={16} /> Revoke</Button>}</div>;
        })}
      </section>
    </div>

    <section className="surface settings-section"><header><div><h2>Revision history</h2><p>V4 rollback republishes a new signed revision. Immutable V1–V3 revisions remain executable for cutover rollback.</p></div><History size={23} /></header>
      <div className="table-scroll"><table><thead><tr><th>Revision</th><th>Engine</th><th>Published</th><th>By</th><th>Manifest</th><th>Source</th><th></th></tr></thead><tbody>{remote.data.revisions.map((revision, index) => <tr key={revision.id}><td><strong>#{revision.id}</strong>{index === 0 ? <span className="status-pill">Latest</span> : null}</td><td>{revision.schema_version === 4 ? "V4" : <span className="status-pill status-pill--muted">Rollback V{revision.schema_version}</span>}</td><td>{formatDate(revision.published_at)}</td><td>{revision.published_by}</td><td><code>{revision.manifest_sha256.slice(0, 14)}…</code></td><td>{revision.source_revision_id ? `Rollback of #${revision.source_revision_id}` : "Draft publish"}</td><td>{revision.schema_version === 4 ? <Button variant="ghost" onClick={() => void rollback(revision.id)} disabled={busy}><RotateCcw size={16} /> Restore as new</Button> : <Button variant="ghost" onClick={() => void activateLegacy(revision.id)} disabled={busy}><RotateCcw size={16} /> Activate signed</Button>}</td></tr>)}</tbody></table></div>
      {remote.data.revisions.length === 0 ? <div className="settings-empty">No revision has been published.</div> : null}
    </section>

    <section className="surface settings-section"><header><div><h2>Audit history</h2><p>Administrative changes and device enrollment events.</p></div></header>
      <div className="audit-list">{remote.data.audit.length === 0 ? <div className="settings-empty">No audit events yet.</div> : remote.data.audit.slice(0, 100).map((record, index) => <div key={`${record.created_at}-${index}`}><span>{formatDate(record.created_at)}</span><strong>{record.action.replaceAll(".", " ")}</strong><code>{record.target}</code><small>{record.actor}</small></div>)}</div>
    </section>
    <footer className="source-footer">IVRdroid is open source · <a href="/source">View source</a></footer>
  </div>;
}

function NtfySettingsCard({
  ntfy,
  token,
  busy,
  onToken,
  onChange,
  onSave,
  onTest,
}: {
  ntfy: NtfySettings;
  token: string;
  busy: boolean;
  onToken: (value: string) => void;
  onChange: (value: NtfySettings) => void;
  onSave: () => void;
  onTest: () => void;
}) {
  const disabled = busy || !ntfy.enabled;
  const updateEvent = <K extends keyof NtfyEvents>(key: K, patch: Partial<NtfyEvents[K]>) => {
    onChange({ ...ntfy, events: { ...ntfy.events, [key]: { ...ntfy.events[key], ...patch } } });
  };
  return (
    <section className="surface settings-section" id="push-notifications">
      <header>
        <div>
          <h2>Push notifications <span className="status-pill status-pill--muted">Effective immediately</span></h2>
          <p>Server-side ntfy alerts. Public topics are world-readable; use a random topic and an access token. Full phone numbers are never sent.</p>
        </div>
        <Bell size={23} />
      </header>
      <label className="toggle-row">
        <input type="checkbox" checked={ntfy.enabled} disabled={busy} onChange={(event) => onChange({ ...ntfy, enabled: event.target.checked })} />
        <span className="toggle" aria-hidden="true" />
        <span>
          <strong>Enable ntfy</strong>
          <small>{ntfy.enabled ? "On · posts after recordings, failures, and device health land on the server" : "Off · no notifications are sent"}</small>
        </span>
      </label>
      <div className="settings-form-grid">
        <Field label="Server URL" hint="HTTPS only. Default is ntfy.sh; a self-hosted server is safer.">
          <input value={ntfy.server_url} disabled={disabled} onChange={(event) => onChange({ ...ntfy, server_url: event.target.value })} />
        </Field>
        <Field label="Topic" hint="1–64 letters, digits, underscores, or hyphens.">
          <input value={ntfy.topic} disabled={disabled} onChange={(event) => onChange({ ...ntfy, topic: event.target.value })} />
        </Field>
        <Field label="Access token" hint={ntfy.token_configured ? "Saved token is kept if this is left blank." : "Optional. Required for reserved ntfy.sh topics."}>
          <input type="password" autoComplete="new-password" value={token} disabled={disabled} placeholder={ntfy.token_configured ? "Token configured" : ""} onChange={(event) => onToken(event.target.value)} />
        </Field>
      </div>
      <div className="subsection-title"><strong>Events</strong></div>
      <div className="ntfy-events">
        {NTFY_EVENTS.map((item) => {
          const event = ntfy.events[item.key];
          const eventDisabled = disabled || !event.enabled;
          return (
            <article className={`ntfy-event ${event.enabled ? "" : "ntfy-event--off"}`} key={item.key}>
              <label className={`toggle-row toggle-row--compact ${disabled ? "toggle-row--disabled" : ""}`}>
                <input type="checkbox" checked={event.enabled} disabled={disabled} onChange={(change) => updateEvent(item.key, { enabled: change.target.checked } as Partial<NtfyEvents[typeof item.key]>)} />
                <span className="toggle" aria-hidden="true" />
                <span><strong>{item.title}</strong><small>{item.description}</small></span>
              </label>
              <div className="settings-form-grid">
                <Field label="Priority">
                  <select value={event.priority} disabled={eventDisabled} onChange={(change) => updateEvent(item.key, { priority: change.target.value as NtfyPriority } as Partial<NtfyEvents[typeof item.key]>)}>
                    {NTFY_PRIORITIES.map((priority) => <option key={priority} value={priority}>{priority}</option>)}
                  </select>
                </Field>
                {"include_masked_caller" in event ? (
                  <label className="ntfy-flag">
                    <input type="checkbox" checked={event.include_masked_caller} disabled={eventDisabled} onChange={(change) => updateEvent(item.key, { include_masked_caller: change.target.checked } as Partial<NtfyEvents[typeof item.key]>)} />
                    Include masked caller
                  </label>
                ) : <span />}
                {item.key === "external_call_failed" && "not_connected" in event ? (
                  <>
                    <label className="ntfy-flag">
                      <input type="checkbox" checked={event.not_connected} disabled={eventDisabled} onChange={(change) => updateEvent(item.key, { not_connected: change.target.checked } as Partial<NtfyEvents[typeof item.key]>)} />
                      Not connected
                    </label>
                    <label className="ntfy-flag">
                      <input type="checkbox" checked={event.system_failure} disabled={eventDisabled} onChange={(change) => updateEvent(item.key, { system_failure: change.target.checked } as Partial<NtfyEvents[typeof item.key]>)} />
                      System failure
                    </label>
                  </>
                ) : null}
                {item.key === "storage_full" && "warn_at_quota_percent" in event ? (
                  <Field label="Warn at server quota %" hint="Tablet spool caps are appliance limits and are not editable here.">
                    <input type="number" min={50} max={100} value={event.warn_at_quota_percent} disabled={eventDisabled} onChange={(change) => updateEvent(item.key, { warn_at_quota_percent: Number(change.target.value) } as Partial<NtfyEvents[typeof item.key]>)} />
                  </Field>
                ) : null}
                {item.key === "tablet_offline" && "idle_timeout_minutes" in event ? (
                  <>
                    <Field label="Idle timeout (minutes)" hint="Last reported call state was idle.">
                      <input type="number" min={5} max={120} value={event.idle_timeout_minutes} disabled={eventDisabled} onChange={(change) => updateEvent(item.key, { idle_timeout_minutes: Number(change.target.value) } as Partial<NtfyEvents[typeof item.key]>)} />
                    </Field>
                    <Field label="In-call timeout (minutes)" hint="Must be at least the idle timeout.">
                      <input type="number" min={15} max={180} value={event.in_call_timeout_minutes} disabled={eventDisabled} onChange={(change) => updateEvent(item.key, { in_call_timeout_minutes: Number(change.target.value) } as Partial<NtfyEvents[typeof item.key]>)} />
                    </Field>
                    <label className={`toggle-row toggle-row--compact ${eventDisabled ? "toggle-row--disabled" : ""}`}>
                      <input type="checkbox" checked={event.notify_when_recovered} disabled={eventDisabled} onChange={(change) => updateEvent(item.key, { notify_when_recovered: change.target.checked } as Partial<NtfyEvents[typeof item.key]>)} />
                      <span className="toggle" aria-hidden="true" />
                      <span><strong>Notify when recovered</strong><small>One notification after an offline alert, when the tablet syncs again.</small></span>
                    </label>
                  </>
                ) : null}
              </div>
            </article>
          );
        })}
      </div>
      <div className="settings-actions">
        <Button variant="secondary" onClick={onTest} disabled={busy || !ntfy.enabled || !ntfy.topic}>Send test</Button>
        <Button onClick={onSave} disabled={busy}>Save</Button>
      </div>
    </section>
  );
}

function ScheduleEditor({ schedule, onChange, onDelete }: { schedule: Schedule; onChange: (schedule: Schedule) => void; onDelete: () => void }) {
  const updateWindow = (index: number, value: WeeklyWindow) => { const weekly = [...schedule.weekly]; weekly[index] = value; onChange({ ...schedule, weekly }); };
  const updateException = (index: number, value: ScheduleException) => { const exceptions = [...schedule.exceptions]; exceptions[index] = value; onChange({ ...schedule, exceptions }); };
  return <article className="schedule-editor">
    <div className="schedule-editor__head"><Field label="Name"><input value={schedule.name} onChange={(event) => onChange({ ...schedule, name: event.target.value })} /></Field><Field label="Identifier"><input value={schedule.id} readOnly /></Field><Field label="Timezone"><input value={schedule.timezone} onChange={(event) => onChange({ ...schedule, timezone: event.target.value })} placeholder="Asia/Tehran" /></Field><button className="icon-button danger-icon" onClick={onDelete} aria-label={`Delete ${schedule.name}`}><Trash2 size={18} /></button></div>
    <div className="schedule-columns"><div><div className="subsection-title"><strong>Weekly hours</strong><Button variant="ghost" onClick={() => onChange({ ...schedule, weekly: [...schedule.weekly, { weekday: 0, start: "09:00", end: "17:00" }] })}><Plus size={15} /> Add hours</Button></div>{schedule.weekly.map((window, index) => <div className="schedule-row" key={`${window.weekday}-${window.start}-${index}`}><select value={window.weekday} onChange={(event) => updateWindow(index, { ...window, weekday: Number(event.target.value) })}>{DAYS.map((day, dayIndex) => <option key={day} value={dayIndex}>{day}</option>)}</select><input type="time" value={window.start} onChange={(event) => updateWindow(index, { ...window, start: event.target.value })} /><span>to</span><input type="time" value={window.end} onChange={(event) => updateWindow(index, { ...window, end: event.target.value })} /><button className="icon-button" onClick={() => onChange({ ...schedule, weekly: schedule.weekly.filter((_, itemIndex) => itemIndex !== index) })}><Trash2 size={16} /></button></div>)}</div>
      <div><div className="subsection-title"><strong>Exceptions</strong><Button variant="ghost" onClick={() => onChange({ ...schedule, exceptions: [...schedule.exceptions, { date: new Date().toISOString().slice(0, 10), state: "closed" }] })}><Plus size={15} /> Add exception</Button></div>{schedule.exceptions.map((exception, index) => <div className="schedule-row" key={`${exception.date}-${index}`}><input type="date" value={exception.date} onChange={(event) => updateException(index, { ...exception, date: event.target.value })} /><select value={exception.state} onChange={(event) => updateException(index, { ...exception, state: event.target.value as ScheduleException["state"] })}><option value="open">Open</option><option value="closed">Closed</option><option value="holiday">Holiday</option></select><button className="icon-button" onClick={() => onChange({ ...schedule, exceptions: schedule.exceptions.filter((_, itemIndex) => itemIndex !== index) })}><Trash2 size={16} /></button></div>)}</div></div>
  </article>;
}
