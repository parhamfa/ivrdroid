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

## Release-one implementation record

Date: 2026-08-05

Android package: `ai.rx1.ivrdroid` v0.5.0-dev (versionCode 6)

Helper package: v0.5.0-dev (versionCode 19)

The release-one implementation adds the dashboard/server, signed contract, Android control
agent, caller-policy engine, and bounded helper revision engine. Fresh local evidence before
device deployment was:

- the complete `./scripts/check.sh` suite passed, including Android unit tests, lint, debug APK,
  native tests, pinned arm64 build, and helper reproducibility;
- Android contract tests reject a manifest changed after signing;
- the FastAPI suite passed 11 tests, including encrypted caller data, prompt versioning,
  single-tablet enrollment, stale acknowledgement rejection, signature fixtures, and call-event
  storage, plus immutable rollback republishing;
- dashboard component tests passed and the production Vite build completed;
- the production Compose model validated with only the web container publishing
  `127.0.0.1:3200`; API and PostgreSQL publish no host ports;
- the cross-language canonical fixture digest is
  `f8b85973fbd828715858d5ccbcb3fd7bd821a0cb1f14244470733407fe5fa4c0`.

Built artifacts at this checkpoint were:

```text
189b8f6fdf8c8f6754326c0bd701105b324ebf322833656c5a808adc7f8d6bb3  app-debug.apk
dbf20fb03032e8a908e020d5b0457d39fdac0524b91cc4c5e086fe5fc9583545  ivrdroid-helper (stripped)
65026da5119027430cf2f03bfb48c80d5985f67ab3a2cb705a527ef51cf2dfd5  IVRdroid-helper-0.5.0-dev-disabled.zip
c8aafd2be8c92d63ca81fbcb0b4e131def85f816be865ab5845c8ac81fe5c78f  IVRdroid-system-app-0.5.0-dev.zip
```

### Control-plane and tablet activation

Rollback evidence was captured before mutation under
`/Users/parhamfatemi/Backups/ivrdroid/20260805T151500Z`. The separate production Compose project
was then deployed under `/Users/parhamfatemi/Services/ivrdroid` on `old-mac`:

- PostgreSQL 16.14, API, and web containers became healthy;
- only the web proxy published `127.0.0.1:3200`; API and PostgreSQL remained internal;
- Alembic reached `0001_control_plane`;
- the final API image ID was
  `sha256:1983464085166d849284f06fde6f1c869a04b94a12c31eaf844b889af9904eee`;
- the web image ID remained
  `sha256:8dfd19b9a9fb07dd78c3cc3f5838199914c653e037e6e2f3d17b2fca7e980af3`;
- the existing `rx1-old-mac` tunnel gained only
  `ivrdroid.rx1.ai -> http://127.0.0.1:3200`; `cloudflared` was not restarted;
- Access precedence was verified for enrollment bypass, device service authentication, and the
  owner email-OTP dashboard.
- `dashboard.rx1.ai` root/health and `sms.rx1.ai/api/v1/health` still returned HTTP 200;
  the Access-protected `textbee.rx1.ai` root still returned its expected HTTP 302.

The tablet initially could not resolve the control plane because Android restricted-networking
mode was correctly rejecting its ordinary application UID. The final fix did not weaken that
policy: the dedicated system-app module grants only
`CONNECTIVITY_USE_RESTRICTED_NETWORKS` to IVRdroid. After reboot, the tablet still reported
restricted networking enabled, IVRdroid reported system/updated-system/privileged flags, the
privileged permission was granted, and the call-screening role remained assigned.

The real SM-T585 then enrolled through the public origin and synchronized. A deployment-time
import exercised the same prompt service used by the dashboard upload route. The resulting
asset was validated as 48 kHz, two-channel PCM16 WAV:

```text
f21482f30100b93cb2d165c238e43dc1f0187a802fd3af87e3b8299fdc6cf7c1  Main menu (1,208,634 bytes; 6,295 ms)
```

Revision 1 contained an allowlist-only caller policy, unknown callers routed to the stock dialer,
one `Asia/Tehran` schedule, prompt playback, one-digit branches for 0/1/2, bounded retry on
timeout/invalid input, and a terminating end-call path. It passed server validation, downloaded,
staged, activated while idle, and was acknowledged by the real tablet. Activation was observed
about 77 seconds after publish, within the 90-second target. The root helper state then showed
active revision 1, staged revision 0, owner-only revision directories/files, and a prompt hash
matching the server asset.

A controlled outage stopped only the IVRdroid API container. During the outage the loopback
health endpoint was unavailable while the tablet retained active revision 1 and the helper
process remained alive. After the API restarted, local health returned and a manual tablet sync
succeeded without replacing the cached revision.

### First live dashboard-revision call

The first two allowlisted calls reached immutable revision 1 outside its saved 09:00-17:00
`Asia/Tehran` window. Both correctly followed `schedule -> end`, restored their mixer snapshots,
and uploaded `SESSION_COMPLETE` records. This exposed a test-configuration mistake rather than a
runtime failure: the intended full-day window had not been persisted before revision 1 was
published.

The schedule was corrected through the dashboard to 00:00-23:59 and published as revision 2.
The tablet downloaded, staged, activated while idle, and acknowledged revision 2. The compiled
open window spans 86,340 seconds, matching the configured one-minute-short-of-24-hours interval.
The subsequent live call at 20:57 local time produced this device-side sequence:

```text
Starting immutable revision 2
pre-answer privacy verified
three bounded Samsung route corrections
prompt injection applied and prompt completed
DTMF 2 detected on both channels
pinned Telecom end-call transaction sent
session privacy snapshot restored and cleared
```

The caller reported normal behavior. The uploaded server record independently reported revision
2, `IVR_HANDLED`, `schedule -> prompt -> menu -> end`, `SESSION_COMPLETE`, and a 14-second
duration. Post-call checks showed helper `READY`, Telecom idle, requested and actual audio mode
`MODE_NORMAL`, no durable or temporary mixer snapshot, and no caller-audio asset. Only the
content-addressed dashboard prompt copies were present.

### Local kill-switch live test

With revision 2 still active and configured to handle the allowlisted caller, the on-tablet
`Enable IVR on this tablet` switch was turned off. A subsequent allowlisted call rang through the
stock Android dialer and was not answered by IVRdroid. Device logs reported that the incoming call
remained with the stock dialer, and the uploaded server record independently reported
`LOCAL_KILL_SWITCH`, revision 2, `STOCK_DIALER`, an empty menu path, and no IVR session duration.

The local switch was then restored to enabled and a manual synchronization completed. Final
server state reported `local_kill_switch: false`, active revision 2, helper `READY`, idle call
state, and no error. This verifies both the local override and restoration to the dashboard-managed
configuration.

### Cached call during control-plane outage

Only the `ivrdroid-api-1` container was stopped; PostgreSQL, the loopback web proxy, Cloudflare
Tunnel, the RX1 Dashboard endpoint, and the tablet helper remained running. The IVRdroid health
endpoint became unavailable while the tablet retained active revision 2, helper `READY`, the
enabled local switch, and the same cached configuration digest.

An allowlisted call during that confirmed outage completed entirely from the cached revision. The
caller reported normal behavior, and tablet logs independently showed revision 2 starting, prompt
playback completing, DTMF digit 1 detected on both channels, the pinned Telecom disconnect, and
the mixer snapshot restored and cleared. Post-call state was `SESSION_COMPLETE`, Telecom idle,
and requested and actual audio mode `MODE_NORMAL`.

The exact stopped API container was restarted and returned to healthy without replacing the web
or database containers. The tablet automatically uploaded the queued 14-second
`schedule -> prompt -> menu -> end` call record after connectivity returned; no manual sync was
used for that upload. Its following automatic sync cleared the recorded outage error. Final
server state reported desired and active revision 2, helper `READY`, idle call state, inactive
local kill switch, and `last_error: null`.

### Final configured digit branch

A subsequent online revision-2 call exercised digit 0. Tablet logs showed the audited prompt
complete, digit 0 detected on both PCM channels, the pinned Telecom disconnect, and the session
snapshot restored and cleared. The dashboard received a 13-second `IVR_HANDLED` record with
`schedule -> prompt -> menu -> end` and `SESSION_COMPLETE`. Final device state was helper
`READY`, Telecom idle, requested and actual audio mode `MODE_NORMAL`, with no snapshot or error.
Together with the preceding digit-1 and digit-2 calls, all configured DTMF branches now have live
coverage.

### Invalid-input retry path

Because revision 2's weekly schedule only opened on Wednesday, a one-day open exception for
Thursday, 2026-08-06, was added through the dashboard and published as immutable revision 3. The
tablet downloaded, staged, activated, and acknowledged revision 3 through its normal foreground
sync. The first device poll still showed revision 2; revision 3 was active about 27 seconds later,
without forcing activation. The helper's compiled configuration contained the bounded full-day
open interval for the exception.

During the subsequent allowlisted call, the caller entered invalid digit 9 three times. The caller
reported that the main prompt replayed and that digit 0 then ended the call as expected. The
uploaded record independently reported revision 3, `IVR_HANDLED`,
`schedule -> prompt -> menu -> repeat -> schedule -> prompt -> menu -> end`,
`SESSION_COMPLETE`, and a 25-second duration. Post-call device state was helper `READY`, Telecom
idle (`mCallState=0`), requested and actual audio mode `MODE_NORMAL`, with no durable or temporary
mixer snapshot. This gives the configured invalid-input branch live coverage; the no-input timeout
branch remains separate and pending.

### No-input timeout retry path

The next revision-3 call exercised the no-input path by sending no DTMF after the first prompt.
Helper timestamps showed three separate bounded waits of approximately 5.08 seconds each. The
second prompt began 15.61 seconds after the first prompt completed, so the user-observed delay was
correctly longer than five seconds: the configured collector permits three attempts before taking
its timeout branch. After the repeated prompt completed, digit 0 was detected on both channels and
ended the call normally.

The uploaded record independently reported revision 3, `IVR_HANDLED`,
`schedule -> prompt -> menu -> repeat -> schedule -> prompt -> menu -> end`,
`SESSION_COMPLETE`, and a 35-second duration. Final device state was helper `READY`, Telecom idle
(`mCallState=0`), requested and actual audio mode `MODE_NORMAL`, with no durable or temporary mixer
snapshot. Both configured retry causes, invalid input and no input, now have live coverage.

### Dashboard IVR-disabled policy

The dashboard policy was changed to `IVR disabled` and published as immutable revision 4. The
tablet activated and acknowledged revision 4 through normal synchronization. A call from the
previously allowlisted test number then remained with the stock Android dialer and was not
answered by IVRdroid. The app log independently reported that the incoming call remained with the
stock dialer under the active caller policy; the helper received no new session.

The uploaded server record reported revision 4, `IVR_DISABLED`, `STOCK_DIALER`, an empty menu
path, and zero IVR duration. The helper remained `READY`, with no mixer snapshot. This verifies
that dashboard-level IVR disablement bypasses the IVR even for a number retained in the draft's
allowlist.

### Allowlist nonmatch policy

The dashboard was returned to `Allowlist only`, the test entry was temporarily removed, and the
result was published as immutable revision 5 with an empty allowlist. After the tablet activated
and acknowledged revision 5, another call from the same known test number remained with the stock
dialer and never started a helper session. The app log again reported stock-dialer routing under
the active caller policy.

The event arrived through the normal asynchronous sync cycle without a manual sync. Its server
record reported revision 5, `NOT_ALLOWLISTED`, `STOCK_DIALER`, an empty menu path, and zero IVR
duration. This distinguishes an allowlist nonmatch from both the revision-4 `IVR_DISABLED` result
and the local kill-switch override.

### Accept-all policy and call-event completion race

The dashboard policy was changed to `Accept all` while the allowlist remained empty and published
as immutable revision 6. After activation and acknowledgement, the same known test caller was
handled by IVRdroid, heard the prompt, selected digit 0, and was disconnected normally. This
proved that the mode itself admitted the caller rather than a retained allowlist entry. Device
evidence showed `SESSION_COMPLETE`, `schedule -> prompt -> menu -> end`, Telecom idle,
`MODE_NORMAL`, and no mixer snapshot.

That first call also exposed a control-plane race. Its `IN_PROGRESS` event reached the server
during the call; the later terminal event used the same call ID, but the server's duplicate check
acknowledged it without updating the stored row. The tablet then correctly removed the
acknowledged local event, leaving the dashboard row permanently in progress.

The event endpoint was changed to permit a same-device `IN_PROGRESS` record to advance to a
terminal result while preserving terminal records against stale retries and rejecting a call ID
owned by another device. The regression test submits an initial event, a terminal replacement,
and a stale initial retry, then verifies the terminal path, result, duration, event details, and
unchanged encrypted caller value. All 11 server tests passed.

Before deployment, the running API image was tagged for rollback and a PostgreSQL custom dump was
validated. The protected rollback set is
`/Users/parhamfatemi/Backups/ivrdroid/20260806T170155Z`. Only `ivrdroid-api-1` was recreated with
the new amd64 image; the IVRdroid web/database containers and every unrelated container retained
their container IDs. The replacement API, Compose web health, and public Access-protected origin
returned their expected healthy responses.

A second live revision-6 call completed normally and uploaded `IVR_HANDLED`,
`schedule -> prompt -> menu -> end`, `SESSION_COMPLETE`, and a 13-second duration. Its upload
occurred only after completion, so it verified the production terminal-record path but did not
recreate the original timing race; the deployed-image regression test covers that transition.
The stranded pre-fix test row was reconciled to the captured terminal path and 13-second duration
from helper evidence, with a `call.repaired` audit entry. Both revision-6 call rows now report
`SESSION_COMPLETE` rather than leaving a false active call.

### Exclusion-list policy

The dashboard was changed to `All except exclusion list`, and the known test number was added to
the exclusion list before publishing immutable revision 7. The tablet activated and acknowledged
revision 7 with one excluded number and unknown callers still routed to the stock dialer.

A call from that excluded number remained with the stock Android dialer and never started a helper
session. The app log independently reported stock-dialer routing. The event subsequently arrived
through the normal sync cycle as revision 7, `EXCLUDED_CALLER`, `STOCK_DIALER`, an empty menu
path, and zero IVR duration. This completes live coverage of all four dashboard caller-policy
modes for a caller with a usable number.

### Hidden-caller constraint and configuration restoration

The available test phone could not place a call with its caller ID hidden. The hidden/unknown
caller path therefore remains untested; the configured stock-dialer behavior is not counted as
live coverage.

After the caller-policy matrix, the exact revision-2 configuration was restored through the
application's rollback endpoint rather than by modifying PostgreSQL. Dashboard automation could
open the browser's native confirmation dialog but could not submit it reliably, so the same
authenticated application operation was invoked locally inside the production API container
after confirming revision 7 was still latest. This created immutable revision 8 with
`source_revision_id: 2`, updated the draft, assigned the desired revision, and recorded a
`revision.rollback_published` audit entry.

The tablet downloaded, staged, activated, and acknowledged revision 8 in roughly 40 seconds.
Server state then reported desired revision 8 and active revision 8. A redacted configuration
check confirmed `ALLOWLIST_ONLY`, one allowed caller, an empty blocklist,
`route_unknown_callers: false`, one `Asia/Tehran` schedule with Wednesday 00:00-23:59, and no
schedule exceptions. This removes the temporary Thursday test exception and restores the exact
pre-matrix policy without rewriting revision history.

### Helper and app autostart verification

Before changing the helper's boot marker, the installed APK and both Magisk modules were captured
and their hashes verified in
`/Users/parhamfatemi/Backups/ivrdroid/20260807T094519Z-reboot-proofing`. The checkpoint retains the
exact helper `disable` marker. Recreating that marker with mode 0644 and rebooting is the scoped
autostart rollback. The live helper self-test passed before mutation with Telecom idle,
`MODE_NORMAL`, revision 8 active, and no mixer snapshot. Only the helper module's `disable` marker
was then removed; no binary, revision, caller policy, Android networking setting, or Wi-Fi setting
was changed.

The first unattended reboot completed without opening the app or placing a call. ADB returned 52
seconds after the reboot request on a new kernel boot ID. The helper supervisor was then a
Magisk-started PID-1 child rather than the previous manual `su` process, its child helper reported
`READY`, active revision 8 and staged revision 0 remained intact, and the marker remained absent.
Android independently showed that `BOOT_COMPLETED` started `SyncService` as foreground service
1401. The app was not stopped, the call-screening role and restricted-network permission remained
assigned, the server received a fresh revision-8 heartbeat, requested and actual audio mode were
both `MODE_NORMAL`, Telecom was idle, and no mixer snapshot existed. Installed APK/helper hashes
were unchanged and the post-boot self-test passed without mutation.

A second unattended reboot was intentionally attempted before claiming repeatability. The tablet's
ADB address remained network-unreachable for more than three minutes and the server received no
post-reboot heartbeat. Wi-Fi diagnostics retrieved after connectivity returned showed that the
saved network was selected about 30 seconds after boot, but its WPA four-way handshake timed out
and Android disconnected. The next connection attempt did not occur until roughly 31 minutes and
48 seconds after boot, while the screen was still off; it then associated, obtained IP
configuration, and validated Internet in about one second. Router pings to the tablet's previous
static address coincided with the retry, but cannot by themselves wake a station that the Wi-Fi
framework records as disassociated, so causation is not claimed. No Wi-Fi remediation was applied
because connectivity recovery remains a separate phase.

The delayed inspection independently established that local autostart had succeeded long before
network recovery. At roughly 33 minutes of uptime, the PID-1 Magisk supervisor had been running
for 32 minutes 57 seconds, its helper child for 32 minutes 34 seconds, and the helper reported
`READY` with revision 8 active and no disable marker. Android showed the foreground `SyncService`
was launched from `BOOT_COMPLETED`; after connectivity returned the server received a fresh
desired/active revision-8 heartbeat. The call-screening role and permissions remained assigned,
Telecom was idle, requested and actual audio modes were `MODE_NORMAL`, and neither durable nor
temporary mixer snapshots existed. This gives app/helper autostart two consecutive unattended
warm-reboot passes while keeping the Wi-Fi failure explicitly separate.

A current-version live call then exercised the helper that Magisk had started during the second
boot. The allowlisted caller matched revision 8, the helper claimed the session, verified
pre-answer microphone/speaker privacy, recovered from Samsung's three bounded route corrections,
and followed the Friday closed path `schedule -> end`. The caller reported the expected quick
termination without a menu. The server independently received revision 8, `IVR_HANDLED`,
`schedule -> end`, `SESSION_COMPLETE`, and a seven-second duration. Final state was helper
`READY`, Telecom idle, requested and actual audio mode `MODE_NORMAL`, no durable or temporary
mixer snapshot, and zero caller-audio files.

The post-activation backup at
`/Users/parhamfatemi/Backups/ivrdroid/20260805T170156Z` contains a validated PostgreSQL custom
dump, prompt archive, runtime configuration/secrets with owner-only permissions, and checksums.

Helper autostart is now enabled on the audited tablet and two consecutive unattended warm reboots
passed. The allowlist policy, open and closed schedule branches, prompt playback, all configured
DTMF branches 0, 1, and 2, automatic disconnect, real tablet acknowledgement, local kill-switch
override and restoration, cached operation during an API outage, automatic event upload and error
clearing after recovery, post-call `MODE_NORMAL`, and absence of caller-audio files now have live
coverage. A physical cold power cycle remains a separate acceptance case. Hidden callers and live
tampered-revision rejection also remain pending and must not be described as passed.

### Reconnect-only boot Wi-Fi guardian

The second reboot's WPA handshake timeout motivated an app-side recovery path, but remote Wi-Fi is
the only tablet management channel. Automatic radio cycling was therefore rejected: the guardian
has no command capable of disabling Wi-Fi. During the first three minutes after boot it can only
enable a radio already reported off or request saved-network reconnect at 45, 90, and 135 seconds.
It does not store an SSID/password, select or rewrite a saved network, or act unless the app call
state, helper state, and Android audio mode are all idle. A Wi-Fi link with a usable address is
never reset merely because Internet validation fails.

Pure policy tests cover an already-connected boot, spaced reconnects, delayed service startup,
an initially disabled radio, an active call for the entire window, a call ending within the
window, a locally connected but unvalidated network, and the hard deadline. The sanitized result
contains only outcome, completion time, boot elapsed time, validation state, and bounded attempt
counts.

The version-7 APK was installed as an updated privileged system app while the unchanged version-6
system-module APK remained available as the rollback base. Its installed SHA-256 was
`338add07b355b13096e2e0eec417476c99dc2256fda52f8751e8e9abd402dc83`. During the supervised warm
reboot, Android selected the existing saved network and began its normal connection attempt; that
first authentication failed and Android returned to its disconnected state. After the failed
attempt had ended, the guardian issued exactly one accepted saved-network reconnect request. It
did not disable or re-enable the radio. The second attempt associated, obtained an address, and
validated Internet, and the guardian recorded `reconnect_recovered` at 49,698 ms after boot with
one reconnect and zero enable requests. ADB returned 67 seconds after the reboot request.

`BOOT_COMPLETED` independently started the foreground sync service and the Magisk supervisor
started the helper. Post-boot state was helper `READY`, active revision 8, staged revision 0,
Telecom idle, requested and actual audio mode `MODE_NORMAL`, and no mixer or caller-audio
snapshot. The production server received the same sanitized guardian result from app
v0.5.1-dev, with desired and active revision 8 and no device error. API, web, and PostgreSQL were
healthy with zero restarts; the loopback health endpoint returned 200, the public origin returned
the expected Access redirect, and the existing RX1, TextBee, and WGMik containers remained up.

This is live coverage of the exact previously observed failed-handshake recovery path, but not a
physical cold power cycle or every possible access-point failure. Those remain separate tests.

### IVR flow canvas navigation

The fixed oversized flow layout was replaced with a bounded viewport while keeping the existing
tree editor and draft contract unchanged. The canvas now supports background drag, trackpad or
wheel pan, cursor-centered Ctrl/Command-wheel zoom, explicit zoom-out/zoom-in controls, a 100%
reset, fit-to-view, and keyboard navigation. Pan bounds keep part of the graph reachable, and
pointer starts on nodes or controls remain editing interactions rather than pan handles.

The dashboard suite passed six tests, including focused coverage for wide-flow fitting, zoom and
reset controls, pointer drag, node interaction exclusion, and keyboard shortcuts. The TypeScript
and Vite production build completed with the expected hashed assets. Rendered QA used the same
five-node draft shape as production: at 1909x1223 all 12 rendered branch cards fit at 52% with the
inspector open, while a 760x900 viewport fit all cards at 30% and kept the controls above the
mobile navigation. Neither viewport produced a console warning or error.

The previous production web image and source were retained under
`/Users/parhamfatemi/Backups/ivrdroid/20260807T132332Z-flow-canvas` on `old-mac`; rollback image
`ivrdroid-web:rollback-20260807T132332Z` resolves to
`sha256:2aba04326c0f198eb6b3b8904b745d73d2b29996d2b8167561c46dee1d2f7262`. Only the web container
was recreated. The deployed image is
`sha256:8c19464775c3b81970786f88b42cb725919cb0c797d634d6153578c21b60f0d3`, healthy with zero
restarts; the API and PostgreSQL container IDs remained unchanged and the RX1, TextBee, and WGMik
containers remained up.

Live Access-authenticated QA on `https://ivrdroid.rx1.ai/ivr-flow` exercised 100% reset, a 250 px
background drag, fit at 52% with the inspector open, zoom-in to 62%, and node selection opening the
`Collect one digit` inspector. After closing the inspector, fit settled at 69% with all 12 rendered
cards inside the canvas. No draft save or publish action was invoked, and the final console log
contained no warning or error.

## 2026-08-07 Flow Studio and runtime V2 cutover

Migration `0002_flow_runtime_v2` was first rehearsed against the exact production backup. It
preserved all nine signed V1 revisions, archived the V1 draft, and initialized a blank V2 draft
without changing the active V1 revision. The production API and web were then deployed as 0.6.1;
PostgreSQL, RX1 Dashboard, TextBee, WGMik, and the existing Cloudflare route were not recreated.
All three IVRdroid containers were healthy after deployment and the public origin retained its
expected Access redirect.

The production V2 draft was rebuilt in the new owned-tree Studio as 13 actions and seven branches:
`intro` leads to one digit collector, digits 1-4 independently own `o-1`, `o-2`, `o-3`, and `o-4`
plus their own End Call actions, and No input, Invalid, and Return limit each own an End Call. The
server accepted the draft at edit version 3. Authenticated browser QA confirmed the four distinct
branch prompts, hidden internal UUIDs, the 12-descendant root deletion warning, server-side
validation, and a simulator trace of `intro -> menu -> digit:1 -> o-1 -> End call`. Digit 1-4,
three invalid inputs, and three timeouts all reached complete simulator states.

Signed V2 revision 10 first activated on the idle SM-T585. Its source hash was
`ad3f34efb7d033460cde63e97e1d991cde12b7403166ca7bb0038d9aed6e7b1e` and program hash was
`4443730dbab450dab28c3cefd32e1bd7779655cda8571992a0696b804843749b`. The instruction tape mapped
digits 1-4 to four separate PLAY/END instruction pairs and six prompt files matched their digest
filenames.

The emergency rollback drill selected immutable V1 revision 9. It initially exposed a real app
compatibility defect: the old V1 schedule cache had been expanded at download time, so the new
compiler rejected the same revision directory as different content. App 0.6.1 now anchors new
compilation to signed `published_at` and accepts an old V1 cache only if the complete document can
be exactly reproduced from the signed manifest and its embedded horizon. Regression tests cover
determinism, production offset timestamps, compatible V1 caches, and tampered-cache rejection.
After installing that fix, revision 9 reached helper `READY`, server desired/active `9/9`, and
retained revision 10 as the previous runtime.

Revision 10 was then restored through the dashboard as new signed V2 revision 11. The tablet
downloaded, verified, staged, activated, and acknowledged it while idle. Final server state was
desired/active `11/11`; helper state was `READY,11,0,REVISION_ACTIVATED`, previous revision was 9,
and app/helper config files had identical SHA-256
`ce93d3c60186da3f85de1793663f35ed85797cad1f7936d3fac06f8bfca6d0d6`. Revision 11 retained the
same source and program hashes as revision 10, and all six prompt assets again matched their
digest filenames.

The final local gate passed Android unit tests, lint, APK assembly, all native policy/protocol
tests, the pinned arm64 helper build, and cross-directory helper reproducibility. The server suite
passed 21 tests; the dashboard passed 14 tests across five files and its production build passed.
The pre-V2 server backup is `/Users/parhamfatemi/Backups/ivrdroid/20260807T160319Z-flow-v2`, the
revision-10 server checkpoint is
`/Users/parhamfatemi/Backups/ivrdroid/20260807T163422Z-v2-revision10`, the pre-V2 tablet checkpoint
is `/Users/parhamfatemi/Backups/ivrdroid/20260807T160319Z-flow-v2-device`, and the immediately
preceding app/module checkpoint is
`/Users/parhamfatemi/Backups/ivrdroid/20260807T165923Z-v2-app-0.6.0`. The final revision-11
checkpoint is `/Users/parhamfatemi/Backups/ivrdroid/20260807T170921Z-v2-revision11-final`; its
custom PostgreSQL dump produced a 76-entry restore list, all recorded checksums passed, and the
revision-11 Ed25519 signature was independently verified against the compiled public key.

The 0.6.1 system-app module is staged but a final reboot was deliberately deferred because the
tablet was unplugged at 6% battery. The running data APK is already 0.6.1 and revision 11 is active.
Real external calls through digits 1-4, invalid, timeout, and return-to-menu paths remain pending;
simulator completion and successful program activation are not reported as physical call proof.

## 2026-08-08 Voicemail and explicit recording V3 code-only gate

Before editing, the complete uncommitted V2 worktree was archived at
`/Users/parhamfatemi/Backups/ivrdroid/20260808T170455Z-pre-voicemail-v3`. The tracked patch SHA-256
is `9749c5062e4e8bcddb5f6554994ae155b1ce852df0d699d08591d909bedcc425`; the untracked archive
SHA-256 is `94d24b38f31168d01d14cf1cadd2eef53ef9bd0e3caee36a9fa13f1dad7a0ece`. A 194-file reconstruction
matched byte-for-byte. Ignored secrets and generated build products were deliberately excluded.

V3 adds only explicit, greeted `record_message` segments. The signed global behavior defaults to
60 seconds and `#`, supports a 10-180 second hard maximum and an optional finish key, and always
stops on caller hangup or the hard limit. Validation requires a distinct `play_prompt` immediately
before each recording and compilation rejects a worst-case path over ten minutes. V1/V2 manifests
and executors remain immutable rollback targets. The V3 cross-language fixture has manifest
SHA-256 `1bdbc56be1fd1b63a571b2462f21b6dbbc8029c1b0d1d94e3de4555322db202b`, source SHA-256
`fbf8eecfeff3296e506d7921d15f50ab2526489d995dff1c3dfc50cc18ab8102`, and program SHA-256
`e83d54c4f864b2b6e8a7acae50e6119438bd954a26ccd15267d56d9d8a4d6a65`.

The code-only validation gate passed:

- `./scripts/check.sh`: 36 debug and 36 release Android unit-test executions, Android lint, debug
  APK assembly, 12 native host executables, the pinned arm64 helper build, and cross-directory
  reproducibility. The V3 app/helper versions are `0.7.0-dev` with version codes 10/21.
- Server: 34 pytest tests passed, covering V3 validation/compilation/simulation/rollback,
  authorization/ownership/idempotency, bounded streaming and offsets, signed finish/duration
  checks, malformed WAV/hash rejection, mono 16 kHz 48-kbps MP3 normalization, AES-GCM storage and
  key rotation, range/no-store delivery, quota reservations and `507`, acknowledgement-before-
  delete, retention tombstones, and abandoned-upload restart.
- Dashboard: 27 Vitest tests across seven files and the TypeScript/Vite production build passed.
  Coverage includes V3 editing/simulation, recording settings versus retention, loading/error/
  empty states, inbox filtering/media/listened/deletion/pending behavior, call links, and drawers.
- A fresh SQLite database upgraded through `0001`, `0002`, and
  `0003_voicemail_recordings`; all three recording tables existed at Alembic head. Compose config
  validation passed. Both pinned `linux/amd64` images built: API
  `sha256:b0ce25cf1ca0643d6021d3d108792fb8b1e10f11500f64a67c88c8634d49868e` and web
  `sha256:acb144ce1bc1a88eff4a7cd4f98f4fb560710c5443a356a0a353f754b63ceeb3`. The API imported as
  contract version 3.0.0 and nginx syntax passed with its expected Compose `api` hostname supplied.
- Disposable localhost browser QA created a V3 revision, enrolled a synthetic device, uploaded a
  UUID-correlated call and a resumable 48 kHz stereo PCM16 fixture, received the verified server
  acknowledgement, and loaded the normalized 1.296-second message. The inbox filter, drawer,
  media control, download event, listened state, call-trace deep link, ready/pending counts, Escape
  focus restoration, desktop layout, and 390x844 layout worked. The compact page and drawer had no
  horizontal overflow, and the browser console had no warning or error.

No production service, production database, Cloudflare route, active revision, or tablet artifact
was changed, and nothing was pushed. Real caller-speech quality remains explicitly **UNVERIFIED**.
Production readiness is blocked until a later consented ten-second physical call proves
intelligible caller speech, no tablet-microphone/ambient capture, no prompt bleed, safe restoration
to `MODE_NORMAL`, no stale mixer snapshot, successful acknowledged upload, and dashboard playback.

## 2026-08-09 V3 production rollout and physical-path test

Deployment was separately authorized on 2026-08-09. Before mutation, the server checkpoint
`/Users/parhamfatemi/Backups/ivrdroid/20260809T121502Z-pre-v3-production` preserved the service
tree, custom PostgreSQL dump and restore list, prompts, images, and baseline. The tablet checkpoint
`/Users/parhamfatemi/Backups/ivrdroid/20260809T121502Z-pre-v3-production-tablet` preserved the
installed 0.6.0/0.6.1 app artifacts, 0.6.0 helper, private state, active V2 configuration, hashes,
and baseline. Every recorded checksum passed before deployment.

The initial production API and web images were respectively
`sha256:20edc13c488a1d0ace882ba61af927e1dc921c3eda3ec2b3ea812fd6c9af36bd` and
`sha256:cb3289fb6cd4151cc56cdd34ba8b9b185d9ffe972c3ba24e8b7fe98f7a3f0023`.
PostgreSQL reached `0003_voicemail_recordings` at head. The existing database container and the
RX1 Dashboard, TextBee, and WGMik containers were not recreated and remained healthy. The
dedicated recording volume is owner-only, and the independent version-1 AES recording key is not
the existing configuration/data key.

The tablet installed the 0.7.0 system-app module and helper without clearing app data. The
installed APK SHA-256 is
`d54596dccd44b104a9e5c193a4080921e59a91b40f7caa8c7ae97e615fe00969`; the helper binary SHA-256
is `be815ef050e3530e2d69cfe1851d808e4594e4c4e777575c68e04467d305ebb6`. Its restricted-networking
mode remained enabled, IVRdroid retained the call-screening role, and helper self-test passed.
Signed V3 revision 13, manifest SHA-256
`d56ecffabe796de4d3c5d713845f8808ee53e75593a3e6bf4474301fc890a652`, inserted one greeted
`record_message` step on digit 4 with a 60-second maximum and `#` finish key. Server and tablet
both acknowledged revision 13; immutable V2 revision 12 remains the immediate rollback target.

A consented external call then traversed the main prompt, digit 4 notice, built-in beep, and the
explicit recording step. Pressing `#` produced helper `SESSION_COMPLETE`. The finalized 48 kHz
stereo PCM16 source was 1,339,244 bytes and 6,975 ms after finish-key trimming. Android encrypted
a 1.34 MB spool item, resumed it as two bounded chunks, received the verified completion
acknowledgement, and only then removed the encrypted spool and root handoff files. The server row
is `ready`, records stop reason `finish_key`, and points to a 42,420-byte, version-1 AES-GCM
encrypted normalized MP3. No abandoned upload row remains.

The production Voicemail inbox rendered the new seven-second message as unlistened with its
drawer, download action, and call-trace link. After the call, Telecom was idle, requested and
actual audio modes were both `MODE_NORMAL`, helper capacity and app spool were zero, and neither
the durable nor temporary mixer snapshot existed. No API/web error or 5xx appeared during the
test. This proves the physical capture-to-inbox transport path, but it does **not** yet prove the
acoustic boundary: intelligible caller speech, absence of tablet-microphone/ambient capture, and
absence of prompt bleed remain pending the caller's dashboard listening report.

The verified post-call server checkpoint is
`/Users/parhamfatemi/Backups/ivrdroid/20260809T131106Z-post-v3-production`; it contains the service
tree, V3 images, validated custom database dump, prompts, encrypted recording media, independent
recording key, baseline, and checksums. The matching tablet checkpoint is
`/Users/parhamfatemi/Backups/ivrdroid/20260809T131106Z-post-v3-production-tablet`. Nothing was
committed or pushed.

## 2026-08-09 Voicemail loudness and dashboard player follow-up

The caller reported that the physical recording contained only the caller voice, with no ambient
tablet-microphone capture and no prompt bleed, but the first normalized MP3 was far too quiet and
required about 40 dB of manual gain to understand comfortably. Measurement placed that production
file at -41.6 LUFS integrated and -24.4 dBFS true peak. The pre-change checkpoint
`/Users/parhamfatemi/Backups/ivrdroid/20260809T133839Z-pre-loudness-fix` preserved the original
encrypted media, database and key material, live source, API image, and checksums.

Server 0.7.1 now selects the audited caller channel instead of folding potentially anti-phase
stereo channels together, then applies `loudnorm=I=-18:TP=-1.5:LRA=7` to non-silent recordings of
at least 500 ms. Short or exactly silent inputs retain the bounded mono-only path. A new quiet,
anti-phase fixture proves that speech remains present and lands between -19.5 and -17.0 LUFS.
All 35 server tests passed. Only `ivrdroid-api-1` was recreated; the web and PostgreSQL container
IDs were unchanged. The live API image is 0.7.1,
`sha256:2fe03189125633a682c519addf0df74edbb788c039074ae3ba999845aefb07aa`.

The existing message was reprocessed through the corrected path to -18.5 LUFS integrated and
-1.9 dBFS true peak. The caller accepted the corrected dashboard playback as appropriately clear
and loud. This closes the listening and acoustic-isolation questions for the captured source, but
one fresh consented call is still required to prove that a newly uploaded message receives the
corrected normalization automatically.

Playing that unlistened message then exposed a dashboard state bug: the successful listened update
removed it from the `Unlistened` query, and the query effect incorrectly destroyed the active
drawer. The selected recording is now independent of filtered-list membership; server state and
the inbox still refresh, while the drawer and the same audio element remain mounted until the
operator closes it. A regression covers the exact transition and the reverse mark-unlistened path
continues to refresh the filtered list.

The dashboard passed 28 Vitest tests across seven files and its TypeScript/Vite production build;
Compose validation and `git diff --check` also passed. Before deployment,
`/Users/parhamfatemi/Backups/ivrdroid/20260809T135827Z-pre-voicemail-player-fix` preserved the live
0.7.0 web image, dashboard source, Compose file, and container metadata with verified checksums.
Web 0.7.1 was built and health-checked on isolated loopback port 3201 before only
`ivrdroid-web-1` was recreated. The API and PostgreSQL container IDs matched the checkpoint. The
live web image is
`sha256:7c4340d8d641c3faaa982cf35beeedd2c9ef71e1cc0c736540cb34e2321a2b8c`, serving bundle
`index-Bn-TCYfF.js` with SHA-256
`b78a2e57bde5c3e7a93bfe42115dd026ebf179b9999d0faa2ca76b347dd91819`.

Authenticated production QA marked the original message unlistened, opened it under the
`Unlistened` filter, and started playback with the keyboard media control. At 00:06 of 00:07 the
inbox correctly showed zero unlistened messages and `Voicemail marked listened`, while the drawer
remained open and the audio control was still playing. Closing the drawer and switching to
`All messages` showed the original item as `Listened`, restoring its pre-test state. The page was
nonblank, had the expected IVRdroid identity and hashed bundle, no framework overlay, and no
browser console messages. Nothing was committed or pushed.

The final current-state checkpoint is
`/Users/parhamfatemi/Backups/ivrdroid/20260809T140945Z-post-voicemail-fixes`. It contains the
0.7.1 API/web images plus the pinned PostgreSQL image, validated custom database dump and restore
list, prompt and encrypted recording media, recording key, synchronized service tree, live
baseline, and checksums; every recorded checksum and archive integrity check passed. No tablet
artifact changed after the existing post-V3 tablet checkpoint.

## 2026-08-10 External-call answer-timeout cleanup fix

The first production no-answer test reached `DIALING` and disconnected the exact operator leg at
the configured 30-second deadline, but the helper sent the generic `HELPER_CANCELLED` reason.
Android therefore published `SYSTEM_FAILURE`, and repeated early `unhold()` requests raced Samsung
Telecom teardown. Late queued requests became Swap operations after the caller was already active,
so the helper's three-second single-call verification failed with `UNVERIFIED_CALL_PREEMPTED`.

The V1 bridge now preserves `ANSWER_TIMEOUT` end to end and accepts only the matching
`NOT_CONNECTED/ANSWER_TIMEOUT` terminal. Android journals each cleanup action before invoking
Telecom, disconnects the exact operator leg once, waits until the operator and conference `Call`
objects are removed, allows a two-second carrier auto-unhold grace, and issues at most one explicit
unhold. The original caller must then remain continuously active for one second. The helper
independently confirms the persisted original Telecom identity as the sole safe call for another
second before any branch runs; mismatched outcomes, cleanup timeout, replacement identity, or an
unsafe topology remain no-branch failures.

The Android external-call tests cover callback/poll storms, disconnected-but-not-removed legs,
carrier auto-unhold, one-shot fallback unhold, active-state resets, same-boot recovery without
command replay, and cleanup timeout. The native suite covers the exact cross-language timeout
fixtures, terminal matching, guardian ownership, and independent caller stability. The complete
`scripts/check.sh` gate passed, including Android debug/release tests, lint, assembly, all 13 native
test domains, arm64 build, and two-path app/helper reproducibility.

Before deployment, the validated tablet rollback at
`/Users/parhamfatemi/Backups/ivrdroid/20260810-110840-external-timeout-cleanup` preserved app data,
the installed APK, and active/staged Magisk modules. Android `0.8.2-dev` code 13 was installed with
APK SHA-256 `4284e00f176c248e48eeb57b9068dd3f6099db53a8ff19ed72ad049c57b23059`;
helper `0.8.1-dev` code 23 was started with binary SHA-256
`f65adb6ab50dab7b24e245ab058fcc97b2d9a9bd6621516be47e310e4d122d59`. Both modules were also
staged without a disable marker for reboot persistence. No reboot or physical call was performed.

Post-deployment, revision 15 remained active, helper state was `READY`, Telecom had zero calls,
audio requested and actual modes were `MODE_NORMAL`, no mixer snapshot existed, the stock Dialer
still held the Dialer role, and IVRdroid retained Call Screening plus both privileged call-control
permissions. Production synchronized app/helper versions, V4 call-control and conversation
recording capabilities, zero spools, idle call state, and no recovery journal. The historical
failure remains in call history; a new no-answer call is required to prove the corrected physical
branch transition.

During the final read-only health probe, an unrelated host `ffmpeg` render was consuming more than
four CPU cores and old-mac load exceeded 30. PostgreSQL twice entered automatic crash recovery;
the IVRdroid deployment had not changed any server container, image, database, or route. Recovery
was allowed to finish without a container restart, only deployment-owned blocked inspection
processes were terminated, and the user render was not touched. After the render ended, database,
API, and web health all returned `healthy` with zero failures, `/health` returned 200, and the
tablet synchronized again at `2026-08-10T08:03:01Z` with revisions 15/15, idle call state, both V4
capabilities true, no recovery pending, local IVR enabled, and no reported error.

## 2026-08-10 External-call merge and caller-hangup cleanup fix

The next physical test proved that the carrier accepted the outgoing operator call but exposed two
failures: IVRdroid did not merge it, and caller hangup left the tablet-to-operator leg active. The
Telecom audit showed the caller as `TC@8` and the operator as `TC@9`. Both calls reported each other
as conferenceable after the operator became active, so the carrier and Samsung Telecom merge path
were available. IVRdroid nevertheless never requested the conference because the helper had sent a
`CANCEL` ten milliseconds before the outgoing `Call` object appeared.

Two independent app defects made that race unsafe. `CALLER_HELD` was published before the engine
committed its new monotonic transition timestamp, allowing the bridge to observe a stale snapshot.
Separately, late outgoing-call ownership depended on the mutable bridge request still being `DIAL`.
Once the helper had replaced that file with `CANCEL`, the app could not associate `TC@9` with the
durable session journal; cleanup therefore had no exact operator identifier and refused to issue a
blind global hangup. Production retained the resulting sanitized `SYSTEM_FAILURE/CLEANUP_TIMEOUT`
events and `EXTERNAL_CALL_PREEMPTED` result.

Android `0.8.3-dev` commits the held transition before publication, waits for the reciprocal
conferenceable callback instead of treating its absence at answer time as final, and then invokes
conference only for the exact owned caller/operator pair. The existing ten-second merge watchdog
still bounds missing capability or merge failure. Outgoing ownership is now reconstructed from the
same-boot recovery journal plus the verified active manifest, signed phone number, direction,
revision, block, timeout, and nonterminal session state; it no longer trusts the mutable request
file. Cleanup may adopt only that unique exact leg and disconnect it when the caller disappears.

Regression tests cover transition publication ordering, delayed conferenceability, reciprocal
conference gating, and late operator ownership after the bridge advances to `CANCEL`. The complete
`scripts/check.sh` gate passed: Android debug/release tests, lint and assembly, all native policy and
protocol suites, the arm64 helper build, and two-path app/helper reproducibility. The installed APK
SHA-256 is `0a191d0f69fd5289b7a2a6b9f163883981a463f3d1418d8c3e2666a0ba3d4eee`; the staged system-app
module ZIP SHA-256 is `f3403e73a9ebea3328d8b8f5877a92e6763635a66867fee10a75051b095866ac`.

The rollback checkpoint is
`/Users/parhamfatemi/Backups/ivrdroid/20260810-195100-external-merge-cleanup`; its app-data archive,
previous installed APK, Magisk-module archive, and candidate artifacts passed checksum and archive
integrity verification. The tablet now runs app `0.8.3-dev` code 14 with helper `0.8.1-dev` code 23;
the stock Dialer remains default, IVRdroid retains Call Screening and privileged call control,
revision 15 is active, Telecom is idle, and no reboot was performed.

The authoritative control plane is the migrated Hetzner host, not old-mac. Hetzner reports runtime
versions 1–4, both external-call capabilities true, protocol 1, no recovery pending, and clean 200
device syncs. Its latest failed call preserves the four external-call events for diagnosis.
Retired old-mac IVRdroid containers were returned to their stopped state. No server image, schema,
database row, or flow revision changed. A fresh physical answered call followed by caller hangup is
still required to prove automatic merge, two-way recording, and exact operator-leg teardown.

## 2026-08-11 Optional digit-branch prompt

The Add digit branch dialog now offers `No prompt`. Choosing it creates a valid owned `end_call`
placeholder as the branch root instead of a prompt block with a null prompt reference. The
placeholder can be replaced with another permitted action from the canvas. The independent safety
contracts remain unchanged: `record_message` still requires a preceding greeting and the mandatory-
recording `external_call` still requires its preceding recording/connection notice.

The dashboard passed 41 tests across seven files and its TypeScript/Vite production build. The
server passed all 65 tests, including a V4 compiler/simulator regression proving that a promptless
digit branch targets `end_call` directly without a prompt instruction. Compose validation and
`git diff --check` also passed. Local rendered QA created the promptless branch through the real
development API, saved it, reloaded it, and retained `All good` with zero prompt cards or console
warnings/errors. The modal also fit within a 390x844 viewport.

Only the Hetzner web container was recreated. Production now runs `ivrdroid-web:0.8.2` at image
digest `sha256:26a14b1f8ff4cd3bf06161d07204644a8341b96f373cc38d760671e2e14471f2`;
the API and PostgreSQL container identities/start times remained unchanged and healthy, both origin
routes returned 200, and RX1 Dashboard plus TextBee neighbors remained up. The verified rollback is
`/opt/ivrdroid/backups/20260811T090529Z-no-prompt`, containing the prior compose/runtime files,
source archive, image archive, checksums, and rollback tag for the former web digest
`sha256:6b7c954656c2558017b24a06a74e16e5f0992127945677626a33af6e92a35037`.

Authenticated production QA loaded `/assets/index-V57rsDP1.js`, exposed `No prompt` alongside the
uploaded prompts, and enabled Add branch. An unsaved digit-5 test changed 44 steps/31 branches to
45/32, remained `All good`, showed digit 5 as `End call`, and kept the Play-prompt card count at 12.
The production V4 simulator accepted that unsaved document and completed the exact trace
`Play prompt (intro) -> Collect one digit (digit:5) -> End call`. Reload discarded the test and
restored 44/31 with Save draft disabled. No production draft was saved and no revision was
published.

## 2026-08-11 Existing prompt removal follow-up

The Play prompt inspector now exposes `No prompt` for existing steps. Choosing it removes the
prompt block structurally and promotes its existing next action, preserving that action's block ID
and complete owned subtree. A prompt without a next action receives a valid `end_call` placeholder.
Undo restores the removed prompt. The editor refuses the operation when the next action is
`record_message` or `external_call`; the dropdown keeps the unavailable option visible and explains
that the recording greeting or notice is required.

The dashboard passed 46 tests across seven files plus its TypeScript/Vite production build. The
server retained all 65 passing tests, including the V4 promptless-branch compiler/simulator case.
Compose validation and `git diff --check` passed. Real-browser local QA used the development API to
prove both rendered states: a safe existing prompt promoted its exact next `end_call`, remained
`All good`, and enabled Undo/Save, while a prompt directly before an external call exposed the
disabled `No prompt — recording notice required` option and its reason. Both states had clean
browser warning/error logs.

Only the Hetzner web container was recreated. Production now runs `ivrdroid-web:0.8.3` at image
digest `sha256:15da3e47ec80da8865fb9403785eaae19dfbecf3e21d5c904948bf6ee7a1451a`.
The API and PostgreSQL container IDs and start times remained byte-for-byte unchanged and healthy;
both origin routes returned 200, and the checked RX1 Dashboard and TextBee neighbors stayed up.
The verified rollback is `/opt/ivrdroid/backups/20260811T093118Z-existing-no-prompt`, with the prior
0.8.2 image and rollback tag, compose/runtime files, source and image archives, pre/post container
evidence, health responses, and verified checksums.

Authenticated production QA loaded `/assets/index-Cua2OK4T.js` and exercised `No prompt` on an
existing unsaved Play prompt. The edit changed 44 steps/31 branches to 43/31, removed only the
prompt, promoted `Collect one digit — menu`, and remained `All good`. The V4 simulator trace began
directly at `Collect one digit (menu)`, with no separate Play prompt instruction. Reload discarded
the QA edit and restored 44/31, `Play prompt — intro`, and a disabled Save draft button. No draft was
saved and no revision was published.

## 2026-08-11 Earlier menu notice for recorded branches

The V4 recording-notice contract now follows the exact owned call path instead of requiring a
branch-local prompt immediately before every `record_message` or `external_call`. A configured
earlier `play_prompt` or `collect_digit` menu prompt covers the recorded action, so the redundant
branch prompt can be changed to `No prompt` while its recorded action and owned outcomes remain in
place. A recorded action with no earlier prompt on its path is still rejected by the editor, local
validation, API validation, and compiler. The inspector warns that, when menu barge-in is enabled,
the spoken notice must finish before any accepted digit can interrupt it.

The dashboard passed 50 tests across seven files and its TypeScript/Vite production build. The
server passed all 69 tests. New coverage proves both voicemail and external-call branches validate,
compile, and simulate directly from a prompted menu, while promptless paths remain invalid. Compose
validation and `git diff --check` passed. Rendered local QA built an existing prompted branch before
an external call, exposed an enabled `No prompt`, retained the external action, passed the real API
validator, and simulated `Collect one digit (digit:0) -> External call` with no branch prompt and no
browser warnings or errors. A descendant regression also keeps `No prompt` disabled when the
selected prompt is the only notice for a recorded action deeper under another branch block.

Production on Hetzner now runs `ivrdroid-api:0.8.2` at image digest
`sha256:22990f3ebd1aeb700da2077162e960b5477417076b3d589f99733d5e0d88f40b` and
`ivrdroid-web:0.8.4` at
`sha256:e933c34428c8c5c03713536d20a6e9cfbb4740626c32c15b224217319c500181`.
PostgreSQL retained container ID
`952ee9d02a882c3f9437bdc0437479d3f99d20b8941f230595e8cc74bdae777e`, its original start time,
healthy state, and migration `0004_external_conversations`; the pre/post non-web container lists
were identical. The rollback directory is
`/opt/ivrdroid/backups/20260811T100246Z-inherited-notice`, with verified database dump, prior API
and web image archives/tags, source archives, Compose/runtime files, container evidence, and
checksums. The verified release source is
`/opt/ivrdroid/releases/0.8.4-20260811T101700Z-inherited-notice`.

Authenticated production QA exercised the direct recorded-branch behavior on the unchanged
44-step/31-branch draft. Digit 0's existing `Play prompt — err` exposed enabled `No prompt` because
the `menu` prompt is earlier on that path. The unsaved edit produced 43/31 with `All good`, retained
`External call — ••••4636`, passed server validation, and simulated the exact trace
`Play prompt (intro) -> Collect one digit (menu, digit:0) -> External call (awaiting)` without the
branch prompt. Reload restored 44/31 and disabled Save draft. API access evidence contains the
validation and simulation requests and no draft PUT or publish request; no production draft or
revision was changed. The final safety-only web rebuild serves `/assets/index-BbUymi53.js`; isolated
candidate and origin checks verified both the earlier-notice option and descendant guard strings.

## 2026-08-11 Tablet inherited-notice sync correction

The preceding production verification was incomplete. It proved the dashboard, API validator,
server compiler, and simulator, but did not exercise the signed revision through the Android
compiler and native helper. When revision 19 was published with digit 0 routed directly from the
prompted menu to `external_call`, the production tablet remained on revision 18 and reported
`Every V4 external call must immediately follow its recording-notice prompt.` Both tablet-side
validators still enforced the superseded immediate-parent rule.

Android and the native helper now use the same V4 owned-path rule as the server: a configured
earlier `play_prompt` or `collect_digit` prompt satisfies the notice requirement for downstream
`record_message` and `external_call` instructions. A recorded action whose complete owned path is
promptless remains invalid. V3 is deliberately unchanged and still requires `record_message` to
immediately follow `play_prompt`. Regression coverage exercises both V4 recorded actions behind a
prompted menu, rejects both promptless equivalents with the contract error, and proves the V3
immediate-greeting rule. The complete `scripts/check.sh` gate passed, including debug/release unit
tests, lint, assembly, all native suites, arm64 helper build, and app/helper reproducibility.

Before the tablet cutover,
`/Users/parhamfatemi/Backups/ivrdroid/20260811-1408-pre-inherited-notice-tablet-fix` preserved the
full private app state and both installed Magisk modules. The 200,545,792-byte archive passed member
checks and has SHA-256
`f1c092c6171b1bdcbf0c3cc2294bda10f32c475f0accd33c036bc284e7e4e63f`.
The no-reboot deployment installed Android `0.8.5-dev` code 16 with APK SHA-256
`6fbdff7de189718af50c80708ba31ee35899d8b135dd47b4d7008086f9af3dc3` and helper
`0.8.3-dev` code 25 with binary SHA-256
`29a883e92dff6a4ac9aee7f32d64fbee807f34b4f3f7d311f1c8a33305004243`. Both active module
trees contain the same verified artifacts for reboot persistence.

The corrected app fetched the signed production revision 19, compiled program SHA-256
`de2cc6e84b367815aef9ca3fec19e7088e485aef55aab937b7525cef9687182e`, and handed the
17,570-byte tape to the corrected helper. The exact accepted instructions include the prompted
`COLLECT` at PC 1 followed directly by digit-0 `EXTERNAL_CALL` at PC 2 with no intervening `PLAY`.
The app and helper copies of the compiled tape both have SHA-256
`f9ab58850643a61dbbc2f11b5fbd54b4e361e0f86d34f2a37299b9f3f335535f`.

Production now reports desired/active revisions `19/19`, app/helper versions
`0.8.5-dev/0.8.3-dev`, `last_error: null`, helper `READY`, result `REVISION_ACTIVATED`, no staged
revision, idle call state, zero recording spools, and no call-control recovery pending. Telecom has
no current calls; requested and actual audio modes are both `MODE_NORMAL`; no mixer snapshot or
helper disable marker exists. The stock Dialer and IVRdroid Call Screening roles remain unchanged.
The retained `call_control_state: SYSTEM_FAILURE` is historical from an earlier external-call
session; recovery is false and it did not block revision activation. No reboot or physical call was
performed during this sync correction.

## 2026-08-11 Caller Policy signed-publish path

Caller Policy now exposes `Review & publish` beside the draft controls. A clean page keeps Save
and Discard disabled. A local edit enables both controls and is visibly marked as unsaved. Review
saves only a dirty candidate, validates it, and presents one signed-revision review covering caller
policy, schedules, recording behavior, and authored flow. Publishing is bound to the reviewed draft
edit version and base revision, so a changed draft or newly published base is rejected for another
review. Broader policy modes and routing unknown callers require an explicit confirmation.

The dashboard passed 55 tests across eight files and its TypeScript/Vite production build. The
complete server suite passed, including structured V4 configuration diffs and stale-review publish
rejection. Compose validation, Python compilation, and `git diff --check` also passed.

The first API candidate (`0.8.3`) exposed macOS AppleDouble `._*.py` files in its Alembic migration
tree and failed before serving traffic. The rollback immediately restored the exact prior API
`0.8.2` and web `0.8.4` images; PostgreSQL stayed healthy at migration
`0004_external_conversations`. The corrected archives and Docker ignore rules exclude AppleDouble
and `.DS_Store` files, and the replacement image passed Alembic head discovery and a real read-only
PostgreSQL `alembic current` check before cutover.

Hetzner production now runs `ivrdroid-api:0.8.4` at
`sha256:97edc52d468ed7e8578e21ff6a0c67f7696f5600bfe88a37824c65240d7d8ff5` and
`ivrdroid-web:0.8.5` at
`sha256:bb29c9d88e409f6f7dd6a7d1fc4fa6fa2131df7f92e9c6a18c3b2711c41acd46`; API, web, and
PostgreSQL are healthy, and only web publishes `127.0.0.1:3200`. The verified rollback is
`/opt/ivrdroid/backups/20260811T133150Z-caller-policy-publish`, and the corrected release source is
`/opt/ivrdroid/releases/20260811T-caller-policy-repack.wD0iKX`.

Authenticated production Chrome QA opened Caller Policy with disabled Save and Discard controls
and a visible Review & publish action. Review compared the current draft with revision 19 and
showed the real pending allowlist addition `baba (••••5137)`, no schedule or recording changes, and
no authored flow changes. The final publish action was deliberately not invoked. A temporary local
switch to Accept all showed the unsaved state and enabled Save/Discard; Discard restored Allowlist
only without a request to the server. Browser logs were clean. API evidence contains only draft
GET, validation, and diff requests: no draft PUT and no publish request. The final database state
remained revision 19, drafts `1|9` and `4|8`, device `SM-T585|19|19`, and migration `0004`.

## 2026-08-11 Overview active caller-policy source

The Overview endpoint previously rendered caller policy from retained draft row 1, the legacy V3
authoring lane. After V4 revision 20 was published and activated with two allowlisted callers, that
legacy row still contained one caller, so Overview incorrectly displayed `1 allowed numbers` even
though Caller Policy, the signed manifest, and the tablet's active revision all contained two.

Overview now resolves caller policy from the enrolled tablet's active signed revision. Only before
the first activation does it fall back to the V4 draft. Regression coverage proves an active
revision wins over both a divergent legacy V3 draft and newer unpublished V4 edits, and separately
proves the pre-activation V4 fallback. The full server suite, Python compilation, Compose
validation, `git diff --check`, all 55 dashboard tests under the supported Node 24 runtime, and the
dashboard production build passed.

The API-only Hetzner deployment installed `ivrdroid-api:0.8.5` at image digest
`sha256:4ffd3e23b8e67162a627454fe424861f33bb02d671f672a00fb63b112f7e86e0`.
The web and PostgreSQL container IDs and start times remained unchanged and healthy. The candidate
first confirmed production PostgreSQL migration `0004_external_conversations`, active revision 20,
and the exact two labels `Primary test phone` and `baba`. The verified rollback is
`/opt/ivrdroid/backups/20260811T165959Z-overview-active-policy`; the deployed release source is
`/opt/ivrdroid/releases/20260811T-overview-active-policy.IlPn5M`.

Authenticated Chrome QA loaded the production Overview at `https://ivrdroid.rx1.ai/`, rendered
active revision 20 and `2 allowed numbers`, navigated to Caller Policy and observed `2 callers`,
then returned to Overview and retained `2 allowed numbers`. The DOM was complete, no framework
overlay appeared, and browser warning/error logs were empty. Final database state remained
revision 20, drafts `1|9` and `4|8`, device `SM-T585|20|20`, and migration `0004`.

## 2026-08-11 V4.1 external-call resolver compatibility fix

Three fresh revision-20 production calls reached `external_call` but followed System failure
before holding or dialing. Their structured events contained `UNSIGNED_REQUEST` without any
preceding `ACK` or `DIALING`. The signed revision and helper request agreed on revision, block,
destination, and timeout. The actual mismatch was local: revision 20 correctly uses compiler
`4.1.0` because prompt barge-in is enabled, while `ExternalCallInstructionResolver` still required
exactly `4.0.0`. The coordinator intentionally collapsed that resolver exception to the sanitized
`UNSIGNED_REQUEST` reason. A subsequent `RECOVERY_UNSIGNED` was secondary journal recovery under
the same incompatible check, not a separate carrier or server failure.

The resolver now retains its V4 schema and revision checks but delegates supported compiler-version
enforcement to the complete deterministic `RevisionCompiler`, which already validates both `4.0.0`
and `4.1.0` and rejects unknown versions. Regression coverage constructs a V4.1 interruptible menu
whose signed branch is an external call, verifies exact scalar resolution, and separately proves
that compiler `4.2.0` remains rejected. The focused resolver suite passed five tests. The complete
`scripts/check.sh` gate passed debug and release unit tests, lint, APK assembly, every native helper
suite, the arm64 helper build, and helper/system-app reproducibility. The unchanged server contract
suite also passed all 73 tests.

Before deployment,
`/Users/parhamfatemi/Backups/ivrdroid/20260811T1828Z-pre-v41-external-resolver-fix` preserved the
installed `0.8.5-dev` APK, critical encrypted app state, active revision 19/20 configs, call-control
journals and spools, both Magisk module trees, and the candidate artifacts. The old APK SHA-256 is
`6fbdff7de189718af50c80708ba31ee35899d8b135dd47b4d7008086f9af3dc3`; the critical-state archive
SHA-256 is `0f6892c4b062b6c4c5be380bc6f43152c7433801af6d23442ad53af718f95cf0`.

The no-reboot app-only deployment installed Android `0.8.6-dev` code 17 with APK SHA-256
`87da0ee21f568ed6537d6e311546f8525a294771c242fb26d652c1ab1ddba3c8`. The installed data APK and
boot-persistent Magisk copy match exactly, retain the prior signing certificate, and have the
expected owner, mode, and SELinux context. Production subsequently reported app/helper
`0.8.6-dev/0.8.3-dev`, desired/active revision `20/20`, helper `READY`, idle call state, no sync
error, no recording or conversation spools, and no call-control recovery pending. Telecom was idle,
requested and actual audio modes were `MODE_NORMAL`, no mixer snapshot or helper disable marker
existed, all three Hetzner containers remained healthy with zero restarts, and direct-origin health
returned `ok`.

A new physical incoming call through a V4.1 menu to `external_call` remains the acceptance gate for
real hold, dial, merge, conversation recording, exact-leg cleanup, and post-call privacy. Automated
and deployment verification alone do not close that carrier-dependent test.
