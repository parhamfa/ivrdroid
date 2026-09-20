import { Check, Download, Headphones, Trash2 } from "lucide-react";
import { useRef, useState } from "react";
import { api } from "../api";
import { Button, Drawer, ErrorState, Loading, formatDate, formatDuration } from "../components/ui";
import { useRemote } from "../hooks";
import type { CallRecord, Recording, SessionAuditEvent } from "../types";
import { callOutcome, finalDisconnect, recordingStatus, recordingProgress, eventDetail, eventTime, sessionAudioStatus, timelineEvents } from "./callAudit";

const eventLabels: Record<SessionAuditEvent["type"], string> = {
  answered: "Call answered", prompt: "Prompt", digit: "Menu input", timeout: "No input",
  invalid: "Invalid input", schedule: "Schedule", return: "Return to menu", voicemail: "Voicemail",
  external_call: "Operator call", ended: "Session ended", gap: "Missing audio",
};

export function CallDetails({ id, autoPlay, onClose, onChange }: {
  id: string; autoPlay: boolean; onClose: () => void; onChange: (value: CallRecord) => void;
}) {
  const remote = useRemote(() => api.call(id), [id]);
  const player = useRef<HTMLAudioElement>(null);
  const seekPending = useRef<number | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const call = remote.data;
  const audit = call?.session_audit;
  const recording = audit?.recording;
  const timeline = timelineEvents(audit?.events ?? []);
  const playable = audit?.state === "ready" && Boolean(recording?.playback_url);

  const update = (value: CallRecord) => { remote.setData(value); onChange(value); };
  const listened = async (value: boolean) => {
    if (!call || !audit || !recording || busy || Boolean(recording.listened_at) === value) return;
    setBusy(true); setError(null);
    try {
      const changed = await api.setRecordingListened(recording.id, value);
      update({ ...call, session_audit: { ...audit, recording: changed } });
    } catch (reason) { setError(reason instanceof Error ? reason.message : "Could not update listened status."); }
    finally { setBusy(false); }
  };
  const remove = async () => {
    if (!call || !recording || !window.confirm("Permanently delete this session audio? Call history and its timeline will remain.")) return;
    setBusy(true); setError(null);
    try {
      await api.deleteRecording(recording.id);
      player.current?.pause();
      update(await api.call(call.id));
    } catch (reason) { setError(reason instanceof Error ? reason.message : "Could not delete session audio."); }
    finally { setBusy(false); }
  };
  const seek = (milliseconds: number) => {
    const audio = player.current;
    if (!audio) return;
    const seconds = milliseconds / 1000;
    seekPending.current = seconds;
    if (audio.readyState >= 1) {
      audio.currentTime = Math.min(seconds, Number.isFinite(audio.duration) ? audio.duration : seconds);
      seekPending.current = null;
    }
  };

  return <Drawer title="Call details" subtitle={call ? formatDate(call.started_at) : undefined} onClose={onClose}>
    {remote.loading && !call ? <Loading label="Loading call details" /> : remote.error ? <ErrorState message={remote.error} retry={remote.refresh} /> : call ? <>
      <section className="session-audio" aria-label="Session audio">
        <div className="session-audio__title"><Headphones size={20} /><h3>Session audio</h3>
          {playable ? <span className="status-pill status-pill--muted">{recording?.listened_at ? "Listened" : "Unlistened"}</span> : null}
        </div>
        {error ? <ErrorState message={error} /> : null}
        {playable && recording ? <>
          {audit?.partial ? <p className="session-audio__partial">Partial recording · {audit.stop_reason.replaceAll("_", " ")}. Available audio ends at {eventTime(audit.duration_ms)}.</p> : null}
          <audio ref={player} aria-label="Full session recording" controls preload="metadata" autoPlay={autoPlay} src={recording.playback_url!}
            onPlay={() => void listened(true)}
            onError={() => setError("Session audio could not be loaded. Refresh the call details to retry.")}
            onLoadedMetadata={() => { if (seekPending.current !== null) seek(seekPending.current * 1000); }} />
          <div className="session-audio__actions">
            {recording.download_url ? <a className="button button--secondary" href={recording.download_url} download><Download size={15} /> Download</a> : null}
            <Button variant="secondary" disabled={busy} onClick={() => void listened(!recording.listened_at)}><Check size={15} /> Mark {recording.listened_at ? "unlistened" : "listened"}</Button>
            <Button variant="ghost" disabled={busy} onClick={() => void remove()} aria-label="Delete session audio"><Trash2 size={16} /></Button>
          </div>
        </> : <p className="inspector-note">{sessionAudioStatus(call)}{audit?.state === "deleted" ? " · Audio was deleted; call history remains." : !audit ? " · This call has no session recording." : ""}</p>}
        {recording && !playable ? <p className="inspector-note">{recordingProgress(recording)}</p> : null}
        {timeline.length ? <>
          <h4>Session timeline</h4>
          <ol className="session-timeline">{timeline.map((event, index) => <li key={`${event.offset_ms}-${index}`}>
            <button type="button" disabled={!playable} onClick={() => seek(event.offset_ms)} aria-label={`Seek to ${eventTime(event.offset_ms)} ${eventLabels[event.type]}`}>
              <time>{eventTime(event.offset_ms)}</time><span>{eventLabels[event.type]}{event.detail ? <small>{eventDetail(event)}</small> : null}</span>
            </button>
          </li>)}</ol>
        </> : <p className="inspector-note">No timestamped events were recorded for this call.</p>}
      </section>
      <dl className="detail-list"><div><dt>Caller</dt><dd>{call.caller ?? call.caller_masked}</dd></div><div><dt>Policy</dt><dd>{call.policy_decision}</dd></div><div><dt>Revision</dt><dd>{call.revision_id ?? "Built-in fallback"}</dd></div><div><dt>Outcome</dt><dd>{callOutcome(call)}</dd></div><div><dt>Final disconnect</dt><dd>{finalDisconnect(call)}</dd></div><div><dt>Resource cleanup</dt><dd>{call.cleanup_status === "pending" ? "Details pending" : call.cleanup_status ?? "Not reported by this app version"}</dd></div>{call.ended_at ? <div><dt>Disconnected</dt><dd>{formatDate(call.ended_at)}</dd></div> : null}<div><dt>Call duration</dt><dd>{formatDuration(call.duration_seconds)}</dd></div></dl>
      <h3>Voicemail &amp; conversations</h3>
      {call.events.some((event) => event.status === "RECORDING_FAILURE") ? <p className="session-audio__partial">A recording failed or ended early. Any surviving audio is listed below; call outcome and final disconnect are reported separately.</p> : null}
      {call.recordings?.filter((item) => item.status !== "deleted").length ? <ul className="call-recordings">{call.recordings.filter((item) => item.status !== "deleted").map((item: Recording) => <li key={item.id}><a href={`/voicemail?recording=${encodeURIComponent(item.id)}`}>{item.kind === "conversation" ? "Operator conversation" : "Voicemail"} · {formatDuration(item.duration_ms / 1000)}</a><span>{recordingStatus(item)}</span></li>)}</ul> : <p className="inspector-note">No voicemail or operator recordings for this call.</p>}
      <details><summary>Execution trace and events</summary><ol className="event-list">{call.menu_path.map((step, index) => <li key={index}>{step.split("|").slice(1).join(" · ") || step}</li>)}</ol><pre className="event-json">{JSON.stringify(call.events, null, 2)}</pre></details>
    </> : null}
  </Drawer>;
}
