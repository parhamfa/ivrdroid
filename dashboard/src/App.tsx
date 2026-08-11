import { useEffect, useState } from "react";
import { api } from "./api";
import { AppShell, type PageKey } from "./components/AppShell";
import { CallsPage } from "./pages/CallsPage";
import { CallerPolicyPage } from "./pages/CallerPolicyPage";
import { FlowPage } from "./pages/FlowPage";
import { OverviewPage } from "./pages/OverviewPage";
import { PromptsPage } from "./pages/PromptsPage";
import { SettingsPage } from "./pages/SettingsPage";
import { VoicemailPage } from "./pages/VoicemailPage";
import type { Device } from "./types";

const ROUTES: Record<string, PageKey> = {
  "/": "overview",
  "/caller-policy": "caller-policy",
  "/ivr-flow": "ivr-flow",
  "/prompts": "prompts",
  "/calls": "calls",
  "/voicemail": "voicemail",
  "/settings": "settings",
};

const TITLES: Record<PageKey, string> = {
  overview: "Overview",
  "caller-policy": "Caller Policy",
  "ivr-flow": "IVR Flow",
  prompts: "Prompts",
  calls: "Calls",
  voicemail: "Recordings",
  settings: "Settings",
};

function currentPage(): PageKey {
  return ROUTES[window.location.pathname.replace(/\/$/, "") || "/"] ?? "overview";
}

export function App() {
  const [page, setPage] = useState<PageKey>(currentPage);
  const [device, setDevice] = useState<Device | null>(null);
  useEffect(() => {
    const load = () => void api.devices().then((devices) => setDevice(devices.find((item) => !item.revoked_at) ?? null)).catch(() => setDevice(null));
    load();
    const timer = window.setInterval(load, 30_000);
    const pop = () => setPage(currentPage());
    window.addEventListener("popstate", pop);
    return () => { window.clearInterval(timer); window.removeEventListener("popstate", pop); };
  }, []);

  const navigate = (next: PageKey, path: string) => { window.history.pushState({}, "", path); setPage(next); };

  return <AppShell page={page} title={TITLES[page]} draft={page === "caller-policy" || page === "ivr-flow" || page === "settings"} device={device} onNavigate={navigate}>
    {page === "overview" ? <OverviewPage device={device} /> : null}
    {page === "caller-policy" ? <CallerPolicyPage /> : null}
    {page === "ivr-flow" ? <FlowPage /> : null}
    {page === "prompts" ? <PromptsPage /> : null}
    {page === "calls" ? <CallsPage /> : null}
    {page === "voicemail" ? <VoicemailPage /> : null}
    {page === "settings" ? <SettingsPage /> : null}
  </AppShell>;
}
