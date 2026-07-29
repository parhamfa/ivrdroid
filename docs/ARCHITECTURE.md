# IVRdroid architecture

## Trust boundary

IVRdroid has two intentionally separate parts.

### Unprivileged Android app

- Obtains Android's call-screening role with explicit user consent.
- Immediately allows every incoming call, preserving the stock dialer and call log.
- Auto-answers only an exact caller compiled into a private test build.
- Writes one fixed `START_MENU` request into its owner-only bridge directory.
- Never executes root shell commands or supplies paths, mixer controls, prompt content, or
  Telecom transaction numbers.

### Privileged device runtime

- Is installed separately on a rooted device and remains disabled at boot during development.
- Resolves the app-private bridge owner and accepts only bounded, exact command bodies.
- Pins manufacturer, model, codename, API, fingerprint, display ID, PCM topology, mixer controls,
  root-owned prompts, and the ROM-specific Telecom transaction.
- Owns the current fixed menu session so Android cannot kill a background app component halfway
  through a call.
- Keeps captured PCM only in memory and logs only the detected digit and detector metrics.
- Durably snapshots the affected mixer values before each prompt and restores them on every
  completion or failure path.

The helper does not decide which callers qualify. The app does not directly access ALSA or
Telecom's private binder surface.

## Verified call sequence

```text
incoming cellular call
  -> CallScreeningService responds ALLOW
  -> caller mismatch: stop; com.android.dialer handles the call
  -> exact match: TelecomManager.acceptRingingCall(AUDIO_ONLY)
  -> app atomically writes START_MENU
  -> helper confirms MODE_IN_CALL
  -> snapshot mixer -> play root-owned main prompt -> restore mixer
  -> capture PCM 0:0 at 48 kHz stereo PCM16
  -> stereo Goertzel detector emits one DTMF digit or an 8-second timeout
  -> 1: sales prompt; 2: support prompt; 0: operator prompt
  -> invalid digit or timeout: replay main menu, at most two retries
  -> terminal prompt completes and mixer is restored
  -> pinned ITelecomService endCall transaction
  -> verify Android leaves MODE_IN_CALL
```

The complete key-2 branch has been verified on the audited tablet.

## Why the menu session is root-owned

The first app-owned session design attempted to start a foreground service from
`CallScreeningService`. Android 12 rejected it with
`ForegroundServiceStartNotAllowedException`. A fixed root launch through Android's `cmd`
shell-command transport also returned `FAILED_TRANSACTION` from the long-running helper.

The final prototype therefore keeps caller policy in Android and the short, fixed audio/menu
session in the already-required device helper. This is deliberate device-appliance engineering,
not a claim that arbitrary Android applications should move business logic into root.

A future configurable release should pass a validated, bounded menu description to a small
native policy engine; it must not add a general root RPC or interpolate configuration into shell
commands.

## Helper protocol

The app-private bridge is:

```text
/data/user/0/ai.rx1.ivrdroid/files/bridge/
  command.request
  status
```

The normal call path writes exactly:

```text
START_MENU\n
```

The helper also retains fixed diagnostic commands for individual prompts and one DTMF capture:

```text
PLAY_MAIN\n
PLAY_SALES\n
PLAY_SUPPORT\n
PLAY_OPERATOR\n
LISTEN_DTMF\n
```

The older `PLAY_ONCE` bring-up request remains separate for regression testing. No command accepts
arguments.

The helper rejects wrong ownership, symlinks, unsafe modes, oversized files, unknown bodies,
property mismatches, altered control types, an unexpected in-call mixer baseline, and unsafe
prompt files.

## Audio and DTMF profile

The audited route uses ALSA card 0, device 0, 48 kHz, stereo, signed PCM16 for playback and
capture.

Prompt injection temporarily changes:

```text
SPK Switch                    Off
AudioMixer CH2 Mixer En       Off
AudioMixer CH2 DOUT Select    DMIX_OUT
```

The observed in-call baseline is:

```text
AudioMixer CH2 DOUT Select    AIF4IN
AudioMixer CH2 Mixer En       On
SPK Switch                    On
```

Those values are validation constraints, not a hard-coded rollback. Each transaction restores
the values it actually snapshotted.

The detector analyzes 25 ms frames on both channels, requires the same DTMF candidate on each,
requires two stable frames, and rearms after two quiet frames. Synthetic tests cover all twelve
telephone keys, asymmetric tone levels, short tones, channel disagreement, and non-DTMF tones.

## Call termination

This ROM has no `cmd telecom` implementation. Its framework identifies
`ITelecomService.Stub.TRANSACTION_endCall` as transaction 33 with one calling-package string.
The helper invokes that exact binder transaction through Android's native `service` client and
then checks that audio leaves `MODE_IN_CALL`.

The transaction number is safe only because the helper also pins the full ROM identity. A ROM
change must be re-audited instead of inheriting transaction 33.

## Milestones

- [x] Preserve the normal dialer while caller-gating auto-answer.
- [x] Build transactional mixer routing with durable snapshots and an independent watchdog.
- [x] Inject root-owned prompts into a live cellular uplink.
- [x] Prove restoration after forced worker death.
- [x] Capture live caller audio without storing it.
- [x] Detect live DTMF and reject non-DTMF fixtures.
- [x] Run a three-option prompt/digit/terminal-prompt menu.
- [x] End the completed call and verify normal audio state.
- [ ] Replace the fixed menu with a validated configuration format and operator UI.
- [ ] Test app force-stop, helper death during capture, reboot during a call, SIM loss, a second
  incoming call, and emergency-call interactions.
- [ ] Add more explicitly audited device profiles.
