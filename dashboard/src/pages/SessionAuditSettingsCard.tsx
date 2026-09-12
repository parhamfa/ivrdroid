import { Headphones } from "lucide-react";
import { useEffect, useState } from "react";
import { api } from "../api";
import { Button, ErrorState, Field, Loading, SuccessMessage, formatBytes } from "../components/ui";
import { useRemote } from "../hooks";

export function SessionAuditSettingsCard() {
  const remote = useRemote(api.sessionAuditSettings, []);
  const [enabled, setEnabled] = useState<boolean | null>(null);
  const [quota, setQuota] = useState<number | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [saved, setSaved] = useState(false);
  const data = remote.data;
  const pending = data?.devices.some((device) => device.applied_version !== data.document.version);
  useEffect(() => {
    if (!pending) return;
    let active = true;
    const interval = window.setInterval(() => {
      void api.sessionAuditSettings().then((value) => { if (active) remote.setData(value); }).catch(() => {});
    }, 10_000);
    return () => { active = false; window.clearInterval(interval); };
  }, [pending, remote.setData]);
  const save = async () => {
    if (!data) return;
    setBusy(true); setError(null); setSaved(false);
    try {
      remote.setData(await api.saveSessionAuditSettings(enabled ?? data.document.enabled, (quota ?? data.document.local_quota_bytes / 1024 ** 2) * 1024 ** 2));
      setEnabled(null); setQuota(null); setSaved(true);
    } catch (reason) { setError(reason instanceof Error ? reason.message : "Could not save session recording settings."); }
    finally { setBusy(false); }
  };
  return <section className="surface settings-section" id="session-auditing">
    <header><div><h2>Call auditing</h2><p>Record answered IVR sessions, including prompts, voicemail and operator conversations. Listen from Call history.</p></div><Headphones size={23} /></header>
    {remote.error ? <ErrorState message={remote.error} retry={remote.refresh} /> : !data ? <Loading label="Loading call auditing settings" /> : <>
      {error ? <ErrorState message={error} /> : null}
      {saved ? <SuccessMessage>Settings saved. The tablet applies them to subsequent calls after an idle sync.</SuccessMessage> : null}
      <label className="session-audit-toggle"><input type="checkbox" checked={enabled ?? data.document.enabled} onChange={(event) => { setEnabled(event.target.checked); setSaved(false); }} /><span><strong>Record full IVR sessions</strong><small>Includes built-in fallback sessions. Stock-dialer calls are excluded.</small></span></label>
      <div className="audit-policy-state"><span>Desired: <strong>{data.document.enabled ? "On" : "Off"}</strong></span>{data.devices.map((device) => <div key={device.id}><span>{device.name}: <strong>{device.enabled ? "On" : "Off"}</strong></span><span className="status-pill status-pill--muted">{device.applied_version === data.document.version ? "Applied" : "Pending synchronization"}</span>{!device.capable ? <small>App and helper update required before enabling.</small> : null}{device.last_error ? <small>{device.last_error}</small> : null}</div>)}</div>
      <Field label="Tablet audit storage limit (MiB)" hint="Shared retention applies. Voicemail and operator uploads take priority."><input type="number" min={64} max={4096} step={64} value={quota ?? data.document.local_quota_bytes / 1024 ** 2} onChange={(event) => setQuota(Number(event.target.value))} /></Field>
      <p className="inspector-note">Server audit limit: {formatBytes(data.server_quota_bytes)} within shared recording storage. Tablet audit uploads: {data.devices.reduce((sum, device) => sum + device.spool_count, 0)} pending · {formatBytes(data.devices.reduce((sum, device) => sum + device.spool_bytes, 0))}.</p>
      <div className="settings-actions"><Button disabled={busy || (quota !== null && (quota < 64 || quota > 4096)) || ((enabled ?? data.document.enabled) && (!data.devices.length || data.devices.some((device) => !device.capable)))} onClick={() => void save()}>Save call auditing settings</Button><small>Applies independently of flow publication.</small></div>
    </>}
  </section>;
}
