# IVRdroid privileged helper

This helper is intentionally device-specific. It refuses to run unless the Android identity,
audio controls, prompt files, app bridge ownership, and audited ROM match the Samsung SM-T585
profile.

For the fixed `START_MENU` request it:

1. waits for Android to report `MODE_IN_CALL`;
2. snapshots all three affected mixer values to root-owned durable storage;
3. starts an independent watchdog before changing the route;
4. plays the root-owned main prompt and restores the exact snapshot;
5. captures 48 kHz stereo PCM16 from TinyALSA without persisting it;
6. detects caller DTMF on both channels;
7. routes 1 to sales, 2 to support, and 0 to operator, with two retries;
8. plays the terminal prompt with another transactional mixer restore;
9. ends the call through the Telecom binder transaction pinned to this ROM;
10. verifies Android leaves `MODE_IN_CALL`.

The app supplies no shell text, mixer names, values, paths, prompts, transaction numbers, or menu
targets.

TinyALSA is vendored at commit `9fab97ca07184371ecad81154d1dadb09d0fa7cf` under its BSD license.
See `third_party/tinyalsa/NOTICE`.

## Build and test

```sh
cmake \
  -S helper \
  -B helper/build-android-arm64 \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-23 \
  -DCMAKE_BUILD_TYPE=Release
cmake --build helper/build-android-arm64
helper/tests/run.sh
```

Warnings are errors for the native project. The host test covers all twelve DTMF keys and several
rejection cases.

## Module layout

```text
/data/adb/modules/ivrdroid_helper/
  disable
  module.prop
  service.sh
  bin/ivrdroid-helper
  prompt.wav
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
status `READY`.

Do not remove `disable` until interruption, reboot, second-call, SIM-loss, and emergency-call
tests are complete.
