# IVRdroid architecture

## Scope

IVRdroid handles one active cellular call and one menu session at a time. V3 adds explicit
voicemail to the single-host control plane and one enrolled SM-T585 while preserving the
appliance boundary: it is not a concurrent PBX, queue, extension, transfer, live-agent,
whole-call recorder, transcription service, or arbitrary-webhook system. A bounded
owner-configured ntfy publisher may post a closed catalog of operational alerts to one topic.

## Trust boundary

### Unprivileged Android app

- Obtains Android's call-screening role with explicit user consent.
- Immediately allows every incoming call, preserving the stock dialer and call log.
- Applies a locally cached signed caller policy. The local tablet kill switch overrides every
  dashboard setting; a missing or invalid policy fails closed to the stock dialer.
- Stores enrollment and Cloudflare service credentials with Android Keystore-backed AES-GCM.
- Polls while idle, pauses network work during calls, and uses WorkManager as a 15-minute recovery
  mechanism.
- Reconciles finalized helper receipts into a 512 MiB Android Keystore-backed AES-GCM spool,
  resumes idempotent 1 MiB uploads only while idle, and removes local audio only after a verified
  acknowledgement matching recording ID, size, and source hash.
- Runs a boot-only, three-minute Wi-Fi guardian because network-constrained WorkManager cannot
  repair a disconnected network. The guardian can enable an already-disabled radio and request
  up to three saved-network reconnects, but it never disables Wi-Fi, stores credentials, changes
  the saved network, or mutates Wi-Fi while the call/helper/audio state is not idle.
- Verifies canonical JSON, the Ed25519 signature, revision identity, source/program hashes,
  prompt hashes and formats, instruction ownership/bounds, and the expanded UTC schedule before
  asking the helper to stage a revision.
- Atomically writes only bounded `STAGE_REVISION`, `ACTIVATE_STAGED`, and UUID-correlated
  `START_MENU` requests into its owner-only bridge directory.
- Waits for helper state `WAITING_FOR_CALL` before asking Telecom to answer.
- Never executes root commands or supplies arbitrary paths, shell text, mixer values, or Telecom
  transaction numbers.

### Privileged device runtime

- Is packaged separately with boot startup disabled by default. Boot startup is enabled only on
  a specifically audited device after its recovery suite passes.
- Resolves the app-private bridge owner and accepts only the three bounded command forms.
- Pins manufacturer, model, codename, API, fingerprint, display ID, PCM topology, four mixer
  controls, prompt format, revision limits, and the ROM-specific Telecom transaction.
- Independently validates staged revision identifiers, manifests, prompt hashes, WAV headers,
  instruction ownership/bounds, schedule horizon, transition limits, and total size before
  copying data into root-owned storage.
- Retains active, previous, and built-in fixed revisions and activates a staged revision only
  while Telecom is idle and audio mode is normal.
- Owns the menu session so Android cannot kill a background app component halfway through a call.
- Keeps ordinary DTMF PCM in memory. It persists caller PCM only while a signed V3
  `record_message` instruction is active, using owner-only temporary/final files and an atomic
  WAV-plus-receipt handoff to the app. Capture, privacy, storage, or preemption failure deletes
  partial audio.
- Uses a root-owned durable snapshot plus an independent guardian for restoration after worker
  death.

The helper does not decide which caller qualifies. The app cannot access ALSA or Telecom's
private binder surface.

### Dashboard and API

- Cloudflare Access authenticates the browser owner; FastAPI verifies the Access JWT audience and
  same-origin writes.
- The exact enrollment path bypasses Access but still requires a single-use eight-digit code,
  ten-minute expiry, per-IP failure limits, and the single-active-tablet rule.
- All other device paths require a dedicated Cloudflare service token at the edge and the
  device's independent 256-bit bearer credential at the API. Only a hash of that bearer
  credential is stored server-side.
- Phone numbers and mutable configuration documents are encrypted in PostgreSQL. Proxy and
  application access logs are disabled or redacted rather than receiving caller numbers.
- Prompt uploads are quota-limited, transcoded to 48 kHz stereo PCM16 WAV, content-addressed,
  and referenced by immutable revisions.
- Recording creation is bound to an acknowledged call, device, V3 revision, block UUID, sequence,
  signed duration/finish behavior, and source receipt. Uploads are offset-checked, chunk-bounded,
  encrypted while incomplete, and rejected for foreign IDs, malformed WAV, hash/size/duration
  disagreement, or quota exhaustion.
- Completed recordings are normalized to mono 16 kHz 48-kbps MP3, encrypted with a separate
  versioned AES-256-GCM key, and stored under random names in a dedicated 5 GiB volume. Admin
  playback supports authenticated byte ranges with `no-store`; deletion and retention preserve
  audit tombstones.
- Publishing signs canonical JSON with an Ed25519 private key mounted outside Git. The app embeds
  only the public verification key.
- The production Compose project exposes only `127.0.0.1:3200`; FastAPI and PostgreSQL have no
  host ports.

## Publish and synchronization sequence

```text
operator edits mutable draft
  -> dashboard reviews caller policy, schedules, recording behavior, and flow against the base revision
  -> broad caller routing requires explicit confirmation; no-op publication stays disabled
  -> publish request is bound to the reviewed draft edit version and base revision
  -> server validates caller lists, assets, schedules, tree ownership/depth/termination
  -> server deterministically compiles the tree plus recording behavior into RuntimeProgramV3
  -> server writes immutable revision and Ed25519 signature in one transaction
  -> enrolled tablet reports active revision and idle/call/storage/helper health
  -> server returns the desired revision
  -> app downloads canonical manifest and content-addressed prompts
  -> app verifies signature, schema, source/program hashes, assets, instruction tape, and limits
  -> helper independently stages and validates the compiled revision
  -> app requests activation only while idle
  -> helper atomically swaps active/staged while retaining the previous revision
  -> app acknowledges the real active revision
  -> dashboard changes pending to applied or error from that acknowledgement
```

Rollback does not mutate an older row. It republishes the selected configuration as a new signed
revision, preserving a linear audit trail. During a server or reverse-proxy outage, caller screening and
menu execution continue from the cached active revision; network work backs off and never runs
during a call.

The Overview caller-policy summary is read from the enrolled tablet's active signed revision. Before
the first activation it falls back to the V4 draft; it does not use the retained legacy V3 authoring row.

## Normal call sequence

```text
incoming cellular call
  -> CallScreeningService responds ALLOW
  -> local kill switch off, missing config, or caller-policy mismatch: stop; stock dialer handles
  -> policy match: app writes START_MENU
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
  -> execute the active bounded flow from its root node
  -> play content-addressed prompts while session privacy remains active
  -> collect one DTMF digit from PCM 0:0 at 48 kHz stereo PCM16 when requested
  -> only at record_message: play built-in beep, capture caller PCM, and stop on configured key,
     signed maximum, or caller hangup
  -> atomically hand a finalized WAV and bounded receipt to the app, or follow on_unavailable
     after a storage/capture failure
  -> follow digit, timeout, invalid, bounded menu-return, and pre-expanded schedule branches
  -> reach an end_call instruction within the transition bound
  -> pinned ITelecomService endCall transaction while privacy remains active
  -> verify Telecom has no call and Android leaves MODE_IN_CALL
  -> stop guardian
  -> restore audited post-call routing plus captured endpoint state
  -> durably clear snapshot
```

The built-in key-2 branch and V2 downloaded-flow are verified on the audited tablet. V3 caller
recording is not: its physical evidence gate is recorded separately in `VERIFICATION.md` and
must not be inferred from unit, host, browser, or synthetic-WAV tests.

## Flow runtime V3

The mutable authoring source is a recursive `FlowDocumentV3`. Each block has an opaque UUID and
owns either one `next` child or a fixed set of branch roots. A normal block cannot be referenced
from another location, so aliases, orphan records, arbitrary jumps, and free-form cycles are not
representable. Deleting a block deletes its entire owned subtree; undo restores that subtree as a
single history operation.

`Collect one digit` owns unique digit branches plus No input, Invalid input, and Return limit
branches. Input attempts and the 1-3 menu-return limit are settings of the owning collector.
`Return to menu` compiles to the nearest structurally owning collector and is rejected outside
that scope or from its own return-limit path.

`Record message` owns required success and unavailable children. It is valid only when a distinct
`Play prompt` block immediately precedes it on that path, preventing accidental capture without
a greeting/notice. All recording blocks use one signed `recording_behavior`: a 10-180 second hard
maximum and an optional telephone finish key. Hangup and the hard maximum always stop capture;
there is no silence detector. Worst-case compiled execution, including prompts, menus, and
recording branches, may not exceed ten minutes.

Publishing records the source tree, compiler version, source hash, compiled program hash,
instruction tape, schedules, and pinned prompt assets in one signed manifest. Compilation order
is deterministic, including DTMF branch ordering and schedule-window expansion anchored to the
signed `published_at` timestamp rather than the device's download time. The app and helper independently reject reused
UUIDs, shared instruction targets, unreachable instructions, invalid branch targets, missing
assets, invalid schedule references, excessive depth/actions, and transition-limit violations.
Runtime traces use the stable block UUID, a human-readable action label, selected input, and menu
return count so Studio simulation, call history, and device diagnostics describe the same path.

Schema V1 and V2 drafts/revisions are archived read-only. Their executors remain available only
for rollback; new authoring and publishing are V3-only. A V1 cache produced by the older
download-time compiler is reusable only when the app can reproduce the complete cached document
from the same signed manifest and its embedded horizon. This preserves rollback without weakening
flow or asset validation.

## Why the menu session is root-owned

An earlier app-owned design attempted to start a foreground service from
`CallScreeningService`. Android 12 rejected it with
`ForegroundServiceStartNotAllowedException`. A fixed root launch through Android's `cmd`
shell-command transport also returned `FAILED_TRANSACTION` from the long-running helper.

The current design therefore keeps caller policy and revision synchronization in Android and the
short audio/menu session in the already-required helper. Release one passes a compiled, bounded
menu description to the native policy engine. It does not add a general root RPC or interpolate
configuration into shell commands.

## Helper protocol

The app-private bridge is:

```text
/data/user/0/ai.rx1.ivrdroid/files/bridge/
  command.request
  status
  last_result
  recording_capacity
  recordings/<recording-uuid>.wav
  recordings/<recording-uuid>.json
```

The accepted request bodies are exactly:

```text
START_MENU <canonical-call-uuid>\n
STAGE_REVISION <positive-decimal-revision> <lowercase-sha256>\n
ACTIVATE_STAGED <positive-decimal-revision>\n
```

The legacy `START_MENU\n` body remains accepted for V1/V2 rollback compatibility, but V3
recording requires the UUID-correlated form. Receipt and WAV filenames contain only random UUIDs;
caller identifiers never enter paths or logs.

Revision files live in additional owner-only bridge subdirectories created by the app. The stage
request contains no path. The helper derives every source and destination itself, re-hashes the
manifest and prompts, validates the compiled configuration, and copies accepted data into a
root-owned revision directory. Activation is rejected while a call is present or audio mode is
not normal.

The helper rejects wrong ownership, symlinks, unsafe modes, oversized files, any other command
body, revision/hash disagreement, device-property mismatches, altered mixer control types, an
unexpected in-call baseline, and unsafe prompt files.

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

During `record_message`, capture remains on the same audited 48 kHz stereo PCM16 caller path. A
small rolling tail is withheld so a configured finish tone can be trimmed before finalization.
Finish-key-disabled, hard-maximum, remote-hangup, repeated-recording, unavailable-storage, and
partial-cleanup paths are separately bounded.

## Failure recovery

The guardian has phase-specific deadlines and a 20-minute absolute ceiling above the compiler's
ten-minute session bound. Recording uses fixed heartbeats with a three-second watchdog and a
separate 30-second finalization deadline, so a configured long recording cannot hide a dead or
wedged worker. The guardian inherits no command input. During a session it enforces only audited
mixer states, and an independent process samples Telecom every 500 ms. The worker channel is a
private `SOCK_SEQPACKET` socket: the worker sends fixed phase bytes, and the guardian sends only
fixed acknowledgements.

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

## External call runtime V4

V4 introduces a narrowly owned multi-call phase for one `external_call` instruction. Unlike an
ordinary second call, the expected outgoing operator leg and verified conference parent are tracked
by the privileged non-UI Android `InCallService`. All other multiple-call states still preempt IVR.
The stock Dialer remains the default UI.

V4 validation and compilation propagate whether each owned path has already played a configured
`play_prompt` or `collect_digit` menu prompt. A direct `record_message` or `external_call` branch is
valid when that earlier prompt contains its recording notice, so a redundant branch-local prompt
may be removed without removing the recorded action. A recorded action with no earlier prompt on
its path remains invalid. Prompt content and the placement of the notice before any enabled barge-in
point remain the flow author's responsibility.

The helper requests the operation using the active signed revision and block identity. Android
resolves the number and timeout from its encrypted verified manifest, holds the exact inbound call,
dials one exact outbound call, and exposes sequenced state once per second. After the operator is
active, the helper arms bounded PCM pre-roll; `RECORDER_READY` gates merge. Durable recording starts
only after Telecom exposes a conference parent containing both owned children.

The guardian suspends its absolute 20-minute ceiling only while that conference identity, controller
heartbeat, recorder heartbeat, and private physical speaker/microphone state remain healthy. A
three-second stale heartbeat fails closed. Cleanup uses `Call.disconnect()` on the tracked operator
leg, never the device-global hangup. Completed, Not connected, and System failure are the only
resumable caller branches; caller hangup is terminal.

Conversation PCM rotates into fsynced 180-second segments and is encrypted by Android in a separate
4 GiB spool while maintaining at least 512 MiB free device space. Post-call synchronization uploads
call history first and ordered conversation segments second. The server binds each logical
conversation to a signed V4 `external_call`, verifies and assembles segments with bounded memory,
and retains one encrypted stereo MP3 under the shared 5 GiB recording quota.

The complete V4 contract, publication capability gate, rollout order, and physical acceptance
matrix are in [EXTERNAL_CALL_V4.md](EXTERNAL_CALL_V4.md). Carrier merge behavior and capture of both
remote voices are explicitly unproved until the deferred tablet tests pass.

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
- [x] Add encrypted enrollment, foreground synchronization, and WorkManager recovery.
- [x] Replace the fixed caller gate with signed cached caller-policy modes and a local kill switch.
- [x] Add immutable signed configuration revisions and cross-language contract fixtures.
- [x] Add prompt conversion/versioning, schedules, call metadata, device
  health, acknowledgements, and rollback in the web control plane.
- [x] Add bounded helper staging/activation and active/previous/built-in recovery fallbacks.
- [x] Replace shared-node authoring with FlowDocumentV2, subtree deletion, undo/redo, validation,
  simulation, tree-aware diff, and publish review.
- [x] Add deterministic RuntimeProgramV2 compilation and independent Android/native tape
  validation while retaining V1 only for rollback.
- [x] Add FlowDocumentV3/RuntimeProgramV3, explicit greeted `record_message` branches, shared
  signed recording behavior, cross-language fixtures, and V1/V2 rollback compatibility.
- [x] Add private capture/handoff, Android encrypted spool/retry, resumable server ingestion,
  encrypted normalized storage, retention, and the dashboard voicemail inbox.
- [x] Add a reconnect-only boot Wi-Fi guardian that cannot switch the radio off.
- [ ] Complete the release-one live call matrix and record each result in `VERIFICATION.md`.
- [ ] Automate device-side reboot, radio-loss, and post-recovery regression tests.
- [ ] Test physical SIM removal or a real carrier outage if a deployment requires that scenario.
- [ ] Validate emergency interaction on a safe Telecom harness; never place a real emergency
  call merely to exercise IVRdroid.
- [ ] Repeat the consented physical-call gate with a fresh message after the 0.7.1 server loudness
  correction. The first call proved caller-only speech, no ambient/tablet microphone, no prompt
  bleed, safe `MODE_NORMAL` restoration, no stale mixer snapshot, acknowledged upload, and
  accepted playback after reprocessing; automatic normalization of a new upload remains unproved.
- [ ] Add more explicitly audited device profiles.
