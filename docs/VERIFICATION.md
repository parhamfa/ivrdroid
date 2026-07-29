# SM-T585 verification record

Date: 2026-07-28  
Package: `ai.rx1.ivrdroid` v0.2.1-dev  
Helper: v0.2.3-dev  
Device: Samsung SM-T585 (`gtaxllte`)  
Runtime: LineageOS 19.1, Android 12 / API 32, Magisk 30.7

## Build checks

- Ten debug unit tests and the same ten release unit tests passed.
- Android lint completed without findings.
- The native helper built for `arm64-v8a` with warnings treated as errors.
- Native DTMF tests covered all `123456789*0#` keys, asymmetric levels, short-tone rejection,
  channel disagreement, and non-DTMF tones.
- The helper archive passed ZIP integrity validation.
- The publish-safe APK was checked and does not contain the private test caller.
- The private installed APK was separately checked and does contain its local gate.

## Fixed-prompt and watchdog bring-up

The initial root bridge injected a complete 5.64-second prompt into a live cellular call and
restored the exact mixer snapshot.

A forced-worker-death test sent `SIGKILL` 1.25 seconds after playback began. The independent
watchdog restored the route before the call ended, cleared the durable snapshot, and allowed the
supervisor to launch a new helper.

## Live DTMF calibration

A read-only PCM 0:0 capture was streamed to an analyzer without saving call audio. Remote digits
1, 5, and 9 were decoded identically on both stereo channels, proving that the caller's tones
reach the audited digital capture path.

The native detector then decoded key 2 in the complete menu test with the following logged
metrics:

```text
Detected caller DTMF digit 2
left confidence=1.00
right confidence=1.00
dominance=268.06/268.33
```

## Complete live IVR result

The allowlisted incoming call completed this sequence:

```text
Allowlisted incoming call matched
Answer request sent
Queued the fixed privileged menu handoff
Starting the fixed root-owned IVR menu session
Mixer transaction applied; playing main prompt
Prompt completed and mixer snapshot was restored
Listening for caller DTMF on audited PCM 0:0
Detected caller DTMF digit 2
Mixer transaction applied; playing support prompt
Prompt completed and mixer snapshot was restored
Sending the pinned Telecom end-call transaction
Result: Parcel(... 00000001)
```

The caller heard the complete main menu, pressed 2, heard the support terminal prompt, and the
tablet disconnected the call automatically.

Immediately afterward:

- helper status: `SESSION_COMPLETE`;
- Android audio mode: `MODE_NORMAL`;
- telephony call state: idle;
- persistent `mixer.snapshot`: absent;
- helper process: alive;
- helper boot marker: `disable` present;
- call-screening holder: `ai.rx1.ivrdroid`;
- default dialer holder: `com.android.dialer`.

## Rejected designs observed live

- Android rejected an app-started background foreground service with
  `ForegroundServiceStartNotAllowedException`.
- The long-running root helper's `cmd activity` and `cmd input` shell-command transactions
  returned `FAILED_TRANSACTION`.

The final path does not depend on either mechanism. The fixed menu runs in the helper and call
termination uses the ROM-pinned native Telecom binder transaction.

## Packaged artifacts

```text
b25c1c597a083278de70dc4965f7b323c95c6794f961e97caecca1a7fea49db6  IVRdroid-0.2.1-dev-debug.apk
00d879a162ca07a60a6c28ed7c4b4068139dcd332f8dc136ef758409715e6da4  IVRdroid-helper-0.2.3-dev-disabled.zip
91d0c6f761c49e97cf5e42ea778f2b351ac940b7039b953e2df88c17ad0444cc  ivrdroid-helper
```

The packaged APK has an empty caller allowlist and cannot auto-answer. The installed private test
APK is intentionally separate. The bundled synthesized prompts are test fixtures and are not
publication-cleared assets.
