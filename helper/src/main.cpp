#include "call_safety_policy.h"
#include "device_profile.h"
#include "dtmf_detector.h"
#include "helper_protocol.h"
#include "menu_policy.h"
#include "mixer_route_policy.h"
#include "privacy_policy.h"
#include "session_snapshot.h"
#include "telecom_guard.h"

#include <android/log.h>
#include <tinyalsa/asoundlib.h>

#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/poll.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <algorithm>
#include <array>
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
constexpr char kBootIdPath[] = "/proc/sys/kernel/random/boot_id";

constexpr char kMainPromptPath[] =
    "/data/adb/modules/ivrdroid_helper/prompts/main-menu.wav";
constexpr char kSalesPromptPath[] =
    "/data/adb/modules/ivrdroid_helper/prompts/sales-unavailable.wav";
constexpr char kSupportPromptPath[] =
    "/data/adb/modules/ivrdroid_helper/prompts/support-unavailable.wav";
constexpr char kOperatorPromptPath[] =
    "/data/adb/modules/ivrdroid_helper/prompts/operator-unavailable.wav";

constexpr char kServicePath[] = "/system/bin/service";
constexpr char kDumpsysPath[] = "/system/bin/dumpsys";
constexpr int kCallWaitIterations = 100;
constexpr useconds_t kCallWaitSleepUs = 100'000;
constexpr int kMixerRouteWaitIterations = 5'000;
constexpr useconds_t kMixerRouteWaitSleepUs = 2'000;
constexpr int kPrivacyEnforcementPollMs = 5;
constexpr int kPrivacyInitialAttempts = 125;
constexpr int kPrivacyRecoveryAttempts = 125;
constexpr int64_t kPrivacyRequiredStableMs = 500;
constexpr int64_t kPrivacyMaximumUnverifiedMs = 250;
constexpr int kGuardianPrivacyArmedWaitMs = 2'000;
constexpr int kGuardianPrivacyReadyWaitMs = 12'000;
constexpr int kCallMonitorPollMs = 500;
constexpr int64_t kSessionIdleConfirmationMs = 500;
constexpr int64_t kSessionUnknownMaximumMs = 3'000;
constexpr int64_t kRecoveryIdleConfirmationMs = 300;
constexpr int64_t kRecoveryAudioLagMaximumMs = 5'000;
constexpr int64_t kRecoverySingleCallConfirmationMs = 100;
constexpr int64_t kRecoveryUnknownMaximumMs = 3'000;
constexpr int64_t kRecoveryHangupRetryMs = 2'000;
constexpr int64_t kRecoveryTotalMaximumMs = 8'000;
constexpr unsigned int kRecoveryMaximumHangupAttempts = 2;
constexpr useconds_t kRecoveryPollSleepUs = 100'000;
constexpr int64_t kSystemIdleConfirmationMs = 500;
constexpr useconds_t kSystemReadinessPollSleepUs = 250'000;
constexpr off_t kMaximumPromptBytes = 4 * 1024 * 1024;
constexpr off_t kMaximumCommandBytes = 64;
constexpr size_t kMaximumTelecomDumpBytes = 512 * 1024;
constexpr size_t kMaximumAudioDumpBytes = 1024 * 1024;
constexpr int64_t kDumpsysTimeoutMs = 1'000;
constexpr int64_t kTelecomEndCallTimeoutMs = 2'000;
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
constexpr char kGuardianArmPrivacy = 'B';
constexpr char kGuardianIdle = 'I';
constexpr char kGuardianDtmf = 'L';
constexpr char kGuardianEndCall = 'E';
constexpr char kGuardianDone = 'D';
constexpr char kGuardianRemoteHangup = 'H';
constexpr char kGuardianAudioFailure = 'A';
constexpr char kGuardianCaptureFailure = 'C';
constexpr char kGuardianEndCallFailure = 'T';
constexpr char kGuardianEmergencyPreempt = 'X';
constexpr char kGuardianExternalPreempt = 'M';
constexpr char kGuardianUnverifiedPreempt = 'U';
constexpr char kGuardianPrivacyArmed = 'Q';
constexpr char kGuardianPrivacyReady = 'R';

volatile sig_atomic_t gStopRequested = 0;
uid_t gAppUid = 0;
const ivrdroid::DeviceProfile* gProfile = nullptr;
std::string gBootId;

int64_t MonotonicMilliseconds();

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

bool InitializeBootIdentity() {
    const int fd = open(kBootIdPath, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        Log(ANDROID_LOG_ERROR, "Cannot read the kernel boot identity.");
        return false;
    }

    std::array<char, 64> value {};
    ssize_t count = -1;
    do {
        count = read(fd, value.data(), value.size());
    } while (count < 0 && errno == EINTR);
    close(fd);
    if (count <= 0) {
        Log(ANDROID_LOG_ERROR, "Kernel boot identity is empty.");
        return false;
    }

    size_t length = static_cast<size_t>(count);
    while (length > 0 &&
           (value[length - 1] == '\n' || value[length - 1] == '\r')) {
        --length;
    }
    const std::string_view bootId(value.data(), length);
    if (!ivrdroid::IsValidBootId(bootId)) {
        Log(ANDROID_LOG_ERROR, "Kernel boot identity has an unsafe format.");
        return false;
    }
    gBootId.assign(bootId);
    return true;
}

uint64_t GenerateSessionId() {
    const int fd = open(
        "/dev/urandom",
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 0;
    uint64_t sessionId = 0;
    const bool readOk = ReadAll(fd, &sessionId, sizeof(sessionId));
    close(fd);
    return readOk && sessionId != 0 ? sessionId : 0;
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

bool CaptureDumpsys(
    const char* service,
    size_t maximumBytes,
    std::string* output) {
    int descriptors[2] = {-1, -1};
    if (pipe2(descriptors, O_CLOEXEC) != 0) return false;

    const pid_t parentPid = getpid();
    const pid_t child = fork();
    if (child < 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        return false;
    }
    if (child == 0) {
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 ||
            getppid() != parentPid) {
            _exit(126);
        }
        close(descriptors[0]);
        if (dup2(descriptors[1], STDOUT_FILENO) < 0) _exit(126);
        close(descriptors[1]);

        const int nullFd = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (nullFd >= 0) {
            dup2(nullFd, STDERR_FILENO);
            close(nullFd);
        }
        execl(
            kDumpsysPath,
            kDumpsysPath,
            service,
            static_cast<char*>(nullptr));
        _exit(127);
    }

    close(descriptors[1]);
    const int existingFlags = fcntl(descriptors[0], F_GETFL);
    if (existingFlags < 0 ||
        fcntl(descriptors[0], F_SETFL, existingFlags | O_NONBLOCK) != 0) {
        close(descriptors[0]);
        kill(child, SIGKILL);
        while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
        return false;
    }

    output->clear();
    bool complete = false;
    bool valid = true;
    const int64_t deadline =
        MonotonicMilliseconds() + kDumpsysTimeoutMs;
    std::array<char, 4096> buffer {};
    while (!complete && valid) {
        const int64_t remaining =
            deadline - MonotonicMilliseconds();
        if (remaining <= 0) {
            valid = false;
            break;
        }

        pollfd descriptor {
            descriptors[0],
            POLLIN | POLLHUP | POLLERR,
            0,
        };
        const int result = poll(
            &descriptor,
            1,
            static_cast<int>(remaining));
        if (result == 0) {
            valid = false;
            break;
        }
        if (result < 0) {
            if (errno == EINTR) continue;
            valid = false;
            break;
        }
        if ((descriptor.revents & (POLLIN | POLLHUP)) != 0) {
            while (true) {
                const ssize_t count = read(
                    descriptors[0],
                    buffer.data(),
                    buffer.size());
                if (count > 0) {
                    if (output->size() + static_cast<size_t>(count) >
                        maximumBytes) {
                        valid = false;
                        break;
                    }
                    output->append(
                        buffer.data(),
                        static_cast<size_t>(count));
                    continue;
                }
                if (count == 0) {
                    complete = true;
                    break;
                }
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                valid = false;
                break;
            }
        }
        if ((descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
            valid = false;
        }
    }
    close(descriptors[0]);

    if (!valid || !complete) kill(child, SIGKILL);
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            valid = false;
            break;
        }
    }
    return
        valid &&
        complete &&
        WIFEXITED(status) &&
        WEXITSTATUS(status) == 0;
}

enum class AudioModeState {
    Normal,
    InCall,
    Unknown,
};

AudioModeState ReadAudioModeState() {
    std::string dump;
    if (!CaptureDumpsys(
            "audio",
            kMaximumAudioDumpBytes,
            &dump)) {
        return AudioModeState::Unknown;
    }
    if (dump.find("Actual mode = MODE_IN_CALL") != std::string::npos) {
        return AudioModeState::InCall;
    }
    return dump.find("Actual mode = MODE_") != std::string::npos
        ? AudioModeState::Normal
        : AudioModeState::Unknown;
}

bool IsAudioInCall() {
    return ReadAudioModeState() == AudioModeState::InCall;
}

ivrdroid::AudioCallDisposition ToAudioCallDisposition(
    AudioModeState state) {
    switch (state) {
        case AudioModeState::Normal:
            return ivrdroid::AudioCallDisposition::Normal;
        case AudioModeState::InCall:
            return ivrdroid::AudioCallDisposition::InCall;
        case AudioModeState::Unknown:
            return ivrdroid::AudioCallDisposition::Unknown;
    }
    return ivrdroid::AudioCallDisposition::Unknown;
}

const char* AudioModeName(AudioModeState state) {
    switch (state) {
        case AudioModeState::Normal:
            return "NORMAL";
        case AudioModeState::InCall:
            return "IN_CALL";
        case AudioModeState::Unknown:
            return "UNKNOWN";
    }
    return "UNKNOWN";
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

struct LiveCallObservation {
    ivrdroid::CallDisposition disposition =
        ivrdroid::CallDisposition::Unknown;
    uint64_t identityHash = 0;
};

LiveCallObservation ReadLiveCallObservation();
ivrdroid::CallDisposition ReadLiveCallState();

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

using RouteValues = ivrdroid::MixerRouteState;

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

RouteValues NormalRoute(const MixerRoute& route) {
    return {
        route.normalDout,
        route.normalMixer,
        route.normalSpeaker,
        route.normalMic,
    };
}

RouteValues PrivacyRoute(const MixerRoute& route) {
    return {route.expectedDout, route.expectedMixer, 0, 0};
}

RouteValues PreAnswerPrivacyRoute(const RouteValues& normal) {
    return ivrdroid::PrivatePreAnswerRoute(normal);
}

bool ValidatePreAnswerBaseline(
    const MixerRoute& route,
    const RouteValues& snapshot) {
    const RouteValues normal = NormalRoute(route);
    const bool valid =
        ivrdroid::IsValidPreAnswerBaseline(
            snapshot,
            normal,
            route.appliedDout);
    if (!valid) {
        Log(
            ANDROID_LOG_ERROR,
            "Refusing unexpected pre-answer mixer baseline: "
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

bool ApplyPreAnswerPrivacyRoute(MixerRoute* route) {
    const RouteValues target =
        PreAnswerPrivacyRoute(NormalRoute(*route));
    // Mute both physical endpoints before changing the route that Android
    // will inherit when Telecom answers the call.
    return
        SetAndVerify(route->speaker, target.speaker) &&
        SetAndVerify(route->mic, target.mic) &&
        SetAndVerify(route->mixerEnable, target.mixer) &&
        SetAndVerify(route->dout, target.dout) &&
        SameRoute(ReadRoute(*route), target);
}

bool EstablishInitialPrivacy(MixerRoute* route) {
    for (int attempt = 0;
         attempt < kPrivacyInitialAttempts && !gStopRequested;
         ++attempt) {
        if (ApplyPreAnswerPrivacyRoute(route)) {
            return true;
        }
        usleep(kMixerRouteWaitSleepUs);
    }
    Log(
        ANDROID_LOG_ERROR,
        "Could not verify the normalized private pre-answer route.");
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
    const RouteValues expected = ExpectedRoute(route);
    const RouteValues privacy = PrivacyRoute(route);
    const RouteValues normal = NormalRoute(route);
    const RouteValues preAnswerPrivacy =
        PreAnswerPrivacyRoute(normal);
    const bool fieldsKnown =
        (current.dout == snapshot.dout ||
         current.dout == injection.dout ||
         current.dout == expected.dout ||
         current.dout == privacy.dout ||
         current.dout == normal.dout) &&
        (current.mixer == snapshot.mixer ||
         current.mixer == injection.mixer ||
         current.mixer == expected.mixer ||
         current.mixer == privacy.mixer ||
         current.mixer == normal.mixer) &&
        (current.speaker == snapshot.speaker ||
         current.speaker == injection.speaker ||
         current.speaker == expected.speaker ||
         current.speaker == privacy.speaker ||
         current.speaker == normal.speaker ||
         current.speaker == preAnswerPrivacy.speaker) &&
        (current.mic == snapshot.mic ||
         current.mic == injection.mic ||
         current.mic == expected.mic ||
         current.mic == privacy.mic ||
         current.mic == normal.mic ||
         current.mic == preAnswerPrivacy.mic);
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

bool RestoreRouteForPreemption(const RouteValues& snapshot) {
    MixerRoute route {};
    if (!OpenMixerRoute(&route)) return false;

    if (!ValidatePreAnswerBaseline(route, snapshot)) {
        Log(
            ANDROID_LOG_ERROR,
            "Preemption snapshot is not the audited pre-answer baseline.");
        CloseMixerRoute(&route);
        return false;
    }

    const RouteValues current = ReadRoute(route);
    if (SameRoute(current, snapshot)) {
        CloseMixerRoute(&route);
        return true;
    }

    const RouteValues expected = ExpectedRoute(route);
    const RouteValues injection = InjectionRoute(route);
    const bool ownedPreAnswerRoute =
        ivrdroid::IsOwnedPreAnswerRoute(
            current,
            snapshot,
            NormalRoute(route));
    const bool ownedActiveCallRoute =
        (current.dout == expected.dout ||
         current.dout == injection.dout) &&
        (current.mixer == expected.mixer ||
         current.mixer == injection.mixer) &&
        (current.speaker == expected.speaker ||
         current.speaker == 0) &&
        (current.mic == expected.mic ||
         current.mic == 0);

    const AudioModeState audioState = ReadAudioModeState();
    const RouteValues* target = nullptr;
    if (audioState == AudioModeState::InCall &&
        ownedActiveCallRoute) {
        target = &expected;
    } else if (audioState == AudioModeState::Normal &&
               ownedPreAnswerRoute) {
        target = &snapshot;
    } else {
        Log(
            ANDROID_LOG_WARN,
            "Call preemption found an external or ambiguous mixer route; "
            "releasing ownership without overwriting it.");
        CloseMixerRoute(&route);
        return true;
    }

    const bool restored =
        SetAndVerify(route.dout, target->dout) &&
        SetAndVerify(route.mixerEnable, target->mixer) &&
        SetAndVerify(route.mic, target->mic) &&
        SetAndVerify(route.speaker, target->speaker) &&
        SameRoute(ReadRoute(route), *target);
    CloseMixerRoute(&route);
    return restored;
}

bool RestoreAuditedPostCallRoute(
    const RouteValues& snapshot) {
    MixerRoute route {};
    if (!OpenMixerRoute(&route)) return false;

    if (!ValidatePreAnswerBaseline(route, snapshot)) {
        CloseMixerRoute(&route);
        return false;
    }

    const RouteValues current = ReadRoute(route);
    const RouteValues normal = NormalRoute(route);
    const RouteValues postCall =
        ivrdroid::AuditedPostCallRoute(
            normal,
            snapshot);
    if (SameRoute(current, postCall)) {
        CloseMixerRoute(&route);
        return true;
    }

    const RouteValues injection = InjectionRoute(route);
    const RouteValues expected = ExpectedRoute(route);
    const RouteValues privacy = PrivacyRoute(route);
    const RouteValues preAnswerPrivacy =
        PreAnswerPrivacyRoute(snapshot);
    const bool fieldsKnown =
        (current.dout == snapshot.dout ||
         current.dout == injection.dout ||
         current.dout == expected.dout ||
         current.dout == privacy.dout ||
         current.dout == normal.dout) &&
        (current.mixer == snapshot.mixer ||
         current.mixer == injection.mixer ||
         current.mixer == expected.mixer ||
         current.mixer == privacy.mixer ||
         current.mixer == normal.mixer) &&
        (current.speaker == snapshot.speaker ||
         current.speaker == injection.speaker ||
         current.speaker == expected.speaker ||
         current.speaker == privacy.speaker ||
         current.speaker == normal.speaker ||
         current.speaker == preAnswerPrivacy.speaker) &&
        (current.mic == snapshot.mic ||
         current.mic == injection.mic ||
         current.mic == expected.mic ||
         current.mic == privacy.mic ||
         current.mic == normal.mic ||
         current.mic == preAnswerPrivacy.mic);
    if (!fieldsKnown) {
        Log(
            ANDROID_LOG_WARN,
            "Pre-answer route changed externally; refusing recovery overwrite.");
        CloseMixerRoute(&route);
        return true;
    }

    const bool restored =
        SetAndVerify(route.dout, postCall.dout) &&
        SetAndVerify(route.mixerEnable, postCall.mixer) &&
        SetAndVerify(route.mic, postCall.mic) &&
        SetAndVerify(route.speaker, postCall.speaker) &&
        SameRoute(ReadRoute(route), postCall);
    CloseMixerRoute(&route);
    return restored;
}

bool PersistSnapshot(
    const RouteValues& route,
    uint64_t callIdentityHash) {
    ivrdroid::PersistentSessionSnapshot snapshot {};
    const uint64_t sessionId = GenerateSessionId();
    if (!ivrdroid::BuildSessionSnapshot(
            gBootId,
            sessionId,
            callIdentityHash,
            route.dout,
            route.mixer,
            route.speaker,
            route.mic,
            &snapshot)) {
        Log(ANDROID_LOG_ERROR, "Could not create a boot-scoped mixer snapshot.");
        return false;
    }

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
    Log(
        ANDROID_LOG_INFO,
        "Persisted mixer transaction %016llx for the current boot.",
        static_cast<unsigned long long>(sessionId));
    return true;
}

bool LoadSnapshot(
    ivrdroid::PersistentSessionSnapshot* snapshot) {
    const int fd = open(kSnapshotPath, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;

    struct stat state {};
    const bool readOk =
        fstat(fd, &state) == 0 &&
        S_ISREG(state.st_mode) &&
        state.st_uid == 0 &&
        state.st_size == static_cast<off_t>(sizeof(*snapshot)) &&
        ReadAll(fd, snapshot, sizeof(*snapshot));
    close(fd);
    if (!readOk ||
        !ivrdroid::ValidateSessionSnapshot(*snapshot)) {
        Log(ANDROID_LOG_ERROR, "Persistent mixer snapshot is invalid.");
        return false;
    }
    return true;
}

RouteValues SnapshotRoute(
    const ivrdroid::PersistentSessionSnapshot& snapshot) {
    return {
        snapshot.dout,
        snapshot.mixer,
        snapshot.speaker,
        snapshot.mic,
    };
}

bool ClearSnapshot() {
    bool changed = false;
    bool cleared = true;
    if (unlink(kSnapshotPath) == 0) {
        changed = true;
    } else if (errno != ENOENT) {
        cleared = false;
    }
    if (unlink(kSnapshotTempPath) == 0) {
        changed = true;
    } else if (errno != ENOENT) {
        cleared = false;
    }
    if (changed && !SyncDirectory(kStateDir)) {
        Log(ANDROID_LOG_ERROR, "Could not durably clear the mixer snapshot.");
        cleared = false;
    }
    return cleared &&
        access(kSnapshotPath, F_OK) != 0 &&
        errno == ENOENT;
}

enum class MixerRecoveryResult {
    NoSnapshot,
    Restored,
    Failed,
};

enum class MixerRestoreTarget {
    OriginalCallRoute,
    AuditedPostCall,
};

MixerRecoveryResult RecoverMixerSnapshot(
    MixerRestoreTarget target) {
    if (access(kSnapshotPath, F_OK) != 0) {
        return errno == ENOENT
            ? MixerRecoveryResult::NoSnapshot
            : MixerRecoveryResult::Failed;
    }
    ivrdroid::PersistentSessionSnapshot snapshot {};
    if (!LoadSnapshot(&snapshot) ||
        ivrdroid::ClassifySnapshotBoot(snapshot, gBootId) !=
            ivrdroid::SnapshotBootRelation::SameBoot) {
        Log(
            ANDROID_LOG_ERROR,
            "Refusing mixer recovery from another boot.");
        return MixerRecoveryResult::Failed;
    }
    const RouteValues route = SnapshotRoute(snapshot);
    const bool restored =
        target == MixerRestoreTarget::OriginalCallRoute
        ? RestoreRouteForPreemption(route)
        : RestoreAuditedPostCallRoute(route);
    if (!restored) return MixerRecoveryResult::Failed;
    return ClearSnapshot()
        ? MixerRecoveryResult::Restored
        : MixerRecoveryResult::Failed;
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

bool WaitForGuardianMessage(
    int fd,
    char expectedMessage,
    int timeoutMilliseconds) {
    const int64_t deadline =
        MonotonicMilliseconds() + timeoutMilliseconds;
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
            return count == 1 && message == expectedMessage;
        }
        if ((descriptor.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
            return false;
        }
    }
    return false;
}

bool WaitForGuardianPrivacyReady(int fd) {
    return WaitForGuardianMessage(
        fd,
        kGuardianPrivacyReady,
        kGuardianPrivacyReadyWaitMs);
}

enum class PromptResult {
    Completed,
    RemoteHangup,
    EmergencyPreempt,
    ExternalPreempt,
    Failed,
};

enum class PrivacyStartResult {
    Started,
    RemoteHangup,
    EmergencyPreempt,
    ExternalPreempt,
    Failed,
};

PromptResult PromptResultForCallDisposition(
    ivrdroid::CallDisposition disposition) {
    switch (disposition) {
        case ivrdroid::CallDisposition::Idle:
            return PromptResult::RemoteHangup;
        case ivrdroid::CallDisposition::Emergency:
            return PromptResult::EmergencyPreempt;
        case ivrdroid::CallDisposition::Multiple:
            return PromptResult::ExternalPreempt;
        case ivrdroid::CallDisposition::SingleSafe:
        case ivrdroid::CallDisposition::Unknown:
            return PromptResult::Failed;
    }
    return PromptResult::Failed;
}

PrivacyStartResult PrivacyResultForCallDisposition(
    ivrdroid::CallDisposition disposition) {
    switch (disposition) {
        case ivrdroid::CallDisposition::Idle:
            return PrivacyStartResult::RemoteHangup;
        case ivrdroid::CallDisposition::Emergency:
            return PrivacyStartResult::EmergencyPreempt;
        case ivrdroid::CallDisposition::Multiple:
            return PrivacyStartResult::ExternalPreempt;
        case ivrdroid::CallDisposition::SingleSafe:
        case ivrdroid::CallDisposition::Unknown:
            return PrivacyStartResult::Failed;
    }
    return PrivacyStartResult::Failed;
}

bool RestorePrivateSession();

PrivacyStartResult BeginPrivateSession(int guardianFd) {
    WriteCurrentState(CurrentState::ArmingPrivacy);

    if (ReadAudioModeState() != AudioModeState::Normal) {
        Log(
            ANDROID_LOG_WARN,
            "Refusing pre-answer privacy outside MODE_NORMAL.");
        return PrivacyResultForCallDisposition(ReadLiveCallState());
    }

    const LiveCallObservation initialCall = ReadLiveCallObservation();
    if (initialCall.disposition !=
            ivrdroid::CallDisposition::SingleSafe ||
        initialCall.identityHash == 0) {
        return PrivacyResultForCallDisposition(
            initialCall.disposition);
    }

    MixerRoute route {};
    if (!OpenMixerRoute(&route)) return PrivacyStartResult::Failed;
    const RouteValues snapshot = ReadRoute(route);
    if (!ValidatePreAnswerBaseline(route, snapshot)) {
        CloseMixerRoute(&route);
        return PrivacyStartResult::Failed;
    }

    if (!PersistSnapshot(snapshot, initialCall.identityHash)) {
        CloseMixerRoute(&route);
        return PrivacyStartResult::Failed;
    }

    const bool applied = EstablishInitialPrivacy(&route);
    if (!applied) {
        CloseMixerRoute(&route);
        Log(
            ANDROID_LOG_ERROR,
            "Initial session privacy failed; retaining the snapshot for recovery.");
        return PrivacyStartResult::Failed;
    }

    if (!NotifyGuardian(guardianFd, kGuardianArmPrivacy) ||
        !WaitForGuardianMessage(
            guardianFd,
            kGuardianPrivacyArmed,
            kGuardianPrivacyArmedWaitMs)) {
        CloseMixerRoute(&route);
        Log(
            ANDROID_LOG_ERROR,
            "Guardian did not verify pre-answer microphone and speaker privacy.");
        return PrivacyStartResult::Failed;
    }
    Log(
        ANDROID_LOG_INFO,
        "Pre-answer privacy verified: tablet microphone and speaker are muted.");
    WriteCurrentState(CurrentState::WaitingForCall);

    bool privateCallRouteFound = false;
    for (int attempt = 0;
         attempt < kMixerRouteWaitIterations && !gStopRequested;
         ++attempt) {
        bool startupRouteReady = false;
        const ivrdroid::PrivacyObservation observation =
            EnforcePrivateControls(&route, &startupRouteReady);
        if (observation != ivrdroid::PrivacyObservation::Contended &&
            startupRouteReady &&
            SameRoute(ReadRoute(route), PrivacyRoute(route))) {
            privateCallRouteFound = true;
            break;
        }
        usleep(kMixerRouteWaitSleepUs);
    }
    CloseMixerRoute(&route);
    if (!privateCallRouteFound) {
        Log(
            ANDROID_LOG_ERROR,
            "Timed out waiting for the audited private in-call mixer route.");
        return PrivacyResultForCallDisposition(ReadLiveCallState());
    }

    if (!WaitForInCall()) {
        return PrivacyResultForCallDisposition(ReadLiveCallState());
    }
    const LiveCallObservation activeCall = ReadLiveCallObservation();
    if (activeCall.disposition !=
        ivrdroid::CallDisposition::SingleSafe) {
        return PrivacyResultForCallDisposition(
            activeCall.disposition);
    }
    if (activeCall.identityHash != initialCall.identityHash) {
        Log(
            ANDROID_LOG_WARN,
            "Telecom call identity changed while IVR privacy was starting.");
        return PrivacyStartResult::ExternalPreempt;
    }

    if (!NotifyGuardian(guardianFd, kGuardianIdle)) {
        return PrivacyStartResult::Failed;
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
    const MixerRecoveryResult result = RecoverMixerSnapshot(
        MixerRestoreTarget::AuditedPostCall);
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
        return PromptResultForCallDisposition(ReadLiveCallState());
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
        return PromptResultForCallDisposition(ReadLiveCallState());
    }
    Log(ANDROID_LOG_INFO, "Prompt completed; session privacy remains active.");
    return PromptResult::Completed;
}

enum class DtmfResultKind {
    Digit,
    Timeout,
    CallEnded,
    EmergencyPreempt,
    ExternalPreempt,
    CaptureError,
    Stopped,
};

struct DtmfResult {
    DtmfResultKind kind;
    char digit;
};

DtmfResult CaptureDtmfDigit(int guardianFd) {
    if (gProfile == nullptr) return {DtmfResultKind::CaptureError, 0};
    if (!IsAudioInCall()) {
        const ivrdroid::CallDisposition disposition =
            ReadLiveCallState();
        if (disposition == ivrdroid::CallDisposition::Idle) {
            return {DtmfResultKind::CallEnded, 0};
        }
        if (disposition == ivrdroid::CallDisposition::Emergency) {
            return {DtmfResultKind::EmergencyPreempt, 0};
        }
        if (disposition == ivrdroid::CallDisposition::Multiple) {
            return {DtmfResultKind::ExternalPreempt, 0};
        }
        return {DtmfResultKind::CaptureError, 0};
    }
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
            const ivrdroid::CallDisposition disposition =
                ReadLiveCallState();
            if (disposition == ivrdroid::CallDisposition::Idle) {
                return {DtmfResultKind::CallEnded, 0};
            }
            if (disposition == ivrdroid::CallDisposition::Emergency) {
                return {DtmfResultKind::EmergencyPreempt, 0};
            }
            if (disposition == ivrdroid::CallDisposition::Multiple) {
                return {DtmfResultKind::ExternalPreempt, 0};
            }
            return {DtmfResultKind::CaptureError, 0};
        }
    }

    pcm_close(input);
    if (gStopRequested) return {DtmfResultKind::Stopped, 0};
    Log(ANDROID_LOG_INFO, "Caller DTMF wait timed out.");
    return {DtmfResultKind::Timeout, 0};
}

bool ReadTelecomDump(std::string* dump) {
    std::string fullDump;
    if (!CaptureDumpsys(
            "telecom",
            kMaximumTelecomDumpBytes,
            &fullDump)) {
        return false;
    }

    const size_t calls = fullDump.find("mCalls:");
    if (calls == std::string::npos) return false;
    const size_t start = fullDump.rfind('\n', calls);
    const size_t audioManager =
        fullDump.find("mCallAudioManager:", calls);
    if (audioManager == std::string::npos) return false;
    const size_t end = fullDump.find('\n', audioManager);
    *dump = fullDump.substr(
        start == std::string::npos ? 0 : start + 1,
        (end == std::string::npos ? fullDump.size() : end + 1) -
            (start == std::string::npos ? 0 : start + 1));
    return true;
}

LiveCallObservation ReadLiveCallObservation() {
    std::string dump;
    if (!ReadTelecomDump(&dump)) {
        return {};
    }
    const ivrdroid::TelecomCallSnapshot snapshot =
        ivrdroid::ParseTelecomCallSnapshot(dump);
    return {
        ivrdroid::ClassifyCallDisposition(snapshot),
        ivrdroid::StableCallIdentityHash(snapshot),
    };
}

ivrdroid::CallDisposition ReadLiveCallState() {
    return ReadLiveCallObservation().disposition;
}

bool SendFixedTelecomEndCall() {
    if (gProfile == nullptr) return false;
    const pid_t parentPid = getpid();
    const pid_t child = fork();
    if (child < 0) {
        Log(ANDROID_LOG_ERROR, "Could not fork the pinned Telecom end-call transaction.");
        return false;
    }
    if (child == 0) {
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 ||
            getppid() != parentPid) {
            _exit(126);
        }
        const int nullFd = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (nullFd >= 0) {
            dup2(nullFd, STDOUT_FILENO);
            dup2(nullFd, STDERR_FILENO);
            close(nullFd);
        }
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

    const int64_t deadline =
        MonotonicMilliseconds() + kTelecomEndCallTimeoutMs;
    while (MonotonicMilliseconds() < deadline) {
        int status = 0;
        const pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        if (result < 0 && errno != EINTR) return false;
        usleep(50'000);
    }
    kill(child, SIGKILL);
    while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
    Log(
        ANDROID_LOG_ERROR,
        "Pinned Telecom end-call transaction timed out.");
    return false;
}

bool ForcePrivateRouteForRecovery();

enum class EndCallResult {
    Ended,
    EmergencyPreempt,
    ExternalPreempt,
    UnverifiedPreempt,
    Failed,
};

EndCallResult EndSingleCallAndWait() {
    ivrdroid::PersistentSessionSnapshot sessionSnapshot {};
    if (!LoadSnapshot(&sessionSnapshot) ||
        ivrdroid::ClassifySnapshotBoot(sessionSnapshot, gBootId) !=
            ivrdroid::SnapshotBootRelation::SameBoot ||
        sessionSnapshot.callIdentityHash == 0) {
        Log(
            ANDROID_LOG_WARN,
            "No trustworthy current-boot call identity is available; "
            "refusing a global hangup.");
        return EndCallResult::UnverifiedPreempt;
    }

    ivrdroid::CallRecoveryPolicy policy(
        kRecoveryIdleConfirmationMs,
        kRecoveryAudioLagMaximumMs,
        kRecoverySingleCallConfirmationMs,
        kRecoveryUnknownMaximumMs,
        kRecoveryHangupRetryMs,
        kRecoveryTotalMaximumMs,
        kRecoveryMaximumHangupAttempts);
    policy.Start(MonotonicMilliseconds());

    while (!gStopRequested) {
        const LiveCallObservation call =
            ReadLiveCallObservation();
        const ivrdroid::CallDisposition disposition =
            call.disposition;
        if (disposition ==
                ivrdroid::CallDisposition::SingleSafe &&
            call.identityHash !=
                sessionSnapshot.callIdentityHash) {
            Log(
                ANDROID_LOG_WARN,
                "Telecom now exposes a different call; refusing to end it.");
            return EndCallResult::ExternalPreempt;
        }
        const AudioModeState audioState = ReadAudioModeState();
        const int64_t now = MonotonicMilliseconds();
        const ivrdroid::CallRecoveryDecision decision =
            policy.Observe(
                disposition,
                ToAudioCallDisposition(audioState),
                now);

        if (decision == ivrdroid::CallRecoveryDecision::EmergencyPreempt) {
            Log(
                ANDROID_LOG_WARN,
                "Emergency call detected; yielding without a Telecom hangup.");
            return EndCallResult::EmergencyPreempt;
        }
        if (decision == ivrdroid::CallRecoveryDecision::ExternalPreempt) {
            Log(
                ANDROID_LOG_WARN,
                "Multiple calls detected; yielding without a global hangup.");
            return EndCallResult::ExternalPreempt;
        }
        if (decision == ivrdroid::CallRecoveryDecision::UnverifiedPreempt) {
            Log(
                ANDROID_LOG_WARN,
                "Telecom state remained unverified; yielding audio ownership "
                "without a global hangup.");
            return EndCallResult::UnverifiedPreempt;
        }
        if (decision == ivrdroid::CallRecoveryDecision::Complete) {
            return EndCallResult::Ended;
        }
        if (decision ==
            ivrdroid::CallRecoveryDecision::CompleteAfterAudioLag) {
            Log(
                ANDROID_LOG_WARN,
                "Telecom remained idle while Android audio mode lagged; "
                "restoring the audited normal route.");
            return EndCallResult::Ended;
        }
        if (decision == ivrdroid::CallRecoveryDecision::Fail) {
            Log(
                ANDROID_LOG_ERROR,
                "Could not reconcile Telecom and audio state within the recovery bound.");
            return EndCallResult::Failed;
        }

        if (disposition != ivrdroid::CallDisposition::Unknown &&
            !ForcePrivateRouteForRecovery()) {
            return EndCallResult::Failed;
        }
        if (decision == ivrdroid::CallRecoveryDecision::RequestHangup) {
            const LiveCallObservation finalCheck =
                ReadLiveCallObservation();
            if (finalCheck.disposition ==
                ivrdroid::CallDisposition::Emergency) {
                return EndCallResult::EmergencyPreempt;
            }
            if (finalCheck.disposition ==
                    ivrdroid::CallDisposition::Multiple ||
                (finalCheck.disposition ==
                    ivrdroid::CallDisposition::SingleSafe &&
                 finalCheck.identityHash !=
                    sessionSnapshot.callIdentityHash)) {
                return EndCallResult::ExternalPreempt;
            }
            if (finalCheck.disposition !=
                    ivrdroid::CallDisposition::SingleSafe ||
                finalCheck.identityHash == 0) {
                return EndCallResult::UnverifiedPreempt;
            }
            Log(
                ANDROID_LOG_INFO,
                "Sending the pinned Telecom end-call transaction.");
            const bool sent = SendFixedTelecomEndCall();
            if (!policy.RecordHangupAttempt(now)) {
                return EndCallResult::Failed;
            }
            if (!sent) {
                Log(
                    ANDROID_LOG_WARN,
                    "Pinned Telecom hangup failed; retaining privacy before one bounded retry.");
            }
        }
        usleep(kRecoveryPollSleepUs);
    }
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
        bool startupRouteReady = false;
        if (EnforcePrivateControls(
                &route,
                &startupRouteReady) !=
            ivrdroid::PrivacyObservation::Contended) {
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

bool ReleaseSessionForPreemption(LastResult result) {
    WriteCurrentState(CurrentState::Preempting);
    bool released = true;
    if (access(kSnapshotPath, F_OK) == 0) {
        ivrdroid::PersistentSessionSnapshot snapshot {};
        if (LoadSnapshot(&snapshot) &&
            ivrdroid::ClassifySnapshotBoot(snapshot, gBootId) ==
                ivrdroid::SnapshotBootRelation::SameBoot) {
            if (!RestoreRouteForPreemption(
                    SnapshotRoute(snapshot))) {
                Log(
                    ANDROID_LOG_ERROR,
                    "Could not restore the owned mixer route during call preemption.");
                released = false;
            }
        } else {
            Log(
                ANDROID_LOG_WARN,
                "Discarding an unusable mixer snapshot during call preemption.");
        }
        if (!ClearSnapshot()) {
            released = false;
        }
    } else if (errno != ENOENT) {
        released = false;
    }

    WriteLastResult(result);
    WriteCurrentState(CurrentState::BlockedByCall);
    return released;
}

bool RecoverAndEndFailedSession(LastResult failureResult) {
    WriteCurrentState(CurrentState::Recovering);
    const EndCallResult endResult = EndSingleCallAndWait();
    if (endResult == EndCallResult::EmergencyPreempt) {
        return ReleaseSessionForPreemption(
            LastResult::EmergencyPreempted);
    }
    if (endResult == EndCallResult::ExternalPreempt) {
        return ReleaseSessionForPreemption(
            LastResult::ExternalCallPreempted);
    }
    if (endResult == EndCallResult::UnverifiedPreempt) {
        return ReleaseSessionForPreemption(
            LastResult::UnverifiedCallPreempted);
    }
    if (endResult == EndCallResult::Failed) {
        ForcePrivateRouteForRecovery();
        WriteLastResult(LastResult::FailedEndCall);
        WriteCurrentState(CurrentState::Error);
        return false;
    }

    const MixerRecoveryResult mixerResult = RecoverMixerSnapshot(
        MixerRestoreTarget::AuditedPostCall);
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

struct CallMonitor {
    int eventFd = -1;
    pid_t pid = -1;
};

struct CallMonitorEvent {
    uint64_t identityHash;
    uint8_t disposition;
    std::array<uint8_t, 7> reserved;
};

static_assert(sizeof(CallMonitorEvent) == 16);

[[noreturn]] void CallMonitorProcess(
    int eventFd,
    int inheritedControlFd,
    pid_t guardianPid) {
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    close(inheritedControlFd);
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 ||
        getppid() != guardianPid) {
        close(eventFd);
        _exit(1);
    }

    while (true) {
        const LiveCallObservation observation =
            ReadLiveCallObservation();
        const CallMonitorEvent event {
            observation.identityHash,
            static_cast<uint8_t>(observation.disposition),
            {},
        };
        const ssize_t sent = send(
            eventFd,
            &event,
            sizeof(event),
            MSG_NOSIGNAL);
        if (sent != static_cast<ssize_t>(sizeof(event))) {
            close(eventFd);
            _exit(0);
        }
        usleep(static_cast<useconds_t>(kCallMonitorPollMs * 1'000));
    }
}

bool StartCallMonitor(
    CallMonitor* monitor,
    int guardianControlFd) {
    int descriptors[2] = {-1, -1};
    if (socketpair(
            AF_UNIX,
            SOCK_SEQPACKET | SOCK_CLOEXEC,
            0,
            descriptors) != 0) {
        return false;
    }

    const pid_t guardianPid = getpid();
    const pid_t child = fork();
    if (child < 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        return false;
    }
    if (child == 0) {
        close(descriptors[0]);
        CallMonitorProcess(
            descriptors[1],
            guardianControlFd,
            guardianPid);
    }

    close(descriptors[1]);
    monitor->eventFd = descriptors[0];
    monitor->pid = child;
    return true;
}

void StopCallMonitor(CallMonitor* monitor) {
    if (monitor->eventFd >= 0) {
        close(monitor->eventFd);
        monitor->eventFd = -1;
    }
    if (monitor->pid <= 0) return;

    kill(monitor->pid, SIGTERM);
    int status = 0;
    while (waitpid(monitor->pid, &status, 0) < 0) {
        if (errno != EINTR) break;
    }
    monitor->pid = -1;
}

[[noreturn]] void PreemptAndExitGuardian(
    int controlFd,
    pid_t workerPid,
    MixerRoute* privacyRoute,
    CallMonitor* callMonitor,
    LastResult result,
    bool stopWorker) {
    if (stopWorker) kill(workerPid, SIGKILL);
    StopCallMonitor(callMonitor);
    CloseMixerRoute(privacyRoute);
    const bool released = ReleaseSessionForPreemption(result);
    close(controlFd);
    _exit(released ? 0 : 1);
}

[[noreturn]] void RecoverAndExitGuardian(
    int controlFd,
    pid_t workerPid,
    MixerRoute* privacyRoute,
    CallMonitor* callMonitor,
    LastResult failureResult,
    bool stopWorker) {
    if (stopWorker) kill(workerPid, SIGKILL);
    StopCallMonitor(callMonitor);
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
    bool privacyArmedSent = false;
    bool privacyReadySent = false;
    unsigned int correctionCount = 0;
    unsigned int consecutiveContendedSamples = 0;
    char currentPhase = 0;
    uint64_t expectedCallIdentityHash = 0;
    RouteValues preAnswerSnapshot {};
    CallMonitor callMonitor;
    ivrdroid::PrivacyStabilityPolicy privacyPolicy(
        kPrivacyRequiredStableMs,
        kPrivacyMaximumUnverifiedMs);
    ivrdroid::SessionCallMonitorPolicy callPolicy(
        kSessionIdleConfirmationMs,
        kSessionUnknownMaximumMs);

    const auto enforcePrivacy = [&]() {
        if (!privacyArmedSent &&
            !ApplyPreAnswerPrivacyRoute(&privacyRoute)) {
            Log(
                ANDROID_LOG_WARN,
                "Pre-answer route changed before guardian verification; retrying.");
        }
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

        if (!privacyArmedSent) {
            const RouteValues preAnswerPrivacy =
                PreAnswerPrivacyRoute(NormalRoute(privacyRoute));
            if (SameRoute(ReadRoute(privacyRoute), preAnswerPrivacy)) {
                if (!NotifyGuardian(
                        controlFd,
                        kGuardianPrivacyArmed)) {
                    Log(
                        ANDROID_LOG_ERROR,
                        "Could not acknowledge pre-answer session privacy.");
                    return false;
                }
                privacyArmedSent = true;
                Log(
                    ANDROID_LOG_INFO,
                    "Guardian verified pre-answer microphone and speaker privacy.");
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
            privacyArmedSent &&
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
        std::array<pollfd, 2> descriptors {{
            {controlFd, POLLIN | POLLHUP, 0},
            {callMonitor.eventFd, POLLIN | POLLHUP, 0},
        }};
        const nfds_t descriptorCount =
            callMonitor.eventFd >= 0 ? 2 : 1;
        const int result = poll(
            descriptors.data(),
            descriptorCount,
            timeout);

        if (result == 0) {
            if (MonotonicMilliseconds() >= nextDeadline) {
                Log(ANDROID_LOG_ERROR, "Session guardian deadline expired.");
                RecoverAndExitGuardian(
                    controlFd,
                    workerPid,
                    &privacyRoute,
                    &callMonitor,
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
                    &callMonitor,
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
                &callMonitor,
                LastResult::RecoveredAndEnded,
                true);
        }

        char phase = 0;
        bool controlClosed = false;
        if ((descriptors[0].revents & POLLIN) != 0) {
            const ssize_t count = recv(controlFd, &phase, 1, 0);
            if (count == 1) {
                currentPhase = phase;
            } else if (count == 0) {
                controlClosed = true;
            } else if (errno != EINTR) {
                controlClosed = true;
            }
        } else if ((descriptors[0].revents &
                    (POLLHUP | POLLERR | POLLNVAL)) != 0) {
            controlClosed = true;
        }

        if (callMonitor.eventFd >= 0 &&
            (descriptors[1].revents & POLLIN) != 0) {
            CallMonitorEvent event {};
            const ssize_t count = recv(
                callMonitor.eventFd,
                &event,
                sizeof(event),
                0);
            const bool reservedClear = std::all_of(
                event.reserved.begin(),
                event.reserved.end(),
                [](uint8_t value) { return value == 0; });
            if (count != static_cast<ssize_t>(sizeof(event)) ||
                event.disposition > static_cast<uint8_t>(
                    ivrdroid::CallDisposition::Unknown) ||
                !reservedClear) {
                Log(
                    ANDROID_LOG_ERROR,
                    "Independent call monitor returned an invalid observation.");
                PreemptAndExitGuardian(
                    controlFd,
                    workerPid,
                    &privacyRoute,
                    &callMonitor,
                    LastResult::UnverifiedCallPreempted,
                    true);
            }

            const auto disposition =
                static_cast<ivrdroid::CallDisposition>(
                    event.disposition);
            if (disposition ==
                    ivrdroid::CallDisposition::SingleSafe &&
                (event.identityHash == 0 ||
                 event.identityHash != expectedCallIdentityHash)) {
                Log(
                    ANDROID_LOG_WARN,
                    "Independent call monitor detected a different call.");
                PreemptAndExitGuardian(
                    controlFd,
                    workerPid,
                    &privacyRoute,
                    &callMonitor,
                    LastResult::ExternalCallPreempted,
                    true);
            }
            const bool endingCall =
                currentPhase == kGuardianEndCall ||
                currentPhase == kGuardianDone;
            const ivrdroid::SessionCallDecision callDecision =
                callPolicy.Observe(
                    disposition,
                    endingCall,
                    MonotonicMilliseconds());
            if (callDecision ==
                ivrdroid::SessionCallDecision::EmergencyPreempt) {
                Log(
                    ANDROID_LOG_WARN,
                    "Call monitor detected an emergency; releasing IVR audio ownership.");
                PreemptAndExitGuardian(
                    controlFd,
                    workerPid,
                    &privacyRoute,
                    &callMonitor,
                    LastResult::EmergencyPreempted,
                    true);
            }
            if (callDecision ==
                ivrdroid::SessionCallDecision::ExternalPreempt) {
                Log(
                    ANDROID_LOG_WARN,
                    "Call monitor detected multiple calls; yielding to Android.");
                PreemptAndExitGuardian(
                    controlFd,
                    workerPid,
                    &privacyRoute,
                    &callMonitor,
                    LastResult::ExternalCallPreempted,
                    true);
            }
            if (callDecision ==
                ivrdroid::SessionCallDecision::RemoteEnded) {
                Log(
                    ANDROID_LOG_INFO,
                    "Call monitor confirmed that the remote call ended.");
                RecoverAndExitGuardian(
                    controlFd,
                    workerPid,
                    &privacyRoute,
                    &callMonitor,
                    LastResult::RemoteHangup,
                    true);
            }
            if (callDecision ==
                ivrdroid::SessionCallDecision::UnverifiedPreempt) {
                Log(
                    ANDROID_LOG_WARN,
                    "Call monitor could not verify Telecom state; "
                    "releasing IVR audio ownership.");
                PreemptAndExitGuardian(
                    controlFd,
                    workerPid,
                    &privacyRoute,
                    &callMonitor,
                    LastResult::UnverifiedCallPreempted,
                    true);
            }
        } else if (callMonitor.eventFd >= 0 &&
                   (descriptors[1].revents &
                    (POLLHUP | POLLERR | POLLNVAL)) != 0) {
            Log(
                ANDROID_LOG_ERROR,
                "Independent call monitor exited; releasing IVR audio ownership.");
            PreemptAndExitGuardian(
                controlFd,
                workerPid,
                &privacyRoute,
                &callMonitor,
                LastResult::UnverifiedCallPreempted,
                true);
        }

        if (controlClosed) {
            RecoverAndExitGuardian(
                controlFd,
                workerPid,
                &privacyRoute,
                &callMonitor,
                LastResult::RecoveredAndEnded,
                false);
        }
        if (phase == 0) {
            if (privacyEnforcementActive && !enforcePrivacy()) {
                RecoverAndExitGuardian(
                    controlFd,
                    workerPid,
                    &privacyRoute,
                    &callMonitor,
                    LastResult::FailedAudio,
                    true);
            }
            continue;
        }

        if (phase == kGuardianDone) {
            StopCallMonitor(&callMonitor);
            CloseMixerRoute(&privacyRoute);
            close(controlFd);
            _exit(0);
        }
        if (phase == kGuardianRemoteHangup) {
            RecoverAndExitGuardian(
                controlFd,
                workerPid,
                &privacyRoute,
                &callMonitor,
                LastResult::RemoteHangup,
                false);
        }
        if (phase == kGuardianEmergencyPreempt) {
            PreemptAndExitGuardian(
                controlFd,
                workerPid,
                &privacyRoute,
                &callMonitor,
                LastResult::EmergencyPreempted,
                false);
        }
        if (phase == kGuardianExternalPreempt) {
            PreemptAndExitGuardian(
                controlFd,
                workerPid,
                &privacyRoute,
                &callMonitor,
                LastResult::ExternalCallPreempted,
                false);
        }
        if (phase == kGuardianUnverifiedPreempt) {
            PreemptAndExitGuardian(
                controlFd,
                workerPid,
                &privacyRoute,
                &callMonitor,
                LastResult::UnverifiedCallPreempted,
                false);
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
            RecoverAndExitGuardian(
                controlFd,
                workerPid,
                &privacyRoute,
                &callMonitor,
                failureResult,
                false);
        }
        if (phase == kGuardianArmPrivacy &&
            !privacyEnforcementActive) {
            ivrdroid::PersistentSessionSnapshot snapshot {};
            if (!LoadSnapshot(&snapshot) ||
                ivrdroid::ClassifySnapshotBoot(snapshot, gBootId) !=
                    ivrdroid::SnapshotBootRelation::SameBoot ||
                snapshot.callIdentityHash == 0) {
                Log(
                    ANDROID_LOG_ERROR,
                    "Session guardian could not load the owned call identity.");
                PreemptAndExitGuardian(
                    controlFd,
                    workerPid,
                    &privacyRoute,
                    &callMonitor,
                    LastResult::UnverifiedCallPreempted,
                    true);
            }
            expectedCallIdentityHash = snapshot.callIdentityHash;
            preAnswerSnapshot = SnapshotRoute(snapshot);
            if (!StartCallMonitor(&callMonitor, controlFd) ||
                !OpenMixerRoute(&privacyRoute)) {
                Log(
                    ANDROID_LOG_ERROR,
                    "Session guardian could not start its independent monitors.");
                RecoverAndExitGuardian(
                    controlFd,
                    workerPid,
                    &privacyRoute,
                    &callMonitor,
                    LastResult::FailedAudio,
                    true);
            }
            if (!ValidatePreAnswerBaseline(
                    privacyRoute,
                    preAnswerSnapshot)) {
                Log(
                    ANDROID_LOG_ERROR,
                    "Session guardian rejected the pre-answer mixer snapshot.");
                RecoverAndExitGuardian(
                    controlFd,
                    workerPid,
                    &privacyRoute,
                    &callMonitor,
                    LastResult::FailedAudio,
                    true);
            }
            callPolicy.Start(MonotonicMilliseconds());
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
                    &callMonitor,
                    LastResult::FailedAudio,
                    true);
            }
        }
        if (phase == kGuardianIdle &&
            !privacyEnforcementActive) {
            Log(
                ANDROID_LOG_ERROR,
                "Session reached the in-call phase before privacy was armed.");
            RecoverAndExitGuardian(
                controlFd,
                workerPid,
                &privacyRoute,
                &callMonitor,
                LastResult::FailedAudio,
                true);
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
                &callMonitor,
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
    EmergencyPreempt,
    ExternalPreempt,
    UnverifiedPreempt,
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
    switch (endResult) {
        case EndCallResult::Ended:
            return SessionOutcome::Complete;
        case EndCallResult::EmergencyPreempt:
            return SessionOutcome::EmergencyPreempt;
        case EndCallResult::ExternalPreempt:
            return SessionOutcome::ExternalPreempt;
        case EndCallResult::UnverifiedPreempt:
            return SessionOutcome::UnverifiedPreempt;
        case EndCallResult::Failed:
            return SessionOutcome::EndCallFailure;
    }
    return SessionOutcome::EndCallFailure;
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
        if (mainPrompt == PromptResult::EmergencyPreempt) {
            return SessionOutcome::EmergencyPreempt;
        }
        if (mainPrompt == PromptResult::ExternalPreempt) {
            return SessionOutcome::ExternalPreempt;
        }
        if (mainPrompt != PromptResult::Completed) {
            return SessionOutcome::AudioFailure;
        }

        const DtmfResult input = CaptureDtmfDigit(guardianFd);
        if (input.kind == DtmfResultKind::CallEnded) {
            return SessionOutcome::RemoteHangup;
        }
        if (input.kind == DtmfResultKind::EmergencyPreempt) {
            return SessionOutcome::EmergencyPreempt;
        }
        if (input.kind == DtmfResultKind::ExternalPreempt) {
            return SessionOutcome::ExternalPreempt;
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
        if (terminal == PromptResult::EmergencyPreempt) {
            return SessionOutcome::EmergencyPreempt;
        }
        if (terminal == PromptResult::ExternalPreempt) {
            return SessionOutcome::ExternalPreempt;
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
    if (privacy == PrivacyStartResult::EmergencyPreempt) {
        return SessionOutcome::EmergencyPreempt;
    }
    if (privacy == PrivacyStartResult::ExternalPreempt) {
        return SessionOutcome::ExternalPreempt;
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
        return true;
    }

    const ivrdroid::CallDisposition disposition = ReadLiveCallState();
    if (disposition != ivrdroid::CallDisposition::SingleSafe) {
        Log(
            ANDROID_LOG_WARN,
            "Rejected START_MENU because Telecom did not expose exactly one "
            "non-emergency call.");
        switch (disposition) {
            case ivrdroid::CallDisposition::Emergency:
                WriteLastResult(LastResult::EmergencyPreempted);
                break;
            case ivrdroid::CallDisposition::Multiple:
                WriteLastResult(LastResult::ExternalCallPreempted);
                break;
            case ivrdroid::CallDisposition::Unknown:
                WriteLastResult(LastResult::UnverifiedCallPreempted);
                break;
            case ivrdroid::CallDisposition::Idle:
                WriteLastResult(LastResult::RejectedRequest);
                break;
            case ivrdroid::CallDisposition::SingleSafe:
                break;
        }
        return true;
    }

    Log(ANDROID_LOG_INFO, "Accepted START_MENU from app UID %u.", gAppUid);
    SessionGuardian guardian;
    if (!StartSessionGuardian(&guardian)) {
        Log(ANDROID_LOG_ERROR, "Could not start the session guardian.");
        WriteLastResult(LastResult::FailedAudio);
        WriteCurrentState(CurrentState::Error);
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
        case SessionOutcome::EmergencyPreempt:
            guardianResult = kGuardianEmergencyPreempt;
            break;
        case SessionOutcome::ExternalPreempt:
            guardianResult = kGuardianExternalPreempt;
            break;
        case SessionOutcome::UnverifiedPreempt:
            guardianResult = kGuardianUnverifiedPreempt;
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
    } else if (outcome == SessionOutcome::EmergencyPreempt) {
        WriteLastResult(LastResult::EmergencyPreempted);
    } else if (outcome == SessionOutcome::ExternalPreempt) {
        WriteLastResult(LastResult::ExternalCallPreempted);
    } else if (outcome == SessionOutcome::UnverifiedPreempt) {
        WriteLastResult(LastResult::UnverifiedCallPreempted);
    }
    return true;
}

bool RecoverStaleSnapshot() {
    if (access(kSnapshotPath, F_OK) != 0) {
        if (errno != ENOENT) return false;
        if (access(kSnapshotTempPath, F_OK) == 0) {
            return ClearSnapshot();
        }
        return true;
    }

    ivrdroid::PersistentSessionSnapshot snapshot {};
    if (!LoadSnapshot(&snapshot)) {
        WriteLastResult(LastResult::FailedRestore);
        WriteCurrentState(CurrentState::Error);
        return false;
    }
    const ivrdroid::SnapshotBootRelation relation =
        ivrdroid::ClassifySnapshotBoot(snapshot, gBootId);
    if (relation == ivrdroid::SnapshotBootRelation::Invalid) {
        WriteLastResult(LastResult::FailedRestore);
        WriteCurrentState(CurrentState::Error);
        return false;
    }

    WriteCurrentState(CurrentState::Recovering);
    if (relation == ivrdroid::SnapshotBootRelation::PreviousBoot) {
        Log(
            ANDROID_LOG_WARN,
            "Found a mixer transaction from a previous boot; "
            "discarding it without ending a call or touching the new "
            "kernel's mixer route.");
        if (!ClearSnapshot()) {
            WriteLastResult(LastResult::FailedRestore);
            WriteCurrentState(CurrentState::Error);
            return false;
        }
        WriteLastResult(LastResult::RecoveredAfterReboot);
        return true;
    }

    Log(
        ANDROID_LOG_WARN,
        "Found an unfinished mixer transaction from the current boot.");
    const EndCallResult endResult = EndSingleCallAndWait();
    if (endResult == EndCallResult::EmergencyPreempt) {
        return ReleaseSessionForPreemption(
            LastResult::EmergencyPreempted);
    }
    if (endResult == EndCallResult::ExternalPreempt) {
        return ReleaseSessionForPreemption(
            LastResult::ExternalCallPreempted);
    }
    if (endResult == EndCallResult::UnverifiedPreempt) {
        return ReleaseSessionForPreemption(
            LastResult::UnverifiedCallPreempted);
    }
    if (endResult == EndCallResult::Failed) {
        ForcePrivateRouteForRecovery();
        WriteLastResult(LastResult::FailedEndCall);
        return false;
    }

    if (RecoverMixerSnapshot(MixerRestoreTarget::AuditedPostCall) ==
        MixerRecoveryResult::Failed) {
        WriteLastResult(LastResult::FailedRestore);
        WriteCurrentState(CurrentState::Error);
        return false;
    }
    WriteLastResult(LastResult::RecoveredAndEnded);
    return true;
}

bool WaitForSystemReady() {
    ivrdroid::SystemReadinessPolicy policy(kSystemIdleConfirmationMs);
    policy.Start(MonotonicMilliseconds());
    bool published = false;
    CurrentState publishedState = CurrentState::WaitingForSystem;

    while (!gStopRequested) {
        const ivrdroid::CallDisposition disposition = ReadLiveCallState();
        const AudioModeState audioState = ReadAudioModeState();
        const ivrdroid::SystemReadinessDecision decision = policy.Observe(
            disposition,
            ToAudioCallDisposition(audioState),
            MonotonicMilliseconds());

        const bool ready =
            decision == ivrdroid::SystemReadinessDecision::Ready;

        CurrentState state = CurrentState::WaitingForSystem;
        if (decision == ivrdroid::SystemReadinessDecision::BlockedByCall) {
            state = CurrentState::BlockedByCall;
        } else if (ready) {
            state = CurrentState::Ready;
        }
        if (!published || state != publishedState) {
            WriteCurrentState(state);
            publishedState = state;
            published = true;
            if (state == CurrentState::BlockedByCall) {
                Log(
                    ANDROID_LOG_INFO,
                    "Helper is yielding while Android owns another call.");
            }
        }
        if (ready) {
            return true;
        }
        usleep(kSystemReadinessPollSleepUs);
    }
    return false;
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
    if (!EnsureStateDirectory() ||
        !InitializeBootIdentity() ||
        !ValidateDevice()) {
        return 11;
    }
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
    WriteCurrentState(CurrentState::WaitingForSystem);
    DiscardRequestQueuedWhileBusy();
    if (!RecoverStaleSnapshot()) {
        unlink(kPidPath);
        close(lockFd);
        return 16;
    }
    if (!WaitForSystemReady()) {
        unlink(kPidPath);
        close(lockFd);
        return 17;
    }

    const int inotifyFd = inotify_init1(IN_CLOEXEC);
    if (inotifyFd < 0) {
        unlink(kPidPath);
        close(lockFd);
        return 18;
    }
    const int watch = inotify_add_watch(
        inotifyFd,
        kBridgeDir,
        IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE_SELF | IN_MOVE_SELF);
    if (watch < 0) {
        close(inotifyFd);
        unlink(kPidPath);
        close(lockFd);
        return 19;
    }

    Log(
        ANDROID_LOG_INFO,
        "Privileged helper is ready for app UID %u with profile %s.",
        gAppUid,
        gProfile->id);

    alignas(inotify_event) char events[4096] = {};
    while (!gStopRequested) {
        const bool processed = ProcessOneCommand();
        if (gStopRequested) break;
        if (processed && !WaitForSystemReady()) break;

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
        !InitializeBootIdentity() ||
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

    const AudioModeState audioState = ReadAudioModeState();
    Log(
        ANDROID_LOG_INFO,
        "Self-test passed without mutation: profile=%s boot=%.8s mode=%s "
        "dout=%d mixer=%d speaker=%d mic=%d appUid=%u.",
        gProfile->id,
        gBootId.c_str(),
        AudioModeName(audioState),
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
