# SM-T585 verification record

Date: 2026-07-29

Package: `ai.rx1.ivrdroid` v0.3.0-dev

Helper: v0.3.4-dev

Device: Samsung SM-T585 (`gtaxllte`)

Runtime: LineageOS 19.1, Android 12 / API 32, Magisk 30.7

## Build checks

- Debug and release Android unit tests passed.
- Android lint completed without findings and the debug APK assembled.
- Native device-profile, helper-protocol, menu-policy, privacy-stability, Telecom-guard, and
  DTMF tests passed.
- The arm64 helper built with warnings treated as errors using pinned Android NDK
  `25.2.9519653`.
- Independent builds from two source/build paths produced the same helper SHA-256.
- The disabled helper archive passed deterministic ZIP integrity validation.
- A publish-safe APK was built with an explicitly empty caller gate and checked against the
  ignored private gate.

## v0.3.3 regression reproduced

Two consecutive allowlisted calls at 15:35:36 and 15:35:47 auto-answered and then disconnected
without playing the menu. In both sessions, early privacy succeeded, Samsung rewrote its route,
and the guardian treated one failed immediate mixer readback as permanent privacy loss. Recovery
ended both calls safely, but the normal path was scheduling-dependent.

The defect was a policy error, not an unsupported audio route: a single 5 ms write/read race was
fatal. v0.3.4 separates three observations—already private, corrected, and temporarily
contended—then:

- requires 500 ms of continuous stable privacy before the first prompt;
- resets that window after a route correction or unexpected startup route;
- tolerates transient contention while retrying every 5 ms;
- enters fail-closed recovery after 250 ms continuously without verified microphone and speaker
  privacy.

The guardian acknowledges stability over a private sequenced socket. The worker cannot start
playback before receiving that acknowledgement.

## Five-call normal-path regression soak

Five consecutive allowlisted calls completed the key-2 path without restarting the helper:

| Call | Session interval | Stable-route result | DTMF 2 dominance | Final result |
|---:|---|---|---:|---|
| 1 | 15:49:32–15:49:46 | 500 ms after 2 corrections | 394 | `SESSION_COMPLETE` |
| 2 | 15:49:58–15:50:12 | 500 ms after 2 corrections | 299 | `SESSION_COMPLETE` |
| 3 | 15:50:23–15:50:37 | 500 ms after 2 corrections | 761 | `SESSION_COMPLETE` |
| 4 | 15:50:45–15:50:59 | 500 ms after 2 corrections | 432 | `SESSION_COMPLETE` |
| 5 | 15:51:07–15:51:22 | 500 ms after 2 corrections | 403 | `SESSION_COMPLETE` |

All five sessions ran in helper PID 7589. Each played the main prompt, detected key 2 on both
PCM channels, played the support prompt, sent the pinned Telecom hangup, and restored the exact
pre-call mixer snapshot. The caller reported no microphone leakage, no tablet-speaker output,
both prompts audible, and automatic disconnect on all five calls.

Observed routes:

```text
main/support prompt  DMIX_OUT / Off / speaker Off / microphone Off
DTMF listening       AIF4IN  / On  / speaker Off / microphone Off
post-call normal     AIF4IN  / Off / speaker On  / microphone Off
```

Final state:

- helper state: `READY`;
- last result: `SESSION_COMPLETE`;
- Android requested/actual audio mode: `MODE_NORMAL`;
- Telecom call list: empty;
- persistent `mixer.snapshot`: absent;
- helper process: alive;
- helper boot marker: `disable` present;
- default dialer: unchanged.

## v0.3.4 forced worker-death recovery

A failure injector validated `/data/adb/ivrdroid/helper.pid`, waited for `PLAYING_MAIN`, allowed
roughly one second of prompt playback, and sent `SIGKILL` only to the menu worker.

The caller heard the prompt stop, heard no tablet microphone or speaker leakage, and was
disconnected automatically.

Device evidence:

```text
15:55:10.046  Answer request sent
15:55:10.387  Early session privacy active
15:55:10.610  Privacy re-applied after vendor route update
15:55:10.811  Privacy re-applied after vendor route update
15:55:11.313  Privacy stable for 500 ms after 2 corrections
15:55:11.369  Main prompt injection started
15:55:12.xxx  PID 7589 killed after target revalidation
15:55:12.598  RECOVERING with speaker Off and microphone Off
15:55:12.621  Guardian sent pinned Telecom end-call transaction
15:55:14.541  Normal route restored
15:55:14.745  Replacement helper PID 22127 returned READY
```

During `PLAYING_MAIN` and `RECOVERING`, both `SPK Switch` and `Main Mic Switch` remained `Off`.
The caller heard about one second of the prompt, then silence, no local audio leakage, and an
automatic disconnect.

Final state:

- helper state: `READY`;
- last result: `RECOVERED_AND_ENDED`;
- Android requested/actual audio mode: `MODE_NORMAL`;
- Telecom call list: empty;
- persistent `mixer.snapshot`: absent;
- normal mixer route restored;
- replacement helper process: alive.

## Regression history

- v0.3.2 used a one-time mute and could restore the microphone before a failed hangup.
- v0.3.3 added session-wide guardian enforcement and terminate-before-restore recovery, but made
  one contested mixer sample fatal.
- v0.3.4 retains the fail-closed ordering, adds bounded contention handling, and gates playback
  on a continuous stable-privacy window.

## Packaged artifacts

```text
e26ae84ffe55f58f48c1e8e593f197cde61f0cab935308d5974bab908f9f5d77  app-debug.apk
cd56dea3821903a8a13c3fb549fc116ab0e5601df00819e9bf00f564b91527fe  ivrdroid-helper
a786fa9b06081d430783af6d5dda194c58814f384f833ef420c4d8622af7241f  IVRdroid-helper-0.3.4-dev-disabled.zip
```

The recorded APK is the publish-safe empty-gate build, not the private APK installed for tablet
testing. Generated artifacts remain ignored by Git. The synthesized prompts are test fixtures
and are not publication-cleared assets.
