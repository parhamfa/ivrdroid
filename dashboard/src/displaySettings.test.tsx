import { act, cleanup, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { api } from "./api";
import { DEFAULT_DISPLAY_SETTINGS } from "./display";
import { DisplaySettingsNotice, DisplaySettingsProvider, useDateFormatter } from "./displaySettings";
import { DisplaySettingsCard } from "./pages/DisplaySettingsCard";
import type { DisplaySettings } from "./types";

vi.mock("./api", () => ({ api: { displaySettings: vi.fn(), saveDisplaySettings: vi.fn() } }));
let stored: DisplaySettings;

function Timestamp({ label }: { label: string }) {
  const format = useDateFormatter();
  return <time aria-label={label}>{format("2026-03-20T21:00:00Z")}</time>;
}

function Panel() {
  return <DisplaySettingsProvider>
    <DisplaySettingsNotice /><DisplaySettingsCard />
    <Timestamp label="Call timestamp" /><Timestamp label="Revision timestamp" />
    <input aria-label="Unrelated draft" defaultValue="original" />
  </DisplaySettingsProvider>;
}

async function ready() {
  await waitFor(() => expect((screen.getByLabelText(/Display timezone/) as HTMLSelectElement).disabled).toBe(false));
}

function selectTimezone(timezone: string) {
  fireEvent.change(screen.getByLabelText(/Display timezone/), { target: { value: timezone } });
}

beforeEach(() => {
  stored = { ...DEFAULT_DISPLAY_SETTINGS };
  vi.mocked(api.displaySettings).mockImplementation(async () => ({ ...stored }));
  vi.mocked(api.saveDisplaySettings).mockImplementation(async (value) => { stored = { ...value }; return { ...stored }; });
});
afterEach(() => { cleanup(); vi.resetAllMocks(); });

describe("shared display preferences", () => {
  it("waits for initial preferences before displaying absolute timestamps", async () => {
    let finish!: (value: DisplaySettings) => void;
    vi.mocked(api.displaySettings).mockReturnValueOnce(new Promise((resolve) => { finish = resolve; }));
    render(<Panel />);
    expect(screen.getByLabelText("Call timestamp").textContent).toBe("…");
    await act(async () => { finish({ timezone: "UTC", date_calendar: "gregorian" }); });
    expect(screen.getByLabelText("Call timestamp").textContent).toBe("Mar 20, 2026, 21:00");
  });

  it("previews changes, saves all consumers immediately, and reloads the server preference", async () => {
    const first = render(<Panel />);
    await ready();
    fireEvent.change(screen.getByLabelText("Unrelated draft"), { target: { value: "unsaved work" } });
    expect(screen.getByLabelText("Call timestamp").textContent).toBe("Farvardin 1, 1405, 00:30");
    selectTimezone("UTC");
    fireEvent.change(screen.getByLabelText(/Calendar/), { target: { value: "gregorian" } });
    expect(screen.getByLabelText("Call timestamp").textContent).toBe("Farvardin 1, 1405, 00:30");
    expect(screen.getByLabelText("Date and time preview").textContent).toMatch(/, 20\d{2}, /);
    fireEvent.click(screen.getByRole("button", { name: "Save display settings" }));
    await screen.findByText("Display settings saved for all administrators.");
    expect(api.saveDisplaySettings).toHaveBeenCalledWith({ timezone: "UTC", date_calendar: "gregorian" });
    expect(screen.getByLabelText("Call timestamp").textContent).toBe("Mar 20, 2026, 21:00");
    expect(screen.getByLabelText("Revision timestamp").textContent).toBe("Mar 20, 2026, 21:00");
    expect((screen.getByLabelText("Unrelated draft") as HTMLInputElement).value).toBe("unsaved work");
    first.unmount();
    render(<Panel />);
    await ready();
    expect(screen.getByLabelText("Call timestamp").textContent).toBe("Mar 20, 2026, 21:00");
    expect((screen.getByLabelText(/Display timezone/) as HTMLSelectElement).value).toBe("UTC");
  });

  it("keeps applied settings on save failure and lets the same selection be retried", async () => {
    render(<Panel />);
    await ready();
    vi.mocked(api.saveDisplaySettings).mockRejectedValueOnce(new Error("Connection lost"));
    selectTimezone("UTC");
    fireEvent.click(screen.getByRole("button", { name: "Save display settings" }));
    expect(await screen.findByRole("alert")).toHaveProperty("textContent", expect.stringContaining("Connection lost"));
    expect(screen.getByLabelText("Call timestamp").textContent).toBe("Farvardin 1, 1405, 00:30");
    expect((screen.getByLabelText(/Display timezone/) as HTMLSelectElement).value).toBe("UTC");
    fireEvent.click(screen.getByRole("button", { name: "Retry" }));
    await screen.findByText("Display settings saved for all administrators.");
    expect(screen.getByLabelText("Call timestamp").textContent).toBe("Esfand 29, 1404, 21:00");
  });

  it("refreshes on focus without overwriting an unsaved display selection", async () => {
    render(<Panel />);
    await ready();
    selectTimezone("UTC");
    stored = { timezone: "Europe/Sofia", date_calendar: "gregorian" };
    fireEvent(window, new Event("focus"));
    await waitFor(() => expect(screen.getByLabelText("Call timestamp").textContent).toBe("Mar 20, 2026, 23:00"));
    expect((screen.getByLabelText(/Display timezone/) as HTMLSelectElement).value).toBe("UTC");
    expect((screen.getByLabelText(/Calendar/) as HTMLSelectElement).value).toBe("persian");
    expect(api.saveDisplaySettings).not.toHaveBeenCalled();
  });

  it("updates clean selectors when another administrator's settings are refreshed", async () => {
    render(<Panel />);
    await ready();
    selectTimezone("UTC");
    selectTimezone("Asia/Tehran");
    stored = { timezone: "Europe/Sofia", date_calendar: "gregorian" };
    fireEvent(window, new Event("focus"));
    await waitFor(() => expect((screen.getByLabelText(/Display timezone/) as HTMLSelectElement).value).toBe("Europe/Sofia"));
    expect((screen.getByLabelText(/Calendar/) as HTMLSelectElement).value).toBe("gregorian");
  });

  it("does not let an older refresh revert a successful save", async () => {
    render(<Panel />);
    await ready();
    let finish!: (value: DisplaySettings) => void;
    vi.mocked(api.displaySettings).mockReturnValueOnce(new Promise((resolve) => { finish = resolve; }));
    fireEvent(window, new Event("focus"));
    selectTimezone("UTC");
    fireEvent.click(screen.getByRole("button", { name: "Save display settings" }));
    await screen.findByText("Display settings saved for all administrators.");
    await act(async () => { finish(DEFAULT_DISPLAY_SETTINGS); });
    expect(screen.getByLabelText("Call timestamp").textContent).toBe("Esfand 29, 1404, 21:00");
  });

  it("shows the last confirmed preference with a retryable refresh warning", async () => {
    stored = { timezone: "UTC", date_calendar: "gregorian" };
    render(<Panel />);
    await ready();
    vi.mocked(api.displaySettings).mockRejectedValueOnce(new Error("Network unavailable"));
    fireEvent(window, new Event("focus"));
    expect(await screen.findByRole("alert")).toHaveProperty("textContent", expect.stringContaining("Using UTC · Gregorian"));
    expect(screen.getByLabelText("Call timestamp").textContent).toBe("Mar 20, 2026, 21:00");
    fireEvent.click(screen.getByRole("button", { name: "Retry" }));
    await waitFor(() => expect(screen.queryByRole("alert")).toBeNull());
  });

  it("uses the documented defaults when initial loading fails", async () => {
    vi.mocked(api.displaySettings).mockRejectedValueOnce(new Error("Unavailable"));
    render(<Panel />);
    await ready();
    expect(screen.getByRole("alert").textContent).toContain("Using Asia/Tehran · Persian/Jalali");
    expect(screen.getByLabelText("Call timestamp").textContent).toBe("Farvardin 1, 1405, 00:30");
  });
});
