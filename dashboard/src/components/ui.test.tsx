import { cleanup, fireEvent, render, screen } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";
import { AppShell } from "./AppShell";
import { formatBytes, formatDate, formatDuration } from "./ui";

afterEach(() => {
  cleanup();
  window.localStorage.clear();
});

describe("dashboard primitives", () => {
  it("formats operational values consistently", () => {
    expect(formatBytes(1_048_576)).toBe("1.0 MB");
    expect(formatDuration(125)).toBe("02:05");
    expect(formatDate(null)).toBe("Never");
  });

  it("renders device health and the approved navigation", () => {
    render(
      <AppShell
        page="overview"
        title="Overview"
        device={{
          id: "device-1",
          display_name: "SM-T585",
          app_version: "1.0",
          helper_version: "1.0",
          desired_revision_id: 3,
          active_revision_id: 3,
          status: {},
          last_seen_at: new Date().toISOString(),
          enrolled_at: new Date().toISOString(),
          revoked_at: null,
        }}
        onNavigate={vi.fn()}
      >
        <p>Dashboard content</p>
      </AppShell>,
    );
    expect(screen.getByRole("heading", { name: "Overview" })).toBeTruthy();
    expect(screen.getByText("Caller Policy")).toBeTruthy();
    expect(screen.getByText("IVR Flow")).toBeTruthy();
    expect(screen.getByText("Online")).toBeTruthy();
    expect(screen.getByText("Dashboard content")).toBeTruthy();
  });

  it("collapses, expands, and persists the navigation rail", () => {
    const rendered = render(
      <AppShell page="ivr-flow" title="IVR Flow" device={null} onNavigate={vi.fn()}>
        <input aria-label="Editor state" defaultValue="unchanged" />
      </AppShell>,
    );
    const shell = rendered.container.querySelector(".app-shell");
    const editor = screen.getByRole("textbox", { name: "Editor state" }) as HTMLInputElement;

    fireEvent.change(editor, { target: { value: "kept" } });
    fireEvent.click(screen.getByRole("button", { name: "Collapse navigation" }));
    expect(shell?.classList.contains("app-shell--sidebar-collapsed")).toBe(true);
    expect(screen.getByRole("button", { name: "Expand navigation" })).toBeTruthy();
    expect(editor.value).toBe("kept");
    expect(window.localStorage.getItem("ivrdroid.sidebar.collapsed.v1")).toBe("true");

    fireEvent.click(screen.getByRole("button", { name: "Expand navigation" }));
    expect(shell?.classList.contains("app-shell--sidebar-collapsed")).toBe(false);
    expect(screen.getByRole("button", { name: "Collapse navigation" })).toBeTruthy();
    expect(editor.value).toBe("kept");
    expect(window.localStorage.getItem("ivrdroid.sidebar.collapsed.v1")).toBe("false");
  });
});
