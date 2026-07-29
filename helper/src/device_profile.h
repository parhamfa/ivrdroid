#pragma once

#include <string>

namespace ivrdroid {

struct DeviceIdentity {
    std::string manufacturer;
    std::string model;
    std::string device;
    std::string androidApi;
    std::string fingerprint;
    std::string displayId;
};

struct DeviceProfile {
    const char* id;
    const char* manufacturer;
    const char* model;
    const char* device;
    const char* androidApi;
    const char* fingerprint;
    const char* displayId;

    int card;
    int playbackDevice;
    int captureDevice;
    unsigned int sampleRate;
    unsigned int channels;

    const char* doutControl;
    const char* mixerControl;
    const char* speakerControl;
    const char* micControl;
    const char* expectedDout;
    const char* expectedMixer;
    int expectedSpeaker;
    int expectedMic;
    const char* appliedDout;
    const char* appliedMixer;
    const char* normalDout;
    const char* normalMixer;
    int normalSpeaker;
    int normalMic;

    const char* telecomEndCallTransaction;
    const char* callingPackage;
};

const DeviceProfile& PinnedDeviceProfile();
bool MatchesDeviceProfile(
    const DeviceProfile& profile,
    const DeviceIdentity& identity);

}  // namespace ivrdroid
