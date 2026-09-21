# IVRdroid privileged helper

This helper is intentionally device-specific. It refuses to serve unless the Android identity,
audio controls, root-owned prompts, app bridge ownership, and audited ROM match the pinned
Samsung SM-T585 profile.

Helper 0.10.3 keeps command results such as `REJECTED_BUSY` in `last_result` and
publishes only call outcomes to `call-outcome` and `call-results/<uuid>.outcome`.
Discarding a queued request during cleanup cannot overwrite the finished call's
`REMOTE_HANGUP` or other terminal result. The `END1` wire format and the 0.10.2 app
and server contract are unchanged; this patch can be deployed to the helper alone.

For a UUID-correlated `START_MENU` request it:

1. claims the request while Telecom still exposes one ringing non-emergency call;
2. records that call's stable identity and snapshots the observed cold/normal mixer route;
3. disables both physical endpoints and normalizes pre-answer routing to `AIF4IN / Off`;
4. waits for the independent guardian to verify privacy before publishing `WAITING_FOR_CALL`;
5. lets the app answer, then verifies `MODE_IN_CALL`, the same call identity, and 500 ms of
   continuously stable privacy;
6. plays root-owned prompts through TinyALSA;
7. captures 48 kHz stereo PCM16 in memory for ordinary DTMF processing; a signed V4.1
   `BARGE_IN` collector keeps capture continuous while a supervised child plays the prompt,
   stops only for a configured digit, and starts the full timeout after playback completes;
8. only for a signed V3 `record_message` instruction, plays a built-in beep and writes an
   owner-only temporary WAV until the configured finish key, hard maximum, or caller hangup;
9. trims the buffered finish tone, atomically hands a finalized WAV plus bounded receipt to the
   app, and deletes partial audio on capture/privacy/preemption failure;
10. for a signed V4 `external_call`, requests an app-owned outgoing leg and records verified
    conference audio to one continuous PCM file in an independent writer process;
11. follows the compiled success, not-connected, system-failure, or unavailable branch, while a
    caller-first hangup always terminates without taking a flow branch;
12. ends only the owned call through the Telecom binder transaction pinned to this ROM;
13. restores audited post-call routing and the captured endpoint state only after the call ends.

Samsung can contest a mixer write while it finalizes the in-call route. The guardian retries
that bounded contention but fails closed after 250 ms without verified microphone and speaker
privacy. Prompt playback cannot start before the guardian acknowledges the stable window.
Recording sends fixed heartbeats into a three-second watchdog and has a separate 30-second
finalization deadline, so a long configured maximum cannot conceal a dead worker.

The version-5 durable snapshot is scoped to one kernel boot, random session, stable call identity,
and four mixer values. If the worker dies, the guardian keeps the call private and attempts
termination first. A failed hangup leaves the snapshot in place so the supervisor can retry
same-boot recovery. A previous-boot snapshot is discarded without ending a call or writing the
new kernel's mixer route.

Emergency, multiple, replacement, and persistently unverified call states preempt the IVR. In
those cases the helper releases only mixer state it can prove it owns and does not issue a global
Telecom hangup. The app supplies no shell text, mixer names, values, paths, prompts, transaction
numbers, or menu targets.

The app-private bridge exposes a mode-0700 `recordings/` inbox and a bounded capacity file. The
helper accepts only canonical UUID filenames, rejects symlinks, wrong ownership, and group/other
permissions, keeps voicemail under its 512 MiB limit, and requires V2 capacity telemetry before
using the separate 4 GiB conversation budget. Continuous writers enforce growing byte budgets,
the filesystem reserve and processing workspace at least once per second. Legacy `START_MENU\n` remains
accepted for V1/V2 rollback; recording and external calls require
`START_MENU <canonical-call-uuid>\n`.

## V4 call-control wire contract

The app creates `bridge/call_control.request`, `bridge/call_control.status`, and
`bridge/call_control.recording_ack` as app-UID mode-0600 regular files before helper startup.
Every direction uses atomic replacement, ASCII, exactly one trailing LF, and at most 512 bytes.
Helper-to-app requests are:

```text
IVRDROID_CALL_CONTROL_V2 DIAL <call_uuid> <revision_id> <block_uuid> <request_seq> <boot_uuid> <elapsed_ms> <phone> <answer_timeout_ms>\n
IVRDROID_CALL_CONTROL_V2 RECORDER_READY <call_uuid> <revision_id> <block_uuid> <request_seq> <boot_uuid> <elapsed_ms>\n
IVRDROID_CALL_CONTROL_V2 CANCEL <call_uuid> <revision_id> <block_uuid> <request_seq> <boot_uuid> <elapsed_ms> <reason>\n
```

App-to-helper status is:

```text
IVRDROID_CALL_CONTROL_V2 <phase> <call_uuid> <revision_id> <block_uuid> <seq> <boot_uuid> <elapsed_ms> <reason>\n
```

`phase` is `ACK`, `CALLER_HELD`, `DIALING`, `OPERATOR_ANSWERED`, `MERGING`, `CONFERENCED`,
`COMPLETED`, `NOT_CONNECTED`, or `SYSTEM_FAILURE`. Request and status sequence numbers strictly
increase for their correlated same-boot stream, and boot-elapsed time cannot decrease. Active
states are replaced every second and become unsafe after three seconds. Setup and merge each have
separate ten-second watchdogs; the configured answer timeout starts only on the first correlated
`DIALING`. `RECORDER_READY` is legal only after bounded in-memory PCM pre-roll is working and gates
merge. No conversation file is created before a correlated `CONFERENCED` status.
`COMPLETED ... OPERATOR_HANGUP` takes the completed branch; `COMPLETED ... CALLER_HANGUP` finalizes
valid audio and terminates without a branch. `SYSTEM_FAILURE ... CLEANUP_TIMEOUT` is likewise a
no-branch fail-closed outcome.

Cleanup is outcome-correlated. An answer deadline publishes `CANCEL ... ANSWER_TIMEOUT` and accepts
only `NOT_CONNECTED ... ANSWER_TIMEOUT`; generic helper cancellation publishes
`CANCEL ... HELPER_CANCELLED` and accepts only `SYSTEM_FAILURE ... HELPER_CANCELLED`. A different
terminal kind or reason is a protocol failure, not a substitute flow outcome. The guardian remains
in owned-dialing mode through cleanup. Before any completed, not-connected, or system-failure
branch, the helper independently requires the original snapshotted Telecom identity to be the sole
safe call continuously for one second. An unverified observation resets that window within a fixed
bound; idle, emergency, multiple, or replacement-call evidence fails closed. Caller-first hangup
and cleanup timeout never take a flow branch.

Capacity V2 is exactly:

```text
IVRDROID_RECORDING_CAPACITY_V2 <voicemail_bytes> <voicemail_count> <conversation_bytes> <conversation_count> <filesystem_free_bytes>\n
```

### Legacy recording compatibility

Legacy conversation files use `<recording_uuid>.<five-digit-segment-index>.wav` with a matching receipt.
Receipts have `version:2`, `kind:"conversation"`, the shared logical `recording_id`, call/revision/
block correlation, equal `sequence` and `segment_index`, UTC capture time, duration, byte size,
SHA-256, and `partial`. Stop reasons are `segment_boundary`, `operator_hangup`, `caller_hangup`,
or `recording_failure`; only `recording_failure` has `partial:true`.

The legacy app protocol acknowledges durably encrypted WAV/receipt pairs:

```text
IVRDROID_CONVERSATION_HANDOFF_V1 <call_uuid> <revision_id> <block_uuid> <recording_uuid> <segment_index> <boot_uuid> <elapsed_ms> <OK|FAILED> <reason>\n
```

`segment_index` is the ordered stream sequence (starting at zero), elapsed time cannot decrease,
`OK` requires reason `-`, and `FAILED` requires a bounded uppercase reason. Version 0.10.0 no longer
uses this acknowledgment or timed rotation for new conversation recordings. Both recorders use
`audio.pcm`, `continuous.context`, and `continuous.sealed`; see
[the continuity contract](../docs/CALL_CONTINUITY.md) for recovery, encryption and upload behavior.

TinyALSA is vendored at commit `9fab97ca07184371ecad81154d1dadb09d0fa7cf` under its BSD license.
See `third_party/tinyalsa/NOTICE`.

## Build and test

From the repository root:

```sh
./helper/tests/run.sh
./scripts/build-helper.sh
./scripts/check-helper-reproducibility.sh
./scripts/package-helper.sh
```

The arm64 build is pinned to Android NDK `25.2.9519653`. Warnings are errors. Host tests cover
device identity, the command/status protocol, menu policy, privacy-stability timing, Telecom
dump parsing and hangup guards, all twelve DTMF keys, prompt-like audio mixed with DTMF, multiple
DTMF rejection cases, V3/V4/V4.1 tape validation, finish-key enabled/disabled behavior,
maximum/hangup finalization, multi-recording flows, V4 call-control grammar/routing/watchdogs,
prompt digit suppression, full-timeout reset, playback-child reaping, outcome-correlated cleanup, original-caller
stability gating, segmented-receipt policy, DTMF trimming,
unavailable storage, partial cleanup, unsafe files, watchdog recovery, and call preemption.

## Module layout

```text
/data/adb/modules/ivrdroid_helper/
  disable
  customize.sh
  module.prop
  service.sh
  bin/ivrdroid-helper
  prompts/
    main-menu.wav
    sales-unavailable.wav
    support-unavailable.wav
    operator-unavailable.wav
```

The packaged `disable` marker is intentional. Installing the module must not silently make an
experimental audio path persistent across reboot. It may be removed on a specifically audited
device only after the recovery suite below passes.

`customize.sh` explicitly assigns executable permissions to the supervisor and helper during
Magisk installation. ZIP permission metadata alone is not portable across Magisk installers.

Before a live call, run the non-mutating self-test as root:

```sh
/data/adb/modules/ivrdroid_helper/bin/ivrdroid-helper --self-test
```

It validates the exact device/build, bridge ownership, prompt files, and mixer topology, then
reports the current values without changing them. Start `service.sh` manually and confirm helper
state `READY`.

For the pinned SM-T585, live tests cover worker death, reboot during a session, a complete first
post-boot call, forced radio-stack loss during a prompt, carrier re-registration, and a complete
post-loss call. Emergency/multiple/replacement/unverified-call behavior is covered by parser and
state-machine fixtures; no real emergency number was called. A new device profile must repeat
those tests before boot startup is enabled. Physical SIM removal or a real carrier outage remains
an additional deployment-specific test, not a claim made by the current record.

No automated test establishes real caller-speech quality. Before enabling V3 in production, a
consented ten-second physical call must prove intelligible caller speech, no tablet microphone or
ambient capture, no prompt bleed, safe `MODE_NORMAL` restoration with no stale mixer snapshot,
successful upload, and dashboard playback.

The supervisor waits for Android boot completion and a stable idle/`MODE_NORMAL` system, applies
bounded restart backoff, and recreates `disable` after seven consecutive short startup failures.
