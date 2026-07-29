#include <android/log.h>
#include <dtmf_detector.h>
#include <tinyalsa/asoundlib.h>

#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <string>
#include <strings.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr char kLogTag[] = "IVRdroidHelper";

constexpr char kBridgeDir[] =
    "/data/user/0/ai.rx1.ivrdroid/files/bridge";
constexpr char kTriggerPath[] =
    "/data/user/0/ai.rx1.ivrdroid/files/bridge/play_once.request";
constexpr char kCommandPath[] =
    "/data/user/0/ai.rx1.ivrdroid/files/bridge/command.request";
constexpr char kStatusPath[] =
    "/data/user/0/ai.rx1.ivrdroid/files/bridge/status";

constexpr char kStateDir[] = "/data/adb/ivrdroid";
constexpr char kLockPath[] = "/data/adb/ivrdroid/helper.lock";
constexpr char kPidPath[] = "/data/adb/ivrdroid/helper.pid";
constexpr char kSnapshotPath[] = "/data/adb/ivrdroid/mixer.snapshot";
constexpr char kSnapshotTempPath[] = "/data/adb/ivrdroid/.mixer.snapshot.tmp";
constexpr char kPromptPath[] =
    "/data/adb/modules/ivrdroid_helper/prompt.wav";
constexpr char kMainPromptPath[] =
    "/data/adb/modules/ivrdroid_helper/prompts/main-menu.wav";
constexpr char kSalesPromptPath[] =
    "/data/adb/modules/ivrdroid_helper/prompts/sales-unavailable.wav";
constexpr char kSupportPromptPath[] =
    "/data/adb/modules/ivrdroid_helper/prompts/support-unavailable.wav";
constexpr char kOperatorPromptPath[] =
    "/data/adb/modules/ivrdroid_helper/prompts/operator-unavailable.wav";

constexpr char kDoutControl[] = "AudioMixer CH2 DOUT Select";
constexpr char kMixerControl[] = "AudioMixer CH2 Mixer En";
constexpr char kSpeakerControl[] = "SPK Switch";

constexpr char kExpectedDout[] = "AIF4IN";
constexpr char kExpectedMixer[] = "On";
constexpr char kAppliedDout[] = "DMIX_OUT";
constexpr char kAppliedMixer[] = "Off";

constexpr char kRequestBody[] = "PLAY_ONCE\n";
constexpr char kPlayMainCommand[] = "PLAY_MAIN\n";
constexpr char kPlaySalesCommand[] = "PLAY_SALES\n";
constexpr char kPlaySupportCommand[] = "PLAY_SUPPORT\n";
constexpr char kPlayOperatorCommand[] = "PLAY_OPERATOR\n";
constexpr char kListenDtmfCommand[] = "LISTEN_DTMF\n";
constexpr char kStartMenuCommand[] = "START_MENU\n";
constexpr char kServicePath[] = "/system/bin/service";
// Pinned ROM: ITelecomService.Stub.TRANSACTION_endCall.
constexpr char kEndCallTransaction[] = "33";
constexpr char kCallingPackage[] = "ai.rx1.ivrdroid";

constexpr int kCard = 0;
constexpr int kDevice = 0;
constexpr int kCaptureDevice = 0;
constexpr int kCallWaitIterations = 100;
constexpr useconds_t kCallWaitSleepUs = 100'000;
constexpr int kWatchdogTimeoutMs = 15'000;
constexpr off_t kMaximumPromptBytes = 4 * 1024 * 1024;
constexpr off_t kMaximumCommandBytes = 32;
constexpr unsigned int kDtmfSampleRate = 48'000;
constexpr unsigned int kDtmfChannels = 2;
constexpr unsigned int kDtmfFrameCount = 1'200;
constexpr unsigned int kDtmfPeriodCount = 4;
constexpr unsigned int kDtmfTimeoutFrames = 8 * 40;
constexpr unsigned int kDtmfCallCheckFrames = 20;
constexpr int kMenuMaximumRetries = 2;

constexpr uint32_t kSnapshotMagic = 0x49565231U;  // IVR1
constexpr uint32_t kSnapshotVersion = 1;
constexpr uint32_t kSnapshotChecksumSalt = 0xA5C39E71U;

volatile sig_atomic_t gStopRequested = 0;
uid_t gAppUid = 0;

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
    const bool matches =
        strcasecmp(GetProperty("ro.product.manufacturer").c_str(), "Samsung") == 0 &&
        GetProperty("ro.product.model") == "SM-T585" &&
        GetProperty("ro.product.device") == "gtaxllte" &&
        GetProperty("ro.build.version.sdk") == "32" &&
        GetProperty("ro.build.fingerprint") ==
            "google/ryu/dragon:8.1.0/OPM1.171019.016/4503492:user/release-keys" &&
        GetProperty("ro.build.display.id") ==
            "lineage_gtaxllte-userdebug 12 SQ3A.220705.004 "
            "eng.k9100i.20250312.020705";

    if (!matches) {
        Log(ANDROID_LOG_ERROR, "Device identity does not match the audited SM-T585 profile.");
    }
    return matches;
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

bool SyncStateDirectory() {
    const int fd = open(
        kStateDir,
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    const bool synced = fsync(fd) == 0;
    close(fd);
    return synced;
}

bool ResolveAndValidateAppDirectory() {
    struct stat state {};
    if (lstat(kBridgeDir, &state) != 0) return false;
    if (!S_ISDIR(state.st_mode) ||
        state.st_uid < 10'000 ||
        (state.st_mode & 0022) != 0) {
        Log(ANDROID_LOG_ERROR, "App bridge directory ownership or mode is unsafe.");
        return false;
    }
    gAppUid = state.st_uid;
    return true;
}

void WriteStatus(const char* status) {
    if (gAppUid == 0) return;

    struct stat state {};
    if (lstat(kStatusPath, &state) != 0 ||
        !S_ISREG(state.st_mode) ||
        state.st_uid != gAppUid ||
        (state.st_mode & 0022) != 0) {
        return;
    }

    const int fd = open(kStatusPath, O_WRONLY | O_TRUNC | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return;
    WriteAll(fd, status, std::strlen(status));
    WriteAll(fd, "\n", 1);
    fsync(fd);
    close(fd);
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
};

struct MixerRoute {
    mixer* device = nullptr;
    mixer_ctl* dout = nullptr;
    mixer_ctl* mixerEnable = nullptr;
    mixer_ctl* speaker = nullptr;
    int expectedDout = -1;
    int expectedMixer = -1;
    int appliedDout = -1;
    int appliedMixer = -1;
};

void CloseMixerRoute(MixerRoute* route) {
    if (route->device != nullptr) mixer_close(route->device);
    *route = {};
}

bool OpenMixerRoute(MixerRoute* route) {
    route->device = mixer_open(kCard);
    if (route->device == nullptr) {
        Log(ANDROID_LOG_ERROR, "Cannot open ALSA mixer card %d.", kCard);
        return false;
    }

    route->dout = mixer_get_ctl_by_name(route->device, kDoutControl);
    route->mixerEnable = mixer_get_ctl_by_name(route->device, kMixerControl);
    route->speaker = mixer_get_ctl_by_name(route->device, kSpeakerControl);
    if (route->dout == nullptr ||
        route->mixerEnable == nullptr ||
        route->speaker == nullptr) {
        Log(ANDROID_LOG_ERROR, "One or more audited mixer controls are absent.");
        CloseMixerRoute(route);
        return false;
    }

    if (mixer_ctl_get_type(route->dout) != MIXER_CTL_TYPE_ENUM ||
        mixer_ctl_get_type(route->mixerEnable) != MIXER_CTL_TYPE_ENUM ||
        mixer_ctl_get_type(route->speaker) != MIXER_CTL_TYPE_BOOL ||
        mixer_ctl_get_num_values(route->dout) != 1 ||
        mixer_ctl_get_num_values(route->mixerEnable) != 1 ||
        mixer_ctl_get_num_values(route->speaker) != 1) {
        Log(ANDROID_LOG_ERROR, "Audited mixer control types or widths changed.");
        CloseMixerRoute(route);
        return false;
    }

    route->expectedDout = FindEnumIndex(route->dout, kExpectedDout);
    route->expectedMixer = FindEnumIndex(route->mixerEnable, kExpectedMixer);
    route->appliedDout = FindEnumIndex(route->dout, kAppliedDout);
    route->appliedMixer = FindEnumIndex(route->mixerEnable, kAppliedMixer);
    if (route->expectedDout < 0 ||
        route->expectedMixer < 0 ||
        route->appliedDout < 0 ||
        route->appliedMixer < 0) {
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
    };
}

bool SameRoute(const RouteValues& left, const RouteValues& right) {
    return left.dout == right.dout &&
        left.mixer == right.mixer &&
        left.speaker == right.speaker;
}

RouteValues AppliedRoute(const MixerRoute& route) {
    return {route.appliedDout, route.appliedMixer, 0};
}

bool ValidateExpectedBaseline(const MixerRoute& route, const RouteValues& snapshot) {
    const RouteValues expected {route.expectedDout, route.expectedMixer, 1};
    if (!SameRoute(snapshot, expected)) {
        Log(
            ANDROID_LOG_ERROR,
            "Refusing unexpected in-call baseline: dout=%d mixer=%d speaker=%d.",
            snapshot.dout,
            snapshot.mixer,
            snapshot.speaker);
        return false;
    }
    return true;
}

bool SetAndVerify(mixer_ctl* control, int value) {
    return mixer_ctl_set_value(control, 0, value) == 0 &&
        mixer_ctl_get_value(control, 0) == value;
}

bool ApplyRoute(MixerRoute* route) {
    if (!SetAndVerify(route->speaker, 0)) return false;
    if (!SetAndVerify(route->mixerEnable, route->appliedMixer)) return false;
    if (!SetAndVerify(route->dout, route->appliedDout)) return false;
    return SameRoute(ReadRoute(*route), AppliedRoute(*route));
}

bool RouteContainsOnlyOurChanges(
    const MixerRoute& route,
    const RouteValues& snapshot,
    const RouteValues& current) {
    const RouteValues applied = AppliedRoute(route);
    const bool fieldsKnown =
        (current.dout == snapshot.dout || current.dout == applied.dout) &&
        (current.mixer == snapshot.mixer || current.mixer == applied.mixer) &&
        (current.speaker == snapshot.speaker || current.speaker == applied.speaker);
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
        SetAndVerify(route.speaker, snapshot.speaker) &&
        SameRoute(ReadRoute(route), snapshot);
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
    uint32_t checksum;
};
#pragma pack(pop)

uint32_t SnapshotChecksum(const SnapshotFile& snapshot) {
    return snapshot.magic ^
        snapshot.version ^
        static_cast<uint32_t>(snapshot.dout) ^
        static_cast<uint32_t>(snapshot.mixer) ^
        static_cast<uint32_t>(snapshot.speaker) ^
        kSnapshotChecksumSalt;
}

bool PersistSnapshot(const RouteValues& route) {
    SnapshotFile snapshot {
        kSnapshotMagic,
        kSnapshotVersion,
        route.dout,
        route.mixer,
        route.speaker,
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
    if (!SyncStateDirectory()) {
        unlink(kSnapshotPath);
        SyncStateDirectory();
        return false;
    }
    return true;
}

bool LoadSnapshot(RouteValues* route) {
    const int fd = open(kSnapshotPath, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;

    SnapshotFile snapshot {};
    const bool readOk = ReadAll(fd, &snapshot, sizeof(snapshot));
    close(fd);
    if (!readOk ||
        snapshot.magic != kSnapshotMagic ||
        snapshot.version != kSnapshotVersion ||
        snapshot.checksum != SnapshotChecksum(snapshot)) {
        Log(ANDROID_LOG_ERROR, "Persistent mixer snapshot is invalid.");
        return false;
    }

    *route = {snapshot.dout, snapshot.mixer, snapshot.speaker};
    return true;
}

void ClearSnapshot() {
    bool changed = false;
    if (unlink(kSnapshotPath) == 0) changed = true;
    if (unlink(kSnapshotTempPath) == 0) changed = true;
    if (changed && !SyncStateDirectory()) {
        Log(ANDROID_LOG_ERROR, "Could not durably clear the mixer snapshot.");
    }
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
        ValidatePromptFile(kPromptPath) &&
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
            if (format.audioFormat != 1 ||
                format.channels != 2 ||
                format.sampleRate != 48'000 ||
                format.bitsPerSample != 16 ||
                format.blockAlign != 4 ||
                format.byteRate != 192'000) {
                return false;
            }
            const uint32_t remainder =
                chunk.size - static_cast<uint32_t>(sizeof(WaveFormat));
            if (remainder > 0 && !SeekForward(file, remainder)) return false;
            foundFormat = true;
        } else if (chunk.id == kDataId) {
            if (!foundFormat || chunk.size == 0 ||
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
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) return false;

    uint32_t dataSize = 0;
    if (!ParseWave(file, &dataSize)) {
        Log(ANDROID_LOG_ERROR, "Prompt is not audited 48 kHz stereo PCM16 WAV.");
        std::fclose(file);
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
    config.silence_threshold = config.stop_threshold;

    pcm* output = pcm_open(kCard, kDevice, PCM_OUT, &config);
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
        if (pcm_writei(output, buffer, frames) < 0) {
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

void WatchdogProcess(int completionFd, RouteValues snapshot) {
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    pollfd descriptor {completionFd, POLLIN | POLLHUP, 0};
    const int result = poll(&descriptor, 1, kWatchdogTimeoutMs);
    char completion = 0;
    if (result > 0 && (descriptor.revents & POLLIN) != 0) {
        read(completionFd, &completion, 1);
    }
    close(completionFd);

    if (completion == 'D') _exit(0);

    Log(
        ANDROID_LOG_WARN,
        "Worker disappeared or timed out; watchdog is restoring the mixer.");
    const bool restored = RestoreRoute(snapshot, true);
    if (restored) ClearSnapshot();
    Log(
        restored ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR,
        restored ? "Watchdog restore completed." : "Watchdog restore failed.");
    _exit(restored ? 0 : 1);
}

bool StartWatchdog(
    const RouteValues& snapshot,
    int* completionFd,
    pid_t* watchdogPid) {
    int descriptors[2] = {-1, -1};
    if (pipe2(descriptors, O_CLOEXEC) != 0) return false;

    const pid_t child = fork();
    if (child < 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        return false;
    }
    if (child == 0) {
        close(descriptors[1]);
        WatchdogProcess(descriptors[0], snapshot);
    }

    close(descriptors[0]);
    *completionFd = descriptors[1];
    *watchdogPid = child;
    return true;
}

void FinishWatchdog(int completionFd, pid_t watchdogPid, bool restored) {
    if (restored) {
        const char done = 'D';
        WriteAll(completionFd, &done, 1);
    }
    close(completionFd);

    int status = 0;
    while (waitpid(watchdogPid, &status, 0) < 0 && errno == EINTR) {
    }
}

bool ProcessPromptRequest(
    const char* promptPath,
    const char* playingStatus,
    const char* completedStatus) {
    WriteStatus("WAITING_FOR_CALL");
    if (!WaitForInCall()) {
        Log(ANDROID_LOG_ERROR, "Timed out waiting for MODE_IN_CALL.");
        WriteStatus("ERROR_NOT_IN_CALL");
        return false;
    }
    if (!ValidatePromptFile(promptPath)) {
        WriteStatus("ERROR_PROMPT");
        return false;
    }

    MixerRoute route {};
    if (!OpenMixerRoute(&route)) {
        WriteStatus("ERROR_MIXER");
        return false;
    }
    const RouteValues snapshot = ReadRoute(route);
    if (!ValidateExpectedBaseline(route, snapshot)) {
        CloseMixerRoute(&route);
        WriteStatus("ERROR_BASELINE");
        return false;
    }
    if (!PersistSnapshot(snapshot)) {
        CloseMixerRoute(&route);
        WriteStatus("ERROR_SNAPSHOT");
        return false;
    }

    int completionFd = -1;
    pid_t watchdogPid = -1;
    if (!StartWatchdog(snapshot, &completionFd, &watchdogPid)) {
        ClearSnapshot();
        CloseMixerRoute(&route);
        WriteStatus("ERROR_WATCHDOG");
        return false;
    }

    bool applied = ApplyRoute(&route);
    CloseMixerRoute(&route);
    if (!applied) {
        Log(ANDROID_LOG_ERROR, "Mixer route apply failed; restoring snapshot.");
        const bool restored = RestoreRoute(snapshot, false);
        if (restored) ClearSnapshot();
        FinishWatchdog(completionFd, watchdogPid, restored);
        WriteStatus(restored ? "ERROR_APPLY_RESTORED" : "ERROR_RESTORE");
        return false;
    }

    Log(ANDROID_LOG_INFO, "Mixer transaction applied; playing audited prompt.");
    WriteStatus(playingStatus);
    const bool played = PlayPrompt(promptPath);
    const bool restored = RestoreRoute(snapshot, true);
    if (restored) ClearSnapshot();
    FinishWatchdog(completionFd, watchdogPid, restored);

    if (!restored) {
        Log(ANDROID_LOG_ERROR, "Mixer restore failed after prompt.");
        WriteStatus("ERROR_RESTORE");
        return false;
    }

    Log(
        played ? ANDROID_LOG_INFO : ANDROID_LOG_WARN,
        played
            ? "Prompt completed and mixer snapshot was restored."
            : "Prompt stopped early and mixer snapshot was restored.");
    WriteStatus(played ? completedStatus : "STOPPED_RESTORED");
    return played;
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

DtmfResult CaptureDtmfDigit() {
    if (!IsAudioInCall()) {
        WriteStatus("CALL_ENDED");
        return {DtmfResultKind::CallEnded, 0};
    }

    pcm_config config {};
    config.channels = kDtmfChannels;
    config.rate = kDtmfSampleRate;
    config.period_size = kDtmfFrameCount;
    config.period_count = kDtmfPeriodCount;
    config.format = PCM_FORMAT_S16_LE;
    config.start_threshold = 0;
    config.stop_threshold = 0;
    config.silence_threshold = 0;

    pcm* input = pcm_open(kCard, kCaptureDevice, PCM_IN, &config);
    if (input == nullptr || !pcm_is_ready(input)) {
        Log(
            ANDROID_LOG_ERROR,
            "Cannot open DTMF capture PCM: %s",
            input == nullptr ? "null handle" : pcm_get_error(input));
        if (input != nullptr) pcm_close(input);
        WriteStatus("ERROR_CAPTURE");
        return {DtmfResultKind::CaptureError, 0};
    }

    std::vector<int16_t> samples(kDtmfFrameCount * kDtmfChannels);
    ivrdroid::StereoDtmfDetector detector(kDtmfSampleRate);
    WriteStatus("LISTENING_DTMF");
    Log(ANDROID_LOG_INFO, "Listening for caller DTMF on audited PCM 0:0.");

    for (unsigned int frame = 0;
         frame < kDtmfTimeoutFrames && !gStopRequested;
         ++frame) {
        const int framesRead =
            pcm_readi(input, samples.data(), kDtmfFrameCount);
        if (framesRead < 0) {
            Log(
                ANDROID_LOG_ERROR,
                "DTMF PCM read failed: %s",
                pcm_get_error(input));
            pcm_close(input);
            WriteStatus("ERROR_CAPTURE");
            return {DtmfResultKind::CaptureError, 0};
        }
        if (framesRead != static_cast<int>(kDtmfFrameCount)) {
            Log(
                ANDROID_LOG_ERROR,
                "DTMF PCM returned an unexpected frame count: %d.",
                framesRead);
            pcm_close(input);
            WriteStatus("ERROR_CAPTURE");
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
            char status[16] = {};
            std::snprintf(status, sizeof(status), "DTMF_%c", digit);
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
            WriteStatus(status);
            return {DtmfResultKind::Digit, digit};
        }

        if ((frame + 1) % kDtmfCallCheckFrames == 0 &&
            !IsAudioInCall()) {
            pcm_close(input);
            WriteStatus("CALL_ENDED");
            return {DtmfResultKind::CallEnded, 0};
        }
    }

    pcm_close(input);
    if (gStopRequested) {
        WriteStatus("STOPPED");
        return {DtmfResultKind::Stopped, 0};
    }
    Log(ANDROID_LOG_INFO, "Caller DTMF wait timed out.");
    WriteStatus("DTMF_TIMEOUT");
    return {DtmfResultKind::Timeout, 0};
}

bool ProcessDtmfRequest() {
    const DtmfResult result = CaptureDtmfDigit();
    return result.kind == DtmfResultKind::Digit ||
        result.kind == DtmfResultKind::Timeout;
}

bool SendFixedTelecomEndCall() {
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
            kEndCallTransaction,
            "s16",
            kCallingPackage,
            static_cast<char*>(nullptr));
        _exit(127);
    }

    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            Log(ANDROID_LOG_ERROR, "Could not wait for the Telecom end-call transaction.");
            return false;
        }
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool FinishMenuSession() {
    if (!IsAudioInCall()) {
        WriteStatus("SESSION_COMPLETE");
        return true;
    }

    WriteStatus("ENDING_CALL");
    Log(ANDROID_LOG_INFO, "Sending the pinned Telecom end-call transaction.");
    if (!SendFixedTelecomEndCall()) {
        Log(ANDROID_LOG_ERROR, "The Telecom end-call transaction failed.");
        WriteStatus("ERROR_END_CALL");
        return false;
    }

    for (int attempt = 0; attempt < 30; ++attempt) {
        if (!IsAudioInCall()) {
            WriteStatus("SESSION_COMPLETE");
            return true;
        }
        usleep(100'000);
    }
    Log(ANDROID_LOG_ERROR, "Telecom returned without ending the active call.");
    WriteStatus("ERROR_END_CALL");
    return false;
}

bool PlayTerminalPrompt(char digit) {
    if (digit == '1') {
        return ProcessPromptRequest(
            kSalesPromptPath,
            "PLAYING_SALES",
            "PROMPT_DONE_SALES");
    }
    if (digit == '2') {
        return ProcessPromptRequest(
            kSupportPromptPath,
            "PLAYING_SUPPORT",
            "PROMPT_DONE_SUPPORT");
    }
    if (digit == '0') {
        return ProcessPromptRequest(
            kOperatorPromptPath,
            "PLAYING_OPERATOR",
            "PROMPT_DONE_OPERATOR");
    }
    return false;
}

bool ProcessStartMenuRequest() {
    Log(ANDROID_LOG_INFO, "Starting the fixed root-owned IVR menu session.");
    int retries = 0;
    while (!gStopRequested) {
        if (!ProcessPromptRequest(
                kMainPromptPath,
                "PLAYING_MAIN",
                "PROMPT_DONE_MAIN")) {
            return false;
        }

        const DtmfResult input = CaptureDtmfDigit();
        if (input.kind == DtmfResultKind::Digit &&
            (input.digit == '1' || input.digit == '2' || input.digit == '0')) {
            if (!PlayTerminalPrompt(input.digit)) return false;
            return FinishMenuSession();
        }
        if (input.kind != DtmfResultKind::Digit &&
            input.kind != DtmfResultKind::Timeout) {
            return false;
        }

        ++retries;
        if (retries > kMenuMaximumRetries) {
            Log(ANDROID_LOG_INFO, "IVR menu retry limit reached.");
            return FinishMenuSession();
        }
        WriteStatus("RETRYING_MENU");
        Log(ANDROID_LOG_INFO, "Retrying the main menu after missing or invalid input.");
    }
    WriteStatus("STOPPED");
    return false;
}

bool ReadAndConsumeTrigger() {
    struct stat state {};
    if (lstat(kTriggerPath, &state) != 0) return false;

    bool valid =
        S_ISREG(state.st_mode) &&
        state.st_uid == gAppUid &&
        (state.st_mode & 0022) == 0 &&
        state.st_size == static_cast<off_t>(sizeof(kRequestBody) - 1);

    std::string body;
    if (valid) {
        const int fd = open(kTriggerPath, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) {
            valid = false;
        } else {
            char content[sizeof(kRequestBody)] = {};
            valid = ReadAll(fd, content, sizeof(kRequestBody) - 1);
            close(fd);
            if (valid) body.assign(content, sizeof(kRequestBody) - 1);
        }
    }
    unlink(kTriggerPath);

    if (!valid || body != kRequestBody) {
        Log(ANDROID_LOG_WARN, "Rejected malformed or incorrectly owned app request.");
        WriteStatus("ERROR_REQUEST");
        return true;
    }

    Log(ANDROID_LOG_INFO, "Accepted fixed PLAY_ONCE request from app UID %u.", gAppUid);
    ProcessPromptRequest(kPromptPath, "PLAYING", "RESTORED");
    return true;
}

bool ReadAndConsumeCommand() {
    struct stat state {};
    if (lstat(kCommandPath, &state) != 0) return false;

    bool valid =
        S_ISREG(state.st_mode) &&
        state.st_uid == gAppUid &&
        (state.st_mode & 0022) == 0 &&
        state.st_size > 0 &&
        state.st_size <= kMaximumCommandBytes;

    std::string body;
    if (valid) {
        const size_t size = static_cast<size_t>(state.st_size);
        const int fd = open(kCommandPath, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) {
            valid = false;
        } else {
            std::vector<char> content(size);
            valid = ReadAll(fd, content.data(), content.size());
            close(fd);
            if (valid) body.assign(content.begin(), content.end());
        }
    }
    unlink(kCommandPath);

    if (!valid) {
        Log(ANDROID_LOG_WARN, "Rejected malformed or incorrectly owned app command.");
        WriteStatus("ERROR_COMMAND");
        return true;
    }

    Log(ANDROID_LOG_INFO, "Accepted fixed menu command from app UID %u.", gAppUid);
    if (body == kStartMenuCommand) {
        ProcessStartMenuRequest();
    } else if (body == kPlayMainCommand) {
        ProcessPromptRequest(
            kMainPromptPath,
            "PLAYING_MAIN",
            "PROMPT_DONE_MAIN");
    } else if (body == kPlaySalesCommand) {
        ProcessPromptRequest(
            kSalesPromptPath,
            "PLAYING_SALES",
            "PROMPT_DONE_SALES");
    } else if (body == kPlaySupportCommand) {
        ProcessPromptRequest(
            kSupportPromptPath,
            "PLAYING_SUPPORT",
            "PROMPT_DONE_SUPPORT");
    } else if (body == kPlayOperatorCommand) {
        ProcessPromptRequest(
            kOperatorPromptPath,
            "PLAYING_OPERATOR",
            "PROMPT_DONE_OPERATOR");
    } else if (body == kListenDtmfCommand) {
        ProcessDtmfRequest();
    } else {
        Log(ANDROID_LOG_WARN, "Rejected unknown fixed menu command.");
        WriteStatus("ERROR_COMMAND");
    }
    return true;
}

void RecoverStaleSnapshot() {
    RouteValues snapshot {};
    if (!LoadSnapshot(&snapshot)) return;

    Log(ANDROID_LOG_WARN, "Found an unfinished mixer transaction; checking route ownership.");
    const bool restored = RestoreRoute(snapshot, true);
    if (restored) {
        ClearSnapshot();
        Log(ANDROID_LOG_INFO, "Unfinished mixer transaction recovered.");
    } else {
        Log(ANDROID_LOG_ERROR, "Could not recover unfinished mixer transaction.");
    }
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
    if (!ResolveAndValidateAppDirectory()) {
        Log(ANDROID_LOG_ERROR, "App bridge directory is unavailable.");
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

    RecoverStaleSnapshot();

    const int inotifyFd = inotify_init1(IN_CLOEXEC);
    if (inotifyFd < 0) {
        unlink(kPidPath);
        close(lockFd);
        return 16;
    }
    const int watch = inotify_add_watch(
        inotifyFd,
        kBridgeDir,
        IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE_SELF | IN_MOVE_SELF);
    if (watch < 0) {
        close(inotifyFd);
        unlink(kPidPath);
        close(lockFd);
        return 17;
    }

    WriteStatus("READY");
    Log(ANDROID_LOG_INFO, "Privileged helper is ready for app UID %u.", gAppUid);

    alignas(inotify_event) char events[4096] = {};
    while (!gStopRequested) {
        ReadAndConsumeCommand();
        ReadAndConsumeTrigger();

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

    WriteStatus("STOPPED");
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
        !ResolveAndValidateAppDirectory() ||
        !ValidateAllPromptFiles()) {
        return 20;
    }

    MixerRoute route {};
    if (!OpenMixerRoute(&route)) return 21;
    const RouteValues current = ReadRoute(route);
    CloseMixerRoute(&route);
    Log(
        ANDROID_LOG_INFO,
        "Self-test passed without mutation: mode=%s dout=%d mixer=%d speaker=%d appUid=%u.",
        IsAudioInCall() ? "IN_CALL" : "NORMAL",
        current.dout,
        current.mixer,
        current.speaker,
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
