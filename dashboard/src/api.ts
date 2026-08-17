import type {
  AuditRecord,
  CallRecord,
  Device,
  DraftConfiguration,
  FlowDiff,
  Overview,
  Prompt,
  Recording,
  RecordingList,
  RecordingSettings,
  Revision,
  RevisionDetail,
  SimulationResult,
  NtfySettings,
  ValidationResult,
} from "./types";

export class ApiError extends Error {
  constructor(
    message: string,
    readonly status: number,
    readonly detail?: unknown,
  ) {
    super(message);
  }
}

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  let response: Response;
  try {
    response = await fetch(path, {
      credentials: "same-origin",
      ...init,
      headers: {
        ...(init?.body instanceof FormData ? {} : { "Content-Type": "application/json" }),
        ...init?.headers,
      },
    });
  } catch (reason) {
    if (reason instanceof TypeError) {
      throw new ApiError(
        "Admin session expired or the network is unavailable. Sign in to IVRdroid in another tab, then retry; unsaved changes remain here.",
        0,
      );
    }
    throw reason;
  }
  if (!response.ok) {
    let detail: unknown;
    try {
      detail = (await response.json()).detail;
    } catch {
      detail = response.statusText;
    }
    const message = Array.isArray(detail) ? detail.join(" ") : String(detail || "Request failed");
    throw new ApiError(message, response.status, detail);
  }
  if (response.status === 204) {
    return undefined as T;
  }
  return (await response.json()) as T;
}

export const api = {
  overview: () => request<Overview>("/api/admin/v1/overview"),
  draft: () => request<DraftConfiguration>("/api/admin/v4/draft"),
  saveDraft: (draft: DraftConfiguration) =>
    request<DraftConfiguration>("/api/admin/v4/draft", {
      method: "PUT",
      body: JSON.stringify(draft),
    }),
  validateDraft: (draft?: DraftConfiguration) =>
    request<ValidationResult>("/api/admin/v4/draft/validate", {
      method: "POST",
      body: draft ? JSON.stringify(draft) : undefined,
    }),
  simulateDraft: (configuration: DraftConfiguration, events: string[]) =>
    request<SimulationResult>("/api/admin/v4/draft/simulate", {
      method: "POST",
      body: JSON.stringify({ configuration, events }),
    }),
  diffDraft: (configuration: DraftConfiguration, baseRevisionId?: number | null) =>
    request<FlowDiff>("/api/admin/v4/draft/diff", {
      method: "POST",
      body: JSON.stringify({ configuration, base_revision_id: baseRevisionId ?? null }),
    }),
  publishDraft: (review: { edit_version: number; base_revision_id: number | null }) =>
    request<Revision>("/api/admin/v4/draft/publish", {
      method: "POST",
      body: JSON.stringify(review),
    }),
  revisions: () => request<Revision[]>("/api/admin/v1/revisions"),
  revision: (revisionId: number) =>
    request<RevisionDetail>(`/api/admin/v4/revisions/${revisionId}`),
  rollback: (revisionId: number) =>
    request<Revision>(`/api/admin/v4/revisions/${revisionId}/rollback`, { method: "POST" }),
  activateLegacy: (revisionId: number) =>
    request<Revision>(`/api/admin/v4/revisions/${revisionId}/activate-rollback`, { method: "POST" }),
  prompts: () => request<Prompt[]>("/api/admin/v1/prompts"),
  uploadPrompt: (name: string, file: File) => {
    const body = new FormData();
    body.append("name", name);
    body.append("file", file);
    return request<Prompt>("/api/admin/v1/prompts", { method: "POST", body });
  },
  deletePrompt: (promptId: string) =>
    request<void>(`/api/admin/v1/prompts/${promptId}`, { method: "DELETE" }),
  devices: () => request<Device[]>("/api/admin/v1/devices"),
  createPairingCode: (displayName: string) =>
    request<{ code: string; expires_at: string }>("/api/admin/v1/pairing-codes", {
      method: "POST",
      body: JSON.stringify({ display_name: displayName }),
    }),
  revokeDevice: (deviceId: string) =>
    request<Device>(`/api/admin/v1/devices/${deviceId}/revoke`, { method: "POST" }),
  calls: () => request<CallRecord[]>("/api/admin/v1/calls"),
  recordings: (unread = false, page = 1, pageSize = 50) => {
    const query = new URLSearchParams({ page: String(page), page_size: String(pageSize) });
    if (unread) query.set("unread", "true");
    return request<RecordingList>(`/api/admin/v1/recordings?${query}`);
  },
  setRecordingListened: (recordingId: string, listened: boolean) =>
    request<Recording>(`/api/admin/v1/recordings/${recordingId}/listened`, {
      method: "PATCH",
      body: JSON.stringify({ listened }),
    }),
  deleteRecording: (recordingId: string) =>
    request<void>(`/api/admin/v1/recordings/${recordingId}`, { method: "DELETE" }),
  recordingSettings: () => request<RecordingSettings>("/api/admin/v1/recording-settings"),
  saveRecordingSettings: (mode: RecordingSettings["mode"], days: number) =>
    request<RecordingSettings>("/api/admin/v1/recording-settings", {
      method: "PUT",
      body: JSON.stringify({ mode, days }),
    }),
  ntfySettings: () => request<NtfySettings>("/api/admin/v1/ntfy-settings"),
  saveNtfySettings: (body: {
    enabled: boolean;
    server_url: string;
    topic: string;
    token?: string | null;
    events: NtfySettings["events"];
  }) =>
    request<NtfySettings>("/api/admin/v1/ntfy-settings", {
      method: "PUT",
      body: JSON.stringify(body),
    }),
  testNtfySettings: () =>
    request<NtfySettings>("/api/admin/v1/ntfy-settings/test", { method: "POST" }),
  audit: () => request<AuditRecord[]>("/api/admin/v1/audit"),
};
