import { describe, expect, it } from "vitest";
import { buildTimeZoneOptions, createDateFormatter, DEFAULT_DISPLAY_SETTINGS, validateDisplaySettings } from "./display";

describe("display timestamps", () => {
  it("crosses midnight and Nowruz in the selected timezone", () => {
    const persian = createDateFormatter(DEFAULT_DISPLAY_SETTINGS);
    expect(persian("2026-03-20T20:29:00Z")).toBe("Esfand 29, 1404, 23:59");
    expect(persian("2026-03-20T20:30:00Z")).toBe("Farvardin 1, 1405, 00:00");
    const gregorian = createDateFormatter({ timezone: "Asia/Tehran", date_calendar: "gregorian" });
    expect(gregorian("2026-03-20T21:00:00Z")).toBe("Mar 21, 2026, 00:30");
    expect(createDateFormatter({ timezone: "UTC", date_calendar: "persian" })("2026-03-20T21:00:00Z")).toBe("Esfand 29, 1404, 21:00");
  });

  it("applies historical DST at the timestamp, not the current offset", () => {
    const format = createDateFormatter({ timezone: "America/New_York", date_calendar: "gregorian" });
    expect(format("2026-03-08T06:59:00Z")).toBe("Mar 8, 2026, 01:59");
    expect(format("2026-03-08T07:00:00Z")).toBe("Mar 8, 2026, 03:00");
    expect(format("2026-11-01T05:30:00Z")).toBe("Nov 1, 2026, 01:30");
    expect(format("2026-11-01T06:30:00Z")).toBe("Nov 1, 2026, 01:30");
  });

  it("preserves explicit offsets and treats legacy offset-free API timestamps as UTC", () => {
    const format = createDateFormatter(DEFAULT_DISPLAY_SETTINGS);
    for (const value of ["2026-03-20T21:00:00Z", "2026-03-21T00:30:00+03:30", "2026-03-20T17:00:00-04:00", "2026-03-20T21:00:00.000000", "2026-03-20 21:00:00"]) {
      expect(format(value)).toBe("Farvardin 1, 1405, 00:30");
    }
  });

  it("keeps missing timestamps readable and malformed timestamps harmless", () => {
    const format = createDateFormatter(DEFAULT_DISPLAY_SETTINGS);
    expect(format(null)).toBe("Never");
    expect(format(undefined)).toBe("Never");
    expect(format("")).toBe("Never");
    expect(format("not-a-timestamp")).toBe("—");
  });

  it("rejects unsupported settings before they reach the formatter", () => {
    expect(() => validateDisplaySettings({ timezone: "invalid", date_calendar: "persian" })).toThrow();
    expect(() => validateDisplaySettings({ timezone: "", date_calendar: "persian" })).toThrow();
  });
});

describe("timezone options", () => {
  it("includes UTC, Tehran, and a saved alias with accurate fractional offsets", () => {
    const options = buildTimeZoneOptions("Asia/Kathmandu", new Date("2026-06-01T00:00:00Z"));
    expect(options.find((item) => item.value === "Asia/Tehran")?.label).toBe("(UTC+03:30) Asia/Tehran");
    expect(options.find((item) => item.value === "Asia/Kathmandu")?.label).toBe("(UTC+05:45) Asia/Kathmandu");
    expect(options.find((item) => item.value === "UTC")?.label).toBe("(UTC+00:00) UTC");
    expect(new Set(options.map((item) => item.value)).size).toBe(options.length);
  });

  it("labels zones using their offset on the current date", () => {
    expect(buildTimeZoneOptions("Europe/Sofia", new Date("2026-06-01T00:00:00Z")).find((item) => item.value === "Europe/Sofia")?.label).toBe("(UTC+03:00) Europe/Sofia");
    expect(buildTimeZoneOptions("Europe/Sofia", new Date("2026-01-01T00:00:00Z")).find((item) => item.value === "Europe/Sofia")?.label).toBe("(UTC+02:00) Europe/Sofia");
  });
});
