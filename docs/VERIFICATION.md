# SM-T585 verification record

Date: 2026-08-02

Package: `ai.rx1.ivrdroid` v0.3.1-dev (versionCode 5)

Helper: v0.4.7-dev (versionCode 18)

Device: Samsung SM-T585 (`gtaxllte`)

Runtime: LineageOS 19.1, Android 12 / API 32, Magisk 30.7

## Build checks

- Android unit tests, lint, and debug APK assembly passed.
- Native device-profile, helper-protocol, menu-policy, privacy-policy, mixer-route,
  boot-scoped snapshot, call-safety, Telecom-parser, Telecom-guard, and DTMF tests passed.
- The arm64 helper built with warnings treated as errors using pinned Android NDK
  `25.2.9519653`.
- Independent builds from separate source/build paths produced the same unstripped helper.
- The disabled helper archive passed deterministic ZIP integrity validation.
- Emergency parsing fixtures cover explicit true/false markers, legacy/member spellings,
  whitespace variations, ambiguous markers, and capability-only text.

The local caller-gated APK is intentionally not recorded as a publishable artifact. Its caller
gate comes from ignored `local.properties`. Public builds must explicitly use an empty gate.

## Privacy-race regression history

v0.3.3 could auto-answer and then disconnect without a prompt. Samsung rewrote the call route
while privacy was arming, and one failed immediate mixer readback was treated as permanent
privacy loss. v0.3.4 separated private, corrected, and temporarily contested observations, then
required 500 ms of continuously stable privacy before playback and allowed at most 250 ms of
continuously unverified endpoint state.

Five consecutive v0.3.4 key-2 calls then completed without a helper restart. Each played the main
prompt, detected key 2 on both PCM channels, played the support prompt, ended the call, and
restored audio. A forced worker `SIGKILL` during the main prompt also passed: the independent
guardian kept both endpoints muted, ended the call, restored audio, cleared the snapshot, and the
supervisor returned a replacement helper to `READY`.

## Cold first-call and pre-answer privacy

Later testing exposed a distinct first-call failure: a cold pre-call route could retain
`DMIX_OUT`. Answering on top of that stale injection route produced either an immediate hangup or
an answered call with no prompt. The helper now performs this transaction before the app is
allowed to answer:

1. verify `MODE_NORMAL` and one non-emergency ringing call;
2. hash that call identity;
3. snapshot the actual audited normal/cold route;
4. mute the tablet microphone and speaker;
5. normalize DOUT/mixer to `AIF4IN / Off`;
6. require an independent guardian acknowledgement;
7. publish `WAITING_FOR_CALL`, allowing the app to answer.

Two consecutive v0.4.6 calls starting from the cold `DMIX_OUT` variant completed. Additional
normal sessions also completed without restarting the helper. User-observed results were no
pre-answer microphone leakage, audible prompts, accepted DTMF, and automatic disconnect.

Post-call recovery uses audited normal DOUT/mixer values and the captured endpoint state. It does
not restore a cold `DMIX_OUT` value merely because that was observed before the call.

## Reboot during a live session

A monitor triggered `adb reboot` approximately one second after `PLAYING_MAIN`. Android shutdown
was not immediate: the caller heard the main prompt to completion and the cellular call ended
about 18 seconds after answer. The important recovery boundary occurred on the next kernel boot:

- boot identity changed from prefix `b5a40a9c` to `bc851028`;
- the helper found the unfinished version-5 snapshot from the previous boot;
- it discarded that snapshot without issuing a Telecom hangup or writing the new kernel's mixer
  route;
- it waited for a stable idle/`MODE_NORMAL` system and returned to `READY`;
- no durable snapshot remained;
- the first post-boot allowlisted call played its prompt normally;
- a subsequent complete menu call also passed.

This validates boot-scoped stale-transaction handling. It does not claim that reboot instantly
cuts a cellular call; shutdown timing remains Android/vendor behavior.

## Forced cellular-radio loss

The valid radio-loss test revalidated and terminated the current `rild` and Samsung `cbd`
processes approximately one second into the main prompt. The caller heard the prompt cut midway.
Helper evidence was:

```text
13:49:45.653  Main prompt injection started
13:49:48.444  Call ended while prompt was playing
13:49:48.492  Prompt stopped early; privacy route was restored
```

The helper reported `REMOTE_HANGUP`, returned to `READY`, restored `MODE_NORMAL`, and cleared the
snapshot. Samsung recreated the radio processes; one verified stale `cbd` process stuck in
`stopping` required a targeted `SIGKILL` before init recreated it cleanly. Voice service then
returned to `IN_SERVICE`.

A follow-up normal call played the main prompt, detected key 2, played the support prompt, used
the pinned Telecom hangup, restored audio, and reported `SESSION_COMPLETE`.

This is evidence for forced radio-stack loss and recovery, not physical SIM removal and not a
real carrier outage.

## Emergency and external-call safety

No real emergency call was placed. Current coverage is deliberately synthetic and policy-level:

- explicit false markers such as `emergency_call=false` remain ordinary calls;
- explicit true and legacy/member emergency markers preempt IVRdroid;
- ambiguous standalone emergency metadata fails closed as emergency;
- capability-only text such as `supportsEmergencyCall=true` is not treated as an active
  emergency call;
- multiple calls, a replaced call identity, or persistently unreadable Telecom state preempt the
  IVR;
- preemption releases only mixer state proven to be IVRdroid-owned and never sends the global
  Telecom hangup;
- recovery may send the pinned global hangup only for the same single non-emergency call recorded
  in a valid current-boot snapshot.

A safe Telecom harness remains preferable to placing a real emergency call for live validation.

## Final v0.4.7 live regression

The final allowlisted key-2 call on v0.4.7 produced:

```text
Accepted START_MENU
pre-answer endpoint privacy verified
three bounded Samsung route corrections
500 ms stable private route
main prompt completed
DTMF 2 detected at 0.99 confidence on both channels
support prompt completed
pinned Telecom end-call transaction sent
session snapshot restored and cleared
```

The caller reported that it worked normally. Post-call device state was independently checked:

- helper state: `READY`;
- last result: `SESSION_COMPLETE`;
- Android requested/actual audio mode: `MODE_NORMAL`;
- Telecom call state: idle (`mCallState=0`);
- voice registration: `IN_SERVICE` (`mVoiceRegState=0`);
- persistent `mixer.snapshot`: absent;
- helper v0.4.7 process: alive;
- helper boot marker: absent, so startup is enabled on this audited tablet;
- default dialer: unchanged.

## Remaining validation boundary

The low-level single-call foundation has live coverage for normal repetition, a cold first call,
worker death, reboot during a session, forced radio-stack loss, post-recovery calls, route
restoration, and privacy enforcement. It does not yet have an automated device-side fault suite,
a physical-SIM-removal test, a real carrier-outage test, or a safe live emergency-call harness.
Those gaps must not be described as already tested.

## Current packaged artifacts

```text
7cb13e100b1eb78c26cfc9d6fc68389e7a8e160ab276a8c07e1bba65c1b5dea6  ivrdroid-helper
e27aeb5801c98bbf9322c0cf60d0bdeaeea1c1d2683983c690c6040d2be62df4  IVRdroid-helper-0.4.7-dev-disabled.zip
```

Generated artifacts remain ignored by Git. The synthesized prompts are test fixtures and are not
publication-cleared assets.
