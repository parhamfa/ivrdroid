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

- Is packaged separately with boot startup disabled by default. Boot startup is enabled only on
  a specifically audited device after its recovery suite passes.
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
  -> helper requires MODE_NORMAL and exactly one non-emergency Telecom call
  -> helper hashes the current call identity
  -> durable snapshot of the observed cold/normal four-control route
  -> microphone Off + speaker Off, then normalize DOUT/mixer to AIF4IN / Off
  -> guardian independently verifies both endpoint mutes
  -> helper publishes WAITING_FOR_CALL
  -> app requests TelecomManager.acceptRingingCall(AUDIO_ONLY)
  -> Samsung establishes the audited AIF4IN / On in-call route
  -> guardian enforces both mute controls every 5 ms during route rewrites
  -> helper confirms MODE_IN_CALL and the same single safe call identity
  -> guardian confirms 500 ms of continuously stable privacy
  -> inject root-owned main prompt while session privacy remains active
  -> capture PCM 0:0 at 48 kHz stereo PCM16
  -> stereo Goertzel detector emits one DTMF digit or an 8-second timeout
  -> 1: sales prompt; 2: support prompt; 0: operator prompt
  -> invalid digit or timeout: replay main menu, at most two retries
  -> terminal prompt
  -> pinned ITelecomService endCall transaction while privacy remains active
  -> verify Telecom has no call and Android leaves MODE_IN_CALL
  -> stop guardian
  -> restore audited post-call routing plus captured endpoint state
  -> durably clear snapshot
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

Before answer, the helper accepts the audited normal route or the observed cold `DMIX_OUT`
variant, snapshots it, mutes both physical endpoints, and normalizes routing to `AIF4IN / Off`.
This prevents Android from inheriting a stale prompt-injection route and closes the microphone
before Telecom answers.

One durable version-5 snapshot stores the four observed values, the kernel boot ID, a random
session ID, the stable Telecom call-identity hash, and a checksum. Post-call recovery deliberately
does not restore a cold `DMIX_OUT` DOUT value: it restores audited normal DOUT/mixer routing while
preserving the captured pre-call microphone and speaker states.

Android can rewrite the route more than once after answering, so the guardian holds microphone
and speaker Off throughout the session instead of relying on a one-time write. A corrected
control or unexpected startup route resets the 500 ms startup-stability window. A contested
mixer read/write is retried; 250 ms of continuously unverified microphone or speaker privacy
aborts the session and enters fail-closed recovery. The worker cannot begin prompt playback
until it receives the guardian's stable-privacy acknowledgement over a private sequenced socket.

The detector analyzes 25 ms frames on both channels, requires the same candidate on each,
requires two stable frames, rearms after two quiet frames, and requires cellular-calibrated
dominance. Synthetic tests cover all twelve telephone keys, asymmetric levels, short tones,
channel disagreement, and non-DTMF tones.

## Failure recovery

The guardian has phase-specific deadlines and a 75-second total ceiling. It inherits no command
input. During a session it enforces only audited mixer states, and an independent process samples
Telecom every 500 ms. The worker channel is a private `SOCK_SEQPACKET` socket: the worker sends
fixed phase bytes, and the guardian sends only fixed acknowledgements.

If the worker dies, times out, or reports an audio/capture/termination failure:

1. retain or re-establish the private route;
2. attempt the guarded single-call Telecom hangup;
3. restore the normal route only after the call is gone;
4. keep the durable snapshot if hangup fails;
5. let the supervisor restart the helper and retry stale-transaction recovery.

This ordering is deliberate. Restoring the call microphone before a failed hangup would expose
background audio to the caller.

Recovery is scoped to the owned call. A global Telecom hangup is permitted only when a valid
same-boot snapshot exists and two live checks identify the same single non-emergency call. An
emergency marker, a second or replacement call, or persistently unverified Telecom state makes
the helper stop IVR work, release only mixer state it can prove it owns, clear the transaction,
and yield to Android without a global hangup.

A snapshot from a previous kernel boot cannot describe the new kernel's mixer ownership. At
startup it is therefore discarded without changing mixer controls or ending any call. The
helper then waits for 500 ms of verified Telecom-idle and `MODE_NORMAL` before accepting work.
The Magisk supervisor backs off after short failures and recreates the package's `disable`
marker after seven consecutive short startup failures.

## Call termination

This ROM has no usable `cmd telecom` implementation. Its framework identifies
`ITelecomService.Stub.TRANSACTION_endCall` as transaction 33 with one calling-package string.
The helper invokes that exact binder transaction through Android's native `service` client.

Before invoking a global hangup, the helper requires its same-boot call identity and parses the
live Telecom dump twice, requiring exactly one non-emergency call with that identity. It then
verifies both an empty Telecom call list and exit from `MODE_IN_CALL`. Emergency parsing handles
explicit true/false markers, treats ambiguous emergency metadata as emergency, and does not
confuse capability-only text such as `supportsEmergencyCall=true` with an active emergency call.
Transaction 33 is safe only for this fully pinned ROM identity.

## Milestones

- [x] Preserve the stock dialer while caller-gating auto-answer.
- [x] Claim the helper session before answering.
- [x] Inject prompts and capture caller audio without storing it.
- [x] Detect live DTMF and run the fixed three-option menu.
- [x] Keep microphone and speaker muted across Samsung route rewrites.
- [x] Gate playback on stable privacy and tolerate bounded mixer contention.
- [x] End completed calls and verify normal audio restoration.
- [x] Recover from forced worker death with a durable snapshot and supervisor retry.
- [x] Produce reproducible native builds and deterministic disabled module archives.
- [x] Recover across a reboot during a live session and complete the first post-boot call.
- [x] Handle forced cellular-radio loss during prompt playback and complete a post-loss call.
- [x] Fail closed for emergency, multiple-call, replaced-call, and unverified-call fixtures.
- [ ] Replace the fixed menu with a validated configuration format and operator UI.
- [ ] Automate device-side reboot, radio-loss, and post-recovery regression tests.
- [ ] Test physical SIM removal or a real carrier outage if a deployment requires that scenario.
- [ ] Validate emergency interaction on a safe Telecom harness; never place a real emergency
  call merely to exercise IVRdroid.
- [ ] Add more explicitly audited device profiles.
