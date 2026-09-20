import { Download, Filter, Headphones, Search } from "lucide-react";
import { useMemo, useState } from "react";
import { api } from "../api";
import { Button, EmptyState, ErrorState, Loading, formatDate, formatDuration } from "../components/ui";
import { useRemote } from "../hooks";
import type { CallRecord } from "../types";
import { CallDetails } from "./CallDetails";
import { audioMatches, callOutcome, sessionAudioStatus, pendingSessionAudio } from "./callAudit";

function csvCell(value: unknown): string {
  return `"${String(value ?? "").replaceAll('"', '""')}"`;
}

function describeTrace(value: string): { blockId?: string; label: string; event?: string } {
  const [blockId, label, event] = value.split("|");
  if (/^[0-9a-f-]{36}$/.test(blockId) && label) return { blockId, label, event };
  return { label: value };
}

function traceText(value: string): string {
  const trace = describeTrace(value);
  return trace.event ? `${trace.label} · ${trace.event}` : trace.label;
}

export function CallsPage() {
  const remote = useRemote(api.calls, []);
  const [search, setSearch] = useState("");
  const [result, setResult] = useState("all");
  const [selected, setSelected] = useState<string | null>(() => new URLSearchParams(window.location.search).get("call"));
  const [autoPlay, setAutoPlay] = useState(false);
  const [audioFilter, setAudioFilter] = useState("all");
  const open = (id: string | null, play = false) => {
    setSelected(id); setAutoPlay(play);
    const url = new URL(window.location.href);
    if (id) url.searchParams.set("call", id); else url.searchParams.delete("call");
    window.history.replaceState(null, "", url);
  };
  const updateCall = (changed: CallRecord) => remote.setData((current) => current?.map((call) => call.id === changed.id ? changed : call) ?? current);

  const calls = useMemo(() => {
    const needle = search.trim().toLowerCase();
    return (remote.data ?? []).filter((call) => {
      const matchesResult = result === "all" || callOutcome(call) === result;
      const haystack = [call.caller ?? "", call.caller_masked, call.policy_decision, call.result, callOutcome(call), ...call.menu_path].join(" ").toLowerCase();
      return matchesResult && audioMatches(call, audioFilter) && (!needle || haystack.includes(needle));
    });
  }, [remote.data, result, search, audioFilter]);

  const exportCsv = () => {
    const rows = [
      ["Started", "Caller", "Policy", "Revision", "Menu path", "Outcome", "Final result", "Duration seconds", "Session audio", "Session audio listened"],
      ...calls.map((call) => [call.started_at, call.caller ?? "", call.policy_decision, call.revision_id ?? "", call.menu_path.map(traceText).join(" > "), callOutcome(call), call.result, call.duration_seconds, sessionAudioStatus(call), call.session_audit?.recording?.listened_at ?? ""]),
    ];
    const blob = new Blob([rows.map((row) => row.map(csvCell).join(",")).join("\n")], { type: "text/csv;charset=utf-8" });
    const url = URL.createObjectURL(blob);
    const anchor = document.createElement("a");
    anchor.href = url;
    anchor.download = `ivrdroid-calls-${new Date().toISOString().slice(0, 10)}.csv`;
    anchor.click();
    URL.revokeObjectURL(url);
  };

  if (remote.loading) return <Loading label="Loading call history" />;
  if (remote.error) return <ErrorState message={remote.error} retry={remote.refresh} />;
  const results = Array.from(new Set((remote.data ?? []).map(callOutcome)));

  return (
    <div className={`split-page ${selected ? "split-page--open" : ""}`}>
      <div className="split-page__main calls-page">
        <section className="call-summary">
          <div><small>Total calls</small><strong>{remote.data?.length ?? 0}</strong></div>
          <div><small>IVR handled</small><strong>{remote.data?.filter((call) => call.policy_decision === "IVR_HANDLED").length ?? 0}</strong></div>
          <div><small>Stock dialer</small><strong>{remote.data?.filter((call) => call.policy_decision !== "IVR_HANDLED").length ?? 0}</strong></div>
          <div><small>Recordings</small><strong>{remote.data?.reduce((total, call) => total + call.recording_count, 0) ?? 0}</strong><span>{remote.data?.reduce((total, call) => total + call.pending_recording_count, 0) ?? 0} pending recordings · {remote.data?.reduce((total, call) => total + pendingSessionAudio(call), 0) ?? 0} pending session audio</span></div>
        </section>
        <section className="surface calls-table">
          <div className="section-toolbar calls-toolbar">
            <h2>Call history</h2>
            <label className="search-input"><Search size={19} /><input value={search} onChange={(event) => setSearch(event.target.value)} placeholder="Search calls" /></label>
            <label className="compact-select call-outcome-filter"><Filter size={17} /><select aria-label="Call outcome filter" value={result} onChange={(event) => setResult(event.target.value)}><option value="all">All outcomes</option>{results.map((item) => <option key={item}>{item}</option>)}</select></label>
            <label className="compact-select"><Headphones size={17} /><select aria-label="Session audio filter" value={audioFilter} onChange={(event) => setAudioFilter(event.target.value)}><option value="all">All session audio</option><option value="unlistened">Unlistened</option><option value="ready">Ready to listen</option><option value="partial">Partial</option><option value="Pending upload">Pending upload</option><option value="Processing audio">Processing audio</option><option value="Processing needs attention">Processing needs attention</option><option value="Upload needs attention">Upload needs attention</option><option value="Recording needs review">Recording needs review</option><option value="Recording failed">Recording failed</option><option value="Unavailable">Unavailable</option><option value="Not recorded">Not recorded</option></select></label>
            <Button variant="secondary" onClick={exportCsv} disabled={!calls.length}><Download size={17} /> Export CSV</Button>
          </div>
          {calls.length === 0 ? <EmptyState title="No matching calls">Call metadata appears here after the tablet uploads acknowledged events.</EmptyState> : (
            <div className="table-scroll"><table><thead><tr><th>Time</th><th>Caller</th><th>Policy</th><th>Revision</th><th>Menu path</th><th>Session audio</th><th>Recordings</th><th>Outcome</th><th>Duration</th></tr></thead><tbody>{calls.map((call) => (
              <tr key={call.id} className="clickable-row" onClick={() => open(call.id)}><td>{formatDate(call.started_at)}</td><td>{call.caller_masked}</td><td>{call.policy_decision}</td><td>{call.revision_id ?? "Built-in"}</td><td>{call.menu_path.map(traceText).join(" › ") || "—"}</td><td className="session-audio-cell">{sessionAudioStatus(call).includes("Listen") ? <Button variant="secondary" aria-label={`Listen to session from ${call.caller_masked} at ${formatDate(call.started_at)}`} onClick={(event) => { event.stopPropagation(); open(call.id, true); }}><Headphones size={15} />{sessionAudioStatus(call)}{!call.session_audit?.recording?.listened_at ? <span className="unlistened-dot" title="Unlistened" /> : null}</Button> : <span>{sessionAudioStatus(call)}</span>}</td><td>{call.recording_count ? `${call.recording_count} ready` : call.pending_recording_count ? `${call.pending_recording_count} pending` : "—"}</td><td className="call-outcome-cell">{callOutcome(call)}</td><td>{formatDuration(call.duration_seconds)}</td></tr>
            ))}</tbody></table></div>
          )}
        </section>
      </div>

      {selected ? <CallDetails key={selected} id={selected} autoPlay={autoPlay} onClose={() => open(null)} onChange={updateCall} /> : null}
    </div>
  );
}
