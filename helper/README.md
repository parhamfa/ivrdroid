# IVRdroid privileged helper

This helper is intentionally device-specific. It refuses to serve unless the Android identity,
audio controls, root-owned prompts, app bridge ownership, and audited ROM match the pinned
Samsung SM-T585 profile.

For the sole `START_MENU` request it:

1. claims the request before the app answers;
2. polls for the audited call route and snapshots four mixer values durably;
3. disables both the tablet microphone and speaker;
4. starts continuous privacy enforcement in an independent guardian;
5. confirms `MODE_IN_CALL`, exactly one safe Telecom call, and 500 ms of stable privacy;
6. plays root-owned prompts through TinyALSA;
7. captures 48 kHz stereo PCM16 without persisting it;
8. detects caller DTMF and routes 1, 2, and 0 with two retries;
9. ends the call through the Telecom binder transaction pinned to this ROM;
10. restores normal audio only after the call has ended.

Samsung can contest a mixer write while it finalizes the in-call route. The guardian retries
that bounded contention but fails closed after 250 ms without verified microphone and speaker
privacy. Prompt playback cannot start before the guardian acknowledges the stable window.

If the worker dies, the guardian keeps the call private and attempts termination first. A failed
hangup leaves the durable snapshot in place so the supervisor can retry recovery. The app
supplies no shell text, mixer names, values, paths, prompts, transaction numbers, or menu targets.

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

The `disable` marker is intentional. Installing the module must not silently make an experimental
audio path persistent across reboot.

Before a live call, run the non-mutating self-test as root:

```sh
/data/adb/modules/ivrdroid_helper/bin/ivrdroid-helper --self-test
```

It validates the exact device/build, bridge ownership, prompt files, and mixer topology, then
reports the current values without changing them. Start `service.sh` manually and confirm helper
state `READY`.

Do not remove `disable` until reboot, SIM/network-loss, and emergency-call recovery tests are
complete.
