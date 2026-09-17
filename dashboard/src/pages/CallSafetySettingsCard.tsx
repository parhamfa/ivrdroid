import { Timer } from "lucide-react";
import { useEffect, useState } from "react";
import { api } from "../api";
import { Button, ErrorState, Field, Loading, SuccessMessage } from "../components/ui";
import { useRemote } from "../hooks";

export function CallSafetySettingsCard() {
  const remote = useRemote(api.callSafetySettings, []);
  const [minutes, setMinutes] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [saved, setSaved] = useState(false);
  const data = remote.data;
  const pending = data?.devices.some((device) => device.applied_version !== data.document.version);
  useEffect(() => {
    if (!pending) return;
    let active = true;
    const timer = window.setInterval(() => {
      void api.callSafetySettings().then((value) => { if (active) remote.setData(value); }).catch(() => {});
    }, 10_000);
    return () => { active = false; window.clearInterval(timer); };
  }, [pending, remote.setData]);
  const value = minutes ?? String(data ? data.document.maximum_call_duration_seconds / 60 : 60);
  const numericValue = Number(value);
  const valid = value.trim() !== "" && Number.isInteger(numericValue) && numericValue >= 1 && numericValue <= 1440;
  async function save() {
    if (!valid) return;
    setBusy(true); setError(null); setSaved(false);
    try { remote.setData(await api.saveCallSafetySettings(numericValue)); setMinutes(null); setSaved(true); }
    catch (reason) { setError(reason instanceof Error ? reason.message : "Could not save call limit."); }
    finally { setBusy(false); }
  }
  return <section className="surface settings-section" id="call-safety">
    <header><div><h2>Call safety</h2><p>End calls left open beyond the configured limit.</p></div><Timer size={23} /></header>
    {remote.error ? <ErrorState message={remote.error} retry={remote.refresh} /> : !data ? <Loading label="Loading call safety settings" /> : <>
      {error ? <ErrorState message={error} /> : null}
      {saved ? <SuccessMessage>Saved. Once applied by the tablet, this limit takes effect for subsequent calls.</SuccessMessage> : null}
      <Field label="Maximum call duration (minutes)" hint="1–1,440 minutes. Starts when the original caller is answered, including menus and transfers. Both call legs end at the limit, without a prompt.">
        <input type="number" aria-label="Maximum call duration (minutes)" min={1} max={1440} step={1} value={value} onChange={(event) => { setMinutes(event.target.value); setSaved(false); }} />
      </Field>
      <div className="audit-policy-state"><span>Desired: <strong>{data.document.maximum_call_duration_seconds / 60} minutes</strong></span>
        {data.devices.map((device) => <div key={device.id}>
          <span>{device.name}: <strong>{device.maximum_call_duration_seconds === null ? "Awaiting acknowledgment" : `${device.maximum_call_duration_seconds / 60} minutes`}</strong></span>
          <span className="status-pill status-pill--muted">{device.capable && device.applied_version === data.document.version ? "Applied" : "Pending synchronization"}</span>
          {!device.capable ? <small>App and helper update required to enforce this setting.</small> : null}
          {device.last_error ? <small>{device.last_error}</small> : null}
        </div>)}
      </div>
      <div className="settings-actions"><Button disabled={busy || !valid} onClick={() => void save()}>Save call limit</Button><small>Calls already answered keep their original limit.</small></div>
    </>}
  </section>;
}
