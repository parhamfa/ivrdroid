#include "device_profile.h"

#include <strings.h>

namespace ivrdroid {

const DeviceProfile& PinnedDeviceProfile() {
    static const DeviceProfile profile {
        "samsung-sm-t585-lineageos-19.1-20250312",
        "Samsung",
        "SM-T585",
        "gtaxllte",
        "32",
        "google/ryu/dragon:8.1.0/OPM1.171019.016/4503492:user/release-keys",
        "lineage_gtaxllte-userdebug 12 SQ3A.220705.004 "
        "eng.k9100i.20250312.020705",
        0,
        0,
        0,
        48'000,
        2,
        "AudioMixer CH2 DOUT Select",
        "AudioMixer CH2 Mixer En",
        "SPK Switch",
        "Main Mic Switch",
        "AIF4IN",
        "On",
        1,
        1,
        "DMIX_OUT",
        "Off",
        "AIF4IN",
        "Off",
        1,
        0,
        "33",
        "ai.rx1.ivrdroid",
    };
    return profile;
}

bool MatchesDeviceProfile(
    const DeviceProfile& profile,
    const DeviceIdentity& identity) {
    return strcasecmp(identity.manufacturer.c_str(), profile.manufacturer) == 0 &&
        identity.model == profile.model &&
        identity.device == profile.device &&
        identity.androidApi == profile.androidApi &&
        identity.fingerprint == profile.fingerprint &&
        identity.displayId == profile.displayId;
}

}  // namespace ivrdroid
