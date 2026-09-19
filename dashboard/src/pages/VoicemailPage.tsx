import { Check, Download, Filter, Inbox, PhoneCall, Trash2 } from "lucide-react";
import { useEffect, useState } from "react";
import { api } from "../api";
import { Button, Drawer, EmptyState, ErrorState, Loading, SuccessMessage, formatDuration } from "../components/ui";
import { useDateFormatter } from "../displaySettings";
import { useRemote } from "../hooks";
import type { Recording } from "../types";
import { recordingProgress, recordingStatus } from "./callAudit";

function stopReason(reason: Recording["stop_reason"]): string {
  return {
    session_complete: "Session complete", preempted: "Preempted", capture_failure: "Capture failure", storage_full: "Storage full", interrupted: "Interrupted", writer_failure: "Recording failure",
    finish_key: "Finish key",
    maximum_duration: "Time limit",
    caller_hangup: "Caller hung up",
    operator_hangup: "Operator disconnected",
    recording_failure: "Recording failure",
    buffer_overrun: "Audio buffer overrun",
    max_call_duration: "Maximum call duration",
    completed: "Conversation complete",
  }[reason];
}

function recordingKind(recording: Recording): "voicemail" | "conversation" | "session_audit" {
  return recording.kind ?? "voicemail";
}

export function VoicemailPage() {
  const formatDate = useDateFormatter();
  const [unreadOnly, setUnreadOnly] = useState(true);
  const [page, setPage] = useState(1);
  const [selected, setSelected] = useState<Recording | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [message, setMessage] = useState<string | null>(null);
  const remote = useRemote(() => api.recordings(unreadOnly, page), [unreadOnly, page]);

  useEffect(() => {
    if (!remote.data) return;
    const requested = new URLSearchParams(window.location.search).get("recording");
    if (requested) {
      const found = remote.data.items.find((item) => item.id === requested);
      if (found) {
        setSelected(found);
        return;
      }
      if (unreadOnly) {
        setUnreadOnly(false);
        setPage(1);
        return;
      }
    }
    if (!selected) return;
    const current = remote.data.items.find((item) => item.id === selected.id);
    if (current) setSelected(current);
  }, [remote.data, unreadOnly, selected?.id]);

  const updateListened = async (recording: Recording, listened: boolean) => {
    if (busy || Boolean(recording.listened_at) === listened) return;
    setBusy(true); setError(null);
    try {
      const updated = await api.setRecordingListened(recording.id, listened);
      setSelected((current) => current?.id === updated.id ? updated : current);
      if (unreadOnly) {
        await remote.refresh();
      } else {
        remote.setData((current) => current ? {
          ...current,
          items: current.items.map((item) => item.id === updated.id ? updated : item),
        } : current);
      }
      setMessage(listened ? "Recording marked listened." : "Recording marked unlistened.");
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : "Could not update recording");
    } finally {
      setBusy(false);
    }
  };

  const remove = async (recording: Recording) => {
    if (!window.confirm(`Permanently delete this ${recordingKind(recording)} audio? The audit tombstone will remain.`)) return;
    setBusy(true); setError(null);
    try {
      await api.deleteRecording(recording.id);
      setSelected(null);
      setMessage("Recording audio deleted.");
      await remote.refresh();
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : "Could not delete recording");
    } finally {
      setBusy(false);
    }
  };

  if (remote.loading && !remote.data) return <Loading label="Loading recordings" />;
  if (remote.error && !remote.data) return <ErrorState message={remote.error} retry={remote.refresh} />;

  const data = remote.data;
  const pageCount = Math.max(1, Math.ceil((data?.total ?? 0) / (data?.page_size ?? 50)));
  return (
    <div className={`split-page ${selected ? "split-page--open" : ""}`}>
      <div className="split-page__main voicemail-page">
        {message ? <SuccessMessage>{message}</SuccessMessage> : null}
        {error || remote.error ? <ErrorState message={error ?? remote.error ?? "Recordings unavailable"} retry={remote.refresh} /> : null}
        <section className="surface calls-table">
          <div className="section-toolbar">
            <h2>Recordings</h2>
            <span>{data?.total ?? 0} recording{data?.total === 1 ? "" : "s"}</span>
            <label className="compact-select"><Filter size={17} /><span className="sr-only">Inbox filter</span><select aria-label="Inbox filter" value={unreadOnly ? "unlistened" : "all"} onChange={(event) => { setUnreadOnly(event.target.value === "unlistened"); setPage(1); }}><option value="unlistened">Unlistened</option><option value="all">All messages</option></select></label>
          </div>
          {!data?.items.length ? <EmptyState title={unreadOnly ? "No unlistened recordings" : "No recordings yet"}>{unreadOnly ? "All available recordings have been listened to." : "Voicemail and completed external-call conversations appear after their encrypted uploads are assembled."}</EmptyState> : (
            <div className="table-scroll"><table><thead><tr><th>Type</th><th>Caller</th><th>Operator</th><th>Time</th><th>Duration</th><th>Stopped by</th><th>Status</th><th></th></tr></thead><tbody>{data.items.map((recording) => (
              <tr key={recording.id} className={!recording.listened_at ? "voicemail-row--unread" : ""}><td><span className={`status-pill ${recordingKind(recording) === "conversation" ? "" : "status-pill--muted"}`}>{recordingKind(recording) === "conversation" ? "Conversation" : "Voicemail"}</span></td><td><button type="button" className="table-link" onClick={() => setSelected(recording)}>{recording.caller ?? recording.caller_masked}</button></td><td>{recording.operator_masked ?? "—"}</td><td>{formatDate(recording.captured_at)}</td><td>{formatDuration(Math.ceil(recording.duration_ms / 1000))}</td><td>{stopReason(recording.stop_reason)}</td><td><span className={`status-pill ${recording.status === "ready" ? "" : "status-pill--muted"}`}>{recording.partial || recording.status !== "ready" ? recordingStatus(recording) : (recording.listened_at ? "Listened" : "New")}</span></td><td className="row-action"><Button variant="ghost" onClick={() => setSelected(recording)}>Open</Button></td></tr>
            ))}</tbody></table></div>
          )}
          {pageCount > 1 ? <div className="pagination"><Button variant="secondary" disabled={page <= 1 || remote.loading} onClick={() => setPage((current) => current - 1)}>Previous</Button><span>Page {page} of {pageCount}</span><Button variant="secondary" disabled={page >= pageCount || remote.loading} onClick={() => setPage((current) => current + 1)}>Next</Button></div> : null}
        </section>
      </div>

      {selected ? <Drawer title={recordingKind(selected) === "conversation" ? "Conversation" : "Voicemail"} subtitle={formatDate(selected.captured_at)} onClose={() => setSelected(null)} footer={<><Button variant="danger" onClick={() => void remove(selected)} disabled={busy || selected.status !== "ready"}><Trash2 size={16} /> Delete</Button><Button variant="secondary" onClick={() => void updateListened(selected, !selected.listened_at)} disabled={busy || selected.status !== "ready"}><Check size={16} /> Mark {selected.listened_at ? "unlistened" : "listened"}</Button></>}>
        <dl className="detail-list"><div><dt>Type</dt><dd>{recordingKind(selected) === "conversation" ? "Conversation" : "Voicemail"}</dd></div><div><dt>Caller</dt><dd>{selected.caller ?? selected.caller_masked}</dd></div>{recordingKind(selected) === "conversation" ? <div><dt>Operator</dt><dd>{selected.operator_masked ?? "Unknown"}</dd></div> : null}<div><dt>Duration</dt><dd>{formatDuration(Math.ceil(selected.duration_ms / 1000))}</dd></div><div><dt>Stopped by</dt><dd>{stopReason(selected.stop_reason)}</dd></div><div><dt>Revision</dt><dd>#{selected.revision_id}</dd></div><div><dt>Status</dt><dd>{recordingStatus(selected)}</dd></div></dl>
        {selected.partial ? <p className="session-audio__partial">Partial recording. Only the verified surviving audio is available; some of the conversation is missing.</p> : null}
        {selected.playback_url ? <section className="voicemail-player"><h3>{recordingKind(selected) === "conversation" ? "Conversation" : "Message"}</h3><audio aria-label={`${recordingKind(selected) === "conversation" ? "Conversation" : "Voicemail"} from ${selected.caller ?? selected.caller_masked}`} controls preload="metadata" src={selected.playback_url} onPlay={() => void updateListened(selected, true)} /><a className="button button--secondary" href={selected.download_url ?? selected.playback_url} download><Download size={16} /> Download MP3</a></section> : <div className="voicemail-pending"><Inbox size={21} /><p>{recordingProgress(selected)}</p></div>}
        <a className="call-trace-link" href={`/calls?call=${encodeURIComponent(selected.call_id)}`}><PhoneCall size={16} /> Open call trace</a>
      </Drawer> : null}
    </div>
  );
}
