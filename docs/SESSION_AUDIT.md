# Full-session auditing (release 0.9.0)

Session auditing is an optional appliance setting, independent of flow publication. It defaults
off. Settings shows the desired signed policy and the tablet's applied version. A change is
verified and acknowledged while idle, then applies to subsequent answered IVR calls. The last
verified policy survives an outage. Missing or invalid local policy means off. Stock-dialer
calls are excluded; built-in fallback IVR sessions are included.

## Capture and failure boundaries

When auditing is enabled, the session guardian owns one capture worker and a separate audit
writer. A bounded shared-memory ring delivers the original 48 kHz stereo PCM to DTMF,
voicemail, and conversation consumers. Audit persistence never feeds mixed prompt audio back
into those consumers. The capture worker remains privileged; the audit writer drops to the app
UID. An audit handoff, disk, or writer failure stops audit persistence and marks coverage
partial. It does not stop the capture producer, call control, or existing recording consumers.
Actual capture failure remains a call-audio failure, as before.

The writer combines the carrier PCM with prompt and beep samples submitted to the playback
PCM. Hardware monotonic timestamps account for queued output. Playback cancellation cuts off
queued prompt samples at the interruption timestamp. A 300 ms writer delay allows cancellation
to settle without delaying the call. Typed events use the same monotonic clock. Real carrier
acceptance must verify both voices and prompts exactly once and alignment within 250 ms; the
synthetic harness alone cannot establish carrier behavior.

Audit WAV segments rotate at 15 seconds. Each second, the writer fsyncs the valid WAV prefix,
then atomically checkpoints context and events. A closed segment is renamed and published with
a SHA-256 receipt. The app encrypts it during the call with its independent Android Keystore
AES-GCM key, reads it back for verification, and only then removes plaintext. The audit spool,
key alias, worker and error reporting are separate from voicemail/operator handoff.

After interruption, recovery validates writer PID, process start time and boot identity. It
truncates the last segment to the durable checkpoint, seals it, and reports partial coverage.
Recovery runs after safety cleanup and again during idle periods. Safety cleanup never waits
for encryption or upload. Worker draining/recovery is bounded. No capture produces Unavailable
with no fabricated audio timestamp. A recovered session's call duration is the observed lower
bound; the exact disconnected time is not invented.

## Control and data contracts

- `GET/PUT /api/admin/v1/audit-recording-settings`: independent signed policy, capability,
  desired/applied state and quota. The default tablet audit quota is 1 GiB.
- Device sync returns the signed policy and records applied-policy acknowledgements. Controls
  synchronize before the recording data plane, so an upload failure cannot prevent a setting
  change. Release version and source commit are diagnostics, separate from API/compiler versions.
- Call metadata carries an immutable audit receipt: policy version, logical recording ID,
  captured timestamp, duration, partial coverage, terminal reason and typed monotonic events.
  Call metadata is acknowledged before audio upload. Policy history remains valid for offline
  recordings uploaded after the setting changes.
- `/api/device/v1/session-recordings` and its segment endpoints implement bounded, resumable,
  hash-verified uploads. Each segment is at most 15 seconds/3 MiB; chunks are at most 1 MiB.
  Corrupt completed uploads restart from zero on retry. The app deletes its encrypted spool only
  after a matching final acknowledgement, including duration and segment count.
- One logical `session_audit` recording belongs to a call, with no flow block. Built-in calls
  may have no revision. Voicemail/conversation flow identity validation remains strict.
- Migration `0006_session_audit` adds policy/acknowledgement tables, call/recording JSON metadata,
  nullable audit identity fields, a recording identity constraint and a unique audit-per-call
  index. Historical calls have no fabricated audio or timeline.

The server reserves 1 GiB for audit recordings within shared recording storage by default.
`IVRDROID_AUDIT_RECORDING_QUOTA_BYTES` configures that subset; the existing overall recording
quota still applies. `IVRDROID_AUDIT_RECORDING_RESERVE_BYTES` defaults to 512 MiB. Tablet audits
leave the existing 512 MiB filesystem reserve plus 64 MiB recording headroom. Server upload and
assembly check both quota and filesystem headroom. Exhaustion retains pending data; shared
retention and explicit deletion reclaim acknowledged media. Voicemail/operator uploads precede
audit uploads. Uploads pause when a call starts.

## Dashboard

Call history has a Session audio column: Listen, Partial · Listen, Pending upload, Unavailable,
or Not recorded. Listen opens Call details with native audio controls, download, deletion and
listened/unlistened state. Clicking a timestamped event seeks to its position. Partial coverage
shows where available audio ends and its reason. Voicemail/operator recordings remain separately
linked and retain their existing inbox. Deletion and retention leave call history, timeline and
recording tombstones intact.

## Verification

Run `scripts/check.sh`, server pytest, and dashboard tests/build. `scripts/build-release.sh`
builds APK, overlay, helper, an isolated hardware harness, and the exact source archive from a
clean commit. The manifest records versions, commit and SHA-256 hashes.

The optional native harness uses synthetic PCM and a separate test bridge; it never touches
Telecom, the microphone or production bridge. It exercises single capture ownership, unmixed
core consumers, interrupted prompt mixing, 15-second rotation, interrupted checkpoints and quota
failure. Run it on the SM-T585 as root to exercise writer privilege dropping too.

The Android instrumentation runner uses a private temporary context and shared-preference
namespace for real Keystore, fsync, during-call handoff, orphan recovery, metadata acknowledgement
and quota failure. It refuses an active Telecom call. Run only in an idle maintenance window:

```sh
adb -s "$SERIAL" install -r app/build/outputs/apk/androidTest/debug/app-debug-androidTest.apk
adb -s "$SERIAL" shell am instrument -w \
  ai.rx1.ivrdroid.test/ai.rx1.ivrdroid.control.SessionAuditAcceptanceInstrumentation
```

Physical acceptance and exact deployed artifacts are recorded in the release evidence, not
inferred from automated checks. See [DEPLOYMENT.md](DEPLOYMENT.md) for rollout and rollback.

Pre-release checks on 2026-09-12 passed 104 Android unit tests in each build variant, Android
lint, 18 native test executables, 94 server tests, 63 dashboard tests and the dashboard build.
Helper binaries and privileged overlay packaging reproduced across separate build paths.
The native harness passed on the SM-T585, including writer interruption and durable recovery.
An isolated PostgreSQL 16 copy migrated from 0005 to 0006 while preserving 105 calls and 17
recording rows. The compatibility rollback source read the migrated schema, a new audit kind,
and decrypted existing audio. Browser checks verified real playback, seeking, listened state,
settings synchronization feedback and layouts at desktop and 390 px mobile widths. These
checks precede deployment; they do not claim physical carrier acceptance or a released tag.
