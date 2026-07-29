# IVRdroid architecture

## Scope

IVRdroid currently handles one active cellular call and one fixed menu session at a time. That is
an explicit appliance constraint, not an attempt to emulate a concurrent PBX on one cellular
line.

## Trust boundary

### Unprivileged Android app

- Obtains Android's call-screening role with explicit user consent.
- Immediately allows every incoming call, preserving the stock dialer and call log.
- Auto-answers only an exact caller compiled into a private test build.
- Atomically writes one exact `START_MENU` request into its owner-only bridge directory.
- Waits for helper state `WAITING_FOR_CALL` before asking Telecom to answer.
- Never executes root commands or supplies paths, prompts, mixer values, menu targets, or
  Telecom transaction numbers.

### Privileged device runtime

- Is installed separately and remains disabled at boot during development.
- Resolves the app-private bridge owner and accepts only one bounded command body.
- Pins manufacturer, model, codename, API, fingerprint, display ID, PCM topology, four mixer
  controls, root-owned prompts, and the ROM-specific Telecom transaction.
- Owns the fixed menu session so Android cannot kill a background app component halfway through
  a call.
- Keeps captured PCM in memory and logs only detected digits and detector metrics.
- Uses a root-owned durable snapshot plus an independent guardian for restoration after worker
  death.

The helper does not decide which caller qualifies. The app cannot access ALSA or Telecom's
private binder surface.

## Normal call sequence

```text
incoming cellular call
  -> CallScreeningService responds ALLOW
  -> caller mismatch: stop; com.android.dialer handles the call
  -> exact match: app writes START_MENU
  -> helper claims the request and publishes WAITING_FOR_CALL
  -> app requests TelecomManager.acceptRingingCall(AUDIO_ONLY)
  -> helper polls for the audited in-call mixer baseline every 2 ms
  -> durable four-control snapshot
  -> microphone Off + speaker Off
  -> guardian enforces both mute controls every 5 ms
  -> helper confirms MODE_IN_CALL and exactly one safe Telecom call
  -> inject root-owned main prompt while session privacy remains active
  -> capture PCM 0:0 at 48 kHz stereo PCM16
  -> stereo Goertzel detector emits one DTMF digit or an 8-second timeout
  -> 1: sales prompt; 2: support prompt; 0: operator prompt
  -> invalid digit or timeout: replay main menu, at most two retries
  -> terminal prompt
  -> pinned ITelecomService endCall transaction while privacy remains active
  -> verify Telecom has no call and Android leaves MODE_IN_CALL
  -> stop guardian
  -> restore normal route and clear snapshot
```

The complete key-2 branch is verified on the audited tablet.

## Why the menu session is root-owned

An earlier app-owned design attempted to start a foreground service from
`CallScreeningService`. Android 12 rejected it with
`ForegroundServiceStartNotAllowedException`. A fixed root launch through Android's `cmd`
shell-command transport also returned `FAILED_TRANSACTION` from the long-running helper.

The current design therefore keeps caller policy in Android and the short audio/menu session in
the already-required device helper. A future configurable release should pass a validated,
bounded menu description to a small native policy engine. It must not add a general root RPC or
interpolate configuration into shell commands.

## Helper protocol

The app-private bridge is:

```text
/data/user/0/ai.rx1.ivrdroid/files/bridge/
  command.request
  status
  last_result
```

The sole accepted request is:

```text
START_MENU\n
```

The helper rejects wrong ownership, symlinks, unsafe modes, oversized files, any other body,
device-property mismatches, altered mixer control types, an unexpected in-call baseline, and
unsafe prompt files.

## Session privacy and audio profile

The audited route uses ALSA card 0, playback/capture device 0, 48 kHz, stereo, signed PCM16.

The observed in-call baseline is:

```text
AudioMixer CH2 DOUT Select    AIF4IN
AudioMixer CH2 Mixer En       On
SPK Switch                    On
Main Mic Switch               On
```

The private listening route is:

```text
AudioMixer CH2 DOUT Select    AIF4IN
AudioMixer CH2 Mixer En       On
SPK Switch                    Off
Main Mic Switch               Off
```

Prompt injection temporarily uses:

```text
AudioMixer CH2 DOUT Select    DMIX_OUT
AudioMixer CH2 Mixer En       Off
SPK Switch                    Off
Main Mic Switch               Off
```

One durable version-2 snapshot stores all four original values. Android can rewrite the route
more than once after answering, so the guardian holds microphone and speaker Off throughout the
session instead of relying on a one-time write.

The detector analyzes 25 ms frames on both channels, requires the same candidate on each,
requires two stable frames, rearms after two quiet frames, and requires cellular-calibrated
dominance. Synthetic tests cover all twelve telephone keys, asymmetric levels, short tones,
channel disagreement, and non-DTMF tones.

## Failure recovery

The guardian has phase-specific deadlines and a 75-second total ceiling. It inherits no command
input and changes only the two audited privacy controls during a session.

If the worker dies, times out, or reports an audio/capture/termination failure:

1. retain or re-establish the private route;
2. attempt the guarded single-call Telecom hangup;
3. restore the normal route only after the call is gone;
4. keep the durable snapshot if hangup fails;
5. let the supervisor restart the helper and retry stale-transaction recovery.

This ordering is deliberate. Restoring the call microphone before a failed hangup would expose
background audio to the caller.

## Call termination

This ROM has no usable `cmd telecom` implementation. Its framework identifies
`ITelecomService.Stub.TRANSACTION_endCall` as transaction 33 with one calling-package string.
The helper invokes that exact binder transaction through Android's native `service` client.

Before invoking a global hangup, the helper parses the live Telecom dump twice and requires
exactly one non-emergency call. It then verifies both an empty Telecom call list and exit from
`MODE_IN_CALL`. Transaction 33 is safe only for this fully pinned ROM identity.

## Milestones

- [x] Preserve the stock dialer while caller-gating auto-answer.
- [x] Claim the helper session before answering.
- [x] Inject prompts and capture caller audio without storing it.
- [x] Detect live DTMF and run the fixed three-option menu.
- [x] Keep microphone and speaker muted across Samsung route rewrites.
- [x] End completed calls and verify normal audio restoration.
- [x] Recover from forced worker death with a durable snapshot and supervisor retry.
- [x] Produce reproducible native builds and deterministic disabled module archives.
- [ ] Replace the fixed menu with a validated configuration format and operator UI.
- [ ] Test reboot during a session, SIM/network loss, and emergency-call interactions.
- [ ] Add more explicitly audited device profiles.
