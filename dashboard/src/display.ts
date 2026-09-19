import type { DisplaySettings } from "./types";

export const DEFAULT_DISPLAY_SETTINGS: DisplaySettings = { timezone: "Asia/Tehran", date_calendar: "persian" };

function validTimeZone(timezone: string): boolean {
  try {
    new Intl.DateTimeFormat("en-US", { timeZone: timezone });
    return true;
  } catch {
    return false;
  }
}

export function validateDisplaySettings(value: DisplaySettings): DisplaySettings {
  if (!value || typeof value.timezone !== "string" || !value.timezone || !validTimeZone(value.timezone)
    || !["gregorian", "persian"].includes(value.date_calendar)) {
    throw new Error("The saved display settings are not supported by this browser.");
  }
  return { timezone: value.timezone, date_calendar: value.date_calendar };
}

export function createDateFormatter(settings: DisplaySettings) {
  const formatter = new Intl.DateTimeFormat("en-US", {
    timeZone: settings.timezone,
    calendar: settings.date_calendar === "persian" ? "persian" : "gregory",
    numberingSystem: "latn",
    year: "numeric", month: "short", day: "numeric",
    hour: "2-digit", minute: "2-digit", hourCycle: "h23",
  });
  return (value: string | null | undefined): string => {
    if (!value) return "Never";
    // Legacy API/SQLite timestamps can omit the UTC suffix. They are never browser-local time.
    const normalized = /^\d{4}-\d{2}-\d{2}[T ]\d{2}:\d{2}(?::\d{2}(?:\.\d+)?)?$/.test(value) ? `${value}Z` : value;
    const date = new Date(normalized);
    if (!Number.isFinite(date.getTime())) return "—";
    const parts = Object.fromEntries(formatter.formatToParts(date).map(({ type, value: part }) => [type, part]));
    // Keep both calendars in the panel's English layout, without the implicit Persian "AP" era.
    return `${parts.month} ${parts.day}, ${parts.year}, ${parts.hour}:${parts.minute}`;
  };
}

export function buildTimeZoneOptions(selected: string, now = new Date()) {
  const supported = (Intl as typeof Intl & { supportedValuesOf?: (key: "timeZone") => string[] }).supportedValuesOf;
  const zones = new Set([
    ...(supported?.("timeZone") ?? []),
    "UTC", "Asia/Tehran", "Europe/Sofia", "Europe/London", "America/New_York", "America/Los_Angeles",
    Intl.DateTimeFormat().resolvedOptions().timeZone, selected,
  ]);
  return [...zones].filter(validTimeZone).map((value) => {
    const name = new Intl.DateTimeFormat("en-US", { timeZone: value, timeZoneName: "longOffset" })
      .formatToParts(now).find((part) => part.type === "timeZoneName")!.value;
    const offset = name === "GMT" ? "+00:00" : name.replace("GMT", "");
    const minutes = (Number(offset.slice(1, 3)) * 60 + Number(offset.slice(4, 6))) * (offset.startsWith("-") ? -1 : 1);
    return { value, label: `(UTC${offset}) ${value}`, minutes };
  }).sort((a, b) => a.minutes - b.minutes || a.value.localeCompare(b.value));
}
