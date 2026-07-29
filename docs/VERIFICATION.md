# SM-T585 verification record

Date: 2026-07-29

Package: `ai.rx1.ivrdroid` v0.3.0-dev

Helper: v0.3.3-dev

Device: Samsung SM-T585 (`gtaxllte`)

Runtime: LineageOS 19.1, Android 12 / API 32, Magisk 30.7

## Build checks

- Debug and release Android unit tests passed.
- Android lint completed without findings and the debug APK assembled.
- Native device-profile, helper-protocol, menu-policy, Telecom-guard, and DTMF tests passed.
- The arm64 helper built with warnings treated as errors using pinned Android NDK
  `25.2.9519653`.
- Independent builds from two source/build paths produced the same helper SHA-256.
- The disabled helper archive passed deterministic ZIP integrity validation.
- A publish-safe APK was built with an explicitly empty caller gate and checked against the
  ignored private gate.

## Normal live call

One allowlisted call completed the key-2 path:

```text
15:15:51.316  Answer request sent
15:15:51.664  Early session privacy active
15:15:51.877  Privacy re-applied after vendor route update
15:15:52.077  Privacy re-applied after vendor route update
15:15:52.195  Session privacy stabilized
15:15:52.265  Main prompt injection started
15:15:58.746  Main prompt completed
15:15:58.831  DTMF capture started
15:16:00.233  Digit 2 detected, dominance 779.75/779.75
15:16:00.298  Support prompt injection started
15:16:03.242  Support prompt completed
15:16:03.347  Pinned Telecom end-call transaction sent
15:16:05.223  Normal route restored and snapshot cleared
```

The caller reported no microphone leakage, no tablet-speaker output, both prompts audible, and
automatic disconnect.

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

## Forced worker-death recovery

A failure injector validated `/data/adb/ivrdroid/helper.pid`, waited for `PLAYING_MAIN`, allowed
roughly one second of prompt playback, and sent `SIGKILL` only to the menu worker.

The caller heard the prompt stop, heard no tablet microphone or speaker leakage, and was
disconnected automatically.

Device evidence:

```text
15:18:04.905  Early session privacy active
15:18:05.132  Privacy re-applied after vendor route update
15:18:05.335  Privacy re-applied after vendor route update
15:18:05.536  Main prompt injection started
15:18:06.xxx  Worker killed
15:18:06.759  Guardian sent pinned Telecom end-call transaction
15:18:11.008  First hangup verification timed out; privacy and snapshot retained
15:18:11.093  Supervisor found the unfinished mixer transaction
15:18:11.172  Stale-transaction recovery retried the Telecom hangup
15:18:12.965  Helper returned READY
```

During `PLAYING_MAIN`, `RECOVERING`, and the retry interval, both `SPK Switch` and
`Main Mic Switch` remained `Off`. The first binder transaction did not complete, so the
supervisor retry—not the first guardian attempt—finished termination. This is an expected
fail-closed recovery path, not evidence that the binder call is perfectly reliable.

Final state:

- helper state: `READY`;
- last result: `RECOVERED_AND_ENDED`;
- Android requested/actual audio mode: `MODE_NORMAL`;
- Telecom call list: empty;
- persistent `mixer.snapshot`: absent;
- normal mixer route restored;
- replacement helper process: alive.

## Regression found and contained

Helper v0.3.2 attempted a one-time mute as soon as the first in-call mixer baseline appeared.
Samsung rewrote that route later, causing the helper to reject prompt playback. Its old recovery
ordering restored the microphone before attempting hangup, which exposed background audio on
failed sessions.

The device was rolled back to v0.3.1 immediately. v0.3.3 replaced the one-time mute with
session-wide guardian enforcement and changed recovery to terminate first and restore second.
Both the normal and forced-death tests above validate those changes.

## Packaged artifacts

```text
e26ae84ffe55f58f48c1e8e593f197cde61f0cab935308d5974bab908f9f5d77  app-debug.apk
90c2e18d6fa9cccb5d966eb091ab906104cdf969bd2ff80816e0a2df213cb3a6  ivrdroid-helper
0c638847dc8cc0cd252db3f85bc66be7bf21b4249779d54a366ee972f7eecf2c  IVRdroid-helper-0.3.3-dev-disabled.zip
```

The recorded APK is the publish-safe empty-gate build, not the private APK installed for tablet
testing. Generated artifacts remain ignored by Git. The synthesized prompts are test fixtures
and are not publication-cleared assets.
