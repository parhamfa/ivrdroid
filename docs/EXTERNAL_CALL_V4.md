# External call and conference runtime V4

V4 adds one signed `external_call` flow operation. It is a local cellular handoff: the tablet
holds the owned inbound caller, dials one operator number, asks Android Telecom to merge the two
legs, records the verified carrier conference, and resumes the caller on one of three owned
branches. The dashboard and server are not in the live call-control loop.

This document describes the code contract. Carrier conference control and two-remote-voice audio
capture remain deployment acceptance gates; automated tests do not claim either physical result.

## Authored block

```json
{
  "block_id": "c4f34633-b0e3-4a74-8830-e60f10c86e68",
  "type": "external_call",
  "phone_number": "03136644636",
  "answer_timeout_seconds": 30,
  "next": {},
  "on_not_connected": {},
  "on_system_failure": {}
}
```

- `phone_number` is an optional `+` followed by 8–15 digits. The server and tablet reject pauses,
  service codes, malformed values, and emergency numbers before compilation or dialing.
- `answer_timeout_seconds` is 5–120 seconds and defaults to 30.
- The block must have an earlier configured `play_prompt` or `collect_digit` menu prompt on its
  exact call path. A branch-local prompt is optional when an earlier prompt already contains the
  complete recording and connection notice; recording still has no per-block disable switch.
- If the covering menu allows prompt interruption, place the complete notice before any point at
  which an accepted digit can interrupt playback. The compiler can prove prompt presence, not the
  spoken content or whether the caller heard words placed after the interruption point.
- `next` is **Completed**: a conference was verified and the operator subsequently disconnected.
- `on_not_connected` handles timeout, busy, rejection, or pre-merge operator disconnect.
- `on_system_failure` handles identity, permission, hold, placement, conference, merge, audio,
  storage, recorder, or recovery failure.
- Caller hangup is terminal and never resumes a branch.
- Telecom `ACTIVE` counts as answered, including an answering machine; V4 does not ask the
  operator to press a key to accept.

FlowDocumentV1 through V3 and their signed runtime programs remain immutable. V4 uses compiler
`4.0.0`, preserves prompt pinning and deterministic signing, and keeps the existing depth-eight and
64-owned-block limits. Human talk time in a healthy verified conference is excluded from the
ten-minute automated-session budget; prompts, dialing, and resumed automated paths are not.

## Local call-control sequence

The app is a privileged non-UI `InCallService`; `com.android.dialer` remains the default call UI.
The app tracks exact Telecom `Call` objects for the original caller, outgoing operator, and verified
conference parent. It never uses the global Telecom hangup to clean up an operator leg.

`CallScreeningService` and `InCallService` are not assumed to expose identical `Call.Details` on
the Samsung ROM. Screening persists a same-boot, owner-only pending session with a salted digest of
the masked caller suffix. The controller may re-key that session to the sole visible incoming,
non-emergency `Call` only when the digest matches and either the initial 30-second claim window, a
previously confirmed same-boot binding, or the signed V4 request authorizes the session. Missing,
different, emergency, or multiple candidates fail closed without mutating any call. Raw caller
numbers are never added to the ownership journal or diagnostic logs.

```text
play notice
  -> verify signed revision, exact caller, number, permissions, and capabilities
  -> hold exact caller
  -> place outgoing operator leg
  -> start answer timer when the operator leg reaches DIALING
  -> wait for operator ACTIVE
  -> open PCM pre-roll and acknowledge RECORDER_READY
  -> require conferenceable caller/operator legs
  -> merge and verify a parent containing both children
  -> commit recording and remain conferenced
  -> operator disconnect: finalize, restore caller, take Completed
```

Setup and merge each have a ten-second watchdog. On Not connected or System failure the controller
disconnects only the exact outgoing leg, restores/unholds the original caller, reacquires the
private IVR route, and resumes the selected branch. An unexpected extra, ambiguous, replaced, or
emergency call retains the existing fail-closed preemption behavior.

The helper/app bridge is an owner-only, atomic, one-line protocol. Requests contain the session,
revision, block, boot identity, sequence/timing data, and desired state. The app resolves and
validates the phone number and timeout against the encrypted active signed V4 manifest instead of
trusting IPC. Status is refreshed once per second. During an established conversation, the native
guardian independently verifies the owned conference while the app recovers. A recording failure
cannot terminate that conversation. Bridge version 2 includes a recovery readiness handshake.

The normal 20-minute guardian remains active except during a healthy owned conference with fresh
heartbeats and the private speaker/microphone state. Physical tablet speaker
and microphone stay disabled; only the two carrier legs should be audible in the recording.

## Conversation recording

The helper opens 48 kHz stereo PCM16 when the operator becomes active and retains only bounded
in-memory pre-roll. Durable audio begins only after the conference parent is verified. A failed
merge discards the pre-merge audio.

Both recorders now write continuous raw PCM files with one-second durable checkpoints. Checkpoints
do not close or split files. App encryption and upload are not prerequisites for continuing the
conversation. There is no ten-second recording acknowledgment deadline. The combined private
inbox and encrypted conversation spool retain their quota and filesystem reserve, with additional
space reserved for processing. Capture/storage errors retain valid partial audio while the established
conversation continues. They cannot disconnect either leg, select System failure, or rewrite a
completed call's outcome. Genuine call-control, ownership, privacy, and guardian failures retain
their existing cleanup behavior. Recording availability is not a prerequisite for the merge.

After the caller's IVR session ends, the app encrypts the private files using a bounded streaming
buffer into one authenticated-record container, then uploads one logical recording. A new call
pauses encryption at buffer boundaries and retains completed encrypted records for resume.
Transient save failures retain validated private input for retry; malformed or unauthorized input
is rejected. See [CALL_CONTINUITY.md](CALL_CONTINUITY.md) for durability and the separate call limit.

Recording diagnostics retain original exceptions, operation stage, duration, file size and free
space in the private `recording-diagnostics/failures.json` journal (64 entries, at most 1 MiB).
Native capture and finalizer failures have separate private latest-error receipts in `bridge`.
Call history displays failures during the conversation separately from the final caller hangup,
and replayed terminal states reuse their original timestamp instead of creating a new incident.

During a live call only lightweight status synchronization continues. Afterward the app uploads
call history, then resumes continuous audio by byte offset. The server accepts audio only for the
matching signed V4 revision and `external_call` block, verifies coverage/size/hash, and durably queues
bounded processing into encrypted stereo MP3. Legacy segments remain accepted during migration.

Voicemail and conversations share the existing automatic/manual retention policy and 5 GiB server
quota. Logs, audit records, call events, and capability status never include the full operator
number.

## Publication gate

Every active device must report all of the following before a V4 draft can publish:

- `runtime_versions` contains `4`;
- `external_call_control_capable` is true;
- `conversation_recording_capable` is true;
- `call_control_protocol_version` is `1` (legacy) or `2` (continuous recording and recovery).

The dashboard may author and save V4 while a device is not ready, but publication is rejected.

## Deployment and physical acceptance

1. Back up the production database/media, active signed V3 revision, installed app/helper modules,
   permissions, hashes, and live process configuration.
2. Apply additive migration `0004_external_conversations` and deploy the V4-capable server and
   dashboard while leaving V3 active.
3. Recheck V3 authoring, calls, voicemail, playback, activation, and rollback.
4. Install the V4 app/helper with IVR disabled. Reboot only if required for privileged permission
   binding, open IVRdroid, and grant its newly requested `CALL_PHONE` runtime permission. Verify the
   stock Dialer remains default and all four publication capabilities appear.
5. Publish a bounded V4 test flow only after readiness is acknowledged.
6. Test successful merge, no-answer timeout, busy/rejection, operator-first hangup, caller-first
   hangup, screen-off operation, controller restart/rebind, and recording failure.
   For the continuity change, use the matching app and helper together: an older helper still
   expects live encryption acknowledgments. On an isolated acceptance call lasting more than six
   minutes, delay saving past ten seconds, exercise both checkpoint boundaries, and inject a
   recording write/storage failure. Neither leg may disconnect or hear a failure prompt because
   of the recorder. Verify partial status, retries, both voices, and the real final hangup afterward.
7. Have caller and operator speak different phrases. Dashboard playback must contain both remote
   voices and no tablet microphone, prompt, or ambient leakage.
8. Keep the prior signed V3 revision immediately activatable until every physical case passes.

Rollback activates the previous signed V3 revision before restoring older tablet packages.
Migration `0004` is additive and remains in place during an image rollback.

Once the tablet already has the privileged overlay and both permissions granted, an app-only patch
can be installed without a reboot. Stage the matching overlay module for reboot persistence, keep
the active data APK at the same or higher version, and re-verify the grants and default Dialer.
