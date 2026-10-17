# September 19 recovery candidate

This work starts from `a72c23f` (0.10.1), in worktree `sept19-call-recovery`, branch
`codex/sept19-call-recovery`. The subsequent display-settings and full-number
dashboard changes are retained in the candidate. Production installation and
historical recovery are a separate execution step. No test result below substitutes
for the physical tablet acceptance run.

## What controls admission

Android owns the telephone connections. The privileged native helper owns prompt
playback, keypad detection, capture and the private audio route. A caller can be
answered when the old telephone/audio ownership has been released, the new caller
is still the exact screened ringing call, and the helper has armed privacy.

The helper publishes a per-session resource-release receipt before waiting for old
recording writers. Android moves the ended session out of its active slot; its
outcome, recording receipts and uploads continue independently. Old writers retain
only their old file identities and memory, with no telephone/mixer/control handles.
Queues are bounded; exhausted recording capacity makes audio unavailable/partial,
while safe calls remain admissible. Small durable ownership records remain necessary
before answering; a completely unwritable application filesystem cannot safely
promise crash-proof call control. Recording quotas retain a separate free-space reserve.

The watchdog is the independent native safety process that observes phone/audio
state and terminates genuinely stuck call-control work. Its five-second post-hangup
escalation is not a recording-finalization deadline. Healthy hangup returns normally.
Six-second stalled writer tests and injected seven-/twenty-/sixty-second readiness
delays exercise independence. Seven seconds is not an observed recovery duration.

## Private contracts and compatibility

All files remain inside the application-owned private bridge, mode 0600 and private
directories. A matched app/helper advertises `call_admission=1;capture_receipt=2`.

| Receipt | Meaning |
| --- | --- |
| `INCOMING1 session boot native-call elapsed` | Fresh (at most two seconds old), exact screened ringing caller; never a permission to answer another call. |
| `call-results/<session>.released`: `RELEASE1 session boot elapsed` | Capture closed and owned audio route restored/released. Old file workers may still be running. |
| `call-results/<session>.cancelled`: `CANCEL1 session boot elapsed` | Admission was revoked. Yield audio without a global telephone hangup. |
| `call-results/<session>.outcome`: `END1 session boot elapsed result` | Outcome belongs only to this session. The matching `.path` retains its own menu path. |
| `PCM2 … frames captureEndMs finalizedMs` | Versioned capture receipt. Frame count, start/end and later finalization are separate; zero finalization means unknown. `PCM1` remains readable. |

Admission/control elapsed times use Android elapsed realtime / `CLOCK_BOOTTIME`.
PCM boundaries use the capture clock (`CLOCK_MONOTONIC`) and its boot identity;
do not compare these clock domains directly. Receipt validation compares capture
duration with its own frame count and checksum, never with file-finalization time.

The server accepts old and new recording manifests. New optional call fields are
`ended_at` and `cleanup_status`; missing final evidence is `END_DETAILS_UNAVAILABLE`
(dashboard: “Details pending”). Recording failure is a separate event and cannot
replace a known caller disconnect. Independent `call-events:batch` receipts merge
late evidence even after the parent call is acknowledged. Acknowledgements remove
only the exact submitted generation/content. Database identities deduplicate alert
generation; this does not promise exactly-once delivery by the external push service.

The migration is additive. Keep the newer schema during application rollback.
Historical corrections have original/applied values and the evidence index checksum
in `call_history_corrections`.

## Candidate checks

Use the checked-in tests rather than editing production policy or call flows:

```sh
./gradlew test lint assembleDebug assembleDebugAndroidTest
./helper/tests/run.sh
./scripts/build-helper.sh
./scripts/check-helper-reproducibility.sh
./scripts/check-system-app-reproducibility.sh
cd server
.venv/bin/pytest
cd ../dashboard
NODE_OPTIONS=--no-experimental-webstorage npm test -- --run
NODE_OPTIONS=--no-experimental-webstorage npm run build
```

`test_call_recovery_postgres.py` additionally needs `IVRDROID_TEST_POSTGRES_URL`
pointing to a disposable local PostgreSQL database. It creates/drops only a unique
test schema and races parent reports, independent events and retries. Container
permission verification needs the actual image layout and UID 10001:

```sh
python -m app.processing_workspace --root /data/recordings
IVRDROID_CONTAINER_PERMISSION_TEST=1 python -m pytest tests/test_processing_permissions.py
```

The process must be UID 10001, `/data` owned by root, and `/data/recordings` owned by
10001, mode 0700. The old sibling `/data/recordings-processing` must be unwritable;
the new `/data/recordings/.processing` must pass create/write/fsync/remove. Never
solve this by granting broad permissions to `/data`.

## Physical acceptance and two-hour soak — required before release

These require installation of the matched candidate on an idle test tablet and
controlled calls. They are not satisfied by native policy/unit tests or by an APK
build. Keep production untouched until the separate deployment/test step is authorized.

1. Capture logcat, native logs, native call snapshots, per-session receipts and
   screenshots. Record APK/helper commit, signing certificate, boot identity and
   active configuration hash. Keep caller policy, IVR flow, duration cap and privacy
   settings unchanged throughout.
2. Reproduce caller hangup during ACK, hold, dialing, answered, recorder gate, merge
   and conference. Test both hangup orders, duplicate completion, late callbacks,
   process death, helper restart and reboot. Expect no protocol rejection or helper
   restart for healthy hangup, no repeated answer/dial after an uncertain dispatch.
3. Ring the next caller immediately and while recovery is deliberately delayed
   (including more than five seconds). Keep that call ringing. Verify automatic
   answering after readiness, cancellation on caller disappearance/manual answer/
   ambiguous calls/kill switch, and no waiting for the entire phone to become idle.
4. Stall recording writers/finalization; exhaust recording quota; deny a test recording
   path; disconnect the server; throttle uploads. Calls must still work when local
   control storage and privacy are healthy. Unacknowledged audio stays preserved.
   Explicit voicemail failure must follow the configured unavailable branch.
5. Run two hours of representative traffic covering those cases plus successful
   menus, voicemail, operator timeout, connected conversation and repeated normal
   hangups. Include the original maximum-duration expiry and privacy tests. Check
   worker counts, CPU, free space, queued audio, duplicate events and late cross-call
   effects at the beginning and end. Stop on any wrong-call answer/dial/disconnect,
   privacy failure or lost unacknowledged audio.

Measure each caller using `IVRdroidAdmission` logs: `ready_ms`, `answer_request_ms`
and `active_observed_ms`. Target `answer_request_ms - ready_ms <= 1000`. This starts
at the app's verified ready observation; also retain native `RELEASE1` and helper
state timestamps to expose earlier observation/arming delays. Report old-call
disconnect→resource release, release→verified readiness, readiness→answer request,
and answer request→Android active separately. Android-active observation includes
callback delay and is not an exact carrier measurement. Do not turn an injected
delay into a production measurement.

## Deployment and rollback runbook — execute separately

1. Reverify the live authority, checkout commit, Alembic revision, container UID,
   mounts and health. Last verified authority was old-mac at
   `/Users/parhamfatemi/Services/ivrdroid/deploy`; do not assume it is still current.
   Compare its live source with this candidate, including display preferences and
   full caller numbers. Identify the exact current tablet/ADB endpoint and wait for
   verified idle. Capture hashes of the running APK/helper, native module files and
   active flow/configuration. Check there are no unrelated working changes to overwrite.
2. Back up the database, encrypted recording volumes, upload chunks, app data and
   pending tablet spool before any deployment/recovery mutation. Keep the existing
   application UID, package/signature, Android Keystore and encryption keys. An APK
   backup alone cannot restore Keystore-protected pending recordings.
3. Apply the compatible backend image and additive migration first. Verify old v1
   uploads and authenticated playback, and run the permission canary as the actual
   application UID. Keep a copy of the old image reference for rollback.
4. Install the matched signed Android APK and helper while idle using the existing
   project deployment procedures. Verify certificate equality before installation;
   do not uninstall/clear data or re-enroll. Verify source commits and private
   capability receipt, then complete physical acceptance/soak. Deploy the dashboard
   after the compatible backend and device pair.
5. Roll back application images/binaries together while idle if acceptance fails.
   Preserve all additive DB state, per-session journals/receipts, encrypted spools,
   module backups and Keystore. Do not run `alembic downgrade`, delete pending
   source, or replay old commands. A v1 device may keep uploading to the newer
   backend; a v2 device must not upload to an older incompatible backend.

## Historical recovery — execute only after backups and compatible backend

Evidence source:
`ivrdroid-release-artifacts/investigations/20260919-crash-timeline/REPORT.md` and its
46-file checksum index. The CLI pins the index checksum
`da70b6014001a8485325edf16867682e833756ee9f5ddebb2226982dea09ffaf`.

Run inside the application environment as its real UID, with the evidence mounted
read-only and a fresh private backup destination on durable storage:

```sh
python -m app.sept19_recovery --evidence /evidence
# Review the exact dry-run output against the saved incident report.
python -m app.sept19_recovery --evidence /evidence --apply --backup-dir /backups/sept19-recovery-unique
```

The CLI verifies all evidence checksums and the two preserved uploads by decrypting
every encrypted chunk and checking the exact source size/hash. It refuses changed
historical outcomes. Before mutation it copies affected original rows, encrypted
sources/media and evidence, fsyncs them and writes checksums. It corrects only the
three proven remote-hangup outcomes/disconnect times, preserves original values and
provenance, restores saved late operator events, and marks historical notification
receipts consumed. The `HELPER_BUSY` call and its zero IVR duration are untouched.
Re-running is idempotent. It requeues the two complete session uploads; it does not
claim playback verification.

| Session | Preserved source and next action |
| --- | --- |
| `8bece748-b775-42b4-9c2b-beaf5f7c4acf` | 20,246,400 PCM bytes, 105.450 s; source SHA256 `851f3aa72cdf05942ce137e000d8f8fb44d38614f8840d52d326a3ed7d2f73e4`; requeue after authenticated backup. |
| `394d15f4-ad5d-4cf4-a219-ffc62cf4c991` | 20,059,200 PCM bytes, 104.475 s; SHA256 `1e4a9a84634ba8e368500c5d504380b79643557f2503aa6b8fd76c3703a2ffac`; restore four saved operator events without failure-alert replay. |
| `93ad1fcf-9d05-474c-898f-0fdbedf897a0` | Tablet-only encrypted prefix, 74.750 s; retain **partial**. Back up encrypted 14,359,092-byte file, checksum `6bbafb4d6cbea311e090c17f1d8663d347530b61b6c61d08f148c1fa01787107`, plus metadata and Keystore identity before resuming upload. The earlier evidence verified presence/hash, not a complete decrypt/backup. |

The interrupted prefix extends beyond the saved call boundary/grace. Resume it
through the app's authenticated Keystore upload. The backend retains such an upload
as **needs attention** with no destructive acknowledgement. Decode an isolated copy,
verify receipt/frame count/source hash and inspect the extra coverage before any
manual release. Do not extend the call duration to make it pass or mark it complete.

For the complete uploads, wait for worker completion, verify decoded media with
ffmpeg and authenticated browser playback, and check anonymous access is refused.
Retain backups until all three sources are accounted for. Record final hashes,
processing states, decoded durations, playback results and original/applied database
fields in the release evidence. Actual historical recovery is incomplete until those
checks have been performed.
