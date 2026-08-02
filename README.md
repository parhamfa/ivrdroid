# IVRdroid

IVRdroid is an experimental cellular IVR appliance for dedicated rooted Android devices. It
keeps Android's stock dialer in place while a call-screening app applies caller policy and a
narrowly scoped native helper owns the audited cellular audio path.

The Android application ID is `ai.rx1.ivrdroid`.

## Current status

The current build is a verified, single-call, three-option IVR foundation:

- Every incoming call is allowed through call screening.
- Only an exact E.164 caller configured in an ignored local build setting is auto-answered.
- The app queues one fixed `START_MENU` request and waits for the helper to claim the session
  before asking Telecom to answer.
- The helper accepts no paths, mixer values, transaction numbers, or shell text from the app.
- The helper plays a main prompt, captures caller audio directly through TinyALSA, detects DTMF
  with a stereo Goertzel detector, and handles keys 1, 2, and 0.
- Before answer, the helper snapshots the observed cold/normal route, mutes both physical
  endpoints, normalizes the telephony route to audited `AIF4IN`, and obtains an independent
  guardian acknowledgement. Android is not allowed to answer before that handshake completes.
- The tablet microphone and speaker remain muted for the entire IVR session. A 5 ms independent
  guardian re-applies that mute when Samsung rewrites the call route, tolerates bounded mixer
  contention, and withholds prompt playback until privacy has remained stable for 500 ms.
- One boot-scoped durable snapshot covers all four audited mixer controls, a random session ID,
  and the current Telecom call identity. Normal completion restores the audited post-call route
  only after the call has ended.
- Failure recovery keeps the call private, attempts the pinned Telecom hangup, and preserves the
  snapshot for a supervisor retry if the first hangup does not complete.
- Emergency, additional, replaced, or persistently unverified calls make the helper release audio
  ownership without issuing a global Telecom hangup.
- No captured call audio is written to storage.

Helper v0.4.7 is installed on the audited tablet with boot startup enabled. Live validation now
covers repeated normal calls, a cold `DMIX_OUT` first-call baseline, worker death, reboot during
an unfinished session, forced cellular-radio loss during prompt playback, carrier
re-registration, and a complete post-loss key-2 call. Every completed call returned to
`SESSION_COMPLETE`, `MODE_NORMAL`, an idle Telecom state, and no durable snapshot. Emergency
handling is validated with Telecom-dump fixtures and state-machine tests only; no real emergency
number was called.

The repeated-call and first-call-after-boot tests remain mandatory. Earlier helpers could either
treat one Samsung mixer race as fatal or carry a cold `DMIX_OUT` route through answer, producing
an immediate hangup or an answered call with no prompt. v0.4.7 requires a continuously stable
privacy window and normalizes the pre-answer route before the app receives permission to answer.

This is not yet a general menu editor, PBX, concurrent-call system, or multi-device release. The
verified runtime is a Samsung SM-T585 running LineageOS 19.1, Android 12 / API 32.

## Build

Run the complete local validation:

```sh
./scripts/check.sh
```

That runs Android unit tests, lint, APK assembly, native host tests, the pinned arm64 build, and a
cross-directory reproducibility check. Package the disabled helper module with:

```sh
./scripts/package-helper.sh
```

The debug APK is written to `app/build/outputs/apk/debug/app-debug.apk`.

For a private caller-gated test build, add this ignored local setting:

```properties
ivrdroid.testCallerE164=+15551234567
```

If the setting is absent, the app has an empty allowlist and cannot auto-answer. Never publish an
APK built with a private caller number.

## Components

- `app/`: unprivileged call screening, exact caller gate, helper-claim handshake, and bounded
  request writer.
- `helper/`: fixed native menu, device profile, TinyALSA prompt/capture path, DTMF detector,
  boot-scoped mixer transaction, privacy guardian, Telecom guard, and disabled-by-default Magisk
  package layout.
- `scripts/`: deterministic helper build, reproducibility, validation, and packaging tools.
- `docs/ARCHITECTURE.md`: trust boundary and verified call/recovery sequence.
- `docs/VERIFICATION.md`: current build and live-device evidence.

## Safe device test

1. Install a private caller-gated debug APK.
2. Open IVRdroid, grant Answer calls and Contacts access, enable gated testing, and approve the
   call-screening role.
3. Confirm `com.android.dialer` remains the default dialer.
4. Install the disabled helper, run its non-mutating self-test, and start it manually.
5. Confirm IVRdroid reports `Audio bridge: READY`.
6. Call from the exact configured number.
7. Verify the tablet microphone and speaker are silent from answer onward.
8. Wait for the full main prompt, then press 1, 2, or 0 once.
9. Verify the terminal prompt plays and IVRdroid disconnects the call.
10. Repeat the complete path at least five times without restarting the helper.
11. Confirm `SESSION_COMPLETE`, `MODE_NORMAL`, an empty Telecom call list, and no
    `/data/adb/ivrdroid/mixer.snapshot`.
12. Call from another number and verify the stock dialer rings without IVRdroid answering.
13. Before enabling boot startup on a new device profile, repeat worker-death, reboot-during-call,
    and cellular-loss recovery tests.

The caller-ID allowlist limits accidental interference; it is not authentication and caller ID
can be spoofed.

## Compatibility

The first profile is narrowly pinned to:

- Samsung SM-T585 (`gtaxllte`)
- LineageOS 19.1
- Android 12 / API 32
- the exact fingerprint, display ID, PCM topology, mixer controls, and Telecom transaction
  recorded for the audited tablet

The helper refuses to serve after a ROM identity change. Another model, build, or update needs a
fresh audio and Telecom audit.

## Publication note

Before a public release, add a project license, replace the synthesized test prompts with
publication-cleared recordings, replace the fixed menu with a validated configuration format,
automate the current fault-injection suite, validate physical-SIM removal if that deployment
scenario matters, and add explicitly audited device profiles.

Vendored TinyALSA retains its BSD license.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the current system boundary.
