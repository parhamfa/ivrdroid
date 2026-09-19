import { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState, type PropsWithChildren } from "react";
import { api } from "./api";
import { ErrorState, Loading } from "./components/ui";
import { createDateFormatter, DEFAULT_DISPLAY_SETTINGS, validateDisplaySettings } from "./display";
import type { DisplaySettings } from "./types";

type DisplaySettingsState = {
  settings: DisplaySettings;
  loading: boolean;
  saving: boolean;
  error: string | null;
  refresh: () => Promise<void>;
  save: (settings: DisplaySettings) => Promise<void>;
};

const DisplaySettingsContext = createContext<DisplaySettingsState | null>(null);

export function DisplaySettingsProvider({ children }: PropsWithChildren) {
  const [settings, setSettings] = useState(DEFAULT_DISPLAY_SETTINGS);
  const [loading, setLoading] = useState(true);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const requestId = useRef(0);
  const savePending = useRef(false);

  const refresh = useCallback(async () => {
    if (savePending.current) return;
    const id = ++requestId.current;
    try {
      const value = validateDisplaySettings(await api.displaySettings());
      if (id === requestId.current) { setSettings(value); setError(null); }
    } catch (reason) {
      if (id === requestId.current) setError(reason instanceof Error ? reason.message : "Could not load display settings.");
    } finally {
      if (id === requestId.current) setLoading(false);
    }
  }, []);

  const save = useCallback(async (next: DisplaySettings) => {
    if (savePending.current) throw new Error("Display settings are already being saved.");
    savePending.current = true;
    setSaving(true);
    const id = ++requestId.current;
    try {
      const value = validateDisplaySettings(await api.saveDisplaySettings(next));
      if (id === requestId.current) { setSettings(value); setError(null); setLoading(false); }
    } finally {
      savePending.current = false;
      if (id === requestId.current) setSaving(false);
    }
  }, []);

  useEffect(() => {
    void refresh();
    const onFocus = () => { void refresh(); };
    window.addEventListener("focus", onFocus);
    return () => { window.removeEventListener("focus", onFocus); requestId.current += 1; };
  }, [refresh]);

  const value = useMemo(() => ({ settings, loading, saving, error, refresh, save }), [settings, loading, saving, error, refresh, save]);
  return <DisplaySettingsContext.Provider value={value}>{children}</DisplaySettingsContext.Provider>;
}

export function useDisplaySettings() {
  const context = useContext(DisplaySettingsContext);
  if (!context) throw new Error("DisplaySettingsProvider is required.");
  return context;
}

export function useDateFormatter() {
  const { settings, loading } = useDisplaySettings();
  return useMemo(() => loading
    ? (value: string | null | undefined) => value ? "…" : "Never"
    : createDateFormatter(settings), [settings, loading]);
}

export function DisplaySettingsNotice() {
  const { error, loading, settings, refresh } = useDisplaySettings();
  if (loading) return <Loading label="Loading display settings" />;
  if (!error) return null;
  const calendar = settings.date_calendar === "persian" ? "Persian/Jalali" : "Gregorian";
  return <ErrorState message={`Display settings could not be refreshed. Using ${settings.timezone} · ${calendar}. ${error}`} retry={() => void refresh()} />;
}
