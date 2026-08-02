# IVRdroid privileged helper

This helper is intentionally device-specific. It refuses to serve unless the Android identity,
audio controls, root-owned prompts, app bridge ownership, and audited ROM match the pinned
Samsung SM-T585 profile.

For the sole `START_MENU` request it:

1. claims the request while Telecom still exposes one ringing non-emergency call;
2. records that call's stable identity and snapshots the observed cold/normal mixer route;
3. disables both physical endpoints and normalizes pre-answer routing to `AIF4IN / Off`;
4. waits for the independent guardian to verify privacy before publishing `WAITING_FOR_CALL`;
5. lets the app answer, then verifies `MODE_IN_CALL`, the same call identity, and 500 ms of
   continuously stable privacy;
6. plays root-owned prompts through TinyALSA;
7. captures 48 kHz stereo PCM16 without persisting it;
8. detects caller DTMF and routes 1, 2, and 0 with two retries;
9. ends only the owned call through the Telecom binder transaction pinned to this ROM;
10. restores audited post-call routing and the captured endpoint state only after the call ends.

Samsung can contest a mixer write while it finalizes the in-call route. The guardian retries
that bounded contention but fails closed after 250 ms without verified microphone and speaker
privacy. Prompt playback cannot start before the guardian acknowledges the stable window.

The version-5 durable snapshot is scoped to one kernel boot, random session, stable call identity,
and four mixer values. If the worker dies, the guardian keeps the call private and attempts
termination first. A failed hangup leaves the snapshot in place so the supervisor can retry
same-boot recovery. A previous-boot snapshot is discarded without ending a call or writing the
new kernel's mixer route.

Emergency, multiple, replacement, and persistently unverified call states preempt the IVR. In
those cases the helper releases only mixer state it can prove it owns and does not issue a global
Telecom hangup. The app supplies no shell text, mixer names, values, paths, prompts, transaction
numbers, or menu targets.

TinyALSA is vendored at commit `9fab97ca07184371ecad81154d1dadb09d0fa7cf` under its BSD license.
See `third_party/tinyalsa/NOTICE`.

## Build and test

From the repository root:

```sh
./helper/tests/run.sh
./scripts/build-helper.sh
./scripts/check-helper-reproducibility.sh
./scripts/package-helper.sh
```

The arm64 build is pinned to Android NDK `25.2.9519653`. Warnings are errors. Host tests cover
device identity, the command/status protocol, menu policy, privacy-stability timing, Telecom
dump parsing and hangup guards, all twelve DTMF keys, and multiple DTMF rejection cases.

## Module layout

```text
/data/adb/modules/ivrdroid_helper/
  disable
  module.prop
  service.sh
  bin/ivrdroid-helper
  prompts/
    main-menu.wav
    sales-unavailable.wav
    support-unavailable.wav
    operator-unavailable.wav
```

The packaged `disable` marker is intentional. Installing the module must not silently make an
experimental audio path persistent across reboot. It may be removed on a specifically audited
device only after the recovery suite below passes.

Before a live call, run the non-mutating self-test as root:

```sh
/data/adb/modules/ivrdroid_helper/bin/ivrdroid-helper --self-test
```

It validates the exact device/build, bridge ownership, prompt files, and mixer topology, then
reports the current values without changing them. Start `service.sh` manually and confirm helper
state `READY`.

For the pinned SM-T585, live tests cover worker death, reboot during a session, a complete first
post-boot call, forced radio-stack loss during a prompt, carrier re-registration, and a complete
post-loss call. Emergency/multiple/replacement/unverified-call behavior is covered by parser and
state-machine fixtures; no real emergency number was called. A new device profile must repeat
those tests before boot startup is enabled. Physical SIM removal or a real carrier outage remains
an additional deployment-specific test, not a claim made by the current record.

The supervisor waits for Android boot completion and a stable idle/`MODE_NORMAL` system, applies
bounded restart backoff, and recreates `disable` after seven consecutive short startup failures.
