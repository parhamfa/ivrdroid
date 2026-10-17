#include "call_control_protocol.h"
#include "async_recording_writer.h"
#include "call_safety_policy.h"
#include "call_lifetime_policy.h"
#include "child_process.h"
#include "device_profile.h"
#include "dtmf_detector.h"
#include "session_audio.h"
#include "external_call_policy.h"
#include "helper_protocol.h"
#include "menu_policy.h"
#include "mixer_route_policy.h"
#include "privacy_policy.h"
#include "prompt_barge_in_policy.h"
#include "recording_policy.h"
#include "revision_config.h"
#include "session_snapshot.h"
#include "sha256.h"
#include "telecom_guard.h"

#include <android/log.h>
#include <tinyalsa/asoundlib.h>

#include <sys/file.h>
#include <sys/mman.h>
#include <sys/inotify.h>
#include <sys/poll.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/system_properties.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <new>
#include <cerrno>
#include <charconv>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <signal.h>
#include <string>
#include <sstream>
#include <time.h>
#include <unordered_map>
#include <unistd.h>
#include <vector>

namespace {

using ivrdroid::protocol::CurrentState;
using ivrdroid::protocol::LastResult;

constexpr char kLogTag[] = "IVRdroidHelper";
constexpr char kHelperVersion[] = "0.10.3";
#ifndef IVRDROID_SOURCE_COMMIT
#define IVRDROID_SOURCE_COMMIT "unknown"
#endif
constexpr char kSourceCommit[] = IVRDROID_SOURCE_COMMIT;
struct GuardianEvidence { std::atomic<int64_t> verifiedConferenceAt {0}; };
GuardianEvidence* gGuardianEvidence = nullptr;

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
constexpr char kActiveRevisionBridgePath[] =
    "/data/user/0/ai.rx1.ivrdroid/files/bridge/active_revision";
constexpr char kActiveRevisionBridgeTempPath[] =
    "/data/user/0/ai.rx1.ivrdroid/files/bridge/.active_revision.tmp";
constexpr char kStagedRevisionBridgePath[] =
    "/data/user/0/ai.rx1.ivrdroid/files/bridge/staged_revision";
constexpr char kStagedRevisionBridgeTempPath[] =
    "/data/user/0/ai.rx1.ivrdroid/files/bridge/.staged_revision.tmp";
constexpr char kSessionPathBridgePath[] =
    "/data/user/0/ai.rx1.ivrdroid/files/bridge/session_path";
constexpr char kSessionPathBridgeTempPath[] =
    "/data/user/0/ai.rx1.ivrdroid/files/bridge/.session_path.tmp";
constexpr char kHelperVersionBridgePath[] =
    "/data/user/0/ai.rx1.ivrdroid/files/bridge/helper_version";
constexpr char kHelperVersionBridgeTempPath[] =
    "/data/user/0/ai.rx1.ivrdroid/files/bridge/.helper_version.tmp";
constexpr char kCapabilitiesBridgePath[] =
    "/data/user/0/ai.rx1.ivrdroid/files/bridge/capabilities";
constexpr char kCapabilitiesBridgeTempPath[] =
    "/data/user/0/ai.rx1.ivrdroid/files/bridge/.capabilities.tmp";
constexpr char kRecordingCapacityPath[] =
    "/data/user/0/ai.rx1.ivrdroid/files/bridge/recording_capacity";
constexpr char kCallControlRequestPath[] =
    "/data/user/0/ai.rx1.ivrdroid/files/bridge/call_control.request";
constexpr char kCallControlRequestTempPath[] =
    "/data/user/0/ai.rx1.ivrdroid/files/bridge/.call_control.request.tmp";
constexpr char kCallControlStatusPath[] =
    "/data/user/0/ai.rx1.ivrdroid/files/bridge/call_control.status";
constexpr char kRecordingInboxDir[] =
    "/data/user/0/ai.rx1.ivrdroid/files/bridge/recordings";
constexpr char kAppRevisionDir[] =
    "/data/user/0/ai.rx1.ivrdroid/files/revisions";

constexpr char kStateDir[] = "/data/adb/ivrdroid";
constexpr char kRevisionDir[] = "/data/adb/ivrdroid/revisions";
constexpr char kLockPath[] = "/data/adb/ivrdroid/helper.lock";
constexpr char kPidPath[] = "/data/adb/ivrdroid/helper.pid";
constexpr char kSnapshotPath[] = "/data/adb/ivrdroid/mixer.snapshot";
constexpr char kSnapshotTempPath[] = "/data/adb/ivrdroid/.mixer.snapshot.tmp";
constexpr char kBootIdPath[] = "/proc/sys/kernel/random/boot_id";
constexpr char kActiveRevisionPath[] = "/data/adb/ivrdroid/active_revision";
constexpr char kActiveRevisionTempPath[] = "/data/adb/ivrdroid/.active_revision.tmp";
constexpr char kPreviousRevisionPath[] = "/data/adb/ivrdroid/previous_revision";
constexpr char kPreviousRevisionTempPath[] = "/data/adb/ivrdroid/.previous_revision.tmp";
constexpr char kStagedRevisionPath[] = "/data/adb/ivrdroid/staged_revision";
constexpr char kStagedRevisionTempPath[] = "/data/adb/ivrdroid/.staged_revision.tmp";

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
constexpr off_t kMaximumPromptBytes = 64 * 1024 * 1024;
constexpr off_t kMaximumCommandBytes = 128;
constexpr off_t kMaximumConfigBytes = 8 * 1024 * 1024;
constexpr off_t kMaximumSessionPathBytes = 8192;
constexpr off_t kMaximumCallControlBytes =
    static_cast<off_t>(ivrdroid::call_control::kMaximumWireBytes);
constexpr off_t kMaximumRecordingCapacityBytes = 256;
constexpr size_t kMaximumTelecomDumpBytes = 512 * 1024;
constexpr size_t kMaximumAudioDumpBytes = 1024 * 1024;
constexpr int64_t kDumpsysTimeoutMs = 1'000;
constexpr int64_t kTelecomEndCallTimeoutMs = 2'000;
constexpr unsigned int kDtmfFrameCount = 1'200;
constexpr unsigned int kDtmfPeriodCount = 4;
constexpr unsigned int kDtmfCallCheckFrames = 20;
constexpr uint64_t kRecordingSpoolLimitBytes = 512ULL * 1024ULL * 1024ULL;
constexpr uint64_t kMaximumRecordingBytes = 40ULL * 1024ULL * 1024ULL;
constexpr unsigned int kRecordingTrimFrames = 4;
constexpr unsigned int kRecordingCallCheckFrames = 4;
constexpr unsigned int kRecordingHeartbeatFrames = 20;
constexpr int64_t kExternalCleanupMaximumMs = 12'000;
constexpr int64_t kExternalCallerSafeStableMs = 1'000;
constexpr int64_t kExternalCallerSafeMaximumMs = 2'500;
constexpr useconds_t kExternalCallerSafePollUs = 100'000;

constexpr int kGuardianWaitForCallMs = 12'000;
constexpr int kGuardianPromptMs = 330'000;
constexpr int kGuardianIdleMs = 12'000;
constexpr int kGuardianDtmfMs = 20'000;
constexpr int kGuardianRecordingHeartbeatMs =
    ivrdroid::kRecordingHeartbeatTimeoutMilliseconds;
constexpr int kGuardianRecordingFinalizeMs =
    ivrdroid::kRecordingFinalizationTimeoutMilliseconds;
constexpr int kGuardianEndCallMs = 6'000;
constexpr int64_t kGuardianTotalMs = 20 * 60'000;

constexpr char kGuardianPrompt = 'P';
constexpr char kGuardianArmPrivacy = 'B';
constexpr char kGuardianIdle = 'I';
constexpr char kGuardianDtmf = 'L';
constexpr char kGuardianRecording = 'V';
constexpr char kGuardianRecordingFinalize = 'F';
constexpr char kGuardianRecordingHangup = 'J';
constexpr char kGuardianOwnedDialing = 'O';
constexpr char kGuardianOwnedConference = 'K';
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
std::string gAuditBlock;

int64_t MonotonicMilliseconds();
std::string gSessionCallUuid = "-";
int gHelperLockFd = -1;
std::vector<pid_t> gDrainingGuardians;
pid_t gAuditRecoveryPid = -1;
std::string gOriginalNativeCaller;
ivrdroid::CallLifetimePolicy gCallLifetimePolicy;
uint64_t gNativeSnapshotSequence = 0;

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
    if (mkdir(kRevisionDir, 0700) != 0 && errno != EEXIST) {
        Log(ANDROID_LOG_ERROR, "Cannot create revision directory: %s", std::strerror(errno));
        return false;
    }
    struct stat revisionState {};
    if (lstat(kRevisionDir, &revisionState) != 0 ||
        !S_ISDIR(revisionState.st_mode) ||
        revisionState.st_uid != 0 ||
        chmod(kRevisionDir, 0700) != 0) {
        Log(ANDROID_LOG_ERROR, "Revision directory ownership or type is unsafe.");
        return false;
    }
    return true;
}

bool IsSafeAppOwnedFile(const char* path, off_t maximumBytes = kMaximumCommandBytes) {
    struct stat state {};
    return lstat(path, &state) == 0 &&
        S_ISREG(state.st_mode) &&
        state.st_uid == gAppUid &&
        (state.st_mode & 0022) == 0 &&
        state.st_size >= 0 &&
        state.st_size <= maximumBytes;
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
        !IsSafeAppOwnedFile(kLastResultPath) ||
        !IsSafeAppOwnedFile(kActiveRevisionBridgePath) ||
        !IsSafeAppOwnedFile(kStagedRevisionBridgePath) ||
        !IsSafeAppOwnedFile(kHelperVersionBridgePath) ||
        !IsSafeAppOwnedFile(kCapabilitiesBridgePath, 512) ||
        !IsSafeAppOwnedFile(
            kRecordingCapacityPath,
            kMaximumRecordingCapacityBytes) ||
        !IsSafeAppOwnedFile(kCallControlRequestPath, kMaximumCallControlBytes) ||
        !IsSafeAppOwnedFile(kCallControlStatusPath, kMaximumCallControlBytes) ||
        !IsSafeAppOwnedFile(kSessionPathBridgePath, kMaximumSessionPathBytes)) {
        Log(ANDROID_LOG_ERROR, "Bridge status files are missing or unsafe.");
        return false;
    }
    struct stat recordingDirectory {};
    if (lstat(kRecordingInboxDir, &recordingDirectory) != 0 ||
        !S_ISDIR(recordingDirectory.st_mode) ||
        recordingDirectory.st_uid != gAppUid ||
        (recordingDirectory.st_mode & 0077) != 0) {
        Log(ANDROID_LOG_ERROR, "Recording bridge directory is missing or unsafe.");
        return false;
    }
    return true;
}

[[maybe_unused]] void CleanupPartialRecordings() {
    DIR* directory = opendir(kRecordingInboxDir);
    if (directory == nullptr) return;
    while (dirent* entry = readdir(directory)) {
        const std::string name(entry->d_name);
        const std::string path = std::string(kRecordingInboxDir) + "/" + name;
        struct stat state {};
        if (lstat(path.c_str(), &state) == 0 &&
            ivrdroid::ShouldRemovePartialRecording(
                name,
                S_ISREG(state.st_mode),
                state.st_uid,
                0,
                gAppUid)) {
            unlink(path.c_str());
            continue;
        }
        if (lstat(path.c_str(), &state) != 0 ||
            !S_ISREG(state.st_mode) ||
            (state.st_uid != 0 && state.st_uid != gAppUid)) {
            continue;
        }
        const bool legacyWave = name.size() == 40 && name.substr(36) == ".wav";
        const bool legacyReceipt = name.size() == 41 && name.substr(36) == ".json";
        bool conversationWave = name.size() == 46 && name.substr(42) == ".wav";
        bool conversationReceipt = name.size() == 47 && name.substr(42) == ".json";
        if (conversationWave || conversationReceipt) {
            uint32_t segmentIndex = 0;
            const std::string index = name.substr(37, 5);
            const auto parsed = std::from_chars(
                index.data(),
                index.data() + index.size(),
                segmentIndex);
            const std::string expectedStem = ivrdroid::ConversationSegmentStem(
                name.substr(0, 36),
                segmentIndex);
            if (parsed.ec != std::errc() ||
                parsed.ptr != index.data() + index.size() ||
                expectedStem != name.substr(0, 42)) {
                conversationWave = false;
                conversationReceipt = false;
            }
        }
        const bool wave = legacyWave || conversationWave;
        const bool receipt = legacyReceipt || conversationReceipt;
        if (wave || receipt) {
            const size_t stemBytes =
                conversationWave || conversationReceipt ? 42 : 36;
            const std::string counterpart =
                std::string(kRecordingInboxDir) + "/" + name.substr(0, stemBytes) +
                (wave ? ".json" : ".wav");
            if (access(counterpart.c_str(), F_OK) != 0 && errno == ENOENT) {
                unlink(path.c_str());
            }
        }
    }
    closedir(directory);
    SyncDirectory(kRecordingInboxDir);
}

bool WriteBridgeValue(
    const char* path,
    const char* temporaryPath,
    const char* value,
    off_t maximumBytes = kMaximumCommandBytes) {
    if (gAppUid == 0 || !IsSafeAppOwnedFile(path, maximumBytes)) return false;
    const size_t valueLength = std::strlen(value);
    if (valueLength == 0 ||
        valueLength + 1 > static_cast<size_t>(maximumBytes)) {
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

bool WriteBridgeWire(
    const char* path,
    const char* temporaryPath,
    const std::string& wire,
    size_t maximumBytes = ivrdroid::call_control::kMaximumWireBytes) {
    if (gAppUid == 0 ||
        !IsSafeAppOwnedFile(path, static_cast<off_t>(maximumBytes)) ||
        wire.empty() || wire.size() > maximumBytes ||
        wire.back() != '\n' || wire.find('\n') != wire.size() - 1 ||
        wire.find('\r') != std::string::npos) {
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
        WriteAll(fd, wire.data(), wire.size()) &&
        fsync(fd) == 0;
    close(fd);
    if (!written || rename(temporaryPath, path) != 0) {
        unlink(temporaryPath);
        return false;
    }
    return SyncDirectory(kBridgeDir);
}

bool ReadBridgeWire(
    const char* path,
    size_t maximumBytes,
    std::string* wire) {
    if (wire == nullptr || gAppUid == 0 || maximumBytes == 0) return false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        struct stat pathState {};
        if (lstat(path, &pathState) != 0 ||
            !S_ISREG(pathState.st_mode) || pathState.st_uid != gAppUid ||
            (pathState.st_mode & 0077) != 0 || pathState.st_size <= 0 ||
            static_cast<uint64_t>(pathState.st_size) > maximumBytes) {
            return false;
        }
        const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) continue;
        struct stat openedState {};
        const bool same = fstat(fd, &openedState) == 0 &&
            openedState.st_dev == pathState.st_dev &&
            openedState.st_ino == pathState.st_ino &&
            openedState.st_uid == gAppUid &&
            (openedState.st_mode & 0077) == 0 &&
            openedState.st_size == pathState.st_size;
        if (!same) {
            close(fd);
            continue;
        }
        std::string content(static_cast<size_t>(openedState.st_size), '\0');
        const bool read = ReadAll(fd, content.data(), content.size());
        close(fd);
        if (!read) continue;
        *wire = std::move(content);
        return true;
    }
    return false;
}

bool PublishCallControlRequest(const ivrdroid::call_control::Request& request) {
    const std::string wire = ivrdroid::call_control::EncodeRequest(request);
    return !wire.empty() && WriteBridgeWire(
        kCallControlRequestPath,
        kCallControlRequestTempPath,
        wire);
}

bool ReadCallControlStatus(ivrdroid::call_control::Status* status) {
    if (status == nullptr) return false;
    std::string wire;
    if (!ReadBridgeWire(
            kCallControlStatusPath,
            ivrdroid::call_control::kMaximumWireBytes,
            &wire)) {
        return false;
    }
    *status = ivrdroid::call_control::ParseStatus(wire);
    return status->kind != ivrdroid::call_control::StatusKind::Invalid;
}

struct RecoveryOwnership {
    std::string session, boot, generation, block;
    ivrdroid::OwnedCallTopology calls;
};
bool ReadRecoveryOwnership(RecoveryOwnership* ownership) {
    std::string wire, magic, extra;
    if (!ownership || !ReadBridgeWire((std::string(kBridgeDir) + "/call_control.ownership").c_str(), 512, &wire)) return false;
    RecoveryOwnership value; std::istringstream input(wire);
    if (!(input >> magic >> value.session >> value.boot >> value.generation >> value.block >> value.calls.caller >> value.calls.operatorCall >> value.calls.conference) ||
        (input >> extra) || magic != "OWN2" || value.boot != gBootId || !ivrdroid::call_control::IsCanonicalUuid(value.session) ||
        !ivrdroid::call_control::IsCanonicalUuid(value.generation) || !ivrdroid::call_control::IsCanonicalUuid(value.block)) return false;
    auto valid = [](const std::string& id) { return id == "-" || (id.rfind("TC@", 0) == 0 && id.size() > 3 && id.size() <= 64 && id.find_first_not_of("TC@0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_-") == std::string::npos); };
    if (!valid(value.calls.caller) || value.calls.caller == "-" || !valid(value.calls.operatorCall) || !valid(value.calls.conference)) return false;
    if (value.calls.operatorCall == "-") value.calls.operatorCall.clear();
    if (value.calls.conference == "-") value.calls.conference.clear();
    *ownership = value; return true;
}
bool OwnershipAgrees(const RecoveryOwnership& ownership, const ivrdroid::TelecomCallSnapshot& snapshot) {
    if (!snapshot.parsed || snapshot.emergencyCallPresent) return false;
    if (snapshot.calls.empty()) return true;
    if (ownership.session != gSessionCallUuid || ownership.calls.caller != gOriginalNativeCaller ||
        !ivrdroid::ContainsOnlyOwnedCalls(snapshot, ownership.calls)) return false;
    if (snapshot.calls.size() == 3) return ivrdroid::MatchesOwnedConference(snapshot, ownership.calls);
    return snapshot.calls.size() <= 2;
}
void AcknowledgeRecovery(const RecoveryOwnership& ownership) {
    const std::string path = std::string(kBridgeDir) + "/call_control.attached";
    const std::string wire = "ATTACHED2 " + ownership.session + " " + ownership.boot + " " + ownership.generation + " " + ownership.block + "\n";
    std::string existing;
    if (!ReadBridgeWire(path.c_str(), 512, &existing) || existing != wire) WriteBridgeWire(path.c_str(), (path + ".tmp").c_str(), wire);
}
void RefreshCallSafetyPolicy() {
    std::string wire;
    ivrdroid::CallLifetimePolicy incoming;
    if (ReadBridgeWire((std::string(kBridgeDir) + "/call-safety-policy").c_str(), 160, &wire) &&
        ivrdroid::ParseCallLifetimePolicy(wire, &incoming) && incoming.version >= gCallLifetimePolicy.version &&
        (incoming.version != gCallLifetimePolicy.version || incoming.maximumSeconds == gCallLifetimePolicy.maximumSeconds)) gCallLifetimePolicy = incoming;
    const std::string state = ivrdroid::FormatCallLifetimePolicy(gCallLifetimePolicy);
    const std::string path = std::string(kBridgeDir) + "/call-safety-state";
    if (!ReadBridgeWire(path.c_str(), 160, &wire) || state != wire)
        WriteBridgeValue(path.c_str(), (path + ".tmp").c_str(), state.substr(0, state.size() - 1).c_str(), 160);
    const std::string protocol = std::string(kBridgeDir) + "/call-safety-protocol";
    if (!ReadBridgeWire(protocol.c_str(), 16, &wire) || wire != "1\n")
        WriteBridgeValue(protocol.c_str(), (protocol + ".tmp").c_str(), "1", 16);
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
    const std::string wire = ivrdroid::protocol::FormatCallOutcome(
        result, gSessionCallUuid, gBootId, MonotonicMilliseconds());
    if (!wire.empty()) {
        const std::string path = std::string(kBridgeDir) + "/call-outcome";
        WriteBridgeWire(path.c_str(), (path + ".tmp").c_str(), wire);
        const std::string own = std::string(kBridgeDir) + "/call-results/" + gSessionCallUuid + ".outcome";
        WriteBridgeWire(own.c_str(), (own + ".tmp").c_str(), wire);
    }
}

void WriteSessionPath(const std::string& path) {
    if (ivrdroid::call_control::IsCanonicalUuid(gSessionCallUuid)) {
        const std::string own = std::string(kBridgeDir) + "/call-results/" + gSessionCallUuid + ".path";
        WriteBridgeValue(own.c_str(), (own + ".tmp").c_str(), path.c_str(), kMaximumSessionPathBytes);
    }
    if (!WriteBridgeValue(
            kSessionPathBridgePath,
            kSessionPathBridgeTempPath,
            path.c_str(),
            kMaximumSessionPathBytes)) {
        Log(ANDROID_LOG_ERROR, "Could not publish helper session path.");
    }
}

void WriteBridgeRevision(
    const char* path,
    const char* temporaryPath,
    uint64_t revisionId) {
    char value[32] = {};
    std::snprintf(
        value,
        sizeof(value),
        "%llu",
        static_cast<unsigned long long>(revisionId));
    if (!WriteBridgeValue(path, temporaryPath, value)) {
        Log(ANDROID_LOG_ERROR, "Could not publish helper revision state.");
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

bool AdmissionCancelled();
bool WaitForInCall() {
    for (int attempt = 0;
         attempt < kCallWaitIterations && !gStopRequested;
         ++attempt) {
        if (AdmissionCancelled()) return false;
        if (IsAudioInCall()) return true;
        usleep(kCallWaitSleepUs);
    }
    return false;
}

struct LiveCallObservation {
    ivrdroid::CallDisposition disposition =
        ivrdroid::CallDisposition::Unknown;
    uint64_t identityHash = 0;
    ivrdroid::TelecomCallSnapshot snapshot {false, 0, false, {}, {}};
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

bool ValidateWaveFile(const char* path) {
    if (!ValidatePromptFile(path)) return false;
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) return false;
    uint32_t dataSize = 0;
    const bool valid = ParseWave(file, &dataSize) && dataSize > 0;
    std::fclose(file);
    return valid;
}

bool WriteRootRevisionValue(
    const char* path,
    const char* temporaryPath,
    uint64_t revisionId) {
    char value[32] = {};
    const int length = std::snprintf(
        value,
        sizeof(value),
        "%llu\n",
        static_cast<unsigned long long>(revisionId));
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(value)) return false;
    unlink(temporaryPath);
    const int descriptor = open(
        temporaryPath,
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
        0600);
    if (descriptor < 0) return false;
    const bool written =
        WriteAll(descriptor, value, static_cast<size_t>(length)) &&
        fsync(descriptor) == 0;
    close(descriptor);
    if (!written || rename(temporaryPath, path) != 0) {
        unlink(temporaryPath);
        return false;
    }
    return SyncDirectory(kStateDir);
}

bool ReadRootRevisionValue(const char* path, uint64_t* revisionId) {
    const int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) return false;
    struct stat state {};
    std::array<char, 32> content {};
    bool valid =
        fstat(descriptor, &state) == 0 &&
        S_ISREG(state.st_mode) &&
        state.st_uid == 0 &&
        (state.st_mode & 0022) == 0 &&
        state.st_size > 0 &&
        state.st_size < static_cast<off_t>(content.size());
    if (valid) valid = ReadAll(descriptor, content.data(), static_cast<size_t>(state.st_size));
    close(descriptor);
    if (!valid) return false;
    std::string_view value(content.data(), static_cast<size_t>(state.st_size));
    if (!value.empty() && value.back() == '\n') value.remove_suffix(1);
    if (value.empty()) return false;
    uint64_t parsed = 0;
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc() || result.ptr != value.data() + value.size()) return false;
    *revisionId = parsed;
    return true;
}

bool InitializeRevisionState() {
    uint64_t ignored = 0;
    if (!ReadRootRevisionValue(kActiveRevisionPath, &ignored) &&
        !WriteRootRevisionValue(kActiveRevisionPath, kActiveRevisionTempPath, 0)) {
        return false;
    }
    if (!ReadRootRevisionValue(kStagedRevisionPath, &ignored) &&
        !WriteRootRevisionValue(kStagedRevisionPath, kStagedRevisionTempPath, 0)) {
        return false;
    }
    uint64_t active = 0;
    uint64_t staged = 0;
    if (!ReadRootRevisionValue(kActiveRevisionPath, &active) ||
        !ReadRootRevisionValue(kStagedRevisionPath, &staged)) {
        return false;
    }
    WriteBridgeRevision(
        kActiveRevisionBridgePath,
        kActiveRevisionBridgeTempPath,
        active);
    WriteBridgeRevision(
        kStagedRevisionBridgePath,
        kStagedRevisionBridgeTempPath,
        staged);
    if (!WriteBridgeValue(
            kHelperVersionBridgePath,
            kHelperVersionBridgeTempPath,
            kHelperVersion)) {
        return false;
    }
    WriteBridgeValue("/data/user/0/ai.rx1.ivrdroid/files/bridge/helper_source_commit",
        "/data/user/0/ai.rx1.ivrdroid/files/bridge/.helper_source_commit.tmp", kSourceCommit);
    return true;
}

bool BuildRevisionPath(
    char* output,
    size_t outputSize,
    const char* base,
    uint64_t revisionId,
    const char* suffix = "") {
    const int length = std::snprintf(
        output,
        outputSize,
        "%s/%llu%s",
        base,
        static_cast<unsigned long long>(revisionId),
        suffix);
    return length > 0 && static_cast<size_t>(length) < outputSize;
}

bool ReadOwnedBoundedFile(
    const char* path,
    uid_t owner,
    off_t maximumBytes,
    std::string* output) {
    const int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) return false;
    struct stat state {};
    bool valid =
        fstat(descriptor, &state) == 0 &&
        S_ISREG(state.st_mode) &&
        state.st_uid == owner &&
        (state.st_mode & 0022) == 0 &&
        state.st_size > 0 &&
        state.st_size <= maximumBytes;
    if (valid) {
        output->resize(static_cast<size_t>(state.st_size));
        valid = ReadAll(descriptor, output->data(), output->size());
    }
    close(descriptor);
    if (!valid) output->clear();
    return valid;
}

bool CopyOwnedFile(
    const char* source,
    const char* destination,
    uid_t sourceOwner,
    off_t expectedSize,
    off_t maximumBytes,
    const std::string& expectedSha256) {
    const int input = open(source, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (input < 0) return false;
    struct stat state {};
    bool valid =
        fstat(input, &state) == 0 &&
        S_ISREG(state.st_mode) &&
        state.st_uid == sourceOwner &&
        (state.st_mode & 0022) == 0 &&
        state.st_size > 0 &&
        state.st_size <= maximumBytes &&
        (expectedSize < 0 || state.st_size == expectedSize);
    const int output = valid
        ? open(destination, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)
        : -1;
    if (output < 0) valid = false;
    ivrdroid::Sha256 hash;
    off_t total = 0;
    std::array<uint8_t, 16 * 1024> buffer {};
    while (valid) {
        ssize_t count = read(input, buffer.data(), buffer.size());
        if (count == 0) break;
        if (count < 0) {
            if (errno == EINTR) continue;
            valid = false;
            break;
        }
        total += count;
        if (total > maximumBytes ||
            !WriteAll(output, buffer.data(), static_cast<size_t>(count))) {
            valid = false;
            break;
        }
        hash.Update(buffer.data(), static_cast<size_t>(count));
    }
    valid = valid && total == state.st_size && fsync(output) == 0;
    close(input);
    if (output >= 0) close(output);
    if (valid && !expectedSha256.empty()) {
        const auto digest = hash.Finish();
        constexpr char alphabet[] = "0123456789abcdef";
        std::string actual;
        actual.reserve(64);
        for (const uint8_t byte : digest) {
            actual.push_back(alphabet[byte >> 4U]);
            actual.push_back(alphabet[byte & 0x0fU]);
        }
        valid = actual == expectedSha256;
    }
    if (!valid) unlink(destination);
    return valid;
}

bool RemoveTemporaryRevision(const char* path) {
    struct stat state {};
    if (lstat(path, &state) != 0) return errno == ENOENT;
    if (!S_ISDIR(state.st_mode) || state.st_uid != 0) return false;
    char promptDir[512] = {};
    if (std::snprintf(promptDir, sizeof(promptDir), "%s/prompts", path) <= 0) return false;
    DIR* directory = opendir(promptDir);
    if (directory != nullptr) {
        while (dirent* entry = readdir(directory)) {
            const std::string name(entry->d_name);
            if (name == "." || name == "..") continue;
            if (name.size() != 68 || name.substr(64) != ".wav" ||
                !ivrdroid::IsLowerHexSha256(name.substr(0, 64))) {
                closedir(directory);
                return false;
            }
            char promptPath[640] = {};
            if (std::snprintf(promptPath, sizeof(promptPath), "%s/%s", promptDir, name.c_str()) <= 0 ||
                unlink(promptPath) != 0) {
                closedir(directory);
                return false;
            }
        }
        closedir(directory);
        if (rmdir(promptDir) != 0) return false;
    } else if (errno != ENOENT) {
        return false;
    }
    char configPath[512] = {};
    if (std::snprintf(configPath, sizeof(configPath), "%s/config.txt", path) <= 0) return false;
    if (unlink(configPath) != 0 && errno != ENOENT) return false;
    return rmdir(path) == 0;
}

bool ValidateRevisionAssets(
    const char* revisionPath,
    const ivrdroid::RevisionConfig& config) {
    for (const ivrdroid::PromptAsset& prompt : config.prompts) {
        char promptPath[640] = {};
        const int length = std::snprintf(
            promptPath,
            sizeof(promptPath),
            "%s/prompts/%s.wav",
            revisionPath,
            prompt.sha256.c_str());
        if (length <= 0 || static_cast<size_t>(length) >= sizeof(promptPath)) return false;
        struct stat state {};
        std::string digest;
        if (lstat(promptPath, &state) != 0 ||
            !S_ISREG(state.st_mode) ||
            state.st_uid != 0 ||
            (state.st_mode & 0022) != 0 ||
            state.st_size != static_cast<off_t>(prompt.sizeBytes) ||
            !ivrdroid::Sha256File(promptPath, kMaximumPromptBytes, &digest) ||
            digest != prompt.sha256 ||
            !ValidateWaveFile(promptPath)) {
            return false;
        }
    }
    return true;
}

bool LoadRootRevision(
    uint64_t revisionId,
    const std::string& expectedManifest,
    bool validateAssets,
    ivrdroid::RevisionConfig* config) {
    if (revisionId == 0) return false;
    char revisionPath[384] = {};
    char configPath[448] = {};
    if (!BuildRevisionPath(revisionPath, sizeof(revisionPath), kRevisionDir, revisionId) ||
        std::snprintf(configPath, sizeof(configPath), "%s/config.txt", revisionPath) <= 0) {
        return false;
    }
    struct stat directoryState {};
    if (lstat(revisionPath, &directoryState) != 0 ||
        !S_ISDIR(directoryState.st_mode) || directoryState.st_uid != 0 ||
        (directoryState.st_mode & 0022) != 0) {
        return false;
    }
    std::string document;
    std::string error;
    if (!ReadOwnedBoundedFile(configPath, 0, kMaximumConfigBytes, &document) ||
        !ivrdroid::ParseRevisionConfig(document, config, &error) ||
        config->revisionId != revisionId ||
        (!expectedManifest.empty() && config->manifestSha256 != expectedManifest)) {
        Log(ANDROID_LOG_ERROR, "Revision %llu configuration rejected: %s.",
            static_cast<unsigned long long>(revisionId), error.c_str());
        return false;
    }
    return !validateAssets || ValidateRevisionAssets(revisionPath, *config);
}

bool StageRevision(uint64_t revisionId, const std::string& manifestSha256) {
    WriteCurrentState(CurrentState::StagingRevision);
    char sourcePath[384] = {};
    char sourceConfigPath[448] = {};
    char finalPath[384] = {};
    char temporaryPath[400] = {};
    if (!BuildRevisionPath(sourcePath, sizeof(sourcePath), kAppRevisionDir, revisionId) ||
        !BuildRevisionPath(finalPath, sizeof(finalPath), kRevisionDir, revisionId) ||
        !BuildRevisionPath(temporaryPath, sizeof(temporaryPath), kRevisionDir, revisionId, ".tmp") ||
        std::snprintf(sourceConfigPath, sizeof(sourceConfigPath), "%s/config.txt", sourcePath) <= 0) {
        return false;
    }

    ivrdroid::RevisionConfig existing;
    if (LoadRootRevision(revisionId, manifestSha256, true, &existing)) {
        const bool selected =
            WriteRootRevisionValue(kStagedRevisionPath, kStagedRevisionTempPath, revisionId);
        if (selected) WriteBridgeRevision(kStagedRevisionBridgePath, kStagedRevisionBridgeTempPath, revisionId);
        return selected;
    }
    struct stat finalState {};
    if (lstat(finalPath, &finalState) == 0 || errno != ENOENT) {
        Log(ANDROID_LOG_ERROR, "Immutable revision ID already exists with different content.");
        return false;
    }

    std::string document;
    std::string parseError;
    ivrdroid::RevisionConfig config;
    if (!ReadOwnedBoundedFile(sourceConfigPath, gAppUid, kMaximumConfigBytes, &document) ||
        !ivrdroid::ParseRevisionConfig(document, &config, &parseError) ||
        config.revisionId != revisionId || config.manifestSha256 != manifestSha256) {
        Log(ANDROID_LOG_ERROR, "App-staged revision rejected: %s.", parseError.c_str());
        return false;
    }
    if (!RemoveTemporaryRevision(temporaryPath) || mkdir(temporaryPath, 0700) != 0) return false;
    char temporaryPromptDir[480] = {};
    char temporaryConfigPath[480] = {};
    if (std::snprintf(temporaryPromptDir, sizeof(temporaryPromptDir), "%s/prompts", temporaryPath) <= 0 ||
        std::snprintf(temporaryConfigPath, sizeof(temporaryConfigPath), "%s/config.txt", temporaryPath) <= 0 ||
        mkdir(temporaryPromptDir, 0700) != 0 ||
        !CopyOwnedFile(sourceConfigPath, temporaryConfigPath, gAppUid, -1, kMaximumConfigBytes, "")) {
        RemoveTemporaryRevision(temporaryPath);
        return false;
    }
    bool valid = true;
    for (const ivrdroid::PromptAsset& prompt : config.prompts) {
        char sourcePrompt[640] = {};
        char targetPrompt[640] = {};
        const int sourceLength = std::snprintf(
            sourcePrompt, sizeof(sourcePrompt), "%s/prompts/%s.wav", sourcePath, prompt.sha256.c_str());
        const int targetLength = std::snprintf(
            targetPrompt, sizeof(targetPrompt), "%s/%s.wav", temporaryPromptDir, prompt.sha256.c_str());
        if (sourceLength <= 0 || targetLength <= 0 ||
            static_cast<size_t>(sourceLength) >= sizeof(sourcePrompt) ||
            static_cast<size_t>(targetLength) >= sizeof(targetPrompt) ||
            !CopyOwnedFile(
                sourcePrompt,
                targetPrompt,
                gAppUid,
                static_cast<off_t>(prompt.sizeBytes),
                kMaximumPromptBytes,
                prompt.sha256) ||
            !ValidateWaveFile(targetPrompt)) {
            valid = false;
            break;
        }
    }
    valid = valid && SyncDirectory(temporaryPromptDir) && SyncDirectory(temporaryPath);
    if (!valid || rename(temporaryPath, finalPath) != 0 || !SyncDirectory(kRevisionDir)) {
        RemoveTemporaryRevision(temporaryPath);
        return false;
    }
    ivrdroid::RevisionConfig staged;
    if (!LoadRootRevision(revisionId, manifestSha256, true, &staged) ||
        !WriteRootRevisionValue(kStagedRevisionPath, kStagedRevisionTempPath, revisionId)) {
        return false;
    }
    WriteBridgeRevision(kStagedRevisionBridgePath, kStagedRevisionBridgeTempPath, revisionId);
    return true;
}

bool ActivateStagedRevision(uint64_t revisionId) {
    WriteCurrentState(CurrentState::ActivatingRevision);
    uint64_t staged = 0;
    if (!ReadRootRevisionValue(kStagedRevisionPath, &staged) || staged != revisionId) return false;
    ivrdroid::RevisionConfig config;
    if (!LoadRootRevision(revisionId, "", true, &config)) return false;
    uint64_t active = 0;
    if (!ReadRootRevisionValue(kActiveRevisionPath, &active)) return false;
    if (active != 0 && active != revisionId &&
        !WriteRootRevisionValue(kPreviousRevisionPath, kPreviousRevisionTempPath, active)) {
        return false;
    }
    if (!WriteRootRevisionValue(kActiveRevisionPath, kActiveRevisionTempPath, revisionId) ||
        !WriteRootRevisionValue(kStagedRevisionPath, kStagedRevisionTempPath, 0)) {
        return false;
    }
    WriteBridgeRevision(kActiveRevisionBridgePath, kActiveRevisionBridgeTempPath, revisionId);
    WriteBridgeRevision(kStagedRevisionBridgePath, kStagedRevisionBridgeTempPath, 0);
    return true;
}

bool LoadRuntimeRevision(ivrdroid::RevisionConfig* config, uint64_t* revisionId) {
    uint64_t active = 0;
    if (ReadRootRevisionValue(kActiveRevisionPath, &active) && active != 0 &&
        LoadRootRevision(active, "", false, config)) {
        *revisionId = active;
        return true;
    }
    uint64_t previous = 0;
    if (ReadRootRevisionValue(kPreviousRevisionPath, &previous) && previous != 0 &&
        LoadRootRevision(previous, "", true, config)) {
        Log(ANDROID_LOG_WARN, "Active revision is unusable; using previous revision %llu.",
            static_cast<unsigned long long>(previous));
        *revisionId = previous;
        return true;
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
        PCM_OUT | PCM_MONOTONIC,
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

    ivrdroid::BeginAuditPrompt();
    ivrdroid::AuditEvent("prompt", gAuditBlock, "play");
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
        ivrdroid::TapAuditPrompt(output, reinterpret_cast<int16_t*>(buffer), frames);
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
    ivrdroid::EndAuditPrompt();
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

bool AdmissionCancelled() {
    std::string wire;
    if (gSessionCallUuid.empty() || !ReadOwnedBoundedFile(
            (std::string(kBridgeDir) + "/call-results/" + gSessionCallUuid + ".cancelled").c_str(),
            gAppUid, 256, &wire)) return false;
    std::istringstream input(wire);
    std::string magic, session, boot, extra;
    int64_t elapsed = 0;
    return (input >> magic >> session >> boot >> elapsed) && !(input >> extra) &&
        magic == "CANCEL1" && session == gSessionCallUuid && boot == gBootId &&
        elapsed > 0 && elapsed <= MonotonicMilliseconds();
}

PrivacyStartResult BeginPrivateSession(int guardianFd) {
    if (AdmissionCancelled()) return PrivacyStartResult::ExternalPreempt;
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
        if (AdmissionCancelled()) {
            CloseMixerRoute(&route);
            return PrivacyStartResult::ExternalPreempt;
        }
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

    ivrdroid::ActivateSessionAudio();
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

DtmfResult CaptureDtmfDigit(int guardianFd, uint32_t timeoutMilliseconds = 8000) {
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

    ivrdroid::SessionCapture* input = ivrdroid::OpenSessionCapture(
        gProfile->card,
        gProfile->captureDevice,
        PCM_IN,
        &config);
    if (input == nullptr || !ivrdroid::SessionCaptureReady(input)) {
        Log(
            ANDROID_LOG_ERROR,
            "Cannot open DTMF capture PCM: %s",
            input == nullptr ? "null handle" : ivrdroid::SessionCaptureError(input));
        if (input != nullptr) ivrdroid::CloseSessionCapture(input);
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

    const uint64_t timeoutFrames = std::max<uint64_t>(
        1,
        (static_cast<uint64_t>(timeoutMilliseconds) * gProfile->sampleRate +
         static_cast<uint64_t>(kDtmfFrameCount) * 1000U - 1U) /
            (static_cast<uint64_t>(kDtmfFrameCount) * 1000U));
    for (uint64_t frame = 0;
         frame < timeoutFrames && !gStopRequested;
         ++frame) {
        const int framesRead =
            ivrdroid::ReadSessionCapture(input, samples.data(), kDtmfFrameCount);
        if (framesRead != static_cast<int>(kDtmfFrameCount)) {
            Log(
                ANDROID_LOG_ERROR,
                "DTMF PCM read failed or returned %d frames: %s",
                framesRead,
                ivrdroid::SessionCaptureError(input));
            ivrdroid::CloseSessionCapture(input);
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
            ivrdroid::AuditEvent("digit", gAuditBlock, std::string(1, digit));
            ivrdroid::CloseSessionCapture(input);
            return {DtmfResultKind::Digit, digit};
        }

        if ((frame + 1) % kDtmfCallCheckFrames == 0 &&
            !IsAudioInCall()) {
            ivrdroid::CloseSessionCapture(input);
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

    ivrdroid::CloseSessionCapture(input);
    if (gStopRequested) return {DtmfResultKind::Stopped, 0};
    Log(ANDROID_LOG_INFO, "Caller DTMF wait timed out.");
    ivrdroid::AuditEvent("timeout", gAuditBlock);
    return {DtmfResultKind::Timeout, 0};
}

DtmfResult CaptureDtmfDigitWithPrompt(
    const char* promptPath,
    const std::unordered_map<char, uint32_t>& configuredDigits,
    int guardianFd,
    uint32_t timeoutMilliseconds) {
    if (gProfile == nullptr || !ValidatePromptFile(promptPath)) {
        return {DtmfResultKind::CaptureError, 0};
    }
    if (!IsAudioInCall()) {
        const ivrdroid::CallDisposition disposition = ReadLiveCallState();
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
    if (!NotifyGuardian(guardianFd, kGuardianPrompt)) {
        return {DtmfResultKind::CaptureError, 0};
    }

    MixerRoute route {};
    if (!OpenMixerRoute(&route)) {
        NotifyGuardian(guardianFd, kGuardianIdle);
        return {DtmfResultKind::CaptureError, 0};
    }
    const RouteValues privacy = PrivacyRoute(route);
    const bool privateRoute = SameRoute(ReadRoute(route), privacy);
    CloseMixerRoute(&route);
    if (!privateRoute) {
        Log(
            ANDROID_LOG_ERROR,
            "Session privacy route changed before interruptible prompt capture.");
        NotifyGuardian(guardianFd, kGuardianIdle);
        return {DtmfResultKind::CaptureError, 0};
    }

    int startPipe[2] = {-1, -1};
    if (socketpair(
            AF_UNIX,
            SOCK_SEQPACKET | SOCK_CLOEXEC,
            0,
            startPipe) != 0) {
        NotifyGuardian(guardianFd, kGuardianIdle);
        return {DtmfResultKind::CaptureError, 0};
    }
    const pid_t parentPid = getpid();
    pid_t playbackChild = fork();
    if (playbackChild < 0) {
        close(startPipe[0]);
        close(startPipe[1]);
        NotifyGuardian(guardianFd, kGuardianIdle);
        return {DtmfResultKind::CaptureError, 0};
    }
    if (playbackChild == 0) {
        close(startPipe[1]);
        close(guardianFd);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 ||
            getppid() != parentPid) {
            close(startPipe[0]);
            _exit(126);
        }
        char start = 0;
        ssize_t count = -1;
        do {
            count = read(startPipe[0], &start, 1);
        } while (count < 0 && errno == EINTR);
        close(startPipe[0]);
        if (count != 1 || start != 'P') _exit(126);
        _exit(PlayPrompt(promptPath) ? 0 : 1);
    }
    close(startPipe[0]);

    pcm_config captureConfig {};
    captureConfig.channels = gProfile->channels;
    captureConfig.rate = gProfile->sampleRate;
    captureConfig.period_size = kDtmfFrameCount;
    captureConfig.period_count = kDtmfPeriodCount;
    captureConfig.format = PCM_FORMAT_S16_LE;
    ivrdroid::SessionCapture* input = ivrdroid::OpenSessionCapture(
        gProfile->card,
        gProfile->captureDevice,
        PCM_IN,
        &captureConfig);
    bool routeApplied = false;

    const auto cleanup = [&](bool terminatePlayback) {
        close(startPipe[1]);
        startPipe[1] = -1;
        bool childClean = true;
        if (playbackChild > 0) {
            childClean = terminatePlayback
                ? ivrdroid::TerminateAndReapChildProcess(&playbackChild)
                : false;
        }
        if (terminatePlayback) ivrdroid::EndAuditPrompt();
        if (input != nullptr) {
            ivrdroid::CloseSessionCapture(input);
            input = nullptr;
        }
        const bool restored = !routeApplied ||
            (RestoreRoute(privacy, true) && HasPrivateSessionRoute());
        routeApplied = false;
        const bool guardianIdle = NotifyGuardian(guardianFd, kGuardianIdle);
        return childClean && restored && guardianIdle;
    };

    if (input == nullptr || !ivrdroid::SessionCaptureReady(input) || ivrdroid::StartSessionCapture(input) != 0) {
        Log(
            ANDROID_LOG_ERROR,
            "Cannot start interruptible DTMF capture PCM: %s",
            input == nullptr ? "null handle" : ivrdroid::SessionCaptureError(input));
        cleanup(true);
        return {DtmfResultKind::CaptureError, 0};
    }

    if (!OpenMixerRoute(&route)) {
        cleanup(true);
        return {DtmfResultKind::CaptureError, 0};
    }
    const bool baselinePrivate = SameRoute(ReadRoute(route), privacy);
    routeApplied = baselinePrivate;
    const bool applied = baselinePrivate && ApplyInjectionRoute(&route);
    CloseMixerRoute(&route);
    if (!applied) {
        Log(
            ANDROID_LOG_ERROR,
            "Interruptible prompt injection failed before playback.");
        cleanup(true);
        return {DtmfResultKind::CaptureError, 0};
    }
    const char start = 'P';
    if (send(startPipe[1], &start, 1, MSG_NOSIGNAL) != 1) {
        cleanup(true);
        return {DtmfResultKind::CaptureError, 0};
    }
    close(startPipe[1]);
    startPipe[1] = -1;
    WriteCurrentState(CurrentState::PlayingMain);
    Log(
        ANDROID_LOG_INFO,
        "Capturing caller DTMF while the audited menu prompt plays.");

    std::vector<int16_t> samples(kDtmfFrameCount * gProfile->channels);
    ivrdroid::StereoDtmfDetector detector(gProfile->sampleRate);
    const uint64_t timeoutFrames = std::max<uint64_t>(
        1,
        (static_cast<uint64_t>(timeoutMilliseconds) * gProfile->sampleRate +
         static_cast<uint64_t>(kDtmfFrameCount) * 1000U - 1U) /
            (static_cast<uint64_t>(kDtmfFrameCount) * 1000U));
    std::string configuredDigitList;
    for (const auto& branch : configuredDigits) {
        configuredDigitList.push_back(branch.first);
    }
    ivrdroid::PromptBargeInAttemptPolicy attemptPolicy(
        configuredDigitList,
        timeoutFrames);
    uint64_t totalFrames = 0;

    while (!gStopRequested) {
        const int framesRead =
            ivrdroid::ReadSessionCapture(input, samples.data(), kDtmfFrameCount);
        if (framesRead != static_cast<int>(kDtmfFrameCount)) {
            Log(
                ANDROID_LOG_ERROR,
                "Interruptible DTMF PCM read failed or returned %d frames: %s",
                framesRead,
                ivrdroid::SessionCaptureError(input));
            cleanup(true);
            return {DtmfResultKind::CaptureError, 0};
        }
        ++totalFrames;

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
                "Detected caller DTMF digit %c during %s "
                "(left=%.2f right=%.2f dominance=%.2f/%.2f).",
                digit,
                attemptPolicy.promptPlaying()
                    ? "prompt playback"
                    : "post-prompt timeout",
                left.confidence,
                right.confidence,
                left.dominance,
                right.dominance);
            ivrdroid::AuditEvent(configuredDigits.count(digit) ? "digit" : "invalid", gAuditBlock, std::string(1, digit));
            if (attemptPolicy.ObserveDigit(digit) ==
                ivrdroid::PromptDigitDecision::Accept) {
                if (!cleanup(attemptPolicy.promptPlaying())) {
                    return {DtmfResultKind::CaptureError, 0};
                }
                return {DtmfResultKind::Digit, digit};
            }
            Log(
                ANDROID_LOG_INFO,
                "Ignoring unconfigured digit %c while the prompt continues.",
                digit);
        }

        if (totalFrames % kDtmfCallCheckFrames == 0 && !IsAudioInCall()) {
            const ivrdroid::CallDisposition disposition = ReadLiveCallState();
            if (!cleanup(true)) {
                return {DtmfResultKind::CaptureError, 0};
            }
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

        if (attemptPolicy.promptPlaying()) {
            const ivrdroid::ChildProcessState childState =
                ivrdroid::PollChildProcess(&playbackChild);
            if (childState == ivrdroid::ChildProcessState::ExitedSuccessfully) {
                const bool restored =
                    RestoreRoute(privacy, true) && HasPrivateSessionRoute();
                if (restored) routeApplied = false;
                if (!restored || !NotifyGuardian(guardianFd, kGuardianDtmf)) {
                    cleanup(false);
                    return {DtmfResultKind::CaptureError, 0};
                }
                attemptPolicy.PromptCompleted();
                WriteCurrentState(CurrentState::ListeningDtmf);
                Log(
                    ANDROID_LOG_INFO,
                    "Prompt completed; starting the full post-prompt DTMF timeout.");
            } else if (childState != ivrdroid::ChildProcessState::Running) {
                cleanup(false);
                return {DtmfResultKind::CaptureError, 0};
            }
        } else if (attemptPolicy.AdvancePostPromptFrame()) {
            if (!cleanup(false)) {
                return {DtmfResultKind::CaptureError, 0};
            }
            Log(ANDROID_LOG_INFO, "Caller DTMF wait timed out after prompt playback.");
            ivrdroid::AuditEvent("timeout", gAuditBlock);
            return {DtmfResultKind::Timeout, 0};
        }
    }

    if (!cleanup(true)) return {DtmfResultKind::CaptureError, 0};
    return {DtmfResultKind::Stopped, 0};
}

bool ReadRecordingCapacity(ivrdroid::RecordingCapacity* capacity) {
    if (capacity == nullptr) return false;
    std::string wire;
    return ReadBridgeWire(
               kRecordingCapacityPath,
               kMaximumRecordingCapacityBytes,
               &wire) &&
        ivrdroid::ParseRecordingCapacity(wire, capacity);
}

enum class RecordingInboxKind {
    Voicemail,
    Conversation,
    Unknown,
};

RecordingInboxKind ClassifyRecordingInboxName(const std::string& name) {
    const bool temporary = !name.empty() && name.front() == '.';
    const size_t start = temporary ? 1 : 0;
    if (name.size() < start + 36 ||
        !ivrdroid::call_control::IsCanonicalUuid(
            std::string_view(name).substr(start, 36))) {
        return RecordingInboxKind::Unknown;
    }
    const std::string suffix = name.substr(start + 36);
    if ((!temporary && (suffix == ".wav" || suffix == ".json")) ||
        (temporary &&
         (suffix == ".wav.partial" || suffix == ".json.tmp"))) {
        return RecordingInboxKind::Voicemail;
    }
    if (suffix.size() < 6 || suffix.front() != '.') {
        return RecordingInboxKind::Unknown;
    }
    uint32_t segmentIndex = 0;
    const std::string digits = suffix.substr(1, 5);
    const auto parsed = std::from_chars(
        digits.data(),
        digits.data() + digits.size(),
        segmentIndex);
    if (parsed.ec != std::errc() ||
        parsed.ptr != digits.data() + digits.size() ||
        ivrdroid::ConversationSegmentStem(
            std::string_view(name).substr(start, 36),
            segmentIndex) != name.substr(start, 42)) {
        return RecordingInboxKind::Unknown;
    }
    const std::string conversationSuffix = name.substr(start + 42);
    if ((!temporary &&
         (conversationSuffix == ".wav" || conversationSuffix == ".json")) ||
        (temporary &&
         (conversationSuffix == ".wav.partial" ||
          conversationSuffix == ".json.tmp"))) {
        return RecordingInboxKind::Conversation;
    }
    return RecordingInboxKind::Unknown;
}

bool MeasureRecordingInbox(
    RecordingInboxKind requestedKind,
    uint64_t* inboxBytes) {
    if (inboxBytes == nullptr) return false;
    *inboxBytes = 0;
    DIR* directory = opendir(kRecordingInboxDir);
    if (directory == nullptr) return false;
    bool safe = true;
    while (dirent* entry = readdir(directory)) {
        if (entry->d_name[0] == '.' &&
            (entry->d_name[1] == '\0' ||
             (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
            continue;
        }
        const std::string name(entry->d_name);
        const RecordingInboxKind kind = ClassifyRecordingInboxName(name);
        const std::string path = std::string(kRecordingInboxDir) + "/" + name;
        struct stat state {};
        if (kind == RecordingInboxKind::Unknown ||
            lstat(path.c_str(), &state) != 0 ||
            !S_ISREG(state.st_mode) ||
            (state.st_uid != gAppUid && state.st_uid != 0) ||
            (state.st_mode & 0077) != 0 || state.st_size < 0) {
            safe = false;
            break;
        }
        if (kind == requestedKind) {
            const uint64_t size = static_cast<uint64_t>(state.st_size);
            if (*inboxBytes > UINT64_MAX - size) {
                safe = false;
                break;
            }
            *inboxBytes += size;
        }
    }
    closedir(directory);
    return safe;
}

bool RecordingStorageAvailable(uint32_t maximumDurationMilliseconds) {
    ivrdroid::RecordingCapacity capacity;
    if (!ReadRecordingCapacity(&capacity) ||
        capacity.voicemailBytes > kRecordingSpoolLimitBytes ||
        capacity.voicemailCount >= 64) {
        return false;
    }
    uint64_t inboxBytes = 0;
    if (!MeasureRecordingInbox(RecordingInboxKind::Voicemail, &inboxBytes) ||
        inboxBytes > kRecordingSpoolLimitBytes) return false;

    const uint64_t required =
        44ULL +
        (static_cast<uint64_t>(maximumDurationMilliseconds) * 48'000ULL * 4ULL) /
            1000ULL;
    struct statvfs filesystem {};
    return required <= kMaximumRecordingBytes &&
        statvfs(kRecordingInboxDir, &filesystem) == 0 &&
        ivrdroid::RecordingStorageFits(
            capacity.voicemailBytes,
            inboxBytes,
            required,
            static_cast<uint64_t>(filesystem.f_bavail) * filesystem.f_frsize,
            kRecordingSpoolLimitBytes);
}

bool ConversationStorageAvailable() {
    ivrdroid::RecordingCapacity capacity;
    if (!ReadRecordingCapacity(&capacity) || capacity.version != 2 ||
        capacity.conversationBytes > ivrdroid::kConversationSpoolLimitBytes) {
        return false;
    }
    uint64_t inboxBytes = 0;
    if (!MeasureRecordingInbox(RecordingInboxKind::Conversation, &inboxBytes) ||
        inboxBytes > ivrdroid::kConversationSpoolLimitBytes) {
        return false;
    }
    constexpr uint64_t required = 44ULL +
        ivrdroid::kConversationSegmentMaximumFrames * 4ULL;
    struct statvfs filesystem {};
    return required <= kMaximumRecordingBytes &&
        statvfs(kRecordingInboxDir, &filesystem) == 0 &&
        ivrdroid::ConversationStorageFits(
            capacity.conversationBytes,
            inboxBytes,
            required,
            static_cast<uint64_t>(filesystem.f_bavail) * filesystem.f_frsize,
            capacity.filesystemFreeBytes);
}

bool GenerateRecordingUuid(std::string* output) {
    if (output == nullptr) return false;
    std::array<uint8_t, 16> bytes {};
    const int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    const bool read = ReadAll(fd, bytes.data(), bytes.size());
    close(fd);
    if (!read) return false;
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3fU) | 0x80U);
    char value[37] = {};
    const int length = std::snprintf(
        value,
        sizeof(value),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
        "%02x%02x%02x%02x%02x%02x",
        bytes[0], bytes[1], bytes[2], bytes[3],
        bytes[4], bytes[5], bytes[6], bytes[7],
        bytes[8], bytes[9], bytes[10], bytes[11],
        bytes[12], bytes[13], bytes[14], bytes[15]);
    if (length != 36) return false;
    *output = value;
    return true;
}

bool WriteWaveHeader(int fd, uint32_t dataBytes) {
    const RiffHeader riff {kRiffId, 36U + dataBytes, kWaveId};
    const ChunkHeader format {kFormatId, static_cast<uint32_t>(sizeof(WaveFormat))};
    const WaveFormat wave {
        1,
        2,
        48'000,
        192'000,
        4,
        16,
    };
    const ChunkHeader data {kDataId, dataBytes};
    if (lseek(fd, 0, SEEK_SET) != 0 ||
        !WriteAll(fd, &riff, sizeof(riff)) ||
        !WriteAll(fd, &format, sizeof(format)) ||
        !WriteAll(fd, &wave, sizeof(wave)) ||
        !WriteAll(fd, &data, sizeof(data))) {
        return false;
    }
    return lseek(fd, 0, SEEK_END) >= 0;
}

bool PlayBuiltInRecordingBeep() {
    if (gProfile == nullptr || gProfile->channels != 2 || gProfile->sampleRate != 48'000) {
        return false;
    }
    pcm_config config {};
    config.channels = 2;
    config.rate = 48'000;
    config.period_size = 1024;
    config.period_count = 2;
    config.format = PCM_FORMAT_S16_LE;
    config.start_threshold = config.period_size;
    config.stop_threshold = config.period_size * config.period_count;
    pcm* output = pcm_open(gProfile->card, gProfile->playbackDevice, PCM_OUT | PCM_MONOTONIC, &config);
    if (output == nullptr || !pcm_is_ready(output)) {
        if (output != nullptr) pcm_close(output);
        return false;
    }
    constexpr uint32_t totalFrames = 24'000;
    constexpr double pi = 3.14159265358979323846;
    std::vector<int16_t> samples(config.period_size * 2);
    uint32_t written = 0;
    ivrdroid::BeginAuditPrompt();
    ivrdroid::AuditEvent("prompt", gAuditBlock, "beep");
    bool success = true;
    while (written < totalFrames && !gStopRequested) {
        const uint32_t frames = std::min<uint32_t>(config.period_size, totalFrames - written);
        for (uint32_t index = 0; index < frames; ++index) {
            const double position = static_cast<double>(written + index) / 48'000.0;
            const int16_t value = static_cast<int16_t>(
                std::lround(6000.0 * std::sin(2.0 * pi * 1000.0 * position)));
            samples[index * 2] = value;
            samples[index * 2 + 1] = value;
        }
        if (pcm_writei(output, samples.data(), frames) != static_cast<int>(frames)) {
            success = false;
            break;
        }
        ivrdroid::TapAuditPrompt(output, samples.data(), frames);
        written += frames;
    }
    if (success && !gStopRequested) pcm_wait(output, 1000);
    pcm_close(output);
    ivrdroid::EndAuditPrompt();
    return success && written == totalFrames && !gStopRequested;
}

PromptResult ProcessRecordingBeep(int guardianFd) {
    if (!IsAudioInCall() || !NotifyGuardian(guardianFd, kGuardianPrompt)) {
        return PromptResultForCallDisposition(ReadLiveCallState());
    }
    MixerRoute route {};
    if (!OpenMixerRoute(&route)) return PromptResult::Failed;
    const RouteValues privacy = PrivacyRoute(route);
    const bool safe = SameRoute(ReadRoute(route), privacy) && ApplyInjectionRoute(&route);
    CloseMixerRoute(&route);
    if (!safe) return PromptResult::Failed;
    const bool played = PlayBuiltInRecordingBeep();
    const bool restored = RestoreRoute(privacy, true);
    if (!restored || !NotifyGuardian(guardianFd, kGuardianIdle)) {
        return PromptResult::Failed;
    }
    return played ? PromptResult::Completed : PromptResultForCallDisposition(ReadLiveCallState());
}

enum class RecordingCaptureKind {
    Completed,
    HangupFinalized,
    Unavailable,
    EmergencyPreempt,
    ExternalPreempt,
    UnverifiedPreempt,
    Failed,
};

struct RecordingCaptureResult {
    RecordingCaptureKind kind = RecordingCaptureKind::Failed;
    const char* stopReason = nullptr;
};



bool CurrentUtcTimestamp(char* output, size_t outputBytes) {
    if (output == nullptr || outputBytes < 25) return false;
    timespec now {};
    if (clock_gettime(CLOCK_REALTIME, &now) != 0 || now.tv_sec <= 0) return false;
    std::string timestamp;
    if (!ivrdroid::FormatUtcTimestamp(
            static_cast<int64_t>(now.tv_sec),
            static_cast<int32_t>(now.tv_nsec / 1'000'000),
            &timestamp) || timestamp.size() + 1 > outputBytes) {
        return false;
    }
    std::memcpy(output, timestamp.c_str(), timestamp.size() + 1);
    return true;
}

bool WriteRecordingReceipt(
    const std::string& recordingUuid,
    const std::string& callUuid,
    uint64_t revisionId,
    const std::string& blockId,
    uint32_t sequence,
    const char* capturedAt,
    uint32_t durationMilliseconds,
    const char* stopReason,
    uint64_t sizeBytes,
    const std::string& sha256) {
    if (capturedAt == nullptr || capturedAt[0] == '\0') return false;
    char receipt[2048] = {};
    const int length = std::snprintf(
        receipt,
        sizeof(receipt),
        "{\"version\":1,\"recording_id\":\"%s\",\"call_id\":\"%s\","
        "\"revision_id\":%llu,\"block_id\":\"%s\",\"sequence\":%u,"
        "\"captured_at\":\"%s\",\"duration_ms\":%u,\"stop_reason\":\"%s\","
        "\"size_bytes\":%llu,\"sha256\":\"%s\"}\n",
        recordingUuid.c_str(),
        callUuid.c_str(),
        static_cast<unsigned long long>(revisionId),
        blockId.c_str(),
        sequence,
        capturedAt,
        durationMilliseconds,
        stopReason,
        static_cast<unsigned long long>(sizeBytes),
        sha256.c_str());
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(receipt)) return false;
    const std::string temporary =
        std::string(kRecordingInboxDir) + "/." + recordingUuid + ".json.tmp";
    const std::string final =
        std::string(kRecordingInboxDir) + "/" + recordingUuid + ".json";
    unlink(temporary.c_str());
    const int fd = open(
        temporary.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
        0600);
    if (fd < 0) return false;
    const bool written =
        WriteAll(fd, receipt, static_cast<size_t>(length)) &&
        fsync(fd) == 0 &&
        fchown(fd, gAppUid, gAppUid) == 0 &&
        fchmod(fd, 0600) == 0;
    close(fd);
    if (!written || rename(temporary.c_str(), final.c_str()) != 0) {
        unlink(temporary.c_str());
        return false;
    }
    return SyncDirectory(kRecordingInboxDir);
}

bool WriteConversationReceipt(
    const std::string& recordingUuid,
    const std::string& callUuid,
    uint64_t revisionId,
    const std::string& blockId,
    uint32_t segmentIndex,
    const char* capturedAt,
    uint32_t durationMilliseconds,
    const char* stopReason,
    uint64_t sizeBytes,
    const std::string& sha256,
    bool partial) {
    const std::string stem = ivrdroid::ConversationSegmentStem(
        recordingUuid,
        segmentIndex);
    if (stem.empty() || capturedAt == nullptr || capturedAt[0] == '\0' ||
        durationMilliseconds == 0 ||
        durationMilliseconds > ivrdroid::kConversationSegmentMaximumMilliseconds ||
        stopReason == nullptr ||
        !ivrdroid::IsConversationStopReason(stopReason, partial) ||
        !ivrdroid::call_control::IsCanonicalUuid(callUuid) ||
        !ivrdroid::call_control::IsCanonicalUuid(blockId) ||
        sha256.size() != 64) {
        return false;
    }
    char receipt[2304] = {};
    const int length = std::snprintf(
        receipt,
        sizeof(receipt),
        "{\"version\":2,\"kind\":\"conversation\","
        "\"recording_id\":\"%s\",\"call_id\":\"%s\","
        "\"revision_id\":%llu,\"block_id\":\"%s\","
        "\"sequence\":%u,\"segment_index\":%u,"
        "\"captured_at\":\"%s\",\"duration_ms\":%u,"
        "\"stop_reason\":\"%s\",\"size_bytes\":%llu,"
        "\"sha256\":\"%s\",\"partial\":%s}\n",
        recordingUuid.c_str(),
        callUuid.c_str(),
        static_cast<unsigned long long>(revisionId),
        blockId.c_str(),
        segmentIndex,
        segmentIndex,
        capturedAt,
        durationMilliseconds,
        stopReason,
        static_cast<unsigned long long>(sizeBytes),
        sha256.c_str(),
        partial ? "true" : "false");
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(receipt)) return false;
    const std::string temporary =
        std::string(kRecordingInboxDir) + "/." + stem + ".json.tmp";
    const std::string final =
        std::string(kRecordingInboxDir) + "/" + stem + ".json";
    const int fd = open(
        temporary.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
        0600);
    if (fd < 0) return false;
    const bool written =
        WriteAll(fd, receipt, static_cast<size_t>(length)) &&
        fsync(fd) == 0 &&
        fchown(fd, gAppUid, gAppUid) == 0 &&
        fchmod(fd, 0600) == 0;
    close(fd);
    if (!written || rename(temporary.c_str(), final.c_str()) != 0) {
        unlink(temporary.c_str());
        return false;
    }
    return SyncDirectory(kRecordingInboxDir);
}

struct ConversationSegment {
    uint32_t index = 0;
    int fd = -1;
    uint64_t frames = 0;
    std::string stem;
    std::string temporaryPath;
    std::string finalPath;
    std::array<char, 32> capturedAt {};
};

void QueueRecordingFailure(const std::string& callUuid, const std::string& blockUuid, const char* reason) {
    std::string id;
    std::array<char, 32> occurredAt {};
    if (!GenerateRecordingUuid(&id) || !CurrentUtcTimestamp(occurredAt.data(), occurredAt.size())) return;
    const std::string directory = std::string(kBridgeDir) + "/recording-failures";
    const std::string temporary = directory + "/." + id + ".tmp";
    const std::string final = directory + "/" + id + ".json";
    const std::string wire = "{\"call_id\":\"" + callUuid + "\",\"block_id\":\"" + blockUuid +
        "\",\"occurred_at\":\"" + occurredAt.data() + "\",\"reason\":\"" + reason + "\"}";
    auto writer = ivrdroid::AsyncRecordingWriter::Start(temporary, gAppUid,
        [wire](int fd) { return WriteAll(fd, wire.data(), wire.size()) && fsync(fd) == 0; },
        [temporary, final, directory](int, uint64_t, const std::string&, bool captured) {
            return captured && rename(temporary.c_str(), final.c_str()) == 0 && SyncDirectory(directory.c_str());
        });
    if (writer) writer->Stop("completed", true);
}

void SaveConversationFailureEvidence(
    const std::string& callUuid, const std::string& recordingUuid,
    const ConversationSegment& segment, const char* stage, int errorNumber,
    bool finalizer = false) {
    std::array<char, 32> occurredAt {};
    CurrentUtcTimestamp(occurredAt.data(), occurredAt.size());
    char evidence[1024] {};
    std::snprintf(evidence, sizeof(evidence),
        "{\"call_id\":\"%s\",\"recording_id\":\"%s\",\"segment_index\":%u,"
        "\"stage\":\"%s\",\"errno\":%d,\"frames\":%llu,\"elapsed_ms\":%lld,\"occurred_at\":\"%s\"}",
        callUuid.c_str(), recordingUuid.c_str(), segment.index, stage, errorNumber,
        static_cast<unsigned long long>(segment.frames),
        static_cast<long long>(MonotonicMilliseconds()), occurredAt.data());
    // Separate slots preserve the child process's original I/O error when the parent
    // subsequently reports its exit. These files are private and survive logcat rotation.
    const std::string path = std::string(kBridgeDir) +
        (finalizer ? "/conversation_finalizer_failure.json" : "/conversation_capture_failure.json");
    const std::string temporary = path + ".tmp";
    WriteBridgeValue(path.c_str(), temporary.c_str(), evidence, sizeof(evidence));
}

void RemoveConversationSegment(ConversationSegment* segment) {
    if (segment == nullptr) return;
    if (segment->fd >= 0) {
        close(segment->fd);
        segment->fd = -1;
    }
    if (!segment->temporaryPath.empty()) unlink(segment->temporaryPath.c_str());
}

[[maybe_unused]] bool OpenConversationSegment(
    const std::string& recordingUuid,
    uint32_t segmentIndex,
    ConversationSegment* segment) {
    if (segment == nullptr || !ConversationStorageAvailable()) {
        return false;
    }
    ConversationSegment opened;
    opened.index = segmentIndex;
    opened.stem = ivrdroid::ConversationSegmentStem(recordingUuid, segmentIndex);
    if (opened.stem.empty()) return false;
    opened.temporaryPath =
        std::string(kRecordingInboxDir) + "/." + opened.stem + ".wav.partial";
    opened.finalPath =
        std::string(kRecordingInboxDir) + "/" + opened.stem + ".wav";
    if (access(opened.finalPath.c_str(), F_OK) == 0 || errno != ENOENT) {
        return false;
    }
    opened.fd = open(
        opened.temporaryPath.c_str(),
        O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
        0600);
    if (opened.fd < 0 || !WriteWaveHeader(opened.fd, 0) ||
        !CurrentUtcTimestamp(opened.capturedAt.data(), opened.capturedAt.size())) {
        RemoveConversationSegment(&opened);
        return false;
    }
    *segment = std::move(opened);
    return true;
}

[[maybe_unused]] bool WriteConversationFrames(
    ConversationSegment* segment,
    const int16_t* samples,
    uint32_t frames) {
    if (segment == nullptr || segment->fd < 0 || samples == nullptr ||
        frames == 0 ||
        segment->frames > ivrdroid::kConversationSegmentMaximumFrames - frames ||
        !WriteAll(
            segment->fd,
            samples,
            static_cast<size_t>(frames) * 2 * sizeof(int16_t))) {
        return false;
    }
    segment->frames += frames;
    return true;
}

bool FinalizeConversationSegment(
    ConversationSegment* segment,
    const std::string& recordingUuid,
    const std::string& callUuid,
    uint64_t revisionId,
    const std::string& blockId,
    const char* stopReason,
    bool partial) {
    if (segment == nullptr || segment->fd < 0 || segment->frames == 0 ||
        segment->frames > ivrdroid::kConversationSegmentMaximumFrames ||
        !ivrdroid::IsConversationStopReason(stopReason, partial)) {
        RemoveConversationSegment(segment);
        return false;
    }
    const uint64_t dataBytes = segment->frames * 4ULL;
    bool finalized = dataBytes <= UINT32_MAX &&
        ftruncate(segment->fd, static_cast<off_t>(44ULL + dataBytes)) == 0 &&
        WriteWaveHeader(segment->fd, static_cast<uint32_t>(dataBytes)) &&
        fsync(segment->fd) == 0 &&
        fchmod(segment->fd, 0600) == 0 &&
        fchown(segment->fd, gAppUid, gAppUid) == 0;
    close(segment->fd);
    segment->fd = -1;
    if (!finalized ||
        rename(segment->temporaryPath.c_str(), segment->finalPath.c_str()) != 0 ||
        !SyncDirectory(kRecordingInboxDir)) {
        SaveConversationFailureEvidence(callUuid, recordingUuid, *segment, "finalize_file", errno, true);
        unlink(segment->temporaryPath.c_str());
        unlink(segment->finalPath.c_str());
        SyncDirectory(kRecordingInboxDir);
        return false;
    }

    struct stat state {};
    std::string sha256;
    const uint32_t durationMilliseconds = static_cast<uint32_t>(
        segment->frames / 48ULL);
    if (durationMilliseconds == 0 ||
        lstat(segment->finalPath.c_str(), &state) != 0 ||
        !S_ISREG(state.st_mode) || state.st_uid != gAppUid ||
        state.st_size <= 44 ||
        !ivrdroid::Sha256File(
            segment->finalPath.c_str(),
            kMaximumRecordingBytes,
            &sha256) ||
        !WriteConversationReceipt(
            recordingUuid,
            callUuid,
            revisionId,
            blockId,
            segment->index,
            segment->capturedAt.data(),
            durationMilliseconds,
            stopReason,
            static_cast<uint64_t>(state.st_size),
            sha256,
            partial)) {
        SaveConversationFailureEvidence(callUuid, recordingUuid, *segment, "finalize_receipt", errno, true);
        unlink(segment->finalPath.c_str());
        const std::string receipt =
            std::string(kRecordingInboxDir) + "/" + segment->stem + ".json";
        const std::string receiptTemporary =
            std::string(kRecordingInboxDir) + "/." + segment->stem + ".json.tmp";
        unlink(receipt.c_str());
        unlink(receiptTemporary.c_str());
        SyncDirectory(kRecordingInboxDir);
        return false;
    }
    return true;
}

struct ConversationFinalizer {
    pid_t pid = -1;
    uint32_t segmentIndex = 0;
};

[[maybe_unused]] bool StartConversationFinalizer(
    ConversationSegment* segment,
    const std::string& recordingUuid,
    const std::string& callUuid,
    uint64_t revisionId,
    const std::string& blockId,
    const char* stopReason,
    bool partial,
    int guardianFd,
    int inheritedSegmentFd,
    std::vector<ConversationFinalizer>* finalizers) {
    if (segment == nullptr || segment->fd < 0 || finalizers == nullptr) return false;
    const pid_t parentPid = getpid();
    const pid_t child = fork();
    if (child < 0) return false;
    if (child == 0) {
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != parentPid) {
            _exit(2);
        }
        close(guardianFd);
        if (inheritedSegmentFd >= 0) close(inheritedSegmentFd);
        const bool finalized = FinalizeConversationSegment(
            segment,
            recordingUuid,
            callUuid,
            revisionId,
            blockId,
            stopReason,
            partial);
        _exit(finalized ? 0 : 1);
    }
    close(segment->fd);
    segment->fd = -1;
    finalizers->push_back({child, segment->index});
    return true;
}

bool CollectConversationFinalizers(
    std::vector<ConversationFinalizer>* finalizers,
    bool waitForAll,
    std::vector<uint32_t>* finalizedSegmentIndexes) {
    if (finalizers == nullptr || finalizedSegmentIndexes == nullptr) return false;
    bool success = true;
    size_t index = 0;
    while (index < finalizers->size()) {
        int status = 0;
        pid_t result = -1;
        do {
            result = waitpid(
                (*finalizers)[index].pid,
                &status,
                waitForAll ? 0 : WNOHANG);
        } while (result < 0 && errno == EINTR);
        if (result == 0) {
            ++index;
            continue;
        }
        if (result < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            success = false;
        } else {
            finalizedSegmentIndexes->push_back(
                (*finalizers)[index].segmentIndex);
        }
        finalizers->erase(finalizers->begin() + static_cast<ptrdiff_t>(index));
    }
    return success;
}

enum class GuardianCallNotice {
    None,
    Hangup,
    Failed,
};

GuardianCallNotice ReadGuardianCallNotice(int guardianFd) {
    pollfd descriptor {guardianFd, POLLIN | POLLHUP | POLLERR, 0};
    const int result = poll(&descriptor, 1, 0);
    if (result < 0) return errno == EINTR
        ? GuardianCallNotice::None
        : GuardianCallNotice::Failed;
    if (result == 0) return GuardianCallNotice::None;
    if ((descriptor.revents & POLLIN) != 0) {
        char message = 0;
        const ssize_t count = recv(guardianFd, &message, 1, MSG_DONTWAIT);
        if (count == 1 && message == kGuardianRecordingHangup) {
            return GuardianCallNotice::Hangup;
        }
        return GuardianCallNotice::Failed;
    }
    return (descriptor.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0
        ? GuardianCallNotice::Failed
        : GuardianCallNotice::None;
}

// A completed private WAV/receipt pair is a durable queue entry. App encryption and
// upload are independent work; neither their latency nor their ACK controls Telecom.
[[maybe_unused]] bool AwaitConversationFinalizers(
    std::vector<ConversationFinalizer>* finalizers,
    int guardianFd) {
    const int64_t deadline = MonotonicMilliseconds() +
        ivrdroid::kRecordingFinalizationTimeoutMilliseconds;
    bool success = true;
    while (!finalizers->empty() && !gStopRequested && MonotonicMilliseconds() <= deadline) {
        if (ReadGuardianCallNotice(guardianFd) == GuardianCallNotice::Failed) return false;
        std::vector<uint32_t> completed;
        if (!CollectConversationFinalizers(finalizers, false, &completed)) success = false;
        if (!finalizers->empty()) usleep(50'000);
    }
    return success && finalizers->empty();
}

void RemoveIncompleteConversationPlaintext(
    const std::string& recordingUuid,
    uint32_t segmentIndex) {
    const std::string stem = ivrdroid::ConversationSegmentStem(
        recordingUuid,
        segmentIndex);
    if (stem.empty()) return;
    const std::string temporaryWave =
        std::string(kRecordingInboxDir) + "/." + stem + ".wav.partial";
    const std::string temporaryReceipt =
        std::string(kRecordingInboxDir) + "/." + stem + ".json.tmp";
    unlink(temporaryWave.c_str());
    unlink(temporaryReceipt.c_str());
    SyncDirectory(kRecordingInboxDir);
}

[[maybe_unused]] void AbortConversationFinalizers(
    std::vector<ConversationFinalizer>* finalizers,
    const std::string& recordingUuid) {
    if (finalizers == nullptr) return;
    for (const ConversationFinalizer& finalizer : *finalizers) {
        if (finalizer.pid > 0) kill(finalizer.pid, SIGKILL);
    }
    for (const ConversationFinalizer& finalizer : *finalizers) {
        if (finalizer.pid <= 0) continue;
        int status = 0;
        while (waitpid(finalizer.pid, &status, 0) < 0 && errno == EINTR) {}
        RemoveIncompleteConversationPlaintext(recordingUuid, finalizer.segmentIndex);
    }
    finalizers->clear();
}

bool CorrelatesCallControlStatus(
    const ivrdroid::call_control::Status& status,
    const ivrdroid::call_control::Request& request) {
    return status.kind != ivrdroid::call_control::StatusKind::Invalid &&
        status.sessionUuid == request.sessionUuid &&
        status.revisionId == request.revisionId &&
        status.blockUuid == request.blockUuid &&
        status.bootUuid == request.bootUuid;
}

enum class ExternalTeardownResult {
    Safe,
    CallerHungUp,
    CleanupTimeout,
    Failed,
};

ExternalTeardownResult AwaitExternalCallTeardown(
    const ivrdroid::call_control::Request& identity,
    uint64_t lastSequence,
    uint64_t lastElapsedMilliseconds,
    ivrdroid::call_control::StatusKind lastKind,
    const std::string& lastReason,
    const ivrdroid::ExternalCallTerminalExpectation& expectation,
    int guardianFd) {
    ivrdroid::ExternalCallTeardownPolicy policy(
        identity.sessionUuid,
        identity.revisionId,
        identity.blockUuid,
        identity.bootUuid,
        lastSequence,
        lastElapsedMilliseconds,
        lastKind,
        lastReason,
        expectation);
    const int64_t deadline =
        MonotonicMilliseconds() + kExternalCleanupMaximumMs;
    while (!gStopRequested && MonotonicMilliseconds() < deadline) {
        if (ReadGuardianCallNotice(guardianFd) == GuardianCallNotice::Failed) {
            return ExternalTeardownResult::Failed;
        }
        ivrdroid::call_control::Status status;
        if (ReadCallControlStatus(&status) &&
            CorrelatesCallControlStatus(status, identity)) {
            using ivrdroid::ExternalCallTeardownDecision;
            const ExternalCallTeardownDecision decision = policy.Observe(status);
            if (decision == ExternalCallTeardownDecision::Heartbeat ||
                decision == ExternalCallTeardownDecision::MatchedTerminal ||
                decision == ExternalCallTeardownDecision::CleanupTimeout) {
                if (!NotifyGuardian(guardianFd, kGuardianOwnedDialing)) {
                    return ExternalTeardownResult::Failed;
                }
            }
            if (decision == ExternalCallTeardownDecision::MatchedTerminal) {
                return status.reason == "CALLER_HANGUP" ? ExternalTeardownResult::CallerHungUp : ExternalTeardownResult::Safe;
            }
            if (decision == ExternalCallTeardownDecision::CleanupTimeout) {
                return ExternalTeardownResult::CleanupTimeout;
            }
            if (decision == ExternalCallTeardownDecision::MismatchedTerminal ||
                decision == ExternalCallTeardownDecision::ProtocolFailure) {
                return ExternalTeardownResult::Failed;
            }
        }
        usleep(50'000);
    }
    return ExternalTeardownResult::Failed;
}

bool ConfirmOriginalCallerSafe(int guardianFd) {
    ivrdroid::PersistentSessionSnapshot snapshot {};
    if (!LoadSnapshot(&snapshot) ||
        ivrdroid::ClassifySnapshotBoot(snapshot, gBootId) !=
            ivrdroid::SnapshotBootRelation::SameBoot ||
        snapshot.callIdentityHash == 0) {
        return false;
    }
    const int64_t startedAt = MonotonicMilliseconds();
    ivrdroid::ExternalCallerSafePolicy policy(
        snapshot.callIdentityHash,
        startedAt,
        kExternalCallerSafeStableMs,
        kExternalCallerSafeMaximumMs);
    while (!gStopRequested) {
        if (ReadGuardianCallNotice(guardianFd) != GuardianCallNotice::None) {
            return false;
        }
        if (!NotifyGuardian(guardianFd, kGuardianOwnedDialing)) {
            return false;
        }
        const LiveCallObservation observation = ReadLiveCallObservation();
        const ivrdroid::ExternalCallerSafeDecision decision = policy.Observe(
            observation.disposition,
            observation.identityHash,
            MonotonicMilliseconds());
        if (decision == ivrdroid::ExternalCallerSafeDecision::Stable) {
            return true;
        }
        if (decision != ivrdroid::ExternalCallerSafeDecision::Waiting) {
            return false;
        }
        usleep(kExternalCallerSafePollUs);
    }
    return false;
}

using ExternalCallExecutionKind = ivrdroid::ExternalCallRuntimeResult;

struct ExternalCallExecutionResult {
    ExternalCallExecutionKind kind = ExternalCallExecutionKind::Failed;
    int64_t conferenceMilliseconds = 0;
};

ExternalCallExecutionResult ExecuteExternalCall(
    const std::string& callUuid,
    uint64_t revisionId,
    const ivrdroid::RevisionInstruction& instruction,
    int guardianFd) {
    if (!ivrdroid::call_control::IsCanonicalUuid(callUuid) ||
        !ivrdroid::call_control::IsCanonicalUuid(instruction.blockId) ||
        !ivrdroid::call_control::IsCanonicalUuid(gBootId)) {
        return {ExternalCallExecutionKind::SystemFailure};
    }

    std::string recordingUuid;
    if (!GenerateRecordingUuid(&recordingUuid)) {
        return {ExternalCallExecutionKind::SystemFailure};
    }
    const int64_t startedAt = MonotonicMilliseconds();
    if (startedAt < 0) {
        return {ExternalCallExecutionKind::SystemFailure};
    }
    const ivrdroid::call_control::Request request =
        ivrdroid::BuildExternalCallDialRequest(
            callUuid,
            revisionId,
            instruction.blockId,
            instruction.externalNumber,
            instruction.answerTimeoutMilliseconds,
            gBootId,
            1,
            static_cast<uint64_t>(startedAt));
    if (request.kind == ivrdroid::call_control::RequestKind::Invalid) {
        return {ExternalCallExecutionKind::SystemFailure};
    }

    ivrdroid::ExternalCallPolicy policy(
        request.sessionUuid,
        request.revisionId,
        request.blockUuid,
        request.bootUuid,
        request.answerTimeoutMilliseconds,
        startedAt);
    uint64_t nextRequestSequence = 2;
    uint64_t lastRequestElapsedMilliseconds =
        static_cast<uint64_t>(startedAt);
    const auto stampRequest = [&](ivrdroid::call_control::Request* message) {
        if (message == nullptr || nextRequestSequence == UINT64_MAX) return false;
        const int64_t now = MonotonicMilliseconds();
        if (now < 0) return false;
        message->sequence = nextRequestSequence++;
        message->elapsedMilliseconds = std::max<uint64_t>(
            lastRequestElapsedMilliseconds,
            static_cast<uint64_t>(now));
        lastRequestElapsedMilliseconds = message->elapsedMilliseconds;
        return true;
    };
    WriteCurrentState(CurrentState::CallingOperator);
    ivrdroid::AuditEvent("external_call", instruction.blockId, "dialing");
    if (!NotifyGuardian(guardianFd, kGuardianOwnedDialing) ||
        !PublishCallControlRequest(request)) {
        return {ExternalCallExecutionKind::Failed};
    }

    pcm_config captureConfig {};
    captureConfig.channels = 2;
    captureConfig.rate = 48'000;
    captureConfig.period_size = kDtmfFrameCount;
    captureConfig.period_count = kDtmfPeriodCount;
    captureConfig.format = PCM_FORMAT_S16_LE;
    ivrdroid::SessionCapture* input = nullptr;
    ConversationSegment segment; // Legacy diagnostic shape; persistence uses a separate writer.
    ivrdroid::ConversationWriter* writer = nullptr;
    std::vector<int16_t> samples(kDtmfFrameCount * 2);
    bool recorderReady = false;
    bool conferenced = false;
    bool cancellationNeeded = false;
    bool teardownNeeded = false;
    bool recordingFailed = false;
    int64_t nativeHeartbeatAt = 0;
    const char* cancellationReason = "HELPER_CANCELLED";
    ExternalCallExecutionKind terminal = ExternalCallExecutionKind::Failed;
    const char* finalStopReason = nullptr;
    bool finalPartial = false;
    int64_t conferenceStartedAt = 0;
    int64_t conferenceMilliseconds = 0;
    auto lastAuditStatus = ivrdroid::call_control::StatusKind::Invalid;

    // Recording is an observer once a conversation is established. A recorder
    // failure may make the recording partial, but cannot change the call outcome.
    const auto stopRecorder = [&](const char* stage) {
        if (!recordingFailed) {
            const int savedErrno = errno;
            Log(ANDROID_LOG_ERROR,
                "Conversation recording failed: call=%s recording=%s segment=%u stage=%s errno=%d frames=%llu",
                callUuid.c_str(), recordingUuid.c_str(), segment.index, stage, savedErrno,
                static_cast<unsigned long long>(segment.frames));
            ivrdroid::AuditEvent("external_call", instruction.blockId,
                std::string("recording_failure:") + stage);
            QueueRecordingFailure(callUuid, instruction.blockId, "RECORDING_FAILURE");
            // The writer owns durable failure evidence. Storage must not block this loop.
        }
        recordingFailed = true;
        finalStopReason = "recording_failure";
        finalPartial = true;
        if (input != nullptr) {
            ivrdroid::CloseSessionCapture(input);
            input = nullptr;
        }
    };

    const auto startRecorder = [&]() {
        // Recording cannot veto an operator connection. Signal the call-control
        // gate even when capture/storage failed; preserve that failure separately.
        input = ivrdroid::OpenSessionCapture(
            gProfile->card,
            gProfile->captureDevice,
            PCM_IN,
            &captureConfig);
        if (input == nullptr || !ivrdroid::SessionCaptureReady(input)) {
            if (input != nullptr) ivrdroid::CloseSessionCapture(input);
            input = nullptr;
            stopRecorder("capture_unavailable");
        }
        if (input != nullptr) {
            const int framesRead = ivrdroid::ReadSessionCapture(input, samples.data(), kDtmfFrameCount);
            if (framesRead != static_cast<int>(kDtmfFrameCount)) stopRecorder("capture_unavailable");
        }
        if (!policy.MarkRecorderReady(MonotonicMilliseconds())) return false;
        ivrdroid::call_control::Request ready = request;
        ready.kind = ivrdroid::call_control::RequestKind::RecorderReady;
        ready.phoneNumber.clear();
        ready.answerTimeoutMilliseconds = 0;
        recorderReady = stampRequest(&ready) && PublishCallControlRequest(ready);
        return recorderReady;
    };

    while (!gStopRequested) {
        const GuardianCallNotice guardianNotice =
            ReadGuardianCallNotice(guardianFd);
        if (guardianNotice == GuardianCallNotice::Failed) {
            terminal = ExternalCallExecutionKind::Failed;
            break;
        }
        if (guardianNotice == GuardianCallNotice::Hangup) {
            terminal = ExternalCallExecutionKind::CallerHangup;
            finalStopReason = conferenced ? "caller_hangup" : nullptr;
            teardownNeeded = true;
            break;
        }

        if (conferenced) {
            const int64_t now = MonotonicMilliseconds();
            const int64_t verifiedAt = gGuardianEvidence ? gGuardianEvidence->verifiedConferenceAt.load(std::memory_order_acquire) : 0;
            if (verifiedAt > 0 && now >= verifiedAt && now - verifiedAt <= 2000) {
                policy.ConfirmIndependentConference(verifiedAt);
                if (now - nativeHeartbeatAt >= 500) {
                    if (!NotifyGuardian(guardianFd, kGuardianOwnedConference)) { terminal = ExternalCallExecutionKind::Failed; break; }
                    nativeHeartbeatAt = now;
                }
            }
        }
        ivrdroid::call_control::Status status;
        ivrdroid::ExternalCallDecision decision =
            ivrdroid::ExternalCallDecision::Duplicate;
        if (ReadCallControlStatus(&status)) {
            decision = policy.Observe(status, MonotonicMilliseconds());
        }
        if (decision != ivrdroid::ExternalCallDecision::Duplicate &&
            decision != ivrdroid::ExternalCallDecision::IgnoredForeign) {
            // Status heartbeats keep the guardian alive; the audit records transitions.
            if (status.kind != lastAuditStatus) {
                ivrdroid::AuditEvent("external_call", instruction.blockId, ivrdroid::call_control::ToString(status.kind));
                lastAuditStatus = status.kind;
            }
            const char phase = policy.stage() == ivrdroid::ExternalCallStage::Conferenced
                ? kGuardianOwnedConference
                : kGuardianOwnedDialing;
            if (decision != ivrdroid::ExternalCallDecision::ControlTimeout &&
                decision != ivrdroid::ExternalCallDecision::ProtocolFailure &&
                !NotifyGuardian(guardianFd, phase)) {
                terminal = ExternalCallExecutionKind::Failed;
                break;
            }
        }

        if (decision == ivrdroid::ExternalCallDecision::StartRecorder &&
            !startRecorder()) {
            cancellationNeeded = true;
            terminal = ExternalCallExecutionKind::SystemFailure;
            break;
        }
        if (decision == ivrdroid::ExternalCallDecision::Merging &&
            !recorderReady) {
            cancellationNeeded = true;
            terminal = ExternalCallExecutionKind::SystemFailure;
            break;
        }
        if (decision == ivrdroid::ExternalCallDecision::Conferenced && !conferenced) {
            if (!NotifyGuardian(guardianFd, kGuardianOwnedConference)) {
                terminal = ExternalCallExecutionKind::Failed;
                break;
            }
            conferenced = true;
            conferenceStartedAt = MonotonicMilliseconds();
            WriteCurrentState(CurrentState::RecordingConversation);
            if (!recorderReady || input == nullptr ||
                !ivrdroid::AllowsConversationPersistenceStart(policy.stage()) ||
                (writer = ivrdroid::StartContinuousConversation(recordingUuid, callUuid, revisionId, instruction.blockId)) == nullptr) {
                stopRecorder("start");
            }
        }
        if (decision == ivrdroid::ExternalCallDecision::Completed) {
            terminal = ExternalCallExecutionKind::Completed;
            finalStopReason = "operator_hangup";
            break;
        }
        if (decision == ivrdroid::ExternalCallDecision::CallerHangup) {
            terminal = ExternalCallExecutionKind::CallerHangup;
            finalStopReason = conferenced ? "caller_hangup" : nullptr;
            break;
        }
        if (decision == ivrdroid::ExternalCallDecision::NotConnected) {
            terminal = ExternalCallExecutionKind::NotConnected;
            break;
        }
        if (decision == ivrdroid::ExternalCallDecision::SystemFailure) {
            terminal = ExternalCallExecutionKind::SystemFailure;
            if (conferenced) {
                finalStopReason = "recording_failure";
                finalPartial = true;
            }
            break;
        }
        if (decision == ivrdroid::ExternalCallDecision::CleanupTimeout) {
            terminal = ExternalCallExecutionKind::CleanupTimeout;
            if (conferenced) {
                finalStopReason = "recording_failure";
                finalPartial = true;
            }
            break;
        }
        if (decision == ivrdroid::ExternalCallDecision::ProtocolFailure ||
            decision == ivrdroid::ExternalCallDecision::ControlTimeout ||
            decision == ivrdroid::ExternalCallDecision::SetupTimeout ||
            decision == ivrdroid::ExternalCallDecision::MergeTimeout) {
            cancellationNeeded = true;
            terminal = ExternalCallExecutionKind::SystemFailure;
            if (conferenced) {
                finalStopReason = "recording_failure";
                finalPartial = true;
            }
            break;
        }
        if (decision == ivrdroid::ExternalCallDecision::AnswerTimeout) {
            cancellationNeeded = true;
            terminal = ExternalCallExecutionKind::NotConnected;
            cancellationReason = "ANSWER_TIMEOUT";
            break;
        }

        const ivrdroid::ExternalCallDecision deadline =
            policy.CheckDeadline(MonotonicMilliseconds());
        if (deadline == ivrdroid::ExternalCallDecision::ControlTimeout ||
            deadline == ivrdroid::ExternalCallDecision::SetupTimeout ||
            deadline == ivrdroid::ExternalCallDecision::MergeTimeout) {
            cancellationNeeded = true;
            terminal = ExternalCallExecutionKind::SystemFailure;
            if (conferenced) {
                finalStopReason = "recording_failure";
                finalPartial = true;
            }
            break;
        }
        if (deadline == ivrdroid::ExternalCallDecision::AnswerTimeout) {
            cancellationNeeded = true;
            terminal = ExternalCallExecutionKind::NotConnected;
            cancellationReason = "ANSWER_TIMEOUT";
            break;
        }

        if (conferenced) {
            if (writer != nullptr && !ivrdroid::ContinuousConversationHealthy(writer)) stopRecorder("writer");
            // Capture and storage run in independent processes; only control work stays here.
            usleep(25'000);
            continue;
        }
        if (input == nullptr) { usleep(25'000); continue; }
        const int framesRead = ivrdroid::ReadSessionCapture(input, samples.data(), kDtmfFrameCount);
        if (framesRead != static_cast<int>(kDtmfFrameCount)) {
            cancellationNeeded = true;
            terminal = ExternalCallExecutionKind::SystemFailure;
            break;
        }

    }

    if (input != nullptr) {
        ivrdroid::CloseSessionCapture(input);
        input = nullptr;
    }
    if (conferenceStartedAt > 0) {
        conferenceMilliseconds = std::max<int64_t>(
            0,
            MonotonicMilliseconds() - conferenceStartedAt);
    }
    if (cancellationNeeded) {
        ivrdroid::call_control::Request cancel = request;
        cancel.kind = ivrdroid::call_control::RequestKind::Cancel;
        cancel.phoneNumber.clear();
        cancel.answerTimeoutMilliseconds = 0;
        cancel.reason = cancellationReason;
        if (!NotifyGuardian(guardianFd, kGuardianOwnedDialing) ||
            !stampRequest(&cancel) ||
            !PublishCallControlRequest(cancel)) {
            terminal = ExternalCallExecutionKind::Failed;
        } else {
            teardownNeeded = true;
        }
    }
    if (teardownNeeded && terminal != ExternalCallExecutionKind::Failed) {
        ivrdroid::ExternalCallTerminalExpectation expectation;
        if (terminal == ExternalCallExecutionKind::NotConnected &&
            std::strcmp(cancellationReason, "ANSWER_TIMEOUT") == 0) {
            expectation = {
                ivrdroid::call_control::StatusKind::NotConnected,
                "ANSWER_TIMEOUT"};
        } else if (terminal == ExternalCallExecutionKind::SystemFailure) {
            expectation = {
                ivrdroid::call_control::StatusKind::SystemFailure,
                "HELPER_CANCELLED"};
        } else if (terminal == ExternalCallExecutionKind::CallerHangup) {
            expectation = {
                ivrdroid::call_control::StatusKind::Completed,
                "CALLER_HANGUP"};
        } else {
            terminal = ExternalCallExecutionKind::Failed;
        }
        if (terminal != ExternalCallExecutionKind::Failed) {
            const ExternalTeardownResult teardown = AwaitExternalCallTeardown(
                request,
                policy.lastSequence(),
                policy.lastElapsedMilliseconds(),
                policy.lastKind(),
                policy.lastReason(),
                expectation,
                guardianFd);
            if (teardown == ExternalTeardownResult::CallerHungUp) {
                terminal = ExternalCallExecutionKind::CallerHangup;
                finalStopReason = conferenced ? "caller_hangup" : nullptr;
            } else if (teardown == ExternalTeardownResult::CleanupTimeout) {
                terminal = ExternalCallExecutionKind::CleanupTimeout;
            } else if (teardown != ExternalTeardownResult::Safe) {
                terminal = ExternalCallExecutionKind::Failed;
            }
        }
    }

    if (ivrdroid::RequiresOriginalCallerSafeConfirmation(terminal) &&
        !ConfirmOriginalCallerSafe(guardianFd)) {
        // The operator may disconnect first and the caller immediately afterward.
        terminal = ReadLiveCallState() == ivrdroid::CallDisposition::Idle
            ? ExternalCallExecutionKind::CallerHangup : ExternalCallExecutionKind::Failed;
        if (terminal == ExternalCallExecutionKind::CallerHangup) finalStopReason = conferenced ? "caller_hangup" : nullptr;
    }

    if (terminal == ExternalCallExecutionKind::CleanupTimeout || terminal == ExternalCallExecutionKind::Failed) {
        ivrdroid::StopContinuousConversation(writer, "interrupted", true);
        return {terminal, conferenceMilliseconds};
    }
    if (recordingFailed) { finalStopReason = "recording_failure"; finalPartial = true; }
    ivrdroid::StopContinuousConversation(writer, finalStopReason ? finalStopReason : "interrupted", finalPartial || !finalStopReason);
    if (terminal == ExternalCallExecutionKind::CallerHangup) {
        return {terminal, conferenceMilliseconds};
    }
    if (!NotifyGuardian(guardianFd, kGuardianIdle)) {
        return {ExternalCallExecutionKind::Failed, conferenceMilliseconds};
    }
    return {terminal, conferenceMilliseconds};
}

RecordingCaptureResult CaptureRecordingMessage(
    const std::string& callUuid,
    uint64_t revisionId,
    const ivrdroid::RevisionInstruction& instruction,
    uint32_t sequence,
    int guardianFd) {
    if (callUuid.empty() ||
        !RecordingStorageAvailable(instruction.maximumDurationMilliseconds)) {
        if (!callUuid.empty()) QueueRecordingFailure(callUuid, instruction.blockId, "STORAGE_UNAVAILABLE");
        return {RecordingCaptureKind::Unavailable, nullptr};
    }
    const PromptResult beep = ProcessRecordingBeep(guardianFd);
    if (beep == PromptResult::RemoteHangup) {
        return {RecordingCaptureKind::HangupFinalized, "caller_hangup"};
    }
    if (beep == PromptResult::EmergencyPreempt) {
        return {RecordingCaptureKind::EmergencyPreempt, nullptr};
    }
    if (beep == PromptResult::ExternalPreempt) {
        return {RecordingCaptureKind::ExternalPreempt, nullptr};
    }
    if (beep != PromptResult::Completed || !HasPrivateSessionRoute()) {
        return {RecordingCaptureKind::Failed, nullptr};
    }

    std::string recordingUuid;
    if (!GenerateRecordingUuid(&recordingUuid)) {
        return {RecordingCaptureKind::Failed, nullptr};
    }
    const std::string temporary =
        std::string(kRecordingInboxDir) + "/." + recordingUuid + ".wav.partial";
    const std::string final =
        std::string(kRecordingInboxDir) + "/" + recordingUuid + ".wav";
    std::array<char, 32> capturedAt {};
    if (!CurrentUtcTimestamp(capturedAt.data(), capturedAt.size())) return {RecordingCaptureKind::Unavailable, nullptr};
    const auto finish = [=](int fd, uint64_t bytes, const std::string& reason, bool captured) {
        // Only immutable identities and this recording's file are available here.
        const uint64_t dataBytes = bytes - bytes % 4;
        bool complete = fd >= 0 && dataBytes > 0 && dataBytes <= UINT32_MAX &&
            ftruncate(fd, 44 + dataBytes) == 0 && WriteWaveHeader(fd, static_cast<uint32_t>(dataBytes)) && fsync(fd) == 0 && captured;
        if (complete) complete = rename(temporary.c_str(), final.c_str()) == 0 && SyncDirectory(kRecordingInboxDir);
        std::string hash;
        if (complete) complete = ivrdroid::Sha256File(final.c_str(), kMaximumRecordingBytes, &hash) &&
            WriteRecordingReceipt(recordingUuid, callUuid, revisionId, instruction.blockId, sequence,
                capturedAt.data(), dataBytes / 192, reason.c_str(), dataBytes + 44, hash);
        if (!complete) {
            std::array<char, 32> occurredAt {}; CurrentUtcTimestamp(occurredAt.data(), occurredAt.size());
            std::ostringstream event;
            event << "{\"call_id\":\"" << callUuid << "\",\"block_id\":\"" << instruction.blockId
                << "\",\"occurred_at\":\"" << occurredAt.data() << "\",\"reason\":\"WRITER_FAILURE\",\"frames\":" << dataBytes / 4 << "}";
            const std::string path = std::string(kBridgeDir) + "/recording-failures/" + recordingUuid + ".json";
            const int evidence = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (evidence >= 0) { const auto wire = event.str(); WriteAll(evidence, wire.data(), wire.size()); fsync(evidence); close(evidence); }
            // Keep the prefix and receipt evidence for recovery. Never publish a successful voicemail.
        }
        return complete;
    };
    auto writer = ivrdroid::AsyncRecordingWriter::Start(temporary, gAppUid,
        [](int fd) { return WriteWaveHeader(fd, 0); }, finish);
    if (!writer) {
        QueueRecordingFailure(callUuid, instruction.blockId, "WRITER_UNAVAILABLE");
        return {RecordingCaptureKind::Unavailable, nullptr};
    }

    pcm_config config {};
    config.channels = 2;
    config.rate = 48'000;
    config.period_size = kDtmfFrameCount;
    config.period_count = kDtmfPeriodCount;
    config.format = PCM_FORMAT_S16_LE;
    ivrdroid::SessionCapture* input = ivrdroid::OpenSessionCapture(gProfile->card, gProfile->captureDevice, PCM_IN, &config);
    if (input == nullptr || !ivrdroid::SessionCaptureReady(input)) {
        if (input != nullptr) ivrdroid::CloseSessionCapture(input);
        return {RecordingCaptureKind::Unavailable, nullptr};
    }
    if (!NotifyGuardian(guardianFd, kGuardianRecording)) {
        ivrdroid::CloseSessionCapture(input); return {RecordingCaptureKind::Failed, nullptr};
    }

    WriteCurrentState(CurrentState::RecordingMessage);
    const uint64_t maximumFrames =
        static_cast<uint64_t>(instruction.maximumDurationMilliseconds) * 48ULL;
    std::vector<int16_t> samples(kDtmfFrameCount * 2);
    std::deque<std::vector<int16_t>> pending;
    ivrdroid::StereoDtmfDetector detector(48'000);
    uint64_t capturedFrames = 0;
    RecordingCaptureKind result = RecordingCaptureKind::Completed;
    const char* stopReason = "maximum_duration";
    bool writeOk = true;

    const auto writeSamples = [&](const std::vector<int16_t>& frame) {
        if (!writer->Append(frame.data(), frame.size() * sizeof(int16_t))) {
            return false;
        }
        return true;
    };
    while (capturedFrames < maximumFrames && !gStopRequested) {
        const auto notice = ReadGuardianCallNotice(guardianFd);
        if (notice == GuardianCallNotice::Hangup) { result = RecordingCaptureKind::HangupFinalized; stopReason = "caller_hangup"; break; }
        if (notice == GuardianCallNotice::Failed) { writeOk = false; break; }
        const int framesRead = ivrdroid::ReadSessionCapture(input, samples.data(), kDtmfFrameCount);
        if (framesRead != static_cast<int>(kDtmfFrameCount)) {
            // Capture can close a moment before the monitor's hangup message.
            if (ReadGuardianCallNotice(guardianFd) == GuardianCallNotice::Hangup ||
                ReadLiveCallState() == ivrdroid::CallDisposition::Idle) {
                result = RecordingCaptureKind::HangupFinalized;
                stopReason = "caller_hangup";
            } else writeOk = false;
            break;
        }
        capturedFrames += kDtmfFrameCount;
        pollfd guardianNotice {guardianFd, POLLIN, 0};
        if (poll(&guardianNotice, 1, 0) > 0 &&
            (guardianNotice.revents & POLLIN) != 0) {
            char message = 0;
            if (recv(guardianFd, &message, 1, MSG_DONTWAIT) == 1 &&
                message == kGuardianRecordingHangup) {
                pending.push_back(samples);
                result = RecordingCaptureKind::HangupFinalized;
                stopReason = "caller_hangup";
                break;
            }
        }
        char digit = 0;
        if (instruction.finishKey != 0) {
            digit = detector.ProcessFrame(samples.data(), kDtmfFrameCount);
            pending.push_back(samples);
            if (ivrdroid::DecideRecordingStop(
                    instruction.finishKey,
                    digit,
                    capturedFrames,
                    maximumFrames,
                    ivrdroid::RecordingCallEvent::Active) ==
                ivrdroid::RecordingStopDecision::FinishKey) {
                ivrdroid::AuditEvent("digit", instruction.blockId, std::string(1, digit));
                pending.clear();
                stopReason = "finish_key";
                break;
            }
            if (pending.size() > kRecordingTrimFrames) {
                writeOk = writeSamples(pending.front());
                pending.pop_front();
            }
        } else {
            writeOk = writeSamples(samples);
        }
        if (!writeOk) break;

        const uint64_t frameIndex = capturedFrames / kDtmfFrameCount;
        if (frameIndex % kRecordingHeartbeatFrames == 0 &&
            (!HasPrivateSessionRoute() ||
             !NotifyGuardian(guardianFd, kGuardianRecording))) {
            writeOk = false;
            break;
        }
        if (frameIndex % kRecordingCallCheckFrames == 0 && !IsAudioInCall()) {
            const ivrdroid::CallDisposition disposition = ReadLiveCallState();
            if (disposition == ivrdroid::CallDisposition::Idle) {
                result = RecordingCaptureKind::HangupFinalized;
                stopReason = "caller_hangup";
            } else if (disposition == ivrdroid::CallDisposition::Emergency) {
                result = RecordingCaptureKind::EmergencyPreempt;
            } else if (disposition == ivrdroid::CallDisposition::Multiple) {
                result = RecordingCaptureKind::ExternalPreempt;
            } else {
                result = RecordingCaptureKind::UnverifiedPreempt;
            }
            break;
        }
    }
    ivrdroid::CloseSessionCapture(input);

    if (result == RecordingCaptureKind::Completed || result == RecordingCaptureKind::HangupFinalized) {
        while (writeOk && !pending.empty()) { writeOk = writeSamples(pending.front()); pending.pop_front(); }
    }
    writer->Stop(stopReason, !gStopRequested && writeOk &&
        (result == RecordingCaptureKind::Completed || result == RecordingCaptureKind::HangupFinalized));
    // A caller who hung up needs no finalization wait. This child owns no audio fd.
    if (result != RecordingCaptureKind::Completed) return {result, stopReason};
    if (!writeOk) return {RecordingCaptureKind::Unavailable, nullptr};
    int64_t heartbeat = 0;
    while (!writer->done() && !gStopRequested) {
        const auto notice = ReadGuardianCallNotice(guardianFd);
        if (notice == GuardianCallNotice::Hangup) return {RecordingCaptureKind::HangupFinalized, "caller_hangup"};
        if (notice == GuardianCallNotice::Failed) return {RecordingCaptureKind::Failed, nullptr};
        const int64_t now = MonotonicMilliseconds();
        if (now - heartbeat >= 250) {
            if (!NotifyGuardian(guardianFd, kGuardianRecording)) return {RecordingCaptureKind::Failed, nullptr};
            heartbeat = now;
        }
        usleep(25'000);
    }
    return writer->failed() || gStopRequested ? RecordingCaptureResult {RecordingCaptureKind::Unavailable, nullptr}
        : RecordingCaptureResult {RecordingCaptureKind::Completed, stopReason};
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
        snapshot,
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
    if (clock_gettime(CLOCK_BOOTTIME, &value) != 0) return 0;
    return static_cast<int64_t>(value.tv_sec) * 1000 +
        static_cast<int64_t>(value.tv_nsec / 1'000'000);
}

int GuardianPhaseTimeout(char phase) {
    switch (phase) {
        case kGuardianPrompt:
            return kGuardianPromptMs;
        case kGuardianDtmf:
            return kGuardianDtmfMs;
        case kGuardianRecording:
            return kGuardianRecordingHeartbeatMs;
        case kGuardianOwnedDialing:
        case kGuardianOwnedConference:
            return static_cast<int>(
                ivrdroid::call_control::kHeartbeatMaximumAgeMilliseconds);
        case kGuardianRecordingFinalize:
            return kGuardianRecordingFinalizeMs;
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
    // Revoked admission must yield to Android, never hang up a caller the app
    // deliberately stopped trying to answer (kill switch or manual answering).
    if (AdmissionCancelled()) return ReleaseSessionForPreemption(LastResult::ExternalCallPreempted);
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
    uint64_t answeredElapsedMs;
    uint64_t observedElapsedMs;
    char nativeSnapshot[2048];
};

static_assert(sizeof(CallMonitorEvent) <= 4096);

[[noreturn]] void CallEvidencePublisher(int fd, pid_t observerPid) {
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != observerPid) _exit(1);
    ivrdroid::CallLifetime lifetime {gSessionCallUuid, gBootId, gCallLifetimePolicy, 0, 0};
    std::string publishedLifetime;
    while (true) {
        CallMonitorEvent latest {};
        ssize_t count;
        do { count = recv(fd, &latest, sizeof(latest), 0); } while (count < 0 && errno == EINTR);
        if (count != static_cast<ssize_t>(sizeof(latest))) _exit(0);
        // Disk may have stalled. Publish the latest observation, never a queue of
        // obsolete snapshots. The observer and guardian do not wait for this process.
        CallMonitorEvent next {};
        while (recv(fd, &next, sizeof(next), MSG_DONTWAIT) == static_cast<ssize_t>(sizeof(next))) latest = next;
        const std::string native(latest.nativeSnapshot, strnlen(latest.nativeSnapshot, sizeof(latest.nativeSnapshot)));
        if (!native.empty() && native.back() == '\n') {
            WriteBridgeWire((std::string(kBridgeDir) + "/native-calls").c_str(),
                (std::string(kBridgeDir) + "/.native-calls.tmp").c_str(), native, 4096);
        }
        if (latest.answeredElapsedMs != 0) lifetime.Answer(latest.answeredElapsedMs);
        const std::string wire = ivrdroid::FormatCallLifetime(lifetime);
        if (lifetime.answeredElapsedMs != 0 && wire != publishedLifetime &&
            WriteBridgeWire((std::string(kBridgeDir) + "/call-lifetime").c_str(),
                (std::string(kBridgeDir) + "/.call-lifetime.tmp").c_str(), wire)) publishedLifetime = wire;
    }
}

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

    int publication[2] {-1, -1};
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, publication) != 0) _exit(1);
    const pid_t observerPid = getpid();
    const pid_t publisher = fork();
    if (publisher < 0) _exit(1);
    if (publisher == 0) {
        close(eventFd); close(publication[0]);
        CallEvidencePublisher(publication[1], observerPid);
    }
    close(publication[1]);

    ivrdroid::CallLifetime lifetime {gSessionCallUuid, gBootId, gCallLifetimePolicy, 0, 0};
    while (true) {
        const LiveCallObservation observation =
            ReadLiveCallObservation();
        for (const auto& call : observation.snapshot.calls) {
            if (call.id == gOriginalNativeCaller && (call.state == "ACTIVE" || call.state == "ANSWERED" || call.state == "ON_HOLD")) {
                lifetime.Answer(static_cast<uint64_t>(MonotonicMilliseconds()));
            }
        }
        CallMonitorEvent event {
            observation.identityHash,
            static_cast<uint8_t>(observation.disposition),
            {},
            lifetime.answeredElapsedMs,
            static_cast<uint64_t>(MonotonicMilliseconds()),
            {},
        };
        const std::string native = ivrdroid::FormatNativeCallSnapshot(observation.snapshot, gBootId,
            gSessionCallUuid, event.observedElapsedMs, ++gNativeSnapshotSequence);
        if (native.size() < sizeof(event.nativeSnapshot)) std::memcpy(event.nativeSnapshot, native.c_str(), native.size() + 1);
        const ssize_t sent = send(
            eventFd,
            &event,
            sizeof(event),
            MSG_NOSIGNAL);
        if (sent != static_cast<ssize_t>(sizeof(event))) {
            close(eventFd);
            _exit(0);
        }
        // A full queue only delays app recovery readiness. It cannot delay call
        // observation, deadline enforcement or the guardian's direct IPC evidence.
        send(publication[0], &event, sizeof(event), MSG_NOSIGNAL | MSG_DONTWAIT);
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

[[noreturn]] void DrainReleasedGuardian(int controlFd, bool safe) {
    safe = ivrdroid::ReleaseSessionCaptureWorker() && safe;
    if (safe) {
        const std::string path = std::string(kBridgeDir) + "/call-results/" + gSessionCallUuid + ".released";
        const std::string wire = "RELEASE1 " + gSessionCallUuid + " " + gBootId + " " +
            std::to_string(MonotonicMilliseconds()) + "\n";
        // Failure to persist an ack leaves recovery conservative.
        safe = WriteBridgeWire(path.c_str(), (path + ".tmp").c_str(), wire);
        if (safe) Log(ANDROID_LOG_INFO, "Local resources released: %s", wire.c_str());
    }
    const char ack = safe ? 'R' : 'E';
    send(controlFd, &ack, 1, MSG_NOSIGNAL);
    close(controlFd);
    // From here this process owns only the OLD recording mappings and children.
    // It must not restore mixers, publish global helper state, or scan new files.
    ivrdroid::DrainSessionAudioWorkers();
    _exit(safe ? 0 : 1);
}

void ScheduleAuditRecovery() {
    if (gAuditRecoveryPid > 0) {
        if (waitpid(gAuditRecoveryPid, nullptr, WNOHANG) == 0) return;
        gAuditRecoveryPid = -1;
    }
    const pid_t child = fork();
    if (child == 0) {
        if (gHelperLockFd >= 0) close(gHelperLockFd);
        ivrdroid::RecoverAuditRecordings(gAppUid);
        _exit(0);
    }
    if (child > 0) gAuditRecoveryPid = child;
}

[[noreturn]] void PreemptAndExitGuardian(
    int controlFd,
    pid_t workerPid,
    MixerRoute* privacyRoute,
    CallMonitor* callMonitor,
    LastResult result,
    bool stopWorker) {
    ivrdroid::StopSessionAudio("preempted");
    if (stopWorker) kill(workerPid, SIGKILL);
    StopCallMonitor(callMonitor);
    CloseMixerRoute(privacyRoute);
    const bool captureReleased = ivrdroid::ReleaseSessionCaptureWorker();
    const bool released = ReleaseSessionForPreemption(result) && captureReleased;
    DrainReleasedGuardian(controlFd, released);
}

[[noreturn]] void RecoverAndExitGuardian(
    int controlFd,
    pid_t workerPid,
    MixerRoute* privacyRoute,
    CallMonitor* callMonitor,
    LastResult failureResult,
    bool stopWorker) {
    ivrdroid::StopSessionAudio(failureResult == LastResult::RemoteHangup ? "caller_hangup" :
        failureResult == LastResult::FailedCapture ? "capture_failure" : "interrupted");
    if (stopWorker) kill(workerPid, SIGKILL);
    StopCallMonitor(callMonitor);
    CloseMixerRoute(privacyRoute);
    const bool captureReleased = ivrdroid::ReleaseSessionCaptureWorker();
    const bool recovered = RecoverAndEndFailedSession(failureResult) && captureReleased;
    DrainReleasedGuardian(controlFd, recovered);
}

EndCallResult EndOwnedCallsAndWait(const ivrdroid::OwnedCallTopology& owned) {
    const int64_t deadline = MonotonicMilliseconds() + 8000;
    unsigned int attempts = 0;
    int64_t lastAttempt = 0;
    while (MonotonicMilliseconds() < deadline) {
        const auto observation = ReadLiveCallObservation();
        if (observation.disposition == ivrdroid::CallDisposition::Emergency) return EndCallResult::EmergencyPreempt;
        if (observation.disposition == ivrdroid::CallDisposition::Unknown) return EndCallResult::UnverifiedPreempt;
        if (observation.disposition == ivrdroid::CallDisposition::Idle) return EndCallResult::Ended;
        if (!ivrdroid::ContainsOnlyOwnedCalls(observation.snapshot, owned)) return EndCallResult::ExternalPreempt;
        const int64_t now = MonotonicMilliseconds();
        if (attempts < 4 && now - lastAttempt >= 1000) {
            ++attempts; lastAttempt = now;
            if (!SendFixedTelecomEndCall()) return EndCallResult::Failed;
        }
        usleep(100000);
    }
    return EndCallResult::Failed;
}

[[noreturn]] void EndOwnedSessionGuardian(int controlFd, pid_t workerPid, MixerRoute* route,
    CallMonitor* monitor, const ivrdroid::OwnedCallTopology& owned, LastResult result) {
    const char* reason = result == LastResult::MaxCallDuration ? "max_call_duration" : "caller_hangup";
    ivrdroid::StopSessionAudio(reason);
    // Stop IVR execution immediately: this path must never play a fallback or warning.
    kill(workerPid, SIGSTOP);
    const EndCallResult ended = EndOwnedCallsAndWait(owned);
    kill(workerPid, SIGKILL);
    StopCallMonitor(monitor);
    CloseMixerRoute(route);
    bool resourcesReleased = ivrdroid::ReleaseSessionCaptureWorker();
    if (ended == EndCallResult::Ended) {
        const auto restored = RecoverMixerSnapshot(MixerRestoreTarget::AuditedPostCall);
        resourcesReleased = resourcesReleased && restored != MixerRecoveryResult::Failed;
        WriteLastResult(restored == MixerRecoveryResult::Failed ? LastResult::FailedRestore : result);
        WriteCurrentState(restored == MixerRecoveryResult::Failed ? CurrentState::Error : CurrentState::Recovering);
    } else {
        ReleaseSessionForPreemption(ended == EndCallResult::EmergencyPreempt ? LastResult::EmergencyPreempted :
            ended == EndCallResult::ExternalPreempt ? LastResult::ExternalCallPreempted : LastResult::UnverifiedCallPreempted);
    }
    DrainReleasedGuardian(controlFd, resourcesReleased && ended == EndCallResult::Ended);
}

void GuardianProcess(int controlFd, pid_t workerPid) {
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    ivrdroid::ExternalCallGuardianBudget totalBudget(
        MonotonicMilliseconds(),
        kGuardianTotalMs);
    int64_t phaseDeadline =
        MonotonicMilliseconds() + kGuardianWaitForCallMs;
    MixerRoute privacyRoute {};
    bool privacyEnforcementActive = false;
    bool privacyArmedSent = false;
    bool privacyReadySent = false;
    bool recordingHangupRequested = false;
    unsigned int correctionCount = 0;
    unsigned int consecutiveContendedSamples = 0;
    char currentPhase = 0;
    uint64_t expectedCallIdentityHash = 0;
    ivrdroid::CallLifetime lifetime {gSessionCallUuid, gBootId, gCallLifetimePolicy, 0, 0};
    ivrdroid::OwnedCallTopology owned {gOriginalNativeCaller, {}, {}};
    bool verifiedConference = false;
    int64_t topologyChangedAt = -1;
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
        ivrdroid::SuperviseSessionAudioWorkers();
        const int64_t now = MonotonicMilliseconds();
        if (lifetime.Expired(gBootId, now)) {
            EndOwnedSessionGuardian(controlFd, workerPid, &privacyRoute, &callMonitor, owned, LastResult::MaxCallDuration);
        }
        const int64_t nextDeadline = std::min(
            totalBudget.deadlineMilliseconds(),
            phaseDeadline);
        if (now >= nextDeadline) {
            Log(ANDROID_LOG_ERROR, "Session guardian deadline expired.");
            RecoverAndExitGuardian(
                controlFd,
                workerPid,
                &privacyRoute,
                &callMonitor,
                LastResult::RecoveredAndEnded,
                true);
        }
        const int64_t nextWake = privacyEnforcementActive
            ? std::min(
                nextDeadline,
                now + static_cast<int64_t>(kPrivacyEnforcementPollMs))
            : std::min(nextDeadline, now + 250);
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
                const char previousPhase = currentPhase;
                currentPhase = phase;
                const int64_t phaseNow = MonotonicMilliseconds();
                phaseDeadline = phaseNow + GuardianPhaseTimeout(phase);
                if (phase == kGuardianOwnedConference &&
                    previousPhase != kGuardianOwnedConference) {
                    totalBudget.EnterConference(phaseNow);
                } else if (previousPhase == kGuardianOwnedConference &&
                           phase != kGuardianOwnedConference) {
                    totalBudget.LeaveConference(phaseNow);
                    verifiedConference = false;
                    topologyChangedAt = -1;
                    if (gGuardianEvidence) gGuardianEvidence->verifiedConferenceAt.store(0, std::memory_order_release);
                }
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
            if (event.answeredElapsedMs > 0 && event.answeredElapsedMs <= static_cast<uint64_t>(MonotonicMilliseconds())) lifetime.Answer(event.answeredElapsedMs);
            ivrdroid::TelecomCallSnapshot native {false, 0, false, {}, {}};
            RecoveryOwnership recovery;
            const bool nativeValid = ivrdroid::ParseNativeCallSnapshot(event.nativeSnapshot, gBootId,
                gSessionCallUuid, MonotonicMilliseconds(), &native);
            if (nativeValid && ReadRecoveryOwnership(&recovery) && OwnershipAgrees(recovery, native)) {
                if (recovery.session == gSessionCallUuid) owned = recovery.calls;
                AcknowledgeRecovery(recovery);
            }
            if (nativeValid && currentPhase == kGuardianOwnedConference) {
                ivrdroid::ResolveOwnedConference(native, &owned);
            }
            if (nativeValid && ivrdroid::MatchesOwnedConference(native, owned)) {
                verifiedConference = true; topologyChangedAt = -1;
            } else if (verifiedConference && nativeValid && ivrdroid::ContainsOnlyOwnedCalls(native, owned)) {
                if (topologyChangedAt < 0) topologyChangedAt = MonotonicMilliseconds();
                ivrdroid::call_control::Status controller;
                const bool absent = !ReadCallControlStatus(&controller) || controller.sessionUuid != gSessionCallUuid ||
                    controller.elapsedMilliseconds + 3000 < static_cast<uint64_t>(MonotonicMilliseconds());
                const auto live = [&](const std::string& id) { return std::any_of(native.calls.begin(), native.calls.end(), [&](const auto& c) { return c.id == id && ivrdroid::IsLiveTelecomState(c.state); }); };
                if (absent && MonotonicMilliseconds() - topologyChangedAt >= 500 && (!live(owned.caller) || !live(owned.operatorCall))) {
                    EndOwnedSessionGuardian(controlFd, workerPid, &privacyRoute, &callMonitor, owned,
                        !live(owned.caller) ? LastResult::RemoteHangup : LastResult::RecoveredOperatorHangup);
                }
            }
            if (verifiedConference && nativeValid && native.liveCallCount > 0 &&
                !ivrdroid::ContainsOnlyOwnedCalls(native, owned)) {
                // A new call or an emergency invalidates conference ownership.
                // Release IVR resources without using a global hangup command.
                PreemptAndExitGuardian(controlFd, workerPid, &privacyRoute, &callMonitor,
                    native.emergencyCallPresent ? LastResult::EmergencyPreempted : LastResult::UnverifiedCallPreempted, true);
            }
            if (verifiedConference && nativeValid && ivrdroid::ContainsOnlyOwnedCalls(native, owned) &&
                currentPhase == kGuardianOwnedConference) {
                if (gGuardianEvidence) gGuardianEvidence->verifiedConferenceAt.store(event.observedElapsedMs, std::memory_order_release);
                // Established ownership, native observation and the original call
                // limit remain authoritative while app/control storage is stalled.
                phaseDeadline = MonotonicMilliseconds() + ivrdroid::call_control::kHeartbeatMaximumAgeMilliseconds;
            }
            const bool ownedCallControl =
                currentPhase == kGuardianOwnedDialing ||
                currentPhase == kGuardianOwnedConference;
            if (!ownedCallControl && disposition ==
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
            ivrdroid::SessionCallDecision callDecision =
                ivrdroid::SessionCallDecision::Continue;
            if (ownedCallControl &&
                (disposition == ivrdroid::CallDisposition::Multiple ||
                 disposition == ivrdroid::CallDisposition::SingleSafe)) {
                callPolicy.Start(MonotonicMilliseconds());
            } else {
                callDecision = callPolicy.Observe(
                    disposition,
                    endingCall,
                    MonotonicMilliseconds());
            }
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
                ivrdroid::StopSessionAudio("caller_hangup");
                if (phase != kGuardianRemoteHangup && !recordingHangupRequested) {
                    // Give every healthy control phase a chance to observe ordinary
                    // hangup. Only stuck call-control work reaches the watchdog.
                    if (!NotifyGuardian(controlFd, kGuardianRecordingHangup)) {
                        RecoverAndExitGuardian(controlFd, workerPid, &privacyRoute, &callMonitor,
                            LastResult::RemoteHangup, true);
                    }
                    recordingHangupRequested = true;
                    phaseDeadline = MonotonicMilliseconds() + 5'000;
                }
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
            ivrdroid::StopSessionAudio("session_complete");
            StopCallMonitor(&callMonitor);
            CloseMixerRoute(&privacyRoute);
            const bool captureReleased = ivrdroid::ReleaseSessionCaptureWorker();
            const bool restored = access(kSnapshotPath, F_OK) == 0 && RestorePrivateSession() && captureReleased;
            WriteLastResult(restored ? LastResult::SessionComplete : LastResult::FailedRestore);
            DrainReleasedGuardian(controlFd, restored);
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
    if (gGuardianEvidence) { munmap(gGuardianEvidence, sizeof(GuardianEvidence)); gGuardianEvidence = nullptr; }
    void* evidence = mmap(nullptr, sizeof(GuardianEvidence), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (evidence == MAP_FAILED) return false;
    gGuardianEvidence = new (evidence) GuardianEvidence();
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
        if (gHelperLockFd >= 0) close(gHelperLockFd);
        close(descriptors[1]);
        ivrdroid::SpawnSessionAudioWorkers(gDrainingGuardians.size() < 8);
        GuardianProcess(descriptors[0], workerPid);
    }

    close(descriptors[0]);
    guardian->controlFd = descriptors[1];
    guardian->pid = child;
    return true;
}

bool FinishSessionGuardian(SessionGuardian* guardian, char result) {
    const bool notified = NotifyGuardian(guardian->controlFd, result);
    char ack = 0;
    bool released = false;
    const int64_t deadline = MonotonicMilliseconds() + kRecoveryTotalMaximumMs + 2000;
    while (notified && MonotonicMilliseconds() < deadline) {
        pollfd fd {guardian->controlFd, POLLIN, 0};
        const int waited = poll(&fd, 1, static_cast<int>(deadline - MonotonicMilliseconds()));
        if (waited < 0 && errno == EINTR) continue;
        if (waited <= 0 || recv(fd.fd, &ack, 1, 0) != 1) break;
        if (ack == kGuardianRecordingHangup) continue; // An earlier hangup notice is not the release ack.
        released = ack == 'R'; break;
    }
    close(guardian->controlFd); guardian->controlFd = -1;
    if (released) {
        gDrainingGuardians.push_back(guardian->pid);
    } else {
        // Do not admit another caller when the resource-release proof is absent.
        int status = 0;
        const int64_t deadline = MonotonicMilliseconds() + 2000;
        while (waitpid(guardian->pid, &status, WNOHANG) == 0 && MonotonicMilliseconds() < deadline) usleep(2000);
        if (waitpid(guardian->pid, &status, WNOHANG) == 0) kill(guardian->pid, SIGKILL);
    }
    guardian->pid = -1;
    return released;
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

SessionOutcome PromptOutcome(PromptResult result) {
    if (result == PromptResult::RemoteHangup) return SessionOutcome::RemoteHangup;
    if (result == PromptResult::EmergencyPreempt) return SessionOutcome::EmergencyPreempt;
    if (result == PromptResult::ExternalPreempt) return SessionOutcome::ExternalPreempt;
    return SessionOutcome::AudioFailure;
}

bool ResolveConfiguredPromptPath(
    const ivrdroid::RevisionConfig& config,
    uint64_t revisionId,
    const std::string& promptId,
    std::string* path) {
    if (path == nullptr) return false;
    const ivrdroid::PromptAsset* prompt = config.FindPrompt(promptId);
    if (prompt == nullptr) return false;
    char value[640] = {};
    const int length = std::snprintf(
        value,
        sizeof(value),
        "%s/%llu/prompts/%s.wav",
        kRevisionDir,
        static_cast<unsigned long long>(revisionId),
        prompt->sha256.c_str());
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(value)) return false;
    *path = value;
    return true;
}

PromptResult ProcessConfiguredPrompt(
    const ivrdroid::RevisionConfig& config,
    uint64_t revisionId,
    const std::string& promptId,
    int guardianFd) {
    std::string path;
    if (!ResolveConfiguredPromptPath(config, revisionId, promptId, &path)) {
        return PromptResult::Failed;
    }
    return ProcessPrompt(path.c_str(), CurrentState::PlayingMain, guardianFd);
}

DtmfResult CaptureConfiguredDtmfWithPrompt(
    const ivrdroid::RevisionConfig& config,
    uint64_t revisionId,
    const ivrdroid::RevisionInstruction& instruction,
    int guardianFd) {
    std::string path;
    if (!ResolveConfiguredPromptPath(
            config,
            revisionId,
            instruction.promptId,
            &path)) {
        return {DtmfResultKind::CaptureError, 0};
    }
    return CaptureDtmfDigitWithPrompt(
        path.c_str(),
        instruction.digitBranches,
        guardianFd,
        instruction.timeoutMilliseconds);
}

const char* RuntimeInstructionLabel(ivrdroid::RevisionInstructionType type) {
    switch (type) {
        case ivrdroid::RevisionInstructionType::PlayPrompt:
            return "Play prompt";
        case ivrdroid::RevisionInstructionType::CollectDigit:
            return "Collect one digit";
        case ivrdroid::RevisionInstructionType::ScheduleBranch:
            return "Check schedule";
        case ivrdroid::RevisionInstructionType::ReturnToMenu:
            return "Return to menu";
        case ivrdroid::RevisionInstructionType::RecordMessage:
            return "Record message";
        case ivrdroid::RevisionInstructionType::ExternalCall:
            return "External call";
        case ivrdroid::RevisionInstructionType::EndCall:
            return "End call";
    }
    return "Unknown";
}

void AppendRuntimeTrace(
    std::string* sessionPath,
    unsigned int* traceCount,
    const ivrdroid::RevisionInstruction& instruction,
    const std::string& event = "") {
    const char* type = "prompt";
    switch (instruction.type) {
        case ivrdroid::RevisionInstructionType::PlayPrompt: type = "prompt"; break;
        case ivrdroid::RevisionInstructionType::CollectDigit: type = event == "timeout" ? "timeout" : event == "invalid" ? "invalid" : "digit"; break;
        case ivrdroid::RevisionInstructionType::ScheduleBranch: type = "schedule"; break;
        case ivrdroid::RevisionInstructionType::ReturnToMenu: type = "return"; break;
        case ivrdroid::RevisionInstructionType::RecordMessage: type = "voicemail"; break;
        case ivrdroid::RevisionInstructionType::ExternalCall: type = "external_call"; break;
        case ivrdroid::RevisionInstructionType::EndCall: type = "ended"; break;
    }
    // Playback and DTMF are timestamped at the audio boundary, not again in the trace.
    if (instruction.type != ivrdroid::RevisionInstructionType::PlayPrompt &&
        instruction.type != ivrdroid::RevisionInstructionType::EndCall &&
        (instruction.type != ivrdroid::RevisionInstructionType::CollectDigit || event == "invalid")) {
        ivrdroid::AuditEvent(type, instruction.blockId, event);
    }
    constexpr unsigned int kMaximumTraceSteps = 64;
    if (*traceCount >= kMaximumTraceSteps) return;
    std::string token = instruction.blockId;
    token.push_back('|');
    token.append(RuntimeInstructionLabel(instruction.type));
    if (!event.empty()) {
        token.push_back('|');
        token.append(event);
    }
    const size_t separator = sessionPath->empty() ? 0 : 1;
    if (sessionPath->size() + separator + token.size() + 1 >
        static_cast<size_t>(kMaximumSessionPathBytes)) {
        return;
    }
    if (!sessionPath->empty()) sessionPath->push_back('>');
    sessionPath->append(token);
    ++*traceCount;
    WriteSessionPath(*sessionPath);
}

SessionOutcome RunConfiguredProgramV2(
    const ivrdroid::RevisionConfig& config,
    uint64_t revisionId,
    const std::string& callUuid,
    int guardianFd) {
    uint32_t current = config.entryPc;
    std::string sessionPath;
    unsigned int traceCount = 0;
    std::unordered_map<uint32_t, uint32_t> menuReturns;
    uint32_t recordingSequence = 0;
    const int64_t automatedStartedAt = MonotonicMilliseconds();
    int64_t excludedConferenceMilliseconds = 0;
    const auto automatedSessionExpired = [&]() {
        if (config.schemaVersion < 3 ||
            config.maximumSessionMilliseconds == 0) {
            return false;
        }
        const int64_t now = MonotonicMilliseconds();
        const int64_t elapsed = std::max<int64_t>(
            0,
            now - automatedStartedAt - excludedConferenceMilliseconds);
        return elapsed >= config.maximumSessionMilliseconds;
    };
    constexpr unsigned int kMaximumTransitions = 512;
    for (unsigned int transition = 0;
         transition < kMaximumTransitions && !gStopRequested;
         ++transition) {
        if (automatedSessionExpired()) return SessionOutcome::AudioFailure;
        const ivrdroid::RevisionInstruction* instruction =
            config.FindInstruction(current);
        if (instruction == nullptr) return SessionOutcome::AudioFailure;
        gAuditBlock = instruction->blockId;

        switch (instruction->type) {
            case ivrdroid::RevisionInstructionType::PlayPrompt: {
                AppendRuntimeTrace(&sessionPath, &traceCount, *instruction);
                const PromptResult result = ProcessConfiguredPrompt(
                    config, revisionId, instruction->promptId, guardianFd);
                if (result != PromptResult::Completed) return PromptOutcome(result);
                current = instruction->nextPc;
                break;
            }
            case ivrdroid::RevisionInstructionType::CollectDigit: {
                bool routed = false;
                for (uint32_t attempt = 0;
                     attempt < instruction->maximumAttempts && !gStopRequested;
                     ++attempt) {
                    if (!instruction->promptId.empty() &&
                        !instruction->allowPromptBargeIn) {
                        const PromptResult prompt = ProcessConfiguredPrompt(
                            config, revisionId, instruction->promptId, guardianFd);
                        if (prompt != PromptResult::Completed) return PromptOutcome(prompt);
                    }
                    const DtmfResult input = instruction->allowPromptBargeIn
                        ? CaptureConfiguredDtmfWithPrompt(
                            config,
                            revisionId,
                            *instruction,
                            guardianFd)
                        : CaptureDtmfDigit(
                            guardianFd,
                            instruction->timeoutMilliseconds);
                    if (input.kind == DtmfResultKind::CallEnded) return SessionOutcome::RemoteHangup;
                    if (input.kind == DtmfResultKind::EmergencyPreempt) return SessionOutcome::EmergencyPreempt;
                    if (input.kind == DtmfResultKind::ExternalPreempt) return SessionOutcome::ExternalPreempt;
                    if (input.kind == DtmfResultKind::CaptureError ||
                        input.kind == DtmfResultKind::Stopped) {
                        return SessionOutcome::CaptureFailure;
                    }
                    if (input.kind == DtmfResultKind::Digit) {
                        const auto branch = instruction->digitBranches.find(input.digit);
                        if (branch != instruction->digitBranches.end()) {
                            AppendRuntimeTrace(
                                &sessionPath,
                                &traceCount,
                                *instruction,
                                std::string("digit:") + input.digit);
                            current = branch->second;
                            routed = true;
                            break;
                        }
                        AppendRuntimeTrace(&sessionPath, &traceCount, *instruction, "invalid");
                        if (attempt + 1 == instruction->maximumAttempts) {
                            current = instruction->onInvalidPc;
                            routed = true;
                        }
                    } else {
                        AppendRuntimeTrace(&sessionPath, &traceCount, *instruction, "timeout");
                        if (attempt + 1 == instruction->maximumAttempts) {
                            current = instruction->onTimeoutPc;
                            routed = true;
                        }
                    }
                    if (!routed) WriteCurrentState(CurrentState::RetryingMenu);
                }
                if (!routed) return SessionOutcome::CaptureFailure;
                break;
            }
            case ivrdroid::RevisionInstructionType::ScheduleBranch: {
                bool holiday = false;
                const time_t now = time(nullptr);
                const bool open = now > 0 && config.IsScheduleOpen(
                    instruction->scheduleId,
                    static_cast<int64_t>(now),
                    &holiday);
                AppendRuntimeTrace(
                    &sessionPath,
                    &traceCount,
                    *instruction,
                    holiday ? "holiday" : (open ? "open" : "closed"));
                current = holiday
                    ? instruction->onHolidayPc
                    : (open ? instruction->onOpenPc : instruction->onClosedPc);
                break;
            }
            case ivrdroid::RevisionInstructionType::ReturnToMenu: {
                const ivrdroid::RevisionInstruction* menu =
                    config.FindInstruction(instruction->menuPc);
                if (menu == nullptr ||
                    menu->type != ivrdroid::RevisionInstructionType::CollectDigit) {
                    return SessionOutcome::AudioFailure;
                }
                uint32_t& count = menuReturns[instruction->menuPc];
                if (count < menu->maximumMenuReturns) {
                    ++count;
                    AppendRuntimeTrace(
                        &sessionPath,
                        &traceCount,
                        *instruction,
                        "return:" + std::to_string(count) + "/" +
                            std::to_string(menu->maximumMenuReturns));
                    current = instruction->menuPc;
                } else {
                    AppendRuntimeTrace(
                        &sessionPath,
                        &traceCount,
                        *instruction,
                        "return-limit");
                    current = menu->onReturnLimitPc;
                }
                break;
            }
            case ivrdroid::RevisionInstructionType::RecordMessage: {
                ivrdroid::AuditEvent("voicemail", instruction->blockId, "start");
                const RecordingCaptureResult recording = CaptureRecordingMessage(
                    callUuid,
                    revisionId,
                    *instruction,
                    recordingSequence++,
                    guardianFd);
                if (recording.kind == RecordingCaptureKind::Unavailable) {
                    AppendRuntimeTrace(
                        &sessionPath, &traceCount, *instruction, "unavailable");
                    if (!NotifyGuardian(guardianFd, kGuardianIdle)) {
                        return SessionOutcome::CaptureFailure;
                    }
                    current = instruction->onUnavailablePc;
                    break;
                }
                if (recording.kind == RecordingCaptureKind::Completed) {
                    const std::string event =
                        std::strcmp(recording.stopReason, "finish_key") == 0
                        ? "finish-key"
                        : "maximum";
                    AppendRuntimeTrace(&sessionPath, &traceCount, *instruction, event);
                    if (!NotifyGuardian(guardianFd, kGuardianIdle)) {
                        return SessionOutcome::CaptureFailure;
                    }
                    current = instruction->nextPc;
                    break;
                }
                if (recording.kind == RecordingCaptureKind::HangupFinalized) {
                    AppendRuntimeTrace(&sessionPath, &traceCount, *instruction, "hangup");
                    return SessionOutcome::RemoteHangup;
                }
                if (recording.kind == RecordingCaptureKind::EmergencyPreempt) {
                    return SessionOutcome::EmergencyPreempt;
                }
                if (recording.kind == RecordingCaptureKind::ExternalPreempt) {
                    return SessionOutcome::ExternalPreempt;
                }
                if (recording.kind == RecordingCaptureKind::UnverifiedPreempt) {
                    return SessionOutcome::UnverifiedPreempt;
                }
                return SessionOutcome::CaptureFailure;
            }
            case ivrdroid::RevisionInstructionType::ExternalCall: {
                const ExternalCallExecutionResult external = ExecuteExternalCall(
                    callUuid,
                    revisionId,
                    *instruction,
                    guardianFd);
                if (external.conferenceMilliseconds > 0) {
                    excludedConferenceMilliseconds = std::min<int64_t>(
                        std::numeric_limits<int64_t>::max(),
                        excludedConferenceMilliseconds >
                                std::numeric_limits<int64_t>::max() -
                                    external.conferenceMilliseconds
                            ? std::numeric_limits<int64_t>::max()
                            : excludedConferenceMilliseconds +
                                external.conferenceMilliseconds);
                }
                if (automatedSessionExpired()) {
                    return SessionOutcome::AudioFailure;
                }
                const ivrdroid::ExternalCallRuntimeRoute route =
                    ivrdroid::RouteExternalCallResult(
                        external.kind,
                        instruction->onCompletedPc,
                        instruction->onNotConnectedPc,
                        instruction->onSystemFailurePc);
                if (route.action ==
                    ivrdroid::ExternalCallRuntimeAction::Branch) {
                    const char* event =
                        external.kind == ExternalCallExecutionKind::Completed
                        ? "operator-hangup"
                        : external.kind == ExternalCallExecutionKind::NotConnected
                            ? "not-connected"
                            : "system-failure";
                    AppendRuntimeTrace(
                        &sessionPath,
                        &traceCount,
                        *instruction,
                        event);
                    current = route.programCounter;
                    break;
                }
                if (route.action ==
                    ivrdroid::ExternalCallRuntimeAction::CallerHangup) {
                    AppendRuntimeTrace(
                        &sessionPath,
                        &traceCount,
                        *instruction,
                        "caller-hangup");
                    return SessionOutcome::RemoteHangup;
                }
                return SessionOutcome::CaptureFailure;
            }
            case ivrdroid::RevisionInstructionType::EndCall:
                AppendRuntimeTrace(&sessionPath, &traceCount, *instruction);
                return FinishSuccessfulMenu(guardianFd);
        }
    }
    return SessionOutcome::AudioFailure;
}

SessionOutcome RunConfiguredMenuBody(
    const ivrdroid::RevisionConfig& config,
    uint64_t revisionId,
    int guardianFd) {
    std::string current = config.rootNode;
    std::string sessionPath;
    std::unordered_map<std::string, uint32_t> repeats;
    constexpr unsigned int maximumTransitions = 512;
    for (unsigned int transition = 0;
         transition < maximumTransitions && !gStopRequested;
         ++transition) {
        const ivrdroid::RevisionNode* node = config.FindNode(current);
        if (node == nullptr) return SessionOutcome::AudioFailure;
        if (!sessionPath.empty()) sessionPath.push_back('>');
        if (sessionPath.size() + node->id.size() >
            static_cast<size_t>(kMaximumSessionPathBytes - 2)) {
            return SessionOutcome::AudioFailure;
        }
        sessionPath.append(node->id);
        WriteSessionPath(sessionPath);

        switch (node->type) {
            case ivrdroid::RevisionNodeType::PlayPrompt: {
                const PromptResult result = ProcessConfiguredPrompt(
                    config, revisionId, node->promptId, guardianFd);
                if (result != PromptResult::Completed) return PromptOutcome(result);
                current = node->next;
                break;
            }
            case ivrdroid::RevisionNodeType::CollectDigit: {
                bool routed = false;
                for (uint32_t attempt = 0;
                     attempt < node->maximumAttempts && !gStopRequested;
                     ++attempt) {
                    if (!node->promptId.empty()) {
                        const PromptResult prompt = ProcessConfiguredPrompt(
                            config, revisionId, node->promptId, guardianFd);
                        if (prompt != PromptResult::Completed) return PromptOutcome(prompt);
                    }
                    const DtmfResult input = CaptureDtmfDigit(
                        guardianFd,
                        node->timeoutMilliseconds);
                    if (input.kind == DtmfResultKind::CallEnded) return SessionOutcome::RemoteHangup;
                    if (input.kind == DtmfResultKind::EmergencyPreempt) return SessionOutcome::EmergencyPreempt;
                    if (input.kind == DtmfResultKind::ExternalPreempt) return SessionOutcome::ExternalPreempt;
                    if (input.kind == DtmfResultKind::CaptureError ||
                        input.kind == DtmfResultKind::Stopped) {
                        return SessionOutcome::CaptureFailure;
                    }
                    if (input.kind == DtmfResultKind::Digit) {
                        const auto branch = node->digitBranches.find(input.digit);
                        if (branch != node->digitBranches.end()) {
                            current = branch->second;
                            routed = true;
                            break;
                        }
                        if (attempt + 1 == node->maximumAttempts) {
                            current = node->onInvalid;
                            routed = true;
                        }
                    } else if (attempt + 1 == node->maximumAttempts) {
                        current = node->onTimeout;
                        routed = true;
                    }
                    if (!routed) WriteCurrentState(CurrentState::RetryingMenu);
                }
                if (!routed) return SessionOutcome::CaptureFailure;
                break;
            }
            case ivrdroid::RevisionNodeType::ScheduleBranch: {
                bool holiday = false;
                const time_t now = time(nullptr);
                const bool open = now > 0 && config.IsScheduleOpen(
                    node->scheduleId,
                    static_cast<int64_t>(now),
                    &holiday);
                current = holiday
                    ? node->onHoliday
                    : (open ? node->onOpen : node->onClosed);
                break;
            }
            case ivrdroid::RevisionNodeType::RepeatMenu: {
                uint32_t& count = repeats[node->id];
                if (count < node->maximumRepeats) {
                    ++count;
                    current = node->repeatTarget;
                } else {
                    current = node->onExhausted;
                }
                break;
            }
            case ivrdroid::RevisionNodeType::EndCall:
                return FinishSuccessfulMenu(guardianFd);
        }
    }
    return SessionOutcome::AudioFailure;
}

SessionOutcome RunSelectedMenu(int guardianFd, const std::string& callUuid) {
    ivrdroid::RevisionConfig config;
    uint64_t revisionId = 0;
    const bool configured = LoadRuntimeRevision(&config, &revisionId);
    if (!configured) {
        WriteSessionPath("builtin");
        return RunFixedMenu(guardianFd);
    }
    Log(
        ANDROID_LOG_INFO,
        "Starting immutable revision %llu.",
        static_cast<unsigned long long>(revisionId));
    const PrivacyStartResult privacy = BeginPrivateSession(guardianFd);
    if (privacy == PrivacyStartResult::RemoteHangup) return SessionOutcome::RemoteHangup;
    if (privacy == PrivacyStartResult::EmergencyPreempt) return SessionOutcome::EmergencyPreempt;
    if (privacy == PrivacyStartResult::ExternalPreempt) return SessionOutcome::ExternalPreempt;
    if (privacy != PrivacyStartResult::Started) return SessionOutcome::AudioFailure;
    return config.schemaVersion >= 2
        ? RunConfiguredProgramV2(config, revisionId, callUuid, guardianFd)
        : RunConfiguredMenuBody(config, revisionId, guardianFd);
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
        Log(ANDROID_LOG_WARN, "Discarded an app request queued while busy.");
        WriteLastResult(LastResult::RejectedBusy);
    }
}

bool VerifiedPendingCaller(const ivrdroid::TelecomCallSnapshot& calls, const std::string& expectedSession);

bool ProcessOneCommand() {
    std::string body;
    if (!ConsumeCommand(&body)) return false;
    gSessionCallUuid.clear(); // Rejections/configuration work do not belong to the last completed call.
    const ivrdroid::protocol::CommandRequest request =
        ivrdroid::protocol::ParseCommandRequest(body);
    if (request.command == ivrdroid::protocol::Command::Invalid) {
        Log(ANDROID_LOG_WARN, "Rejected malformed or unknown app request.");
        WriteLastResult(LastResult::RejectedRequest);
        return true;
    }

    if (request.command == ivrdroid::protocol::Command::StageRevision ||
        request.command == ivrdroid::protocol::Command::ActivateStaged) {
        if (ReadLiveCallState() != ivrdroid::CallDisposition::Idle ||
            ReadAudioModeState() != AudioModeState::Normal) {
            Log(ANDROID_LOG_WARN, "Rejected revision operation while call or audio state was busy.");
            WriteLastResult(LastResult::RejectedBusy);
            return true;
        }
        const bool completed =
            request.command == ivrdroid::protocol::Command::StageRevision
            ? StageRevision(request.revisionId, request.manifestSha256)
            : ActivateStagedRevision(request.revisionId);
        WriteLastResult(
            completed
            ? (request.command == ivrdroid::protocol::Command::StageRevision
                ? LastResult::RevisionStaged
                : LastResult::RevisionActivated)
            : LastResult::RevisionRejected);
        if (!completed) {
            Log(ANDROID_LOG_ERROR, "Revision operation %llu was rejected.",
                static_cast<unsigned long long>(request.revisionId));
        }
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

    if (!VerifiedPendingCaller(ReadLiveCallObservation().snapshot, request.callUuid)) {
        Log(ANDROID_LOG_WARN, "START_MENU no longer identifies the verified ringing caller.");
        WriteLastResult(LastResult::RejectedRequest);
        return true;
    }
    Log(ANDROID_LOG_INFO, "Accepted START_MENU from app UID %u.", gAppUid);
    gSessionCallUuid = request.callUuid;
    gOriginalNativeCaller = ReadLiveCallObservation().snapshot.singleCallIdentity;
    gAuditBlock.clear();
    ivrdroid::PrepareSessionAudio(request.callUuid, gAppUid, gProfile->card, gProfile->captureDevice);
    SessionGuardian guardian;
    if (!StartSessionGuardian(&guardian)) {
        ivrdroid::ReleaseSessionAudio();
        Log(ANDROID_LOG_ERROR, "Could not start the session guardian.");
        WriteLastResult(LastResult::FailedAudio);
        WriteCurrentState(CurrentState::Error);
        return true;
    }

    const SessionOutcome outcome = RunSelectedMenu(guardian.controlFd, request.callUuid);
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

    ivrdroid::StopSessionAudio(outcome == SessionOutcome::Complete ? "session_complete" :
        outcome == SessionOutcome::RemoteHangup ? "caller_hangup" :
        outcome == SessionOutcome::CaptureFailure ? "capture_failure" : "interrupted");
    const bool guardianSucceeded =
        FinishSessionGuardian(&guardian, guardianResult);
    ivrdroid::ReleaseSessionAudio();
    ScheduleAuditRecovery();
    DiscardRequestQueuedWhileBusy();
    if (!guardianSucceeded) {
        Log(ANDROID_LOG_ERROR, "Session recovery did not complete cleanly.");
        gStopRequested = 1;
        return true;
    }

    if (outcome == SessionOutcome::Complete ||
        outcome == SessionOutcome::RemoteHangup) {
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

bool VerifiedPendingCaller(const ivrdroid::TelecomCallSnapshot& calls, const std::string& expectedSession) {
    std::string wire;
    if (!ReadOwnedBoundedFile((std::string(kBridgeDir) + "/incoming-caller").c_str(), gAppUid, 512, &wire)) return false;
    std::istringstream in(wire);
    std::string magic, session, boot, caller, extra; int64_t at = 0;
    if (!(in >> magic >> session >> boot >> caller >> at) || (in >> extra) || magic != "INCOMING1" ||
        !ivrdroid::call_control::IsCanonicalUuid(session) || boot != gBootId ||
        (!expectedSession.empty() && session != expectedSession) ||
        at > MonotonicMilliseconds() || MonotonicMilliseconds() - at > 2000) return false;
    return ivrdroid::IsOnlyRingingCaller(calls, caller);
}

bool WaitForSystemReady() {
    ivrdroid::SystemReadinessPolicy policy(kSystemIdleConfirmationMs);
    policy.Start(MonotonicMilliseconds());
    bool published = false;
    CurrentState publishedState = CurrentState::WaitingForSystem;

    while (!gStopRequested) {
        const auto observed = ReadLiveCallObservation();
        const bool pending = VerifiedPendingCaller(observed.snapshot, "");
        const auto native = ivrdroid::FormatNativeCallSnapshot(observed.snapshot, gBootId, "-",
            MonotonicMilliseconds(), ++gNativeSnapshotSequence);
        if (!native.empty()) WriteBridgeWire((std::string(kBridgeDir) + "/native-calls").c_str(),
            (std::string(kBridgeDir) + "/.native-calls.tmp").c_str(), native);
        const AudioModeState audioState = ReadAudioModeState();
        const ivrdroid::SystemReadinessDecision decision = policy.Observe(
            pending ? ivrdroid::CallDisposition::Idle : observed.disposition,
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
    if (!WriteBridgeValue(
            kCapabilitiesBridgePath,
            kCapabilitiesBridgeTempPath,
            ivrdroid::protocol::kCapabilities, 512)) {
        Log(ANDROID_LOG_ERROR, "Could not publish helper capabilities.");
        return 12;
    }
    if (!ValidateAllPromptFiles()) return 13;
    if (!InitializeRevisionState()) return 13;

    const int lockFd = open(kLockPath, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lockFd < 0 || flock(lockFd, LOCK_EX | LOCK_NB) != 0) {
        if (lockFd >= 0) close(lockFd);
        Log(ANDROID_LOG_ERROR, "Another helper instance already owns the lock.");
        return 14;
    }
    gHelperLockFd = lockFd;
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
    ScheduleAuditRecovery();
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
    int64_t lastAuditRecovery = MonotonicMilliseconds();
    int64_t lastIdleObservation = -1000;
    while (!gStopRequested) {
        gDrainingGuardians.erase(std::remove_if(gDrainingGuardians.begin(), gDrainingGuardians.end(),
            [](pid_t child) { return waitpid(child, nullptr, WNOHANG) != 0; }), gDrainingGuardians.end());
        // Publishing into the watched directory wakes inotify itself. Keep native
        // polling bounded while still processing app commands immediately.
        if (MonotonicMilliseconds() - lastIdleObservation >= 500) {
            lastIdleObservation = MonotonicMilliseconds();
            const auto idleObservation = ReadLiveCallObservation();
            const auto native = ivrdroid::FormatNativeCallSnapshot(idleObservation.snapshot, gBootId, "-", MonotonicMilliseconds(), ++gNativeSnapshotSequence);
            if (!native.empty()) WriteBridgeValue((std::string(kBridgeDir) + "/native-calls").c_str(),
                (std::string(kBridgeDir) + "/.native-calls.tmp").c_str(), native.substr(0, native.size() - 1).c_str(), 4096);
            if (idleObservation.disposition == ivrdroid::CallDisposition::Idle && ReadAudioModeState() == AudioModeState::Normal) {
                gSessionCallUuid = "-"; gOriginalNativeCaller.clear();
                RefreshCallSafetyPolicy();
                RecoveryOwnership ownership;
                if (ReadRecoveryOwnership(&ownership)) AcknowledgeRecovery(ownership);
                ivrdroid::RefreshAuditPolicy(gAppUid);
                if (MonotonicMilliseconds() - lastAuditRecovery >= 30000) {
                    ScheduleAuditRecovery();
                    lastAuditRecovery = MonotonicMilliseconds();
                }
            }
        }
        const bool processed = ProcessOneCommand();
        if (gStopRequested) break;
        if (processed && !WaitForSystemReady()) break;

        pollfd descriptor {inotifyFd, POLLIN, 0};
        const int pollResult = poll(&descriptor, 1, 500);
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

    if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("IVRdroid helper %s source=%s\n", kHelperVersion, kSourceCommit);
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--serve") == 0) {
        return Serve();
    }
    if (argc == 2 && std::strcmp(argv[1], "--self-test") == 0) {
        return SelfTest();
    }

    std::fprintf(stderr, "usage: %s --serve | --self-test | --version\n", argv[0]);
    return 2;
}
