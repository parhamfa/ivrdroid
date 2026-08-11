import { useEffect, useState, type ReactNode } from "react";
import {
  ChevronLeft,
  Clock3,
  FileAudio,
  Workflow,
  Home,
  PhoneCall,
  Voicemail,
  RefreshCw,
  Settings,
  ShieldCheck,
  Smartphone,
} from "lucide-react";
import type { Device } from "../types";

export type PageKey = "overview" | "caller-policy" | "ivr-flow" | "prompts" | "calls" | "voicemail" | "settings";

const NAVIGATION: Array<{ key: PageKey; label: string; icon: typeof Home; path: string }> = [
  { key: "overview", label: "Overview", icon: Home, path: "/" },
  { key: "caller-policy", label: "Caller Policy", icon: ShieldCheck, path: "/caller-policy" },
  { key: "ivr-flow", label: "IVR Flow", icon: Workflow, path: "/ivr-flow" },
  { key: "prompts", label: "Prompts", icon: FileAudio, path: "/prompts" },
  { key: "calls", label: "Calls", icon: PhoneCall, path: "/calls" },
  { key: "voicemail", label: "Recordings", icon: Voicemail, path: "/voicemail" },
  { key: "settings", label: "Settings", icon: Settings, path: "/settings" },
];

const SIDEBAR_STORAGE_KEY = "ivrdroid.sidebar.collapsed.v1";
const COMPACT_SIDEBAR_QUERY = "(max-width: 1120px)";

function storedSidebarPreference(): boolean | null {
  try {
    const stored = window.localStorage.getItem(SIDEBAR_STORAGE_KEY);
    if (stored === "true") return true;
    if (stored === "false") return false;
  } catch {
    // Storage can be unavailable in hardened browser modes; the control still works in memory.
  }
  return null;
}

function compactViewport(): boolean {
  return typeof window.matchMedia === "function" && window.matchMedia(COMPACT_SIDEBAR_QUERY).matches;
}

export function AppShell({
  page,
  title,
  draft,
  device,
  action,
  onNavigate,
  children,
}: {
  page: PageKey;
  title: string;
  draft?: boolean;
  device: Device | null;
  action?: ReactNode;
  onNavigate: (key: PageKey, path: string) => void;
  children: ReactNode;
}) {
  const [sidebarPreference, setSidebarPreference] = useState<boolean | null>(storedSidebarPreference);
  const [viewportIsCompact, setViewportIsCompact] = useState(compactViewport);
  const sidebarCollapsed = sidebarPreference ?? viewportIsCompact;
  const online = device?.last_seen_at
    ? Date.now() - new Date(device.last_seen_at).getTime() < 3 * 60_000
    : false;
  const syncText = device?.last_seen_at
    ? new Intl.RelativeTimeFormat(undefined, { numeric: "auto" }).format(
        -Math.max(1, Math.round((Date.now() - new Date(device.last_seen_at).getTime()) / 60_000)),
        "minute",
      )
    : "Never synced";

  useEffect(() => {
    if (typeof window.matchMedia !== "function") return;
    const query = window.matchMedia(COMPACT_SIDEBAR_QUERY);
    const onChange = (event: MediaQueryListEvent) => setViewportIsCompact(event.matches);
    query.addEventListener("change", onChange);
    return () => query.removeEventListener("change", onChange);
  }, []);

  useEffect(() => {
    if (sidebarPreference === null) return;
    try {
      window.localStorage.setItem(SIDEBAR_STORAGE_KEY, String(sidebarPreference));
    } catch {
      // Storage can be unavailable in hardened browser modes; the control still works in memory.
    }
  }, [sidebarPreference]);

  const toggleSidebar = () => {
    setSidebarPreference((current) => !(current ?? viewportIsCompact));
  };

  return (
    <div className={`app-shell ${sidebarCollapsed ? "app-shell--sidebar-collapsed" : ""}`}>
      <aside className={`sidebar ${sidebarCollapsed ? "sidebar--collapsed" : ""}`}>
        <button className="brand" onClick={() => onNavigate("overview", "/")} aria-label="IVRdroid overview" title={sidebarCollapsed ? "IVRdroid" : undefined}>
          <span className="brand__mark"><PhoneCall size={25} /></span>
          <span>IVRdroid</span>
        </button>
        <nav id="primary-navigation" aria-label="Main navigation">
          {NAVIGATION.map((item) => {
            const Icon = item.icon;
            return (
              <button
                key={item.key}
                className={`nav-item ${page === item.key ? "nav-item--active" : ""}`}
                onClick={() => onNavigate(item.key, item.path)}
                aria-current={page === item.key ? "page" : undefined}
                title={sidebarCollapsed ? item.label : undefined}
              >
                <Icon size={21} />
                <span>{item.label}</span>
              </button>
            );
          })}
        </nav>
        <button
          type="button"
          className="sidebar__footer"
          onClick={toggleSidebar}
          aria-controls="primary-navigation"
          aria-expanded={!sidebarCollapsed}
          aria-label={sidebarCollapsed ? "Expand navigation" : "Collapse navigation"}
          title={sidebarCollapsed ? "Expand navigation" : "Collapse navigation"}
        >
          <ChevronLeft className="sidebar__footer-icon" size={20} />
          <span>{sidebarCollapsed ? "Expand" : "Collapse"}</span>
        </button>
      </aside>

      <div className="workspace">
        {page !== "ivr-flow" ? <header className="topbar">
          <div className="topbar__title">
            <h1>{title}</h1>
            {draft ? <span className="draft-label">Draft</span> : null}
          </div>
          <div className="topbar__status">
            <span className="device-indicator">
              <Smartphone size={20} />
              <span>{device?.display_name ?? "No tablet"}</span>
              <i className={online ? "online" : "offline"} />
              <span>{online ? "Online" : "Offline"}</span>
            </span>
            <span className="sync-indicator">
              {device?.last_seen_at ? <RefreshCw size={19} /> : <Clock3 size={19} />}
              <span>{syncText}</span>
            </span>
            {action}
          </div>
        </header> : null}
        <main className={`page ${page === "ivr-flow" ? "page--studio" : ""}`}>{children}</main>
      </div>
    </div>
  );
}
