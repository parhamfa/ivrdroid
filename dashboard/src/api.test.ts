import { afterEach, describe, expect, it, vi } from "vitest";
import { api } from "./api";

afterEach(() => {
  vi.unstubAllGlobals();
});

describe("API transport errors", () => {
  it("turns a blocked Access redirect into an actionable retry message", async () => {
    vi.stubGlobal("fetch", vi.fn().mockRejectedValue(new TypeError("Failed to fetch")));

    await expect(api.draft()).rejects.toMatchObject({
      status: 0,
      message: expect.stringContaining("Sign in to IVRdroid in another tab"),
    });
  });

  it("uses the additive V4 authoring surface", async () => {
    const draft = {
      schema_version: 4,
      edit_version: 1,
      caller_policy: { mode: "ALLOWLIST_ONLY", route_unknown_callers: false, allowlist: [], blocklist: [] },
      schedules: [],
      recording_behavior: { maximum_duration_seconds: 60, finish_key: "#" },
      flow: { root: null },
    };
    const fetchMock = vi.fn().mockResolvedValue(new Response(JSON.stringify(draft), {
      status: 200,
      headers: { "Content-Type": "application/json" },
    }));
    vi.stubGlobal("fetch", fetchMock);

    await expect(api.draft()).resolves.toEqual(draft);
    expect(fetchMock).toHaveBeenCalledWith("/api/admin/v4/draft", expect.objectContaining({ credentials: "same-origin" }));
  });

  it("binds publication to the exact reviewed draft and base revision", async () => {
    const revision = {
      id: 20,
      schema_version: 4,
      manifest_sha256: "a".repeat(64),
      signature_b64: "signed",
      source_revision_id: null,
      published_at: "2026-08-11T12:00:00Z",
      published_by: "owner@example.com",
    };
    const fetchMock = vi.fn().mockResolvedValue(new Response(JSON.stringify(revision), {
      status: 200,
      headers: { "Content-Type": "application/json" },
    }));
    vi.stubGlobal("fetch", fetchMock);

    await expect(api.publishDraft({ edit_version: 8, base_revision_id: 19 })).resolves.toEqual(revision);
    expect(fetchMock).toHaveBeenCalledWith("/api/admin/v4/draft/publish", expect.objectContaining({
      method: "POST",
      body: JSON.stringify({ edit_version: 8, base_revision_id: 19 }),
    }));
  });
});
