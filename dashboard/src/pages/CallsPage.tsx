import { Download, Filter, Search } from "lucide-react";
import { useEffect, useMemo, useState } from "react";
import { api } from "../api";
import { Button, Drawer, EmptyState, ErrorState, Loading, formatDate, formatDuration } from "../components/ui";
import { useRemote } from "../hooks";
import type { CallRecord } from "../types";

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
  const [selected, setSelected] = useState<CallRecord | null>(null);

  useEffect(() => {
    if (!remote.data) return;
    const requested = new URLSearchParams(window.location.search).get("call");
    if (requested) setSelected(remote.data.find((call) => call.id === requested) ?? null);
  }, [remote.data]);

  const calls = useMemo(() => {
    const needle = search.trim().toLowerCase();
    return (remote.data ?? []).filter((call) => {
      const matchesResult = result === "all" || call.result === result;
      const haystack = [call.caller ?? "", call.caller_masked, call.policy_decision, call.result, ...call.menu_path].join(" ").toLowerCase();
      return matchesResult && (!needle || haystack.includes(needle));
    });
  }, [remote.data, result, search]);

  const exportCsv = () => {
    const rows = [
      ["Started", "Caller", "Policy", "Revision", "Menu path", "Result", "Duration seconds"],
      ...calls.map((call) => [call.started_at, call.caller ?? "", call.policy_decision, call.revision_id ?? "", call.menu_path.map(traceText).join(" > "), call.result, call.duration_seconds]),
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
  const results = Array.from(new Set((remote.data ?? []).map((call) => call.result)));

  return (
    <div className={`split-page ${selected ? "split-page--open" : ""}`}>
      <div className="split-page__main calls-page">
        <section className="call-summary">
          <div><small>Total calls</small><strong>{remote.data?.length ?? 0}</strong></div>
          <div><small>IVR handled</small><strong>{remote.data?.filter((call) => call.policy_decision === "IVR_HANDLED").length ?? 0}</strong></div>
          <div><small>Stock dialer</small><strong>{remote.data?.filter((call) => call.policy_decision !== "IVR_HANDLED").length ?? 0}</strong></div>
          <div><small>Recordings</small><strong>{remote.data?.reduce((total, call) => total + call.recording_count, 0) ?? 0}</strong><span>{remote.data?.reduce((total, call) => total + call.pending_recording_count, 0) ?? 0} pending upload</span></div>
        </section>
        <section className="surface calls-table">
          <div className="section-toolbar">
            <h2>Call history</h2>
            <label className="search-input"><Search size={19} /><input value={search} onChange={(event) => setSearch(event.target.value)} placeholder="Search calls" /></label>
            <label className="compact-select"><Filter size={17} /><select value={result} onChange={(event) => setResult(event.target.value)}><option value="all">All results</option>{results.map((item) => <option key={item}>{item}</option>)}</select></label>
            <Button variant="secondary" onClick={exportCsv} disabled={!calls.length}><Download size={17} /> Export CSV</Button>
          </div>
          {calls.length === 0 ? <EmptyState title="No matching calls">Call metadata appears here after the tablet uploads acknowledged events.</EmptyState> : (
            <div className="table-scroll"><table><thead><tr><th>Time</th><th>Caller</th><th>Policy</th><th>Revision</th><th>Menu path</th><th>Recordings</th><th>Result</th><th>Duration</th></tr></thead><tbody>{calls.map((call) => (
              <tr key={call.id} className="clickable-row" onClick={() => setSelected(call)}><td>{formatDate(call.started_at)}</td><td>{call.caller_masked}</td><td>{call.policy_decision}</td><td>{call.revision_id ?? "Built-in"}</td><td>{call.menu_path.map(traceText).join(" › ") || "—"}</td><td>{call.recording_count ? `${call.recording_count} ready` : call.pending_recording_count ? `${call.pending_recording_count} pending` : "—"}</td><td>{call.result}</td><td>{formatDuration(call.duration_seconds)}</td></tr>
            ))}</tbody></table></div>
          )}
        </section>
      </div>

      {selected ? <Drawer title="Call details" subtitle={formatDate(selected.started_at)} onClose={() => setSelected(null)}>
        <dl className="detail-list"><div><dt>Caller</dt><dd>{selected.caller ?? selected.caller_masked}</dd></div><div><dt>Policy decision</dt><dd>{selected.policy_decision}</dd></div><div><dt>Revision</dt><dd>{selected.revision_id ?? "Built-in fallback"}</dd></div><div><dt>Result</dt><dd>{selected.result}</dd></div><div><dt>Duration</dt><dd>{formatDuration(selected.duration_seconds)}</dd></div><div><dt>Recordings</dt><dd>{selected.recording_count ? `${selected.recording_count} available` : selected.pending_recording_count ? `${selected.pending_recording_count} pending upload` : "None"}</dd></div><div><dt>Device</dt><dd>{selected.device_id}</dd></div></dl>
        <h3>Execution trace</h3><ol className="event-list">{selected.menu_path.length ? selected.menu_path.map((step, index) => { const trace = describeTrace(step); return <li key={`${step}-${index}`}><strong>{trace.label}</strong>{trace.event ? <span> · {trace.event}</span> : null}{trace.blockId ? <code>{trace.blockId}</code> : null}</li>; }) : <li>No IVR steps reported</li>}</ol>
        <h3>Events</h3><pre className="event-json">{JSON.stringify(selected.events, null, 2)}</pre>
        {selected.recording_count || selected.pending_recording_count ? <p className="inspector-note"><a href="/voicemail">Open recordings</a> to play or manage audio from this call.</p> : <p className="inspector-note">No voicemail or external-call conversation produced audio for this call.</p>}
      </Drawer> : null}
    </div>
  );
}
