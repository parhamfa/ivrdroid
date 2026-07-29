#include "device_profile.h"

#include <cassert>
#include <iostream>

int main() {
    const ivrdroid::DeviceProfile& profile =
        ivrdroid::PinnedDeviceProfile();
    const ivrdroid::DeviceIdentity exact {
        profile.manufacturer,
        profile.model,
        profile.device,
        profile.androidApi,
        profile.fingerprint,
        profile.displayId,
    };

    assert(ivrdroid::MatchesDeviceProfile(profile, exact));
    assert(profile.card == 0);
    assert(profile.playbackDevice == 0);
    assert(profile.captureDevice == 0);
    assert(profile.sampleRate == 48'000);
    assert(profile.channels == 2);
    assert(profile.expectedSpeaker == 1);
    assert(profile.expectedMic == 1);
    assert(profile.normalSpeaker == 1);
    assert(profile.normalMic == 0);

    auto changedFingerprint = exact;
    changedFingerprint.fingerprint += "-changed";
    assert(!ivrdroid::MatchesDeviceProfile(profile, changedFingerprint));

    auto changedDisplay = exact;
    changedDisplay.displayId += "-changed";
    assert(!ivrdroid::MatchesDeviceProfile(profile, changedDisplay));

    auto changedApi = exact;
    changedApi.androidApi = "33";
    assert(!ivrdroid::MatchesDeviceProfile(profile, changedApi));

    std::cout << "Device profile tests passed." << std::endl;
    return 0;
}
