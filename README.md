# IVRdroid

IVRdroid is an experimental cellular IVR appliance for dedicated rooted Android devices. It
keeps Android's normal dialer in place while a separate call-screening app applies caller policy
and a narrowly scoped root helper handles the device-specific audio path.

The Android application ID is `ai.rx1.ivrdroid`.

## Current status

The current build is a verified fixed three-option IVR prototype:

- Every incoming call is allowed through call screening.
- Only an exact, locally configured E.164 caller is auto-answered.
- The app submits one fixed `START_MENU` request; it sends no paths, mixer values, or shell text
  to root.
- The root runtime plays a main prompt, captures caller audio directly from TinyALSA, detects
  DTMF with a stereo Goertzel detector, and handles keys 1, 2, and 0.
- A valid selection plays its terminal prompt and ends the call through the Telecom binder
  transaction pinned to the audited ROM.
- Prompt playback snapshots the three affected mixer controls and restores their exact values
  afterward.
- An independent 15-second watchdog restores the mixer after worker death or timeout.
- No captured call audio is written to storage.

The complete key-2 path was verified live: answer, main prompt, DTMF detection, support prompt,
mixer restoration, and automatic disconnect.

This is not yet a general menu editor, PBX, or multi-device release. The verified runtime is a
Samsung SM-T585 running LineageOS 19.1, Android 12 / API 32.

## Build

Use Android Studio, or:

```sh
./gradlew test lint assembleDebug
```

The debug APK is written to `app/build/outputs/apk/debug/app-debug.apk`.

For a private, caller-gated test build, add this ignored local setting:

```properties
ivrdroid.testCallerE164=+15551234567
```

If the setting is absent, the built app has an empty allowlist and cannot auto-answer. Never
publish an APK built with a private caller number.

## Components

- `app/`: unprivileged Android call screening, exact caller gate, and bounded request writer.
- `helper/`: device-pinned native menu, TinyALSA prompt/capture path, DTMF detector, mixer
  transaction, watchdog, and disabled-by-default Magisk layout.
- `docs/ARCHITECTURE.md`: trust boundary and the verified call sequence.
- `docs/VERIFICATION.md`: build and live-device evidence.

## Safe device test

1. Install the private debug APK.
2. Open IVRdroid and grant Answer calls and Contacts access.
3. Enable gated call testing and approve Android's call-screening role.
4. Confirm the normal dialer remains `com.android.dialer`.
5. Install and self-test the helper as described in `helper/README.md`.
6. Confirm the app reports `Audio bridge: READY`.
7. Call from the exact configured number.
8. Wait for the complete main prompt, then press 1, 2, or 0 once.
9. Verify the selected terminal prompt plays and the call disconnects.
10. Confirm helper status `SESSION_COMPLETE`, Android audio mode `MODE_NORMAL`, and no
    `mixer.snapshot` file.
11. Call from another number and verify the normal dialer rings without IVRdroid answering.

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

Before a public release, add a project license, replace the macOS-synthesized test prompts with
redistributable recordings, make menus and prompts configurable through a validated format, add
native menu-policy tests, remove the unused foreground-service experiment, exercise interruption
and reboot cases, and add explicitly audited device profiles.

Vendored TinyALSA retains its BSD license.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the current system boundary.
