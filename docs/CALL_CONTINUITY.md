# Continuous recordings and call continuity (0.10.0)

The original caller defines the session. A signed, independently versioned call-safety policy
sets its maximum duration (60 minutes by default; 1–1,440 minutes). The app and native guardian
snapshot the applied policy before answering. The first observed answer starts a CLOCK_BOOTTIME
deadline; transfers, controller restarts, time-zone changes and wall-clock adjustments cannot
extend it. Expiry terminates the independently verified owned legs without a prompt or fallback
and records MAX_CALL_DURATION. Other prompt, dialing and privacy watchdogs remain in effect.

## Recovery authority

The encrypted general session journal exists even with whole-call auditing disabled. Application
startup restores it before network synchronization. Answer and dialing intent is persisted before
Telecom is called. Recovery reconciles observed state rather than replaying uncertain commands.

Bridge version 2 requires matching app/helper builds. In addition to the versioned command/status
wire, it carries NATIVE2 call snapshots, OWN2 caller/operator/conference ownership, ATTACHED2
session/boot/controller-generation acknowledgment, and CALL1 answer/deadline metadata. Call
identities and graph structure come from the pinned Android 12 Telecom implementation. An empty
or partial initial InCallService callback list cannot establish that the call ended. Matching
native and callback topology must settle across distinct observations before app recovery resumes.

After a confirmed conference loses the app heartbeat, the native observer and guardian preserve
it only while the same owned graph remains independently verified. The call limit continues.
A party disappearing while the app is absent triggers safe cleanup of the remaining owned leg.
Unverified or emergency topology releases IVR ownership; it never authorizes an indiscriminate
hangup. Native fallback hangup is limited to snapshots containing exclusively the owned legs.
The device's pinned Telecom transaction and actual carrier behavior still require physical tests.

The native observer sends direct IPC to the guardian. A separate process publishes its disk
snapshots; disk publication cannot stall observation. Established-conference proof also reaches
the controller through shared memory, independently of app heartbeats or recording writes.

## Recording ownership and disk durability

Both recorders write one continuous 48 kHz stereo PCM16 file, in separate native writer processes.
The conversation recorder starts after confirmed operator connection; the session recorder also
covers IVR prompts. There is no timed file rotation or save deadline that can terminate a call.
RECORDER_READY remains the bridge message name for permission to proceed with the merge; failure
to capture audio no longer vetoes the connection. Recording failures remain separate evidence.

Each writer uses a bounded shared audio ring and checkpoints audio plus original identity,
boot, process-start identity, capture time and committed frame count every second. Byte budgets
are enforced on every append; quotas, free space and processing workspace are refreshed each
second. The reserve includes the largest plaintext file's encrypted-container overhead. Full or
slow storage preserves the committed prefix and records incomplete coverage. A dead/reused PID
or changed boot, not elapsed time alone, establishes that an unfinished writer can be recovered.

After the call, one recording at a time is encrypted and verified with 64 KiB buffers. IVRSPL2 is
one file with authenticated bounded records and a mandatory authenticated SHA-256 footer. The
recording identity, total size, byte position and record length are authenticated. Reads for
resumable uploads allocate at most a 1 MiB transport buffer plus bounded crypto buffers. Legacy
IVRSPL1 files remain readable. New calls interrupt background audio work; plaintext is retained
until a verified encrypted file and its metadata are durable. The encrypted device copy remains
until the server durably acknowledges completed processing.

The continuous upload API provides immutable/idempotent creation, byte-offset resume, status,
queued completion and explicit reset of rejected data. PostgreSQL serializes recording updates;
a cross-worker advisory lock serializes processing and releases on worker death. Upload sources
survive retries/restarts/capacity failure. Raw PCM avoids RIFF's file-size ceiling. Existing
MP3 encryption, authenticated playback and HTTP range seeking remain compatible.

## Evidence and migration

Call outcome, final disconnect and recording health are separate. The dashboard exposes the
requested/applied call limit, recovery state, helper supervisor, partial coverage and actionable
processing errors. Diagnostics retain original exceptions, operation duration, native errno,
free bytes, writer/process identity and session/recovery state in bounded private records.

Legacy endpoints remain available, but cannot mutate continuous recording identities. Migrations
0007 and 0008 are additive; routine rollback must keep their tables and new recordings. The
rollback-context builder preserves their models, read compatibility and source-retention logic.
Prefer retaining the compatible new backend while reverting the matched app/helper pair.

A legacy boundary segment can be published as partial only through explicit recovery evidence:
the call ended, device inspection established no surviving writer/source, the reviewed segment
hashes still match, and every included segment is verified and contiguous. Recovery does not
rewrite the original segment into a fictitious terminal segment. Elapsed age alone is insufficient.

## Release acceptance

Automated acceptance includes protocol/version compatibility, call-policy limits and signatures,
empty/caller-first/operator-first/conference-first callback sequences, 1/3/10/40/130-second
controller outages, recovery without redial, original deadline preservation, independent graph
and emergency checks, continuous capture over six minutes, committed-prefix recovery, byte
budgets, crypto truncation/tampering/interruption, upload retry/corruption/capacity/restart,
legacy partial recovery, dashboard settings and history. Increasing 8/64/512 MiB recordings must
also process under a 128 MiB **test** heap. Production heap limits and OS/LMKD settings do not change.

Physical release acceptance remains separate: a >6-minute two-voice conference, both recorders,
both hangup orders, screen off, controller outages, a shortened call cap followed by verified
60-minute restoration, and a two-hour representative soak. Record results and gaps honestly;
synthetic Telecom tests cannot establish carrier conference behavior.
