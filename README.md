# IVRdroid

IVRdroid is a single-tablet cellular IVR appliance for a dedicated rooted Android device. It
keeps Android's stock dialer in place, applies a signed dashboard-managed caller policy, and uses
a narrowly scoped native helper for the audited cellular audio path.

The Android application ID is `ai.rx1.ivrdroid`.

## Full-session auditing in 0.9.0

Optional full-session recording covers answered IVR calls, including built-in fallback,
voicemail and operator conversations. Settings controls it independently of flow publication;
Call history provides playback and a seekable event timeline. Capture is shared, but audit
persistence and failure handling are isolated from existing recordings. See
[docs/SESSION_AUDIT.md](docs/SESSION_AUDIT.md) for durability, quotas and verification.

## V4 external call scope

V4 adds a signed `external_call` flow step that holds the owned caller, dials one configured
operator, verifies and records the carrier conference, and resumes through Completed, Not
connected, or System failure branches. Call control stays local to the tablet and physical carrier
merge plus two-voice capture remain deployment acceptance gates. See
[docs/EXTERNAL_CALL_V4.md](docs/EXTERNAL_CALL_V4.md) for the contract, safety model, rollout, and
test matrix.

## V3 voicemail foundation

V3 adds explicit voicemail to the web-controlled appliance without turning it into a general
PBX; full-session auditing is a separate, optional layer:

- Caller policy modes: IVR disabled, allowlist only, accept all, and accept all except a
  blocklist. Hidden or unparseable callers are separately configurable and default to the stock
  dialer.
- A local tablet kill switch always wins over downloaded policy.
- WAV, MP3, and OGG prompt upload, validation, preview, versioning, and conversion to 48 kHz
  stereo PCM16 WAV.
- An owned-tree Flow Studio for prompts, independent one-digit DTMF branches, schedules, bounded
  return-to-menu actions, retries, timeouts, and end-call blocks. Author-facing target IDs and
  shared graph nodes do not exist.
- Immutable Ed25519-signed revisions, idle-only activation, acknowledgement, rollback, device
  health, audit history, and non-audio call events.
- A `record_message` block with explicit success and unavailable paths. Validation requires a
  separate `play_prompt` greeting immediately before it; a built-in beep follows the greeting.
- One signed global recording behavior shared by every recording block: 10-180 seconds (60 by
  default) and an optional DTMF finish key (`#` by default). Hangup and the hard limit always stop
  recording; there is deliberately no silence detector.
- Deterministic server compilation from `FlowDocumentV3` into a bounded `RuntimeProgramV3` tape
  that is independently verified by the Android app and native helper. V1 and V2 revisions and
  executors remain immutable rollback targets.
- Offline operation from the last-known-good revision, with the built-in fixed menu retained as
  the final recovery fallback.
- Post-call resumable upload from a 512 MiB Android Keystore-encrypted spool, server-side WAV
  validation and MP3 normalization, a separately encrypted 5 GiB media volume, retention policy,
  and a dashboard voicemail inbox.

The control plane is a React/Vite dashboard and FastAPI/PostgreSQL service deployed as the
separate `ivrdroid` Compose project. Only the web proxy is published, on
`127.0.0.1:3200`; the API and database remain on internal Docker networks. The intended public
origin is `https://ivrdroid.rx1.ai`. Production runs on `old-mac` under `/Users/parhamfatemi/Services/ivrdroid/deploy`.
The existing Cloudflare Tunnel connects the public host to the loopback-only web port.

This release supports one Samsung SM-T585. The identifiers and APIs are fleet-shaped, but the
server deliberately rejects enrollment of a second active tablet.

## Audio and privacy boundary

The native helper keeps the previously audited privacy and recovery model:

- Android must obtain a helper claim before answering a matching caller.
- The tablet microphone and speaker remain muted throughout an IVR session.
- Prompt injection and DTMF capture use the pinned SM-T585 TinyALSA route.
- A V4.1 collector may opt in to continuous capture during its menu prompt. Only configured
  digits stop playback; other keys are ignored until playback finishes, when the full timeout
  begins.
- Core consumers inspect original captured PCM. Explicit voicemail and operator recording
  retain their existing behavior; optional session auditing persists a separate mixed copy.
- During that instruction only, the audited 48 kHz stereo PCM16 caller path is written to an
  owner-restricted temporary WAV. The finish tone is trimmed, failed/partial captures are
  deleted, and finalized files are atomically handed to the app.
- The app encrypts finalized audio with an Android Keystore-backed AES-GCM key before it enters
  the durable spool. It uploads only after the call and pauses immediately for a new call. Local
  audio is deleted only after a matching verified server acknowledgement.
- Emergency, additional, replaced, or unverified calls fail closed and are yielded back to
  Android without an unsafe global hangup.

Dated carrier acceptance evidence lives in [docs/VERIFICATION.md](docs/VERIFICATION.md).
A successful build or synthetic harness is not proof of a working physical call path.

## Build and validation

Run the complete local validation:

```sh
./scripts/check.sh
```

It runs Android unit tests, lint, APK assembly, native host tests, the pinned arm64 helper build,
and a cross-directory reproducibility check. Package the disabled helper module with:

```sh
./scripts/package-helper.sh
```

The pinned tablet keeps Android restricted-networking mode enabled. Package the APK as the
narrow system-app Magisk module that grants the restricted-network permission and the privileged
non-UI in-call-control permission required by V4:

```sh
./scripts/package-system-app.sh
```

The debug APK is written to `app/build/outputs/apk/debug/app-debug.apk`. The helper package is
written under `helper/dist/`, and the system-app package is written under `app/dist/`.
After installing the V4 package, open IVRdroid and grant its newly requested `CALL_PHONE` runtime
permission before expecting the tablet to advertise external-call capability.

The dashboard's **Settings → Display** card saves one shared timezone and calendar for all
administrators, initially `Asia/Tehran` and Persian/Jalali. Changes apply after saving and are
refreshed when another browser window regains focus. IVR schedule rules, stored timestamps,
CSV exports, and elapsed audio times are independent of these presentation preferences.
Deploy the API with migration `0009_display_settings` before deploying the updated dashboard.

Run the server tests with:

```sh
cd server
python -m pytest
```

Run the dashboard checks with:

```sh
cd dashboard
npm test
npm run build
```

## Components

- `app/`: call screening, local safety switch, encrypted enrollment state, V1/V2/V3/V4
  signed-revision verification, encrypted recording spool, resumable uploads, synchronization,
  and bounded helper requests.
- `helper/`: bounded V4/V3/V2 instruction-tape engines, the legacy V1 rollback engine, device
  profile, TinyALSA prompt/DTMF/explicit-recording path, durable mixer transaction, privacy
  guardian, Telecom guard, and last-known-good revision storage.
- `server/`: FastAPI admin/device APIs, validation, encryption, revision signing, prompt
  conversion, resumable recording ingestion, encrypted MP3 storage, retention, and Alembic
  migrations.
- `dashboard/`: the operator dashboard for policy, flows, prompts, calls, recordings, devices,
  revisions, and retention.
- `contracts/`: cross-language canonical configuration fixtures.
- `deploy/`: the loopback-only production Compose definition. Runtime `.env` and secrets are
  intentionally ignored.
- `docs/ARCHITECTURE.md`: trust boundaries and control/data flow.
- `docs/EXTERNAL_CALL_V4.md`: the V4 conference contract, safety model, and rollout gate.
- `docs/DEPLOYMENT.md`: old-mac, tablet, backup, immutable release and scoped rollback runbook.
- `docs/VERIFICATION.md`: dated build and live-device evidence; it does not treat unrun live-call
  scenarios as passed.

## Production rollout

The authorized release target is old-mac and the enrolled SM-T585. Preserve the existing app
signing identity, enrollment, active revision and all media. Deploy the additive backend first
with auditing off, then the APK, persistent privileged overlay and helper while idle. Verify
post-reboot synchronization before enabling controlled physical acceptance calls. Tag only the
accepted deployed commit. Follow [docs/DEPLOYMENT.md](docs/DEPLOYMENT.md); do not re-enroll an
already enrolled tablet or downgrade the database during a routine rollback.
