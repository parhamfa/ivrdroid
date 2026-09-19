import {
  Bot,
  Check,
  Database,
  FileCheck2,
  HardDrive,
  Phone,
  ShieldCheck,
} from "lucide-react";
import { api } from "../api";
import { useRemote } from "../hooks";
import type { Device } from "../types";
import { EmptyState, ErrorState, Loading, formatBytes, formatDuration } from "../components/ui";
import { useDateFormatter } from "../displaySettings";

export function OverviewPage({ device }: { device: Device | null }) {
  const formatDate = useDateFormatter();
  const remote = useRemote(async () => {
    const [overview, calls, revisions] = await Promise.all([api.overview(), api.calls(), api.revisions()]);
    return { overview, calls: calls.slice(0, 5), revisions };
  }, []);

  if (remote.loading) return <Loading label="Loading dashboard" />;
  if (remote.error || !remote.data) return <ErrorState message={remote.error ?? "Dashboard unavailable"} retry={remote.refresh} />;

  const { overview, calls, revisions } = remote.data;
  const activeRevision = device?.active_revision_id ?? overview.active_revision;
  const desiredRevision = device?.desired_revision_id;
  const helperState = String(device?.status?.helper_state ?? "Not reported");
  const audioMode = String(device?.status?.audio_mode ?? "Not reported");
  const policyLabel = {
    IVR_DISABLED: "IVR disabled",
    ALLOWLIST_ONLY: "Allowlist only",
    ACCEPT_ALL: "Accept all",
    ACCEPT_ALL_EXCEPT_BLOCKLIST: "All except exclusion list",
  }[overview.caller_policy.mode];
  const listCount = overview.caller_policy.mode === "ACCEPT_ALL_EXCEPT_BLOCKLIST"
    ? overview.caller_policy.blocklist.length
    : overview.caller_policy.allowlist.length;

  const deployment = [
    { label: "Draft", detail: "Current configuration is editable", done: true },
    { label: "Published", detail: revisions[0] ? `Revision ${revisions[0].id} published` : "No revision published", done: revisions.length > 0 },
    { label: "Downloaded", detail: desiredRevision ? `Revision ${desiredRevision} assigned to tablet` : "Waiting for first assignment", done: desiredRevision != null },
    { label: "Activated", detail: activeRevision ? `Revision ${activeRevision} active` : "Tablet has not activated a revision", done: activeRevision != null },
    { label: "Acknowledged", detail: desiredRevision && activeRevision === desiredRevision ? "Tablet acknowledged the desired revision" : "Waiting for tablet acknowledgement", done: desiredRevision != null && activeRevision === desiredRevision },
  ];

  return (
    <div className="overview-page">
      <section className="health-band" aria-label="Device health">
        <div><FileCheck2 /><span><small>Active revision</small><strong>{activeRevision ?? "—"}</strong></span></div>
        <div><Bot /><span><small>Helper</small><strong className={helperState === "READY" ? "text-success" : ""}>{helperState}</strong></span></div>
        <div><Database /><span><small>Audio mode</small><strong>{audioMode}</strong></span></div>
        <div><HardDrive /><span><small>Prompt storage</small><strong>{formatBytes(overview.prompt_bytes)} of {formatBytes(overview.prompt_quota_bytes)}</strong><small>{overview.recording_count} recordings · {overview.pending_recording_count} pending</small></span></div>
      </section>

      <div className="overview-grid">
        <section className="surface deployment-panel">
          <h2>Deployment status</h2>
          <ol className="deployment-list">
            {deployment.map((item) => (
              <li key={item.label} className={item.done ? "complete" : "pending"}>
                <span className="deployment-dot">{item.done ? <Check size={15} /> : null}</span>
                <div><strong>{item.label}</strong><small>{item.detail}</small></div>
                <time>{item.done && revisions[0] ? formatDate(revisions[0].published_at) : "—"}</time>
              </li>
            ))}
          </ol>
        </section>

        <section className="surface policy-summary">
          <h2>Caller policy</h2>
          <div><ShieldCheck /><span>{policyLabel}</span></div>
          <div><Phone /><span>{listCount} {overview.caller_policy.mode === "ACCEPT_ALL_EXCEPT_BLOCKLIST" ? "excluded" : "allowed"} numbers</span></div>
          <div><Phone /><span>{overview.caller_policy.route_unknown_callers ? "Unknown callers reach IVR" : "Unknown callers use stock dialer"}</span></div>
        </section>
      </div>

      <section className="surface recent-calls">
        <h2>Recent calls</h2>
        {calls.length === 0 ? (
          <EmptyState title="No calls reported">Call metadata will appear after the enrolled tablet completes its first synchronized call.</EmptyState>
        ) : (
          <div className="table-scroll">
            <table>
              <thead><tr><th>Time</th><th>Caller</th><th>Policy</th><th>Path</th><th>Result</th><th>Duration</th></tr></thead>
              <tbody>
                {calls.map((call) => (
                  <tr key={call.id}>
                    <td>{formatDate(call.started_at)}</td>
                    <td>{call.caller_masked}</td>
                    <td>{call.policy_decision}</td>
                    <td>{call.menu_path.join(" › ") || "—"}</td>
                    <td className={call.result === "Completed" ? "text-success" : ""}>{call.result}</td>
                    <td>{formatDuration(call.duration_seconds)}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </section>
    </div>
  );
}
