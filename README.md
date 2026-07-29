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
- The tablet microphone and speaker remain muted for the entire IVR session. A 5 ms independent
  guardian re-applies that mute when Samsung rewrites the call route.
- One durable snapshot covers all four audited mixer controls for the whole session. Normal
  completion restores it only after the call has ended.
- Failure recovery keeps the call private, attempts the pinned Telecom hangup, and preserves the
  snapshot for a supervisor retry if the first hangup does not complete.
- No captured call audio is written to storage.

Helper v0.3.3 was verified live on both a complete key-2 call and a forced worker-death call. The
normal call played both prompts and disconnected. The failure test interrupted playback, kept
both local audio paths muted, retried a timed-out Telecom hangup, restored the normal route, and
reported `RECOVERED_AND_ENDED`.

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
  session-wide mixer transaction, privacy guardian, Telecom guard, and disabled Magisk layout.
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
10. Confirm `SESSION_COMPLETE`, `MODE_NORMAL`, an empty Telecom call list, and no
    `/data/adb/ivrdroid/mixer.snapshot`.
11. Call from another number and verify the stock dialer rings without IVRdroid answering.

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
exercise reboot/SIM-loss/emergency-call failure cases, and add explicitly audited device
profiles.

Vendored TinyALSA retains its BSD license.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the current system boundary.
