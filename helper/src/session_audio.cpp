#include "session_audio.h"
#include "audit_policy.h"
#include "call_control_protocol.h"
#include "sha256.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <grp.h>
#include <new>
#include <signal.h>
#include <sstream>
#include <string>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace ivrdroid {
namespace {
#ifndef IVRDROID_AUDIT_BRIDGE
#define IVRDROID_AUDIT_BRIDGE "/data/user/0/ai.rx1.ivrdroid/files/bridge"
#endif
constexpr char kBridge[] = IVRDROID_AUDIT_BRIDGE;
constexpr char kInbox[] = IVRDROID_AUDIT_BRIDGE "/session-audit";
constexpr size_t kRingSize = 128;
constexpr size_t kEventLimit = 4096;
constexpr int64_t kFrameNs = 25'000'000;
struct AudioFrame {
    std::atomic<uint64_t> sequence {0};
    std::atomic<int64_t> timeNs {0};
    std::atomic<uint32_t> frames {0}, prompt {0};
    std::atomic<int16_t> samples[kAuditFrameCount * 2] {};
};
struct Event {
    std::atomic<bool> ready {false};
    int64_t timeNs = 0;
    char type[20] {};
    char block[37] {};
    char detail[81] {};
};
struct SharedAudio {
    std::atomic<bool> active {false}, stop {false}, ready {false}, failed {false}, writerFailed {false}, captureClosed {false};
    std::atomic<uint64_t> produced {0}, prompts {0};
    std::atomic<uint32_t> eventCount {0}, promptEpoch {0};
    std::atomic<int64_t> baseNs {0}, wallMs {0};
    std::atomic<int> reason {0};
    std::atomic<int64_t> promptEnds[kRingSize] {};
    AudioFrame input[kRingSize], output[kRingSize];
    Event events[kEventLimit];
};
static_assert(std::atomic<uint64_t>::is_always_lock_free, "IPC counters must be lock free");
SharedAudio* shared = nullptr;
AuditPolicy policy;
uid_t appUid = 0;
std::string callId;
int captureCard = 0, captureDevice = 0;
pid_t capturePid = -1, writerPid = -1;
volatile sig_atomic_t childStop = 0;
void StopChild(int) { childStop = 1; }
int64_t ClockNs(clockid_t clock) {
    timespec time {};
    return clock_gettime(clock, &time) == 0 ? time.tv_sec * 1'000'000'000LL + time.tv_nsec : 0;
}
int Reason(const char* value) {
    const char* values[] = {"", "session_complete", "caller_hangup", "preempted", "capture_failure", "storage_full", "interrupted", "writer_failure"};
    for (int i = 1; i < 8; ++i) if (std::strcmp(value, values[i]) == 0) return i;
    return 6;
}
const char* ReasonText(int reason) {
    const char* values[] = {"interrupted", "session_complete", "caller_hangup", "preempted", "capture_failure", "storage_full", "interrupted", "writer_failure"};
    return values[std::clamp(reason, 0, 7)];
}
bool WriteAllBytes(int fd, const void* value, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(value);
    while (size > 0) {
        const ssize_t count = write(fd, bytes, size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        bytes += count; size -= static_cast<size_t>(count);
    }
    return true;
}
bool SyncDirectory(const std::string& path) {
    const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    const bool ok = fsync(fd) == 0; close(fd); return ok;
}
bool SafeFile(const std::string& path, off_t maximum) {
    struct stat value {};
    return lstat(path.c_str(), &value) == 0 && S_ISREG(value.st_mode) && value.st_uid == appUid &&
        (value.st_mode & 0077) == 0 && value.st_size >= 0 && value.st_size <= maximum;
}
std::string ReadFile(const std::string& path, off_t maximum) {
    if (!SafeFile(path, maximum)) return "";
    std::ifstream input(path); return std::string(std::istreambuf_iterator<char>(input), {});
}
bool AtomicFile(const std::string& path, const std::string& value) {
    const std::string temporary = path + ".tmp";
    const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    const bool ok = fchmod(fd, 0600) == 0 && fchown(fd, appUid, appUid) == 0 &&
        WriteAllBytes(fd, value.data(), value.size()) && fsync(fd) == 0;
    close(fd);
    return ok && rename(temporary.c_str(), path.c_str()) == 0 && SyncDirectory(path.substr(0, path.find_last_of('/')));
}
bool PrivateDirectory(const std::string& path) {
    if (mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) return false;
    struct stat value {};
    if (lstat(path.c_str(), &value) != 0 || !S_ISDIR(value.st_mode) ||
        (value.st_uid != 0 && value.st_uid != appUid)) return false;
    return chmod(path.c_str(), 0700) == 0 && chown(path.c_str(), appUid, appUid) == 0;
}
std::string Timestamp(int64_t milliseconds) {
    time_t seconds = milliseconds / 1000;
    tm value {}; gmtime_r(&seconds, &value);
    char text[40] {};
    std::snprintf(text, sizeof(text), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", value.tm_year + 1900,
        value.tm_mon + 1, value.tm_mday, value.tm_hour, value.tm_min, value.tm_sec, static_cast<int>(milliseconds % 1000));
    return text;
}
std::string Stem(uint32_t index) {
    char text[16] {}; std::snprintf(text, sizeof(text), "%05u", index); return text;
}
bool WaveHeader(int fd, uint64_t frames) {
    uint32_t header[11] = {0x46464952, static_cast<uint32_t>(36 + frames * 4), 0x45564157, 0x20746d66,
        16, 0x00020001, 48000, 192000, 0x00100004, 0x61746164, static_cast<uint32_t>(frames * 4)};
    return pwrite(fd, header, sizeof(header), 0) == static_cast<ssize_t>(sizeof(header));
}
uint64_t DirectoryBytes(const std::string& path) {
    DIR* directory = opendir(path.c_str()); if (!directory) return 0;
    uint64_t total = 0;
    while (auto* entry = readdir(directory)) {
        if (entry->d_name[0] == '.') continue;
        const std::string item = path + "/" + entry->d_name;
        struct stat value {};
        if (lstat(item.c_str(), &value) != 0) continue;
        if (S_ISREG(value.st_mode)) total += value.st_size;
        else if (S_ISDIR(value.st_mode) && call_control::IsCanonicalUuid(entry->d_name)) total += DirectoryBytes(item);
    }
    closedir(directory); return total;
}
bool StorageAvailable() {
    uint64_t spool = 0;
    std::istringstream capacity(ReadFile(std::string(kBridge) + "/audit-capacity", 80));
    if (!(capacity >> spool)) return false;
    struct statvfs disk {};
    if (statvfs(kInbox, &disk) != 0) return false;
    return AuditStorageFits(spool, DirectoryBytes(kInbox), disk.f_bavail * static_cast<uint64_t>(disk.f_frsize), policy.quotaBytes);
}
uint64_t ProcessStart(pid_t pid) {
    std::ifstream input("/proc/" + std::to_string(pid) + "/stat");
    std::string line; std::getline(input, line);
    const auto end = line.find_last_of(')'); if (end == std::string::npos) return 0;
    std::istringstream fields(line.substr(end + 1)); std::string token;
    for (int field = 3; field <= 22; ++field) if (!(fields >> token)) return 0;
    try { return std::stoull(token); } catch (...) { return 0; }
}
std::string BootId() {
    std::ifstream input("/proc/sys/kernel/random/boot_id"); std::string value; input >> value; return value;
}
struct Context {
    std::string id, boot;
    uint64_t version = 0, total = 0, processStart = 0;
    int64_t wall = 0, base = 0;
    pid_t pid = -1;
    std::string directory() const { return std::string(kInbox) + "/" + id; }
};
bool SaveContext(const Context& value) {
    std::ostringstream output;
    output << "AUDIT1 " << value.id << ' ' << value.version << ' ' << value.wall << ' ' << value.base << ' '
        << value.total << ' ' << value.pid << ' ' << value.processStart << ' ' << value.boot << '\n';
    return AtomicFile(value.directory() + "/context", output.str());
}
std::string Timeline(const Context& context) {
    std::vector<std::pair<int64_t, std::string>> entries;
    if (shared) {
        // Reserve one receipt event for the partial-coverage boundary after recovery.
        for (uint32_t i = 0; i < std::min<uint32_t>(shared->eventCount.load(), kEventLimit - 1); ++i) {
            const Event& event = shared->events[i]; if (!event.ready.load(std::memory_order_acquire)) continue;
            const int64_t offset = std::max<int64_t>(0, (event.timeNs - context.base) / 1'000'000);
            if (offset > static_cast<int64_t>(context.total / 48) + 1000) continue;
            std::ostringstream out;
            out << "{\"offset_ms\":" << offset << ",\"type\":\"" << event.type << "\",\"block_id\":";
            if (event.block[0]) out << '"' << event.block << '"'; else out << "null";
            out << ",\"detail\":\"" << event.detail << "\"}";
            entries.emplace_back(offset, out.str());
        }
        std::stable_sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    }
    std::string result = "[";
    for (const auto& entry : entries) { if (result.size() > 1) result += ','; result += entry.second; }
    return result + "]";
}
bool SaveReport(const Context& context, int reason, bool checkpoint = false) {
    std::string events = shared && context.id == callId ? Timeline(context) : ReadFile(context.directory() + "/events", 1024 * 1024);
    if (events.empty()) events = "[]";
    if (checkpoint) return AtomicFile(context.directory() + "/events", events);
    if (reason > 2 && context.total && events.back() == ']') {
        events.pop_back();
        if (events.size() > 1) events += ',';
        events += "{\"offset_ms\":" + std::to_string(context.total / 48) +
            ",\"type\":\"gap\",\"block_id\":null,\"detail\":\"" + ReasonText(reason) + "\"}]";
    }
    std::ostringstream out;
    out << "{\"recording_id\":\"" << context.id << "\",\"policy_version\":" << context.version
        << ",\"state\":\"" << (context.total ? "pending_upload" : "unavailable") << "\",\"captured_at\":"
        << (context.total ? "\"" + Timestamp(context.wall) + "\"" : "null") << ",\"duration_ms\":" << context.total / 48 << ",\"partial\":"
        << (reason > 2 ? "true" : "false") << ",\"stop_reason\":\"" << ReasonText(reason) << "\",\"events\":" << events << '}';
    return AtomicFile(context.directory() + "/report.json", out.str());
}
bool SealSegment(const Context& context, uint32_t index, uint64_t frames, int reason) {
    if (frames == 0) return true;
    const std::string stem = context.directory() + "/" + Stem(index);
    const int fd = open((stem + ".wav.partial").c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd >= 0) {
        const bool ok = ftruncate(fd, 44 + frames * 4) == 0 && WaveHeader(fd, frames) && fsync(fd) == 0;
        close(fd);
        if (!ok || rename((stem + ".wav.partial").c_str(), (stem + ".wav").c_str()) != 0 || !SyncDirectory(context.directory())) return false;
    } else if (!SafeFile(stem + ".wav", 44 + frames * 4)) return false;
    std::string digest;
    if (!Sha256File((stem + ".wav").c_str(), 3 * 1024 * 1024, &digest)) return false;
    std::ostringstream out;
    out << "{\"kind\":\"session_audit\",\"recording_id\":\"" << context.id << "\",\"call_id\":\"" << context.id
        << "\",\"policy_version\":" << context.version << ",\"segment_index\":" << index
        << ",\"captured_at\":\"" << Timestamp(context.wall + index * 15000LL) << "\",\"duration_ms\":" << frames / 48
        << ",\"stop_reason\":\"" << (reason ? ReasonText(reason) : "segment_boundary") << "\",\"partial\":"
        << (reason > 2 ? "true" : "false") << ",\"size_bytes\":" << 44 + frames * 4 << ",\"sha256\":\"" << digest << "\"}";
    return AtomicFile(stem + ".json", out.str());
}
void CaptureWorker() {
    signal(SIGTERM, StopChild); signal(SIGINT, StopChild);
    while (!shared->active.load() && !shared->stop.load() && !childStop) usleep(1000);
    if (shared->stop.load() || childStop) { shared->captureClosed.store(true); _exit(0); }
    pcm_config config {}; config.channels = 2; config.rate = 48000;
    config.period_size = kAuditFrameCount; config.period_count = 4; config.format = PCM_FORMAT_S16_LE;
    pcm* input = pcm_open(captureCard, captureDevice, PCM_IN | PCM_MONOTONIC, &config);
    if (!input || !pcm_is_ready(input)) {
        if (input) pcm_close(input);
        shared->captureClosed.store(true); shared->failed.store(true); _exit(1);
    }
    int16_t samples[kAuditFrameCount * 2] {};
    uint64_t sequence = 0;
    while (!shared->stop.load() && !childStop) {
        if (pcm_readi(input, samples, kAuditFrameCount) != static_cast<int>(kAuditFrameCount)) {
            shared->failed.store(true); break;
        }
        if (sequence == 0) {
            timespec stamp {}; unsigned int available = 0;
            int64_t base = ClockNs(CLOCK_MONOTONIC) - kFrameNs;
            if (pcm_get_htimestamp(input, &available, &stamp) == 0 && stamp.tv_sec > 0) {
                base = stamp.tv_sec * 1'000'000'000LL + stamp.tv_nsec - (available + kAuditFrameCount) * 1'000'000'000LL / 48000;
            }
            shared->baseNs.store(base);
            shared->wallMs.store((ClockNs(CLOCK_REALTIME) - ClockNs(CLOCK_MONOTONIC) + base) / 1'000'000);
            shared->ready.store(true, std::memory_order_release);
        }
        ++sequence;
        AudioFrame& frame = shared->input[sequence % kRingSize];
        frame.sequence.store(0, std::memory_order_release);
        for (size_t i = 0; i < kAuditFrameCount * 2; ++i) frame.samples[i].store(samples[i], std::memory_order_relaxed);
        frame.timeNs = shared->baseNs.load() + (sequence - 1) * kFrameNs;
        frame.frames = kAuditFrameCount;
        frame.sequence.store(sequence, std::memory_order_release);
        shared->produced.store(sequence, std::memory_order_release);
    }
    pcm_close(input); shared->ready.store(false); shared->captureClosed.store(true); _exit(0);
}
struct CopiedFrame { int64_t timeNs = 0; uint32_t frames = 0, prompt = 0; int16_t samples[kAuditFrameCount * 2] {}; };
bool CopyFrame(const AudioFrame& source, uint64_t expected, CopiedFrame* target) {
    if (source.sequence.load(std::memory_order_acquire) != expected) return false;
    target->timeNs = source.timeNs.load(std::memory_order_relaxed);
    target->frames = source.frames.load(std::memory_order_relaxed);
    target->prompt = source.prompt.load(std::memory_order_relaxed);
    for (size_t i = 0; i < kAuditFrameCount * 2; ++i) target->samples[i] = source.samples[i].load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    return source.sequence.load(std::memory_order_acquire) == expected;
}
struct PromptChunk { int64_t start = 0; uint32_t epoch = 0; std::vector<int16_t> samples; };
void WriterWorker() {
    signal(SIGTERM, StopChild); signal(SIGINT, StopChild);
    while (shared->produced.load() == 0 && !shared->stop.load() && !shared->failed.load() && !childStop) usleep(1000);
    Context context {callId, BootId(), policy.version, 0, ProcessStart(getpid()), shared->wallMs.load(), shared->baseNs.load(), getpid()};
    if (!context.wall) context.wall = ClockNs(CLOCK_REALTIME) / 1'000'000;
    if (!PrivateDirectory(context.directory()) || !SaveContext(context)) { shared->writerFailed.store(true); _exit(1); }
    uint64_t next = 1, nextPrompt = 1;
    int fd = -1, reason = 0;
    uint32_t index = 0;
    uint64_t segmentFrames = 0;
    std::deque<PromptChunk> prompts;
    while (!childStop) {
        if (context.total % 48000 == 0 && SafeFile(context.directory() + "/abort", 80)) { reason = 7; break; }
        const uint64_t produced = shared->produced.load(std::memory_order_acquire);
        if (next > produced) {
            if (shared->stop.load() || shared->failed.load()) break;
            usleep(2000); continue;
        }
        // Lag only the audit writer, allowing rendered prompt cancellation to settle.
        if (!shared->stop.load() && ClockNs(CLOCK_MONOTONIC) - (context.base + (next - 1) * kFrameNs) < 300'000'000) {
            usleep(2000); continue;
        }
        CopiedFrame frame;
        if (!CopyFrame(shared->input[next % kRingSize], next, &frame)) { reason = 7; break; }
        const uint64_t producedPrompts = shared->prompts.load(std::memory_order_acquire);
        while (nextPrompt <= producedPrompts) {
            CopiedFrame prompt;
            if (!CopyFrame(shared->output[nextPrompt % kRingSize], nextPrompt, &prompt)) { reason = 7; break; }
            prompts.push_back({(prompt.timeNs - context.base) * 48000 / 1'000'000'000LL, prompt.prompt,
                std::vector<int16_t>(prompt.samples, prompt.samples + prompt.frames * 2)});
            ++nextPrompt;
        }
        if (reason) break;
        for (auto& prompt : prompts) {
            const int64_t cutNs = shared->promptEnds[prompt.epoch % kRingSize].load();
            const int64_t end = cutNs > 0 ? (cutNs - context.base) * 48000 / 1'000'000'000LL : INT64_MAX;
            for (uint32_t i = 0; i < kAuditFrameCount; ++i) {
                const int64_t at = static_cast<int64_t>(context.total + i);
                const int64_t position = at - prompt.start;
                if (position < 0 || at >= end || position >= static_cast<int64_t>(prompt.samples.size() / 2)) continue;
                frame.samples[i * 2] = MixAuditSample(frame.samples[i * 2], prompt.samples[position * 2]);
                frame.samples[i * 2 + 1] = MixAuditSample(frame.samples[i * 2 + 1], prompt.samples[position * 2 + 1]);
            }
        }
        while (!prompts.empty() && prompts.front().start + static_cast<int64_t>(prompts.front().samples.size() / 2) <= static_cast<int64_t>(context.total)) prompts.pop_front();
        if (fd < 0) {
            if (!StorageAvailable()) { reason = 5; break; }
            const std::string path = context.directory() + "/" + Stem(index) + ".wav.partial";
            fd = open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (fd < 0 || fchown(fd, appUid, appUid) != 0 || !WaveHeader(fd, 0) || lseek(fd, 44, SEEK_SET) != 44) { reason = 7; break; }
        }
        if (!WriteAllBytes(fd, frame.samples, sizeof(frame.samples))) { reason = errno == ENOSPC ? 5 : 7; break; }
        context.total += kAuditFrameCount; segmentFrames += kAuditFrameCount; ++next;
        if (segmentFrames % 48000 == 0 && (!WaveHeader(fd, segmentFrames) || fsync(fd) != 0 || !SaveContext(context) || !SaveReport(context, 0, true))) { reason = 7; break; }
        if (segmentFrames == kAuditSegmentFrames) {
            close(fd); fd = -1;
            if (!SealSegment(context, index, segmentFrames, 0)) {
                // Keep the last committed context intact for journal recovery.
                shared->writerFailed.store(true); _exit(1);
            }
            ++index; segmentFrames = 0;
        }
    }
    // The guardian may still be classifying an observed disconnect. Audit waits here,
    // after capture has stopped, without delaying Telecom or audio cleanup.
    const int64_t reasonDeadline = ClockNs(CLOCK_MONOTONIC) + 500'000'000;
    while (reason == 0 && shared->reason.load() == 0 && shared->failed.load() && !childStop &&
        ClockNs(CLOCK_MONOTONIC) < reasonDeadline) usleep(2000);
    if (reason == 0) reason = shared->reason.load();
    if (reason == 0) reason = shared->failed.load() ? 4 : 6;
    if (fd >= 0) close(fd);
    if (segmentFrames && !SealSegment(context, index, segmentFrames, reason)) {
        shared->writerFailed.store(true); _exit(1);
    }
    SaveContext(context);
    SaveReport(context, reason);
    if (reason > 2) shared->writerFailed.store(true);
    _exit(0);
}
} // namespace

void RefreshAuditPolicy(uid_t uid) {
    appUid = uid;
    AuditPolicy incoming;
    if (!ParseAuditPolicy(ReadFile(std::string(kBridge) + "/audit-policy", 160), &incoming)) incoming = AuditPolicy{};
    policy = incoming;
    const std::string state = "version=" + std::to_string(policy.version) + "\nenabled=" + (policy.enabled ? "1" : "0") + "\nquota=" + std::to_string(policy.quotaBytes) + "\n";
    if (ReadFile(std::string(kBridge) + "/audit-state", 160) != state) AtomicFile(std::string(kBridge) + "/audit-state", state);
}
void PrepareSessionAudio(const std::string& uuid, uid_t uid, int card, int device) {
    ReleaseSessionAudio(); appUid = uid; callId = uuid; captureCard = card; captureDevice = device;
    if (!policy.enabled || !call_control::IsCanonicalUuid(uuid) || !PrivateDirectory(kInbox)) return;
    void* memory = mmap(nullptr, sizeof(SharedAudio), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (memory != MAP_FAILED) shared = new (memory) SharedAudio();
}
void CloseInheritedDescriptors() {
    DIR* descriptors = opendir("/proc/self/fd");
    if (!descriptors) return;
    const int directoryFd = dirfd(descriptors);
    while (auto* entry = readdir(descriptors)) {
        char* end = nullptr; const long fd = std::strtol(entry->d_name, &end, 10);
        if (*entry->d_name && *end == 0 && fd > 2 && fd != directoryFd) close(static_cast<int>(fd));
    }
    closedir(descriptors);
}
bool DropAuditPrivileges() {
    if (geteuid() == appUid) return true;
    return setgroups(0, nullptr) == 0 && setgid(appUid) == 0 && setuid(appUid) == 0;
}
void SpawnSessionAudioWorkers() {
    if (!shared) return;
    const pid_t parent = getpid();
    capturePid = fork();
    if (capturePid == 0) { prctl(PR_SET_PDEATHSIG, SIGTERM); if (getppid() != parent) _exit(1); CloseInheritedDescriptors(); CaptureWorker(); }
    if (capturePid < 0) { shared->captureClosed.store(true); shared->failed.store(true); }
    writerPid = fork();
    if (writerPid == 0) {
        CloseInheritedDescriptors();
        if (!DropAuditPrivileges()) { shared->writerFailed.store(true); _exit(1); }
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != parent) _exit(1);
        WriterWorker();
    }
    if (writerPid < 0) shared->writerFailed.store(true);
}
void ActivateSessionAudio() {
    if (!shared) return;
    shared->active.store(true);
    const int64_t deadline = ClockNs(CLOCK_MONOTONIC) + 500'000'000;
    while (!shared->ready.load() && !shared->failed.load() && ClockNs(CLOCK_MONOTONIC) < deadline) usleep(1000);
    AuditEvent("answered");
}
void StopSessionAudio(const char* reason) {
    if (!shared) return;
    int expected = 0;
    if (shared->reason.compare_exchange_strong(expected, Reason(reason))) AuditEvent("ended", "", reason);
    shared->stop.store(true);
}
void SuperviseSessionAudioWorkers() {
    if (!shared) return;
    if (capturePid > 0 && waitpid(capturePid, nullptr, WNOHANG) == capturePid) {
        capturePid = -1;
        shared->ready.store(false); shared->captureClosed.store(true);
        if (!shared->stop.load()) shared->failed.store(true);
    }
    if (writerPid > 0 && waitpid(writerPid, nullptr, WNOHANG) == writerPid) {
        writerPid = -1;
        if (!shared->stop.load()) shared->writerFailed.store(true);
    }
}
void DrainSessionAudioWorkers() {
    if (!shared) return;
    StopSessionAudio("interrupted");
    const int64_t deadline = ClockNs(CLOCK_MONOTONIC) + 1'500'000'000;
    for (pid_t* child : {&capturePid, &writerPid}) {
        if (*child <= 0) continue;
        while (waitpid(*child, nullptr, WNOHANG) == 0 && ClockNs(CLOCK_MONOTONIC) < deadline) usleep(2000);
        if (waitpid(*child, nullptr, WNOHANG) == 0) { kill(*child, SIGKILL); waitpid(*child, nullptr, 0); }
        *child = -1;
    }
}
void ReleaseSessionAudio() {
    if (shared) { munmap(shared, sizeof(SharedAudio)); shared = nullptr; }
}
void AuditEvent(const char* type, const std::string& block, const std::string& detail) {
    if (!shared || !shared->active.load() || shared->writerFailed.load()) return;
    const uint32_t index = shared->eventCount.fetch_add(1);
    if (index >= kEventLimit) return;
    Event& event = shared->events[index]; event.timeNs = ClockNs(CLOCK_MONOTONIC);
    std::snprintf(event.type, sizeof(event.type), "%s", type);
    if (call_control::IsCanonicalUuid(block)) std::snprintf(event.block, sizeof(event.block), "%s", block.c_str());
    std::string safe; unsigned int digits = 0;
    for (char c : detail) {
        if (c >= '0' && c <= '9') ++digits;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || std::strchr("_ .:#/*-", c)) safe.push_back(c);
    }
    if (digits >= 8) safe = "";
    std::snprintf(event.detail, sizeof(event.detail), "%s", safe.c_str());
    event.ready.store(true, std::memory_order_release);
}
void BeginAuditPrompt() {
    if (!shared) return;
    const auto epoch = shared->promptEpoch.fetch_add(1) + 1;
    shared->promptEnds[epoch % kRingSize].store(0);
}
void TapAuditPrompt(pcm* output, const int16_t* samples, unsigned int frames) {
    if (!shared || shared->writerFailed.load() || !shared->active.load()) return;
    timespec stamp {}; unsigned int available = 0;
    int64_t start = ClockNs(CLOCK_MONOTONIC);
    if (pcm_get_htimestamp(output, &available, &stamp) == 0 && stamp.tv_sec > 0) {
        const int64_t queued = static_cast<int64_t>(pcm_get_buffer_size(output)) - available;
        start = stamp.tv_sec * 1'000'000'000LL + stamp.tv_nsec + (queued - frames) * 1'000'000'000LL / 48000;
    }
    for (unsigned int offset = 0; offset < frames; offset += kAuditFrameCount) {
        const uint64_t sequence = shared->prompts.load() + 1;
        AudioFrame& frame = shared->output[sequence % kRingSize]; frame.sequence.store(0);
        frame.frames = std::min(kAuditFrameCount, frames - offset);
        frame.timeNs = start + offset * 1'000'000'000LL / 48000;
        frame.prompt = shared->promptEpoch.load();
        for (size_t i = 0; i < frame.frames.load() * 2; ++i) frame.samples[i].store(samples[offset * 2 + i], std::memory_order_relaxed);
        frame.sequence.store(sequence, std::memory_order_release);
        shared->prompts.store(sequence, std::memory_order_release);
    }
}
void EndAuditPrompt() {
    if (shared) shared->promptEnds[shared->promptEpoch.load() % kRingSize].store(ClockNs(CLOCK_MONOTONIC));
}
struct SessionCapture { pcm* direct = nullptr; uint64_t next = 0; bool broker = false; };
SessionCapture* OpenSessionCapture(unsigned int card, unsigned int device, unsigned int flags, const pcm_config* config) {
    auto* input = new SessionCapture;
    if (shared && shared->active.load()) {
        const int64_t deadline = ClockNs(CLOCK_MONOTONIC) + 2'000'000'000;
        while (!shared->ready.load() && !shared->captureClosed.load() && ClockNs(CLOCK_MONOTONIC) < deadline) usleep(1000);
    }
    input->broker = shared && shared->ready.load() && !shared->failed.load();
    if (input->broker) input->next = shared->produced.load() + 1;
    else {
        // Never open a second PCM while the shared producer may still own it.
        if (shared && shared->active.load() && !shared->captureClosed.load()) { delete input; return nullptr; }
        input->direct = pcm_open(card, device, flags, config);
    }
    return input;
}
bool SessionCaptureReady(SessionCapture* input) { return input && (input->broker ? shared && shared->ready.load() : input->direct && pcm_is_ready(input->direct)); }
int ReadSessionCapture(SessionCapture* input, void* samples, unsigned int frames) {
    if (!input) return -1;
    if (!input->broker) return pcm_readi(input->direct, samples, frames);
    if (frames != kAuditFrameCount) return -1;
    const int64_t deadline = ClockNs(CLOCK_MONOTONIC) + 2'000'000'000;
    while (shared->produced.load() < input->next) {
        if (shared->failed.load() || shared->stop.load() || ClockNs(CLOCK_MONOTONIC) >= deadline) return -1;
        usleep(1000);
    }
    CopiedFrame frame;
    if (!CopyFrame(shared->input[input->next % kRingSize], input->next, &frame)) return -1;
    std::memcpy(samples, frame.samples, frames * 4); ++input->next; return frames;
}
int StartSessionCapture(SessionCapture* input) { return input ? (input->broker ? 0 : pcm_start(input->direct)) : -1; }
const char* SessionCaptureError(SessionCapture* input) { return input && input->direct ? pcm_get_error(input->direct) : "Session capture unavailable or overrun"; }
void CloseSessionCapture(SessionCapture* input) { if (input) { if (input->direct) pcm_close(input->direct); delete input; } }

void RecoverAuditRecordings(uid_t uid) {
    appUid = uid;
    if (geteuid() != uid) {
        const pid_t child = fork();
        if (child == 0) {
            CloseInheritedDescriptors();
            if (!DropAuditPrivileges()) _exit(1);
            RecoverAuditRecordings(uid); _exit(0);
        }
        if (child > 0) {
            const int64_t deadline = ClockNs(CLOCK_MONOTONIC) + 1'500'000'000;
            while (waitpid(child, nullptr, WNOHANG) == 0 && ClockNs(CLOCK_MONOTONIC) < deadline) usleep(2000);
            if (waitpid(child, nullptr, WNOHANG) == 0) { kill(child, SIGKILL); waitpid(child, nullptr, 0); }
        }
        return;
    } if (!PrivateDirectory(kInbox)) return;
    DIR* directory = opendir(kInbox); if (!directory) return;
    while (auto* entry = readdir(directory)) {
        if (!call_control::IsCanonicalUuid(entry->d_name)) continue;
        const std::string path = std::string(kInbox) + "/" + entry->d_name;
        if (SafeFile(path + "/report.json", 1024 * 1024)) continue;
        std::istringstream metadata(ReadFile(path + "/context", 512));
        Context context; std::string magic;
        if (!(metadata >> magic >> context.id >> context.version >> context.wall >> context.base >> context.total >> context.pid >> context.processStart >> context.boot) ||
            magic != "AUDIT1" || context.id != entry->d_name || context.total > 48000ULL * 86400 || context.wall <= 0) continue;
        if (context.boot == BootId() && context.pid > 0 && context.processStart != 0 && ProcessStart(context.pid) == context.processStart) continue;
        uint32_t index = context.total / kAuditSegmentFrames;
        // A checkpoint can reach a boundary before the atomic segment rename.
        if (index > 0 && SafeFile(path + "/" + Stem(index - 1) + ".wav.partial", 44 + kAuditSegmentFrames * 4)) {
            if (!SealSegment(context, index - 1, kAuditSegmentFrames, 0)) {
                context.total = (index - 1) * kAuditSegmentFrames; --index;
            }
        }
        const uint64_t frames = context.total % kAuditSegmentFrames;
        if (frames && !SealSegment(context, index, frames, 6)) context.total -= frames;
        SaveReport(context, 6);
    }
    closedir(directory);
}
} // namespace ivrdroid
