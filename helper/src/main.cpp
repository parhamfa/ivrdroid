#include "device_profile.h"
#include "dtmf_detector.h"
#include "helper_protocol.h"
#include "menu_policy.h"
#include "privacy_policy.h"
#include "telecom_guard.h"

#include <android/log.h>
#include <tinyalsa/asoundlib.h>

#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <string>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace {

using ivrdroid::protocol::CurrentState;
using ivrdroid::protocol::LastResult;

constexpr char kLogTag[] = "IVRdroidHelper";

constexpr char kBridgeDir[] =
    "/data/user/0/ai.rx1.ivrdroid/files/bridge";
constexpr char kCommandPath[] =
    "/data/user/0/ai.rx1.ivrdroid/files/bridge/command.request";
constexpr char kCommandTempPath[] =
    "/data/user/0/ai.rx1.ivrdroid/files/bridge/.command.request.tmp";
constexpr char kStatusPath[] =
    "/data/user/0/ai.rx1.ivrdroid/files/bridge/status";
constexpr char kStatusTempPath[] =
    "/data/user/0/ai.rx1.ivrdroid/files/bridge/.status.tmp";
constexpr char kLastResultPath[] =
    "/data/user/0/ai.rx1.ivrdroid/files/bridge/last_result";
constexpr char kLastResultTempPath[] =
    "/data/user/0/ai.rx1.ivrdroid/files/bridge/.last_result.tmp";

constexpr char kStateDir[] = "/data/adb/ivrdroid";
constexpr char kLockPath[] = "/data/adb/ivrdroid/helper.lock";
constexpr char kPidPath[] = "/data/adb/ivrdroid/helper.pid";
constexpr char kSnapshotPath[] = "/data/adb/ivrdroid/mixer.snapshot";
constexpr char kSnapshotTempPath[] = "/data/adb/ivrdroid/.mixer.snapshot.tmp";

constexpr char kMainPromptPath[] =
    "/data/adb/modules/ivrdroid_helper/prompts/main-menu.wav";
constexpr char kSalesPromptPath[] =
    "/data/adb/modules/ivrdroid_helper/prompts/sales-unavailable.wav";
constexpr char kSupportPromptPath[] =
    "/data/adb/modules/ivrdroid_helper/prompts/support-unavailable.wav";
constexpr char kOperatorPromptPath[] =
    "/data/adb/modules/ivrdroid_helper/prompts/operator-unavailable.wav";

constexpr char kServicePath[] = "/system/bin/service";
constexpr int kCallWaitIterations = 100;
constexpr useconds_t kCallWaitSleepUs = 100'000;
constexpr int kMixerRouteWaitIterations = 5'000;
constexpr useconds_t kMixerRouteWaitSleepUs = 2'000;
constexpr int kPrivacyEnforcementPollMs = 5;
constexpr int kPrivacyInitialAttempts = 125;
constexpr int kPrivacyRecoveryAttempts = 125;
constexpr int64_t kPrivacyRequiredStableMs = 500;
constexpr int64_t kPrivacyMaximumUnverifiedMs = 250;
constexpr int kGuardianPrivacyReadyWaitMs = 12'000;
constexpr off_t kMaximumPromptBytes = 4 * 1024 * 1024;
constexpr off_t kMaximumCommandBytes = 64;
constexpr size_t kMaximumTelecomDumpBytes = 512 * 1024;
constexpr unsigned int kDtmfFrameCount = 1'200;
constexpr unsigned int kDtmfPeriodCount = 4;
constexpr unsigned int kDtmfTimeoutFrames = 8 * 40;
constexpr unsigned int kDtmfCallCheckFrames = 20;

constexpr int kGuardianWaitForCallMs = 12'000;
constexpr int kGuardianPromptMs = 15'000;
constexpr int kGuardianIdleMs = 12'000;
constexpr int kGuardianDtmfMs = 12'000;
constexpr int kGuardianEndCallMs = 6'000;
constexpr int64_t kGuardianTotalMs = 75'000;

constexpr char kGuardianPrompt = 'P';
constexpr char kGuardianIdle = 'I';
constexpr char kGuardianDtmf = 'L';
constexpr char kGuardianEndCall = 'E';
constexpr char kGuardianDone = 'D';
constexpr char kGuardianRemoteHangup = 'H';
constexpr char kGuardianAudioFailure = 'A';
constexpr char kGuardianCaptureFailure = 'C';
constexpr char kGuardianEndCallFailure = 'T';
constexpr char kGuardianPrivacyReady = 'R';

constexpr uint32_t kSnapshotMagic = 0x49565231U;  // IVR1
constexpr uint32_t kSnapshotVersion = 2;
constexpr uint32_t kSnapshotChecksumSalt = 0xA5C39E71U;

volatile sig_atomic_t gStopRequested = 0;
uid_t gAppUid = 0;
const ivrdroid::DeviceProfile* gProfile = nullptr;

void Log(int priority, const char* format, ...) {
    va_list args;
    va_start(args, format);
    va_list logcatArgs;
    va_copy(logcatArgs, args);
    __android_log_vprint(priority, kLogTag, format, logcatArgs);
    va_end(logcatArgs);

    std::vfprintf(stderr, format, args);
    std::fputc('\n', stderr);
    std::fflush(stderr);
    va_end(args);
}

void HandleSignal(int) {
    gStopRequested = 1;
}

bool WriteAll(int fd, const void* data, size_t size) {
    const auto* cursor = static_cast<const uint8_t*>(data);
    size_t remaining = size;
    while (remaining > 0) {
        const ssize_t written = write(fd, cursor, remaining);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        cursor += written;
        remaining -= static_cast<size_t>(written);
    }
    return true;
}

bool ReadAll(int fd, void* data, size_t size) {
    auto* cursor = static_cast<uint8_t*>(data);
    size_t remaining = size;
    while (remaining > 0) {
        const ssize_t count = read(fd, cursor, remaining);
        if (count == 0) return false;
        if (count < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        cursor += count;
        remaining -= static_cast<size_t>(count);
    }
    return true;
}

std::string GetProperty(const char* key) {
    char value[PROP_VALUE_MAX] = {};
    const int length = __system_property_get(key, value);
    return length > 0 ? std::string(value, static_cast<size_t>(length)) : std::string();
}

bool ValidateDevice() {
    const ivrdroid::DeviceProfile& profile = ivrdroid::PinnedDeviceProfile();
    const ivrdroid::DeviceIdentity identity {
        GetProperty("ro.product.manufacturer"),
        GetProperty("ro.product.model"),
        GetProperty("ro.product.device"),
        GetProperty("ro.build.version.sdk"),
        GetProperty("ro.build.fingerprint"),
        GetProperty("ro.build.display.id"),
    };
    if (!ivrdroid::MatchesDeviceProfile(profile, identity)) {
        Log(ANDROID_LOG_ERROR, "Device identity does not match a compiled audited profile.");
        return false;
    }
    gProfile = &profile;
    return true;
}

bool SyncDirectory(const char* path) {
    const int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    const bool synced = fsync(fd) == 0;
    close(fd);
    return synced;
}

bool EnsureStateDirectory() {
    if (mkdir(kStateDir, 0700) != 0 && errno != EEXIST) {
        Log(ANDROID_LOG_ERROR, "Cannot create state directory: %s", std::strerror(errno));
        return false;
    }

    struct stat state {};
    if (lstat(kStateDir, &state) != 0 ||
        !S_ISDIR(state.st_mode) ||
        state.st_uid != 0) {
        Log(ANDROID_LOG_ERROR, "State directory ownership or type is unsafe.");
        return false;
    }
    if (chmod(kStateDir, 0700) != 0) {
        Log(ANDROID_LOG_ERROR, "Cannot secure state directory: %s", std::strerror(errno));
        return false;
    }
    return true;
}

bool IsSafeAppOwnedFile(const char* path) {
    struct stat state {};
    return lstat(path, &state) == 0 &&
        S_ISREG(state.st_mode) &&
        state.st_uid == gAppUid &&
        (state.st_mode & 0022) == 0 &&
        state.st_size >= 0 &&
        state.st_size <= kMaximumCommandBytes;
}

bool ResolveAndValidateBridge() {
    struct stat state {};
    if (lstat(kBridgeDir, &state) != 0 ||
        !S_ISDIR(state.st_mode) ||
        state.st_uid < 10'000 ||
        (state.st_mode & 0022) != 0) {
        Log(ANDROID_LOG_ERROR, "App bridge directory ownership or mode is unsafe.");
        return false;
    }
    gAppUid = state.st_uid;
    if (!IsSafeAppOwnedFile(kStatusPath) ||
        !IsSafeAppOwnedFile(kLastResultPath)) {
        Log(ANDROID_LOG_ERROR, "Bridge status files are missing or unsafe.");
        return false;
    }
    return true;
}

bool WriteBridgeValue(
    const char* path,
    const char* temporaryPath,
    const char* value) {
    if (gAppUid == 0 || !IsSafeAppOwnedFile(path)) return false;
    const size_t valueLength = std::strlen(value);
    if (valueLength == 0 ||
        valueLength + 1 > static_cast<size_t>(kMaximumCommandBytes)) {
        return false;
    }

    unlink(temporaryPath);
    const int fd = open(
        temporaryPath,
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
        0600);
    if (fd < 0) return false;

    const bool written =
        fchown(fd, gAppUid, gAppUid) == 0 &&
        fchmod(fd, 0600) == 0 &&
        WriteAll(fd, value, valueLength) &&
        WriteAll(fd, "\n", 1) &&
        fsync(fd) == 0;
    close(fd);
    if (!written || rename(temporaryPath, path) != 0) {
        unlink(temporaryPath);
        return false;
    }
    return SyncDirectory(kBridgeDir);
}

void WriteCurrentState(CurrentState state) {
    if (!WriteBridgeValue(
            kStatusPath,
            kStatusTempPath,
            ivrdroid::protocol::ToString(state))) {
        Log(ANDROID_LOG_ERROR, "Could not publish helper current state.");
    }
}

void WriteLastResult(LastResult result) {
    if (!WriteBridgeValue(
            kLastResultPath,
            kLastResultTempPath,
            ivrdroid::protocol::ToString(result))) {
        Log(ANDROID_LOG_ERROR, "Could not publish helper session result.");
    }
}

bool IsAudioInCall() {
    FILE* pipe = popen("/system/bin/dumpsys audio 2>/dev/null", "r");
    if (pipe == nullptr) return false;

    bool inCall = false;
    char line[512] = {};
    while (std::fgets(line, sizeof(line), pipe) != nullptr) {
        if (std::strstr(line, "Actual mode = MODE_IN_CALL") != nullptr) {
            inCall = true;
            break;
        }
    }
    pclose(pipe);
    return inCall;
}

bool WaitForInCall() {
    for (int attempt = 0;
         attempt < kCallWaitIterations && !gStopRequested;
         ++attempt) {
        if (IsAudioInCall()) return true;
        usleep(kCallWaitSleepUs);
    }
    return false;
}

enum class LiveCallState {
    None,
    SingleSafe,
    UnsafeOrUnknown,
};

LiveCallState ReadLiveCallState();

int FindEnumIndex(mixer_ctl* control, const char* value) {
    const unsigned int count = mixer_ctl_get_num_enums(control);
    for (unsigned int index = 0; index < count; ++index) {
        const char* candidate = mixer_ctl_get_enum_string(control, index);
        if (candidate != nullptr && std::strcmp(candidate, value) == 0) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

struct RouteValues {
    int dout = -1;
    int mixer = -1;
    int speaker = -1;
    int mic = -1;
};

struct MixerRoute {
    mixer* device = nullptr;
    mixer_ctl* dout = nullptr;
    mixer_ctl* mixerEnable = nullptr;
    mixer_ctl* speaker = nullptr;
    mixer_ctl* mic = nullptr;
    int expectedDout = -1;
    int expectedMixer = -1;
    int expectedSpeaker = -1;
    int expectedMic = -1;
    int appliedDout = -1;
    int appliedMixer = -1;
    int normalDout = -1;
    int normalMixer = -1;
    int normalSpeaker = -1;
    int normalMic = -1;
};

void CloseMixerRoute(MixerRoute* route) {
    if (route->device != nullptr) mixer_close(route->device);
    *route = {};
}

bool OpenMixerRoute(MixerRoute* route) {
    if (gProfile == nullptr) return false;
    route->device = mixer_open(gProfile->card);
    if (route->device == nullptr) {
        Log(ANDROID_LOG_ERROR, "Cannot open ALSA mixer card %d.", gProfile->card);
        return false;
    }

    route->dout = mixer_get_ctl_by_name(route->device, gProfile->doutControl);
    route->mixerEnable =
        mixer_get_ctl_by_name(route->device, gProfile->mixerControl);
    route->speaker =
        mixer_get_ctl_by_name(route->device, gProfile->speakerControl);
    route->mic =
        mixer_get_ctl_by_name(route->device, gProfile->micControl);
    if (route->dout == nullptr ||
        route->mixerEnable == nullptr ||
        route->speaker == nullptr ||
        route->mic == nullptr) {
        Log(ANDROID_LOG_ERROR, "One or more audited mixer controls are absent.");
        CloseMixerRoute(route);
        return false;
    }

    if (mixer_ctl_get_type(route->dout) != MIXER_CTL_TYPE_ENUM ||
        mixer_ctl_get_type(route->mixerEnable) != MIXER_CTL_TYPE_ENUM ||
        mixer_ctl_get_type(route->speaker) != MIXER_CTL_TYPE_BOOL ||
        mixer_ctl_get_type(route->mic) != MIXER_CTL_TYPE_BOOL ||
        mixer_ctl_get_num_values(route->dout) != 1 ||
        mixer_ctl_get_num_values(route->mixerEnable) != 1 ||
        mixer_ctl_get_num_values(route->speaker) != 1 ||
        mixer_ctl_get_num_values(route->mic) != 1) {
        Log(ANDROID_LOG_ERROR, "Audited mixer control types or widths changed.");
        CloseMixerRoute(route);
        return false;
    }

    route->expectedDout = FindEnumIndex(route->dout, gProfile->expectedDout);
    route->expectedMixer = FindEnumIndex(route->mixerEnable, gProfile->expectedMixer);
    route->expectedSpeaker = gProfile->expectedSpeaker;
    route->expectedMic = gProfile->expectedMic;
    route->appliedDout = FindEnumIndex(route->dout, gProfile->appliedDout);
    route->appliedMixer = FindEnumIndex(route->mixerEnable, gProfile->appliedMixer);
    route->normalDout = FindEnumIndex(route->dout, gProfile->normalDout);
    route->normalMixer = FindEnumIndex(route->mixerEnable, gProfile->normalMixer);
    route->normalSpeaker = gProfile->normalSpeaker;
    route->normalMic = gProfile->normalMic;
    if (route->expectedDout < 0 ||
        route->expectedMixer < 0 ||
        (route->expectedSpeaker != 0 && route->expectedSpeaker != 1) ||
        (route->expectedMic != 0 && route->expectedMic != 1) ||
        route->appliedDout < 0 ||
        route->appliedMixer < 0 ||
        route->normalDout < 0 ||
        route->normalMixer < 0 ||
        (route->normalSpeaker != 0 && route->normalSpeaker != 1) ||
        (route->normalMic != 0 && route->normalMic != 1)) {
        Log(ANDROID_LOG_ERROR, "Audited mixer enum choices changed.");
        CloseMixerRoute(route);
        return false;
    }
    return true;
}

RouteValues ReadRoute(const MixerRoute& route) {
    return {
        mixer_ctl_get_value(route.dout, 0),
        mixer_ctl_get_value(route.mixerEnable, 0),
        mixer_ctl_get_value(route.speaker, 0),
        mixer_ctl_get_value(route.mic, 0),
    };
}

bool SameRoute(const RouteValues& left, const RouteValues& right) {
    return left.dout == right.dout &&
        left.mixer == right.mixer &&
        left.speaker == right.speaker &&
        left.mic == right.mic;
}

RouteValues InjectionRoute(const MixerRoute& route) {
    return {route.appliedDout, route.appliedMixer, 0, 0};
}

RouteValues ExpectedRoute(const MixerRoute& route) {
    return {
        route.expectedDout,
        route.expectedMixer,
        route.expectedSpeaker,
        route.expectedMic,
    };
}

RouteValues PrivacyRoute(const MixerRoute& route) {
    return {route.expectedDout, route.expectedMixer, 0, 0};
}

bool ValidateExpectedBaseline(const MixerRoute& route, const RouteValues& snapshot) {
    const RouteValues expected = ExpectedRoute(route);
    if (!SameRoute(snapshot, expected)) {
        Log(
            ANDROID_LOG_ERROR,
            "Refusing unexpected in-call baseline: "
            "dout=%d mixer=%d speaker=%d mic=%d.",
            snapshot.dout,
            snapshot.mixer,
            snapshot.speaker,
            snapshot.mic);
        return false;
    }
    return true;
}

bool SetAndVerify(mixer_ctl* control, int value) {
    return mixer_ctl_set_value(control, 0, value) == 0 &&
        mixer_ctl_get_value(control, 0) == value;
}

bool ApplyPrivacyRoute(MixerRoute* route) {
    if (!SetAndVerify(route->speaker, 0)) return false;
    if (!SetAndVerify(route->mic, 0)) return false;
    if (!SetAndVerify(route->mixerEnable, route->expectedMixer)) return false;
    if (!SetAndVerify(route->dout, route->expectedDout)) return false;
    return SameRoute(ReadRoute(*route), PrivacyRoute(*route));
}

ivrdroid::PrivacyObservation EnforcePrivateControls(
    MixerRoute* route,
    bool* startupRouteReady) {
    const RouteValues current = ReadRoute(*route);
    const bool corrected = current.speaker != 0 || current.mic != 0;
    if (current.speaker != 0) {
        mixer_ctl_set_value(route->speaker, 0, 0);
    }
    if (current.mic != 0) {
        mixer_ctl_set_value(route->mic, 0, 0);
    }

    const RouteValues verified = ReadRoute(*route);
    *startupRouteReady =
        verified.dout == route->expectedDout &&
        verified.mixer == route->expectedMixer;
    if (verified.speaker != 0 || verified.mic != 0) {
        return ivrdroid::PrivacyObservation::Contended;
    }
    return corrected
        ? ivrdroid::PrivacyObservation::Corrected
        : ivrdroid::PrivacyObservation::Private;
}

bool EstablishInitialPrivacy(MixerRoute* route) {
    for (int attempt = 0;
         attempt < kPrivacyInitialAttempts && !gStopRequested;
         ++attempt) {
        bool startupRouteReady = false;
        const ivrdroid::PrivacyObservation observation =
            EnforcePrivateControls(route, &startupRouteReady);
        if (observation != ivrdroid::PrivacyObservation::Contended) {
            return true;
        }
        usleep(kMixerRouteWaitSleepUs);
    }
    Log(
        ANDROID_LOG_ERROR,
        "Could not verify the initial microphone and speaker mute.");
    return false;
}

bool ApplyInjectionRoute(MixerRoute* route) {
    if (!SetAndVerify(route->speaker, 0)) return false;
    if (!SetAndVerify(route->mic, 0)) return false;
    if (!SetAndVerify(route->mixerEnable, route->appliedMixer)) return false;
    if (!SetAndVerify(route->dout, route->appliedDout)) return false;
    return SameRoute(ReadRoute(*route), InjectionRoute(*route));
}

bool RouteContainsOnlyOurChanges(
    const MixerRoute& route,
    const RouteValues& snapshot,
    const RouteValues& current) {
    const RouteValues injection = InjectionRoute(route);
    const RouteValues privacy = PrivacyRoute(route);
    const bool fieldsKnown =
        (current.dout == snapshot.dout ||
         current.dout == injection.dout ||
         current.dout == privacy.dout) &&
        (current.mixer == snapshot.mixer ||
         current.mixer == injection.mixer ||
         current.mixer == privacy.mixer) &&
        (current.speaker == snapshot.speaker ||
         current.speaker == injection.speaker ||
         current.speaker == privacy.speaker) &&
        (current.mic == snapshot.mic ||
         current.mic == injection.mic ||
         current.mic == privacy.mic);
    return fieldsKnown && !SameRoute(current, snapshot);
}

bool RestoreRoute(const RouteValues& snapshot, bool onlyIfOwned) {
    MixerRoute route {};
    if (!OpenMixerRoute(&route)) return false;

    const RouteValues current = ReadRoute(route);
    if (SameRoute(current, snapshot)) {
        CloseMixerRoute(&route);
        return true;
    }
    if (onlyIfOwned && !RouteContainsOnlyOurChanges(route, snapshot, current)) {
        Log(
            ANDROID_LOG_WARN,
            "Mixer ownership changed externally; refusing to overwrite the new route.");
        CloseMixerRoute(&route);
        return true;
    }

    const bool restored =
        SetAndVerify(route.dout, snapshot.dout) &&
        SetAndVerify(route.mixerEnable, snapshot.mixer) &&
        SetAndVerify(route.mic, snapshot.mic) &&
        SetAndVerify(route.speaker, snapshot.speaker) &&
        SameRoute(ReadRoute(route), snapshot);
    CloseMixerRoute(&route);
    return restored;
}

bool RestoreNormalRouteAfterEndedCall(const RouteValues& snapshot) {
    MixerRoute route {};
    if (!OpenMixerRoute(&route)) return false;

    const RouteValues current = ReadRoute(route);
    const RouteValues normal {
        route.normalDout,
        route.normalMixer,
        route.normalSpeaker,
        route.normalMic,
    };
    if (SameRoute(current, normal)) {
        CloseMixerRoute(&route);
        return true;
    }

    const RouteValues injection = InjectionRoute(route);
    const RouteValues privacy = PrivacyRoute(route);
    const bool fieldsKnown =
        (current.dout == snapshot.dout ||
         current.dout == injection.dout ||
         current.dout == privacy.dout ||
         current.dout == normal.dout) &&
        (current.mixer == snapshot.mixer ||
         current.mixer == injection.mixer ||
         current.mixer == privacy.mixer ||
         current.mixer == normal.mixer) &&
        (current.speaker == snapshot.speaker ||
         current.speaker == injection.speaker ||
         current.speaker == privacy.speaker ||
         current.speaker == normal.speaker) &&
        (current.mic == snapshot.mic ||
         current.mic == injection.mic ||
         current.mic == privacy.mic ||
         current.mic == normal.mic);
    if (!fieldsKnown) {
        Log(
            ANDROID_LOG_WARN,
            "Normal route changed externally; refusing recovery overwrite.");
        CloseMixerRoute(&route);
        return true;
    }

    const bool restored =
        SetAndVerify(route.dout, normal.dout) &&
        SetAndVerify(route.mixerEnable, normal.mixer) &&
        SetAndVerify(route.mic, normal.mic) &&
        SetAndVerify(route.speaker, normal.speaker) &&
        SameRoute(ReadRoute(route), normal);
    CloseMixerRoute(&route);
    return restored;
}

#pragma pack(push, 1)
struct SnapshotFile {
    uint32_t magic;
    uint32_t version;
    int32_t dout;
    int32_t mixer;
    int32_t speaker;
    int32_t mic;
    uint32_t checksum;
};
#pragma pack(pop)

uint32_t SnapshotChecksum(const SnapshotFile& snapshot) {
    return snapshot.magic ^
        snapshot.version ^
        static_cast<uint32_t>(snapshot.dout) ^
        static_cast<uint32_t>(snapshot.mixer) ^
        static_cast<uint32_t>(snapshot.speaker) ^
        static_cast<uint32_t>(snapshot.mic) ^
        kSnapshotChecksumSalt;
}

bool PersistSnapshot(const RouteValues& route) {
    SnapshotFile snapshot {
        kSnapshotMagic,
        kSnapshotVersion,
        route.dout,
        route.mixer,
        route.speaker,
        route.mic,
        0,
    };
    snapshot.checksum = SnapshotChecksum(snapshot);

    unlink(kSnapshotTempPath);
    const int fd = open(
        kSnapshotTempPath,
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
        0600);
    if (fd < 0) return false;

    const bool written = WriteAll(fd, &snapshot, sizeof(snapshot)) && fsync(fd) == 0;
    close(fd);
    if (!written || rename(kSnapshotTempPath, kSnapshotPath) != 0) {
        unlink(kSnapshotTempPath);
        return false;
    }
    if (!SyncDirectory(kStateDir)) {
        unlink(kSnapshotPath);
        SyncDirectory(kStateDir);
        return false;
    }
    return true;
}

bool LoadSnapshot(RouteValues* route) {
    const int fd = open(kSnapshotPath, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;

    struct stat state {};
    SnapshotFile snapshot {};
    const bool readOk =
        fstat(fd, &state) == 0 &&
        S_ISREG(state.st_mode) &&
        state.st_uid == 0 &&
        state.st_size == static_cast<off_t>(sizeof(snapshot)) &&
        ReadAll(fd, &snapshot, sizeof(snapshot));
    close(fd);
    if (!readOk ||
        snapshot.magic != kSnapshotMagic ||
        snapshot.version != kSnapshotVersion ||
        snapshot.checksum != SnapshotChecksum(snapshot)) {
        Log(ANDROID_LOG_ERROR, "Persistent mixer snapshot is invalid.");
        return false;
    }

    *route = {snapshot.dout, snapshot.mixer, snapshot.speaker, snapshot.mic};
    return true;
}

void ClearSnapshot() {
    bool changed = false;
    if (unlink(kSnapshotPath) == 0) changed = true;
    if (unlink(kSnapshotTempPath) == 0) changed = true;
    if (changed && !SyncDirectory(kStateDir)) {
        Log(ANDROID_LOG_ERROR, "Could not durably clear the mixer snapshot.");
    }
}

enum class MixerRecoveryResult {
    NoSnapshot,
    Restored,
    Failed,
};

MixerRecoveryResult RecoverMixerSnapshot() {
    if (access(kSnapshotPath, F_OK) != 0) {
        return errno == ENOENT
            ? MixerRecoveryResult::NoSnapshot
            : MixerRecoveryResult::Failed;
    }
    RouteValues snapshot {};
    if (!LoadSnapshot(&snapshot)) {
        return MixerRecoveryResult::Failed;
    }
    const bool restored = IsAudioInCall()
        ? RestoreRoute(snapshot, true)
        : RestoreNormalRouteAfterEndedCall(snapshot);
    if (!restored) return MixerRecoveryResult::Failed;
    ClearSnapshot();
    return access(kSnapshotPath, F_OK) == 0
        ? MixerRecoveryResult::Failed
        : MixerRecoveryResult::Restored;
}

bool ValidatePromptFile(const char* path) {
    struct stat state {};
    if (lstat(path, &state) != 0 ||
        !S_ISREG(state.st_mode) ||
        state.st_uid != 0 ||
        (state.st_mode & 0022) != 0 ||
        state.st_size <= 44 ||
        state.st_size > kMaximumPromptBytes) {
        Log(ANDROID_LOG_ERROR, "Prompt file ownership, mode, type, or size is unsafe.");
        return false;
    }
    return true;
}

bool ValidateAllPromptFiles() {
    return
        ValidatePromptFile(kMainPromptPath) &&
        ValidatePromptFile(kSalesPromptPath) &&
        ValidatePromptFile(kSupportPromptPath) &&
        ValidatePromptFile(kOperatorPromptPath);
}

constexpr uint32_t kRiffId = 0x46464952U;
constexpr uint32_t kWaveId = 0x45564157U;
constexpr uint32_t kFormatId = 0x20746D66U;
constexpr uint32_t kDataId = 0x61746164U;

#pragma pack(push, 1)
struct RiffHeader {
    uint32_t riffId;
    uint32_t size;
    uint32_t waveId;
};

struct ChunkHeader {
    uint32_t id;
    uint32_t size;
};

struct WaveFormat {
    uint16_t audioFormat;
    uint16_t channels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
};
#pragma pack(pop)

bool SeekForward(FILE* file, uint32_t bytes) {
    const long padded = static_cast<long>(bytes + (bytes & 1U));
    return fseek(file, padded, SEEK_CUR) == 0;
}

bool ParseWave(FILE* file, uint32_t* dataSize) {
    if (gProfile == nullptr) return false;
    RiffHeader riff {};
    if (std::fread(&riff, sizeof(riff), 1, file) != 1 ||
        riff.riffId != kRiffId ||
        riff.waveId != kWaveId) {
        return false;
    }

    bool foundFormat = false;
    for (int chunks = 0; chunks < 32; ++chunks) {
        ChunkHeader chunk {};
        if (std::fread(&chunk, sizeof(chunk), 1, file) != 1) return false;

        if (chunk.id == kFormatId) {
            if (chunk.size < sizeof(WaveFormat)) return false;
            WaveFormat format {};
            if (std::fread(&format, sizeof(format), 1, file) != 1) return false;
            const uint16_t expectedBlockAlign =
                static_cast<uint16_t>(gProfile->channels * sizeof(int16_t));
            if (format.audioFormat != 1 ||
                format.channels != gProfile->channels ||
                format.sampleRate != gProfile->sampleRate ||
                format.bitsPerSample != 16 ||
                format.blockAlign != expectedBlockAlign ||
                format.byteRate != gProfile->sampleRate * expectedBlockAlign) {
                return false;
            }
            const uint32_t remainder =
                chunk.size - static_cast<uint32_t>(sizeof(WaveFormat));
            if (remainder > 0 && !SeekForward(file, remainder)) return false;
            foundFormat = true;
        } else if (chunk.id == kDataId) {
            const uint32_t expectedBlockAlign =
                static_cast<uint32_t>(gProfile->channels * sizeof(int16_t));
            if (!foundFormat || chunk.size == 0 ||
                chunk.size % expectedBlockAlign != 0 ||
                chunk.size > static_cast<uint32_t>(kMaximumPromptBytes)) {
                return false;
            }
            *dataSize = chunk.size;
            return true;
        } else if (!SeekForward(file, chunk.size)) {
            return false;
        }
    }
    return false;
}

bool PlayPrompt(const char* path) {
    if (gProfile == nullptr) return false;
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) return false;

    uint32_t dataSize = 0;
    if (!ParseWave(file, &dataSize)) {
        Log(ANDROID_LOG_ERROR, "Prompt is not audited PCM16 WAV.");
        std::fclose(file);
        return false;
    }

    pcm_config config {};
    config.channels = gProfile->channels;
    config.rate = gProfile->sampleRate;
    config.period_size = 1024;
    config.period_count = 2;
    config.format = PCM_FORMAT_S16_LE;
    config.start_threshold = config.period_size;
    config.stop_threshold = config.period_size * config.period_count;
    config.silence_threshold = config.stop_threshold;

    pcm* output = pcm_open(
        gProfile->card,
        gProfile->playbackDevice,
        PCM_OUT,
        &config);
    if (output == nullptr || !pcm_is_ready(output)) {
        Log(
            ANDROID_LOG_ERROR,
            "Cannot open PCM playback: %s",
            output == nullptr ? "null handle" : pcm_get_error(output));
        if (output != nullptr) pcm_close(output);
        std::fclose(file);
        return false;
    }

    const size_t bufferSize = pcm_frames_to_bytes(output, config.period_size);
    auto* buffer = static_cast<uint8_t*>(std::malloc(bufferSize));
    if (buffer == nullptr) {
        pcm_close(output);
        std::fclose(file);
        return false;
    }

    bool success = true;
    uint32_t remaining = dataSize;
    int blocksUntilCallCheck = 1;
    while (remaining > 0 && !gStopRequested) {
        const size_t wanted = remaining < bufferSize ? remaining : bufferSize;
        const size_t count = std::fread(buffer, 1, wanted, file);
        if (count != wanted) {
            success = false;
            break;
        }

        const unsigned int frames = pcm_bytes_to_frames(output, count);
        const int framesWritten = pcm_writei(output, buffer, frames);
        if (framesWritten != static_cast<int>(frames)) {
            Log(ANDROID_LOG_ERROR, "PCM write failed: %s", pcm_get_error(output));
            success = false;
            break;
        }
        remaining -= static_cast<uint32_t>(count);

        if (--blocksUntilCallCheck == 0) {
            blocksUntilCallCheck = 24;
            if (!IsAudioInCall()) {
                Log(ANDROID_LOG_WARN, "Call ended while prompt was playing.");
                success = false;
                break;
            }
        }
    }

    if (success && !gStopRequested) pcm_wait(output, 2000);
    std::free(buffer);
    pcm_close(output);
    std::fclose(file);
    return success && remaining == 0 && !gStopRequested;
}

int64_t MonotonicMilliseconds();

bool NotifyGuardian(int fd, char phase) {
    if (fd < 0) return false;
    while (true) {
        const ssize_t sent = send(fd, &phase, 1, MSG_NOSIGNAL);
        if (sent == 1) return true;
        if (sent < 0 && errno == EINTR) continue;
        return false;
    }
}

bool WaitForGuardianPrivacyReady(int fd) {
    const int64_t deadline =
        MonotonicMilliseconds() + kGuardianPrivacyReadyWaitMs;
    while (!gStopRequested) {
        const int64_t remaining =
            deadline - MonotonicMilliseconds();
        if (remaining <= 0) return false;

        pollfd descriptor {fd, POLLIN | POLLHUP, 0};
        const int result = poll(
            &descriptor,
            1,
            static_cast<int>(remaining));
        if (result == 0) return false;
        if (result < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if ((descriptor.revents & POLLIN) != 0) {
            char message = 0;
            const ssize_t count = recv(fd, &message, 1, 0);
            return count == 1 && message == kGuardianPrivacyReady;
        }
        if ((descriptor.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
            return false;
        }
    }
    return false;
}

enum class PromptResult {
    Completed,
    RemoteHangup,
    Failed,
};

enum class PrivacyStartResult {
    Started,
    RemoteHangup,
    Failed,
};

bool RestorePrivateSession();

PrivacyStartResult BeginPrivateSession(int guardianFd) {
    MixerRoute route {};
    if (!OpenMixerRoute(&route)) return PrivacyStartResult::Failed;
    WriteCurrentState(CurrentState::WaitingForCall);

    RouteValues snapshot {};
    bool baselineFound = false;
    for (int attempt = 0;
         attempt < kMixerRouteWaitIterations && !gStopRequested;
         ++attempt) {
        const RouteValues current = ReadRoute(route);
        if (SameRoute(current, ExpectedRoute(route))) {
            snapshot = current;
            baselineFound = true;
            break;
        }
        usleep(kMixerRouteWaitSleepUs);
    }
    if (!baselineFound || !ValidateExpectedBaseline(route, snapshot)) {
        CloseMixerRoute(&route);
        Log(ANDROID_LOG_ERROR, "Timed out waiting for the audited in-call mixer route.");
        return ReadLiveCallState() == LiveCallState::None
            ? PrivacyStartResult::RemoteHangup
            : PrivacyStartResult::Failed;
    }

    if (!PersistSnapshot(snapshot)) {
        CloseMixerRoute(&route);
        return PrivacyStartResult::Failed;
    }

    const bool applied = EstablishInitialPrivacy(&route);
    CloseMixerRoute(&route);
    if (!applied) {
        Log(
            ANDROID_LOG_ERROR,
            "Initial session privacy failed; retaining the snapshot for recovery.");
        return PrivacyStartResult::Failed;
    }

    if (!NotifyGuardian(guardianFd, kGuardianIdle)) {
        return PrivacyStartResult::Failed;
    }
    Log(
        ANDROID_LOG_INFO,
        "Early session privacy active: tablet microphone and speaker are muted.");

    if (!WaitForInCall() ||
        ReadLiveCallState() != LiveCallState::SingleSafe) {
        const LiveCallState callState = ReadLiveCallState();
        return callState == LiveCallState::None
            ? PrivacyStartResult::RemoteHangup
            : PrivacyStartResult::Failed;
    }

    if (!WaitForGuardianPrivacyReady(guardianFd)) {
        Log(
            ANDROID_LOG_ERROR,
            "Guardian did not confirm a stable private in-call route.");
        return PrivacyStartResult::Failed;
    }
    Log(
        ANDROID_LOG_INFO,
        "Session privacy stabilized after Android finished routing the call.");
    return PrivacyStartResult::Started;
}

bool HasPrivateSessionRoute() {
    MixerRoute route {};
    if (!OpenMixerRoute(&route)) return false;
    const bool privateRoute =
        SameRoute(ReadRoute(route), PrivacyRoute(route));
    CloseMixerRoute(&route);
    return privateRoute;
}

bool RestorePrivateSession() {
    const MixerRecoveryResult result = RecoverMixerSnapshot();
    if (result != MixerRecoveryResult::Restored) {
        Log(ANDROID_LOG_ERROR, "Could not restore the session privacy snapshot.");
        return false;
    }
    Log(ANDROID_LOG_INFO, "Session privacy snapshot restored and cleared.");
    return true;
}

PromptResult ProcessPrompt(
    const char* promptPath,
    CurrentState playingState,
    int guardianFd) {
    if (!IsAudioInCall()) {
        return ReadLiveCallState() == LiveCallState::None
            ? PromptResult::RemoteHangup
            : PromptResult::Failed;
    }
    if (!ValidatePromptFile(promptPath) ||
        !NotifyGuardian(guardianFd, kGuardianPrompt)) {
        return PromptResult::Failed;
    }

    MixerRoute route {};
    if (!OpenMixerRoute(&route)) return PromptResult::Failed;
    const RouteValues privacy = PrivacyRoute(route);
    if (!SameRoute(ReadRoute(route), privacy)) {
        Log(
            ANDROID_LOG_ERROR,
            "Session privacy route changed before prompt playback.");
        CloseMixerRoute(&route);
        return PromptResult::Failed;
    }

    const bool applied = ApplyInjectionRoute(&route);
    CloseMixerRoute(&route);
    if (!applied) {
        Log(ANDROID_LOG_ERROR, "Prompt injection failed; restoring privacy route.");
        RestoreRoute(privacy, false);
        return PromptResult::Failed;
    }

    WriteCurrentState(playingState);
    Log(ANDROID_LOG_INFO, "Prompt injection applied; playing audited prompt.");
    const bool played = PlayPrompt(promptPath);
    const bool restored = RestoreRoute(privacy, true);
    if (!restored || !NotifyGuardian(guardianFd, kGuardianIdle)) {
        Log(ANDROID_LOG_ERROR, "Privacy route restore failed after prompt.");
        return PromptResult::Failed;
    }

    if (!played) {
        Log(ANDROID_LOG_WARN, "Prompt stopped early; privacy route was restored.");
        return ReadLiveCallState() == LiveCallState::None
            ? PromptResult::RemoteHangup
            : PromptResult::Failed;
    }
    Log(ANDROID_LOG_INFO, "Prompt completed; session privacy remains active.");
    return PromptResult::Completed;
}

enum class DtmfResultKind {
    Digit,
    Timeout,
    CallEnded,
    CaptureError,
    Stopped,
};

struct DtmfResult {
    DtmfResultKind kind;
    char digit;
};

DtmfResult CaptureDtmfDigit(int guardianFd) {
    if (gProfile == nullptr) return {DtmfResultKind::CaptureError, 0};
    if (!IsAudioInCall()) return {DtmfResultKind::CallEnded, 0};
    if (!HasPrivateSessionRoute()) {
        Log(ANDROID_LOG_ERROR, "Session privacy route changed before DTMF capture.");
        return {DtmfResultKind::CaptureError, 0};
    }
    if (!NotifyGuardian(guardianFd, kGuardianDtmf)) {
        return {DtmfResultKind::CaptureError, 0};
    }

    pcm_config config {};
    config.channels = gProfile->channels;
    config.rate = gProfile->sampleRate;
    config.period_size = kDtmfFrameCount;
    config.period_count = kDtmfPeriodCount;
    config.format = PCM_FORMAT_S16_LE;

    pcm* input = pcm_open(
        gProfile->card,
        gProfile->captureDevice,
        PCM_IN,
        &config);
    if (input == nullptr || !pcm_is_ready(input)) {
        Log(
            ANDROID_LOG_ERROR,
            "Cannot open DTMF capture PCM: %s",
            input == nullptr ? "null handle" : pcm_get_error(input));
        if (input != nullptr) pcm_close(input);
        return {DtmfResultKind::CaptureError, 0};
    }

    std::vector<int16_t> samples(kDtmfFrameCount * gProfile->channels);
    ivrdroid::StereoDtmfDetector detector(gProfile->sampleRate);
    WriteCurrentState(CurrentState::ListeningDtmf);
    Log(
        ANDROID_LOG_INFO,
        "Listening for caller DTMF on audited PCM %d:%d.",
        gProfile->card,
        gProfile->captureDevice);

    for (unsigned int frame = 0;
         frame < kDtmfTimeoutFrames && !gStopRequested;
         ++frame) {
        const int framesRead =
            pcm_readi(input, samples.data(), kDtmfFrameCount);
        if (framesRead != static_cast<int>(kDtmfFrameCount)) {
            Log(
                ANDROID_LOG_ERROR,
                "DTMF PCM read failed or returned %d frames: %s",
                framesRead,
                pcm_get_error(input));
            pcm_close(input);
            return {DtmfResultKind::CaptureError, 0};
        }

        ivrdroid::DtmfFrameAnalysis left {};
        ivrdroid::DtmfFrameAnalysis right {};
        const char digit = detector.ProcessFrame(
            samples.data(),
            kDtmfFrameCount,
            &left,
            &right);
        if (digit != 0) {
            Log(
                ANDROID_LOG_INFO,
                "Detected caller DTMF digit %c "
                "(left=%.2f right=%.2f dominance=%.2f/%.2f).",
                digit,
                left.confidence,
                right.confidence,
                left.dominance,
                right.dominance);
            pcm_close(input);
            return {DtmfResultKind::Digit, digit};
        }

        if ((frame + 1) % kDtmfCallCheckFrames == 0 &&
            !IsAudioInCall()) {
            pcm_close(input);
            return ReadLiveCallState() == LiveCallState::None
                ? DtmfResult {DtmfResultKind::CallEnded, 0}
                : DtmfResult {DtmfResultKind::CaptureError, 0};
        }
    }

    pcm_close(input);
    if (gStopRequested) return {DtmfResultKind::Stopped, 0};
    Log(ANDROID_LOG_INFO, "Caller DTMF wait timed out.");
    return {DtmfResultKind::Timeout, 0};
}

bool ReadTelecomDump(std::string* dump) {
    FILE* pipe = popen("/system/bin/dumpsys telecom 2>/dev/null", "r");
    if (pipe == nullptr) return false;

    dump->clear();
    bool foundStart = false;
    bool foundEnd = false;
    char line[4096] = {};
    while (std::fgets(line, sizeof(line), pipe) != nullptr) {
        const bool isStart = std::strstr(line, "mCalls:") != nullptr;
        if (isStart) foundStart = true;
        if (foundStart) {
            const size_t length = std::strlen(line);
            if (dump->size() + length > kMaximumTelecomDumpBytes) break;
            dump->append(line, length);
            if (std::strstr(line, "mCallAudioManager:") != nullptr) {
                foundEnd = true;
                break;
            }
        }
    }
    pclose(pipe);
    return foundStart && foundEnd;
}

LiveCallState ReadLiveCallState() {
    std::string dump;
    if (!ReadTelecomDump(&dump)) return LiveCallState::UnsafeOrUnknown;
    const ivrdroid::TelecomCallSnapshot snapshot =
        ivrdroid::ParseTelecomCallSnapshot(dump);
    if (!snapshot.parsed) return LiveCallState::UnsafeOrUnknown;
    if (snapshot.liveCallCount == 0) return LiveCallState::None;
    return ivrdroid::CanForceEndSingleCall(snapshot)
        ? LiveCallState::SingleSafe
        : LiveCallState::UnsafeOrUnknown;
}

bool SendFixedTelecomEndCall() {
    if (gProfile == nullptr) return false;
    const pid_t child = fork();
    if (child < 0) {
        Log(ANDROID_LOG_ERROR, "Could not fork the pinned Telecom end-call transaction.");
        return false;
    }
    if (child == 0) {
        execl(
            kServicePath,
            kServicePath,
            "call",
            "telecom",
            gProfile->telecomEndCallTransaction,
            "s16",
            gProfile->callingPackage,
            static_cast<char*>(nullptr));
        _exit(127);
    }

    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

enum class EndCallResult {
    Ended,
    NoCall,
    Skipped,
    Failed,
};

EndCallResult EndSingleCallAndWait() {
    const LiveCallState initialState = ReadLiveCallState();
    if (initialState == LiveCallState::None) return EndCallResult::NoCall;
    if (initialState != LiveCallState::SingleSafe) {
        Log(ANDROID_LOG_WARN, "Refusing global hangup: Telecom state is unsafe.");
        return EndCallResult::Skipped;
    }

    const LiveCallState confirmedState = ReadLiveCallState();
    if (confirmedState == LiveCallState::None) return EndCallResult::NoCall;
    if (confirmedState != LiveCallState::SingleSafe) {
        Log(
            ANDROID_LOG_WARN,
            "Refusing global hangup: Telecom state changed before execution.");
        return EndCallResult::Skipped;
    }

    Log(ANDROID_LOG_INFO, "Sending the pinned Telecom end-call transaction.");
    if (!SendFixedTelecomEndCall()) return EndCallResult::Failed;
    for (int attempt = 0; attempt < 30; ++attempt) {
        if (ReadLiveCallState() == LiveCallState::None &&
            !IsAudioInCall()) {
            return EndCallResult::Ended;
        }
        usleep(100'000);
    }
    Log(ANDROID_LOG_ERROR, "Telecom returned without ending the active call.");
    return EndCallResult::Failed;
}

int64_t MonotonicMilliseconds() {
    timespec value {};
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return static_cast<int64_t>(value.tv_sec) * 1000 +
        static_cast<int64_t>(value.tv_nsec / 1'000'000);
}

int GuardianPhaseTimeout(char phase) {
    switch (phase) {
        case kGuardianPrompt:
            return kGuardianPromptMs;
        case kGuardianDtmf:
            return kGuardianDtmfMs;
        case kGuardianEndCall:
            return kGuardianEndCallMs;
        case kGuardianIdle:
            return kGuardianIdleMs;
        default:
            return kGuardianWaitForCallMs;
    }
}

bool ForcePrivateRouteForRecovery() {
    if (access(kSnapshotPath, F_OK) != 0) {
        return errno == ENOENT;
    }

    MixerRoute route {};
    if (!OpenMixerRoute(&route)) return false;
    bool applied = false;
    for (int attempt = 0;
         attempt < kPrivacyRecoveryAttempts && !gStopRequested;
         ++attempt) {
        if (ApplyPrivacyRoute(&route)) {
            applied = true;
            break;
        }
        usleep(kMixerRouteWaitSleepUs);
    }
    CloseMixerRoute(&route);
    if (!applied) {
        Log(
            ANDROID_LOG_ERROR,
            "Could not retain session privacy before recovery.");
    }
    return applied;
}

bool RecoverAndEndFailedSession(LastResult failureResult) {
    WriteCurrentState(CurrentState::Recovering);
    if (!ForcePrivateRouteForRecovery()) {
        WriteLastResult(LastResult::FailedRestore);
        WriteCurrentState(CurrentState::Error);
        Log(ANDROID_LOG_ERROR, "Session guardian could not retain mixer privacy.");
        return false;
    }

    const EndCallResult endResult = EndSingleCallAndWait();
    if (endResult == EndCallResult::Skipped) {
        ForcePrivateRouteForRecovery();
        WriteLastResult(LastResult::RecoveryHangupSkipped);
        WriteCurrentState(CurrentState::Error);
        return false;
    }
    if (endResult == EndCallResult::Failed) {
        ForcePrivateRouteForRecovery();
        WriteLastResult(LastResult::FailedEndCall);
        WriteCurrentState(CurrentState::Error);
        return false;
    }

    const MixerRecoveryResult mixerResult = RecoverMixerSnapshot();
    if (mixerResult == MixerRecoveryResult::Failed) {
        WriteLastResult(LastResult::FailedRestore);
        WriteCurrentState(CurrentState::Error);
        Log(ANDROID_LOG_ERROR, "Session guardian could not restore the mixer.");
        return false;
    }

    WriteLastResult(failureResult);
    WriteCurrentState(CurrentState::Error);
    return true;
}

[[noreturn]] void RecoverAndExitGuardian(
    int controlFd,
    pid_t workerPid,
    MixerRoute* privacyRoute,
    LastResult failureResult,
    bool stopWorker) {
    if (stopWorker) kill(workerPid, SIGKILL);
    CloseMixerRoute(privacyRoute);
    const bool recovered = RecoverAndEndFailedSession(failureResult);
    close(controlFd);
    _exit(recovered ? 0 : 1);
}

void GuardianProcess(int controlFd, pid_t workerPid) {
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    const int64_t totalDeadline = MonotonicMilliseconds() + kGuardianTotalMs;
    int64_t phaseDeadline =
        MonotonicMilliseconds() + kGuardianWaitForCallMs;
    MixerRoute privacyRoute {};
    bool privacyEnforcementActive = false;
    bool privacyReadySent = false;
    unsigned int correctionCount = 0;
    unsigned int consecutiveContendedSamples = 0;
    ivrdroid::PrivacyStabilityPolicy privacyPolicy(
        kPrivacyRequiredStableMs,
        kPrivacyMaximumUnverifiedMs);

    const auto enforcePrivacy = [&]() {
        bool startupRouteReady = false;
        const ivrdroid::PrivacyObservation observation =
            EnforcePrivateControls(
                &privacyRoute,
                &startupRouteReady);
        const ivrdroid::PrivacyDecision decision = privacyPolicy.Observe(
            observation,
            startupRouteReady,
            MonotonicMilliseconds());

        if (observation == ivrdroid::PrivacyObservation::Contended) {
            if (consecutiveContendedSamples == 0) {
                Log(
                    ANDROID_LOG_WARN,
                    "Vendor route update contested the privacy mute; retrying.");
            }
            ++consecutiveContendedSamples;
        } else {
            if (consecutiveContendedSamples > 0) {
                Log(
                    ANDROID_LOG_INFO,
                    "Privacy mute recovered after %u contested samples.",
                    consecutiveContendedSamples);
                consecutiveContendedSamples = 0;
            }
            if (observation == ivrdroid::PrivacyObservation::Corrected) {
                ++correctionCount;
                Log(
                    ANDROID_LOG_WARN,
                    "Re-applied session privacy after a vendor route update.");
            }
        }

        if (decision == ivrdroid::PrivacyDecision::Abort) {
            Log(
                ANDROID_LOG_ERROR,
                "Session privacy could not be verified for %lld ms.",
                static_cast<long long>(kPrivacyMaximumUnverifiedMs));
            return false;
        }
        if (decision == ivrdroid::PrivacyDecision::Ready &&
            !privacyReadySent) {
            if (!NotifyGuardian(controlFd, kGuardianPrivacyReady)) {
                Log(
                    ANDROID_LOG_ERROR,
                    "Could not acknowledge stable session privacy.");
                return false;
            }
            privacyReadySent = true;
            Log(
                ANDROID_LOG_INFO,
                "Session privacy held stable for %lld ms after %u corrections.",
                static_cast<long long>(kPrivacyRequiredStableMs),
                correctionCount);
        }
        return true;
    };

    while (true) {
        const int64_t now = MonotonicMilliseconds();
        const int64_t nextDeadline = std::min(totalDeadline, phaseDeadline);
        const int64_t nextWake = privacyEnforcementActive
            ? std::min(
                nextDeadline,
                now + static_cast<int64_t>(kPrivacyEnforcementPollMs))
            : nextDeadline;
        const int timeout = static_cast<int>(
            std::max<int64_t>(0, nextWake - now));
        pollfd descriptor {controlFd, POLLIN | POLLHUP, 0};
        const int result = poll(&descriptor, 1, timeout);

        if (result == 0) {
            if (MonotonicMilliseconds() >= nextDeadline) {
                Log(ANDROID_LOG_ERROR, "Session guardian deadline expired.");
                RecoverAndExitGuardian(
                    controlFd,
                    workerPid,
                    &privacyRoute,
                    LastResult::RecoveredAndEnded,
                    true);
            }
            if (privacyEnforcementActive && !enforcePrivacy()) {
                Log(
                    ANDROID_LOG_ERROR,
                    "Session guardian could not enforce microphone and speaker privacy.");
                RecoverAndExitGuardian(
                    controlFd,
                    workerPid,
                    &privacyRoute,
                    LastResult::FailedAudio,
                    true);
            }
            continue;
        }
        if (result < 0) {
            if (errno == EINTR) continue;
            RecoverAndExitGuardian(
                controlFd,
                workerPid,
                &privacyRoute,
                LastResult::RecoveredAndEnded,
                true);
        }

        char phase = 0;
        const ssize_t count = read(controlFd, &phase, 1);
        if (count == 0) {
            RecoverAndExitGuardian(
                controlFd,
                workerPid,
                &privacyRoute,
                LastResult::RecoveredAndEnded,
                false);
        }
        if (count < 0) {
            if (errno == EINTR) continue;
            RecoverAndExitGuardian(
                controlFd,
                workerPid,
                &privacyRoute,
                LastResult::RecoveredAndEnded,
                true);
        }

        if (phase == kGuardianDone || phase == kGuardianRemoteHangup) {
            CloseMixerRoute(&privacyRoute);
            close(controlFd);
            _exit(0);
        }
        if (phase == kGuardianAudioFailure ||
            phase == kGuardianCaptureFailure ||
            phase == kGuardianEndCallFailure) {
            const LastResult failureResult =
                phase == kGuardianCaptureFailure
                    ? LastResult::FailedCapture
                    : phase == kGuardianEndCallFailure
                        ? LastResult::FailedEndCall
                        : LastResult::FailedAudio;
            CloseMixerRoute(&privacyRoute);
            const bool recovered = RecoverAndEndFailedSession(failureResult);
            close(controlFd);
            _exit(recovered ? 0 : 1);
        }
        if (phase == kGuardianIdle && !privacyEnforcementActive) {
            if (!OpenMixerRoute(&privacyRoute)) {
                Log(
                    ANDROID_LOG_ERROR,
                    "Session guardian could not start privacy enforcement.");
                RecoverAndExitGuardian(
                    controlFd,
                    workerPid,
                    &privacyRoute,
                    LastResult::FailedAudio,
                    true);
            }
            privacyPolicy.Start(MonotonicMilliseconds());
            privacyEnforcementActive = true;
            if (!enforcePrivacy()) {
                Log(
                    ANDROID_LOG_ERROR,
                    "Session guardian could not establish privacy enforcement.");
                RecoverAndExitGuardian(
                    controlFd,
                    workerPid,
                    &privacyRoute,
                    LastResult::FailedAudio,
                    true);
            }
        }
        phaseDeadline =
            MonotonicMilliseconds() + GuardianPhaseTimeout(phase);
        if (privacyEnforcementActive && !enforcePrivacy()) {
            Log(
                ANDROID_LOG_ERROR,
                "Session guardian lost microphone and speaker privacy.");
            RecoverAndExitGuardian(
                controlFd,
                workerPid,
                &privacyRoute,
                LastResult::FailedAudio,
                true);
        }
    }
}

struct SessionGuardian {
    int controlFd = -1;
    pid_t pid = -1;
};

bool StartSessionGuardian(SessionGuardian* guardian) {
    int descriptors[2] = {-1, -1};
    if (socketpair(
            AF_UNIX,
            SOCK_SEQPACKET | SOCK_CLOEXEC,
            0,
            descriptors) != 0) {
        return false;
    }

    const pid_t workerPid = getpid();
    const pid_t child = fork();
    if (child < 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        return false;
    }
    if (child == 0) {
        close(descriptors[1]);
        GuardianProcess(descriptors[0], workerPid);
    }

    close(descriptors[0]);
    guardian->controlFd = descriptors[1];
    guardian->pid = child;
    return true;
}

bool FinishSessionGuardian(SessionGuardian* guardian, char result) {
    const bool notified = NotifyGuardian(guardian->controlFd, result);
    close(guardian->controlFd);
    guardian->controlFd = -1;

    int status = 0;
    while (waitpid(guardian->pid, &status, 0) < 0) {
        if (errno != EINTR) return false;
    }
    guardian->pid = -1;
    return notified &&
        WIFEXITED(status) &&
        WEXITSTATUS(status) == 0;
}

enum class SessionOutcome {
    Complete,
    RemoteHangup,
    AudioFailure,
    CaptureFailure,
    EndCallFailure,
};

PromptResult PlayTerminalPrompt(
    ivrdroid::MenuPrompt prompt,
    int guardianFd) {
    switch (prompt) {
        case ivrdroid::MenuPrompt::Sales:
            return ProcessPrompt(
                kSalesPromptPath,
                CurrentState::PlayingSales,
                guardianFd);
        case ivrdroid::MenuPrompt::Support:
            return ProcessPrompt(
                kSupportPromptPath,
                CurrentState::PlayingSupport,
                guardianFd);
        case ivrdroid::MenuPrompt::Operator:
            return ProcessPrompt(
                kOperatorPromptPath,
                CurrentState::PlayingOperator,
                guardianFd);
        case ivrdroid::MenuPrompt::Main:
            break;
    }
    return PromptResult::Failed;
}

SessionOutcome FinishSuccessfulMenu(int guardianFd) {
    WriteCurrentState(CurrentState::EndingCall);
    if (!NotifyGuardian(guardianFd, kGuardianEndCall)) {
        return SessionOutcome::EndCallFailure;
    }
    const EndCallResult endResult = EndSingleCallAndWait();
    return endResult == EndCallResult::Ended ||
            endResult == EndCallResult::NoCall
        ? SessionOutcome::Complete
        : SessionOutcome::EndCallFailure;
}

SessionOutcome RunFixedMenuBody(int guardianFd) {
    ivrdroid::MenuPolicy policy;

    while (!gStopRequested) {
        const PromptResult mainPrompt = ProcessPrompt(
            kMainPromptPath,
            CurrentState::PlayingMain,
            guardianFd);
        if (mainPrompt == PromptResult::RemoteHangup) {
            return SessionOutcome::RemoteHangup;
        }
        if (mainPrompt != PromptResult::Completed) {
            return SessionOutcome::AudioFailure;
        }

        const DtmfResult input = CaptureDtmfDigit(guardianFd);
        if (input.kind == DtmfResultKind::CallEnded) {
            return SessionOutcome::RemoteHangup;
        }
        if (input.kind == DtmfResultKind::CaptureError ||
            input.kind == DtmfResultKind::Stopped) {
            return SessionOutcome::CaptureFailure;
        }

        const ivrdroid::MenuInput policyInput {
            input.kind == DtmfResultKind::Digit
                ? ivrdroid::MenuInputKind::Digit
                : ivrdroid::MenuInputKind::Timeout,
            input.digit,
        };
        const ivrdroid::MenuDecision decision = policy.Handle(policyInput);
        if (decision.kind == ivrdroid::MenuDecisionKind::Retry) {
            WriteCurrentState(CurrentState::RetryingMenu);
            Log(
                ANDROID_LOG_INFO,
                "Retrying the main menu after missing or invalid input.");
            continue;
        }
        if (decision.kind == ivrdroid::MenuDecisionKind::Finish) {
            Log(ANDROID_LOG_INFO, "IVR menu retry limit reached.");
            return FinishSuccessfulMenu(guardianFd);
        }

        const PromptResult terminal =
            PlayTerminalPrompt(decision.prompt, guardianFd);
        if (terminal == PromptResult::RemoteHangup) {
            return SessionOutcome::RemoteHangup;
        }
        if (terminal != PromptResult::Completed) {
            return SessionOutcome::AudioFailure;
        }
        return FinishSuccessfulMenu(guardianFd);
    }
    return SessionOutcome::AudioFailure;
}

SessionOutcome RunFixedMenu(int guardianFd) {
    Log(ANDROID_LOG_INFO, "Starting the fixed single-call IVR menu session.");

    const PrivacyStartResult privacy = BeginPrivateSession(guardianFd);
    if (privacy == PrivacyStartResult::RemoteHangup) {
        return SessionOutcome::RemoteHangup;
    }
    if (privacy != PrivacyStartResult::Started) {
        return SessionOutcome::AudioFailure;
    }

    return RunFixedMenuBody(guardianFd);
}

bool IsSafeCommandFile(const struct stat& state) {
    return S_ISREG(state.st_mode) &&
        state.st_uid == gAppUid &&
        (state.st_mode & 0022) == 0 &&
        state.st_size > 0 &&
        state.st_size <= kMaximumCommandBytes;
}

bool ConsumeCommand(std::string* body) {
    struct stat pathState {};
    if (lstat(kCommandPath, &pathState) != 0) return false;

    bool valid = IsSafeCommandFile(pathState);
    if (valid) {
        const int fd = open(kCommandPath, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) {
            valid = false;
        } else {
            struct stat openState {};
            valid =
                fstat(fd, &openState) == 0 &&
                IsSafeCommandFile(openState) &&
                openState.st_dev == pathState.st_dev &&
                openState.st_ino == pathState.st_ino;
            if (valid) {
                const size_t size = static_cast<size_t>(openState.st_size);
                std::vector<char> content(size);
                valid = ReadAll(fd, content.data(), content.size());
                if (valid) body->assign(content.begin(), content.end());
            }
            close(fd);
        }
    }
    unlink(kCommandPath);
    if (!valid) body->clear();
    return true;
}

void DiscardRequestQueuedWhileBusy() {
    unlink(kCommandTempPath);
    if (unlink(kCommandPath) == 0) {
        Log(ANDROID_LOG_WARN, "Discarded a START_MENU request queued while busy.");
        WriteLastResult(LastResult::RejectedBusy);
    }
}

bool ProcessOneCommand() {
    std::string body;
    if (!ConsumeCommand(&body)) return false;
    if (ivrdroid::protocol::ParseCommand(body) !=
        ivrdroid::protocol::Command::StartMenu) {
        Log(ANDROID_LOG_WARN, "Rejected malformed or unknown app request.");
        WriteLastResult(LastResult::RejectedRequest);
        WriteCurrentState(CurrentState::Ready);
        return true;
    }

    Log(ANDROID_LOG_INFO, "Accepted START_MENU from app UID %u.", gAppUid);
    SessionGuardian guardian;
    if (!StartSessionGuardian(&guardian)) {
        Log(ANDROID_LOG_ERROR, "Could not start the session guardian.");
        WriteLastResult(LastResult::FailedAudio);
        WriteCurrentState(CurrentState::Error);
        const EndCallResult endResult = EndSingleCallAndWait();
        if (endResult == EndCallResult::Ended ||
            endResult == EndCallResult::NoCall) {
            WriteCurrentState(CurrentState::Ready);
        }
        return true;
    }

    const SessionOutcome outcome = RunFixedMenu(guardian.controlFd);
    char guardianResult = kGuardianAudioFailure;
    switch (outcome) {
        case SessionOutcome::Complete:
            guardianResult = kGuardianDone;
            break;
        case SessionOutcome::RemoteHangup:
            guardianResult = kGuardianRemoteHangup;
            break;
        case SessionOutcome::AudioFailure:
            guardianResult = kGuardianAudioFailure;
            break;
        case SessionOutcome::CaptureFailure:
            guardianResult = kGuardianCaptureFailure;
            break;
        case SessionOutcome::EndCallFailure:
            guardianResult = kGuardianEndCallFailure;
            break;
    }

    const bool guardianSucceeded =
        FinishSessionGuardian(&guardian, guardianResult);
    DiscardRequestQueuedWhileBusy();
    if (!guardianSucceeded) {
        Log(ANDROID_LOG_ERROR, "Session recovery did not complete cleanly.");
        gStopRequested = 1;
        return true;
    }

    if (outcome == SessionOutcome::Complete ||
        outcome == SessionOutcome::RemoteHangup) {
        const bool hasSnapshot = access(kSnapshotPath, F_OK) == 0;
        if ((outcome == SessionOutcome::Complete && !hasSnapshot) ||
            (hasSnapshot && !RestorePrivateSession())) {
            Log(
                ANDROID_LOG_ERROR,
                "Completed session did not restore its durable mixer snapshot.");
            WriteLastResult(LastResult::FailedRestore);
            WriteCurrentState(CurrentState::Error);
            gStopRequested = 1;
            return true;
        }
        WriteLastResult(
            outcome == SessionOutcome::Complete
                ? LastResult::SessionComplete
                : LastResult::RemoteHangup);
    }
    WriteCurrentState(CurrentState::Ready);
    return true;
}

bool RecoverStaleSnapshot() {
    if (access(kSnapshotPath, F_OK) != 0) {
        if (errno != ENOENT) return false;
        if (access(kSnapshotTempPath, F_OK) == 0) {
            ClearSnapshot();
        }
        return true;
    }

    Log(ANDROID_LOG_WARN, "Found an unfinished mixer transaction.");
    WriteCurrentState(CurrentState::Recovering);
    if (!ForcePrivateRouteForRecovery()) {
        WriteLastResult(LastResult::FailedRestore);
        WriteCurrentState(CurrentState::Error);
        return false;
    }

    const EndCallResult endResult = EndSingleCallAndWait();
    if (endResult == EndCallResult::Skipped) {
        ForcePrivateRouteForRecovery();
        WriteLastResult(LastResult::RecoveryHangupSkipped);
        return false;
    }
    if (endResult == EndCallResult::Failed) {
        ForcePrivateRouteForRecovery();
        WriteLastResult(LastResult::FailedEndCall);
        return false;
    }

    if (RecoverMixerSnapshot() == MixerRecoveryResult::Failed) {
        WriteLastResult(LastResult::FailedRestore);
        WriteCurrentState(CurrentState::Error);
        return false;
    }
    WriteLastResult(LastResult::RecoveredAndEnded);
    return true;
}

bool WritePidFile() {
    const int fd = open(
        kPidPath,
        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
        0600);
    if (fd < 0) return false;
    char pid[32] = {};
    const int length = std::snprintf(pid, sizeof(pid), "%d\n", getpid());
    const bool ok =
        length > 0 &&
        static_cast<size_t>(length) < sizeof(pid) &&
        WriteAll(fd, pid, static_cast<size_t>(length)) &&
        fsync(fd) == 0;
    close(fd);
    return ok;
}

int Serve() {
    if (geteuid() != 0) {
        Log(ANDROID_LOG_ERROR, "Helper must run as root.");
        return 10;
    }
    if (!EnsureStateDirectory() || !ValidateDevice()) return 11;
    if (!ResolveAndValidateBridge()) {
        Log(ANDROID_LOG_ERROR, "App bridge is unavailable.");
        return 12;
    }
    if (!ValidateAllPromptFiles()) return 13;

    const int lockFd = open(kLockPath, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lockFd < 0 || flock(lockFd, LOCK_EX | LOCK_NB) != 0) {
        if (lockFd >= 0) close(lockFd);
        Log(ANDROID_LOG_ERROR, "Another helper instance already owns the lock.");
        return 14;
    }
    if (!WritePidFile()) {
        close(lockFd);
        return 15;
    }
    if (!RecoverStaleSnapshot()) {
        unlink(kPidPath);
        close(lockFd);
        return 16;
    }

    const int inotifyFd = inotify_init1(IN_CLOEXEC);
    if (inotifyFd < 0) {
        unlink(kPidPath);
        close(lockFd);
        return 17;
    }
    const int watch = inotify_add_watch(
        inotifyFd,
        kBridgeDir,
        IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE_SELF | IN_MOVE_SELF);
    if (watch < 0) {
        close(inotifyFd);
        unlink(kPidPath);
        close(lockFd);
        return 18;
    }

    WriteCurrentState(CurrentState::Ready);
    Log(
        ANDROID_LOG_INFO,
        "Privileged helper is ready for app UID %u with profile %s.",
        gAppUid,
        gProfile->id);

    alignas(inotify_event) char events[4096] = {};
    while (!gStopRequested) {
        ProcessOneCommand();
        if (gStopRequested) break;

        pollfd descriptor {inotifyFd, POLLIN, 0};
        const int pollResult = poll(&descriptor, 1, 1000);
        if (pollResult < 0 && errno != EINTR) break;
        if (pollResult <= 0 || (descriptor.revents & POLLIN) == 0) continue;

        const ssize_t bytes = read(inotifyFd, events, sizeof(events));
        if (bytes <= 0) {
            if (errno == EINTR) continue;
            break;
        }
        size_t offset = 0;
        while (offset < static_cast<size_t>(bytes)) {
            const auto* event =
                reinterpret_cast<const inotify_event*>(events + offset);
            if ((event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) != 0) {
                gStopRequested = 1;
            }
            offset += sizeof(inotify_event) + event->len;
        }
    }

    WriteCurrentState(CurrentState::Stopped);
    inotify_rm_watch(inotifyFd, watch);
    close(inotifyFd);
    unlink(kPidPath);
    flock(lockFd, LOCK_UN);
    close(lockFd);
    Log(ANDROID_LOG_INFO, "Privileged helper stopped.");
    return 0;
}

int SelfTest() {
    if (geteuid() != 0 ||
        !EnsureStateDirectory() ||
        !ValidateDevice() ||
        !ResolveAndValidateBridge() ||
        !ValidateAllPromptFiles()) {
        return 20;
    }

    MixerRoute route {};
    if (!OpenMixerRoute(&route)) return 21;
    const RouteValues current = ReadRoute(route);
    CloseMixerRoute(&route);

    std::string telecomDump;
    const bool telecomParsed =
        ReadTelecomDump(&telecomDump) &&
        ivrdroid::ParseTelecomCallSnapshot(telecomDump).parsed;
    if (!telecomParsed) {
        Log(ANDROID_LOG_ERROR, "Self-test could not parse the pinned Telecom call list.");
        return 22;
    }

    Log(
        ANDROID_LOG_INFO,
        "Self-test passed without mutation: profile=%s mode=%s "
        "dout=%d mixer=%d speaker=%d mic=%d appUid=%u.",
        gProfile->id,
        IsAudioInCall() ? "IN_CALL" : "NORMAL",
        current.dout,
        current.mixer,
        current.speaker,
        current.mic,
        gAppUid);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    signal(SIGINT, HandleSignal);
    signal(SIGTERM, HandleSignal);
    signal(SIGHUP, HandleSignal);

    if (argc == 2 && std::strcmp(argv[1], "--serve") == 0) {
        return Serve();
    }
    if (argc == 2 && std::strcmp(argv[1], "--self-test") == 0) {
        return SelfTest();
    }

    std::fprintf(stderr, "usage: %s --serve | --self-test\n", argv[0]);
    return 2;
}
