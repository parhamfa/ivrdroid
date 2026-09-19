import { Clock3 } from "lucide-react";
import { useEffect, useMemo, useState } from "react";
import { Button, ErrorState, Field, SuccessMessage } from "../components/ui";
import { buildTimeZoneOptions, createDateFormatter } from "../display";
import { useDisplaySettings } from "../displaySettings";
import type { DisplaySettings } from "../types";

export function DisplaySettingsCard() {
  const { settings, loading, saving, save } = useDisplaySettings();
  const [edited, setEdited] = useState<DisplaySettings | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [saved, setSaved] = useState(false);
  const [now, setNow] = useState(() => new Date());
  const selection = edited ?? settings;
  const zones = useMemo(() => buildTimeZoneOptions(selection.timezone, now), [selection.timezone, now]);
  const formatPreview = useMemo(() => createDateFormatter(selection), [selection]);
  const dirty = selection.timezone !== settings.timezone || selection.date_calendar !== settings.date_calendar;
  useEffect(() => {
    const timer = window.setInterval(() => setNow(new Date()), 60_000);
    return () => window.clearInterval(timer);
  }, []);

  const change = (value: Partial<DisplaySettings>) => {
    const next = { ...selection, ...value };
    setEdited(next.timezone === settings.timezone && next.date_calendar === settings.date_calendar ? null : next);
    setSaved(false); setError(null);
  };
  const submit = async () => {
    setError(null); setSaved(false);
    try { await save(selection); setEdited(null); setSaved(true); }
    catch (reason) { setError(reason instanceof Error ? reason.message : "Could not save display settings."); }
  };

  return <section className="surface settings-section" aria-labelledby="display-settings-title">
    <header><div><h2 id="display-settings-title">Display</h2><p>Shared by all administrators. Saved preferences apply immediately to dates and timestamps across the panel.</p></div><Clock3 size={23} /></header>
    <div className="settings-form-grid">
      <Field label="Display timezone" hint="UTC offsets reflect the current date; daylight saving is handled for each timestamp.">
        <select value={selection.timezone} disabled={loading || saving} onChange={(event) => change({ timezone: event.target.value })}>
          {zones.map((zone) => <option key={zone.value} value={zone.value}>{zone.label}</option>)}
        </select>
      </Field>
      <Field label="Calendar" hint="Dates use English month names, Latin digits, and 24-hour time.">
        <select value={selection.date_calendar} disabled={loading || saving} onChange={(event) => change({ date_calendar: event.target.value as DisplaySettings["date_calendar"] })}>
          <option value="gregorian">Gregorian</option><option value="persian">Persian / Jalali</option>
        </select>
      </Field>
    </div>
    <div className="display-preview"><span>Preview</span><output aria-label="Date and time preview">{loading ? "Loading preferences…" : formatPreview(now.toISOString())}</output><small>{selection.timezone}</small></div>
    <p className="inspector-note">IVR schedules keep their own timezone and calendar rules.</p>
    {error ? <ErrorState message={error} retry={() => void submit()} /> : null}
    {saved && !dirty ? <SuccessMessage>Display settings saved for all administrators.</SuccessMessage> : null}
    <div className="settings-actions"><Button disabled={loading || saving || !dirty} onClick={() => void submit()}>{saving ? "Saving…" : "Save display settings"}</Button></div>
  </section>;
}
