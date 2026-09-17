#include "continuous_recording.h"
#include "call_control_protocol.h"

#include <cerrno>
#include <fcntl.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

namespace ivrdroid {
namespace {
int64_t ElapsedMs() { timespec t {}; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }
bool WriteBytes(int fd, const void* bytes, size_t size) {
    const auto* at = static_cast<const uint8_t*>(bytes);
    while (size) {
        const ssize_t count = write(fd, at, size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        at += count; size -= static_cast<size_t>(count);
    }
    return true;
}
bool Valid(const ContinuousRecordingContext& c) {
    return (c.kind == "conversation" || c.kind == "session_audit") && call_control::IsCanonicalUuid(c.id) &&
        call_control::IsCanonicalUuid(c.callId) && call_control::IsCanonicalUuid(c.bootId) &&
        (c.kind == "conversation" ? c.revision > 0 && call_control::IsCanonicalUuid(c.blockId) : c.policy > 0 && c.blockId == "-") &&
        c.wallMs > 0 && c.elapsedMs > 0 && c.pid > 0 && c.processStart > 0 && c.frames <= 48'000ULL * 86'400;
}
}

std::string FormatContinuousContext(const ContinuousRecordingContext& c) {
    if (!Valid(c)) return "";
    std::ostringstream out;
    out << "PCM1 " << c.kind << ' ' << c.id << ' ' << c.callId << ' ' << c.bootId << ' '
        << c.revision << ' ' << c.blockId << ' ' << c.policy << ' ' << c.wallMs << ' ' << c.elapsedMs << ' '
        << c.pid << ' ' << c.processStart << ' ' << c.frames << '\n';
    return out.str();
}
bool ParseContinuousContext(const std::string& text, ContinuousRecordingContext* context) {
    if (!context || text.size() > 1024) return false;
    ContinuousRecordingContext c; std::string magic, extra; std::istringstream in(text);
    if (!(in >> magic >> c.kind >> c.id >> c.callId >> c.bootId >> c.revision >> c.blockId >> c.policy >> c.wallMs >> c.elapsedMs >> c.pid >> c.processStart >> c.frames) ||
        magic != "PCM1" || (in >> extra) || !Valid(c)) return false;
    *context = c; return true;
}
ContinuousPcmFile::ContinuousPcmFile(std::string directory, uid_t owner, ContinuousRecordingContext context)
    : directory_(std::move(directory)), owner_(owner), context_(std::move(context)) {}
ContinuousPcmFile::~ContinuousPcmFile() { if (fd_ >= 0) close(fd_); }
bool ContinuousPcmFile::Failure(const char* operation, int error, int64_t startedMs) {
    struct statvfs disk {};
    const uint64_t free = statvfs(directory_.c_str(), &disk) == 0 ? disk.f_bavail * static_cast<uint64_t>(disk.f_frsize) : 0;
    std::ostringstream out;
    out << "{\"operation\":\"" << operation << "\",\"errno\":" << error
        << ",\"duration_ms\":" << ElapsedMs() - startedMs << ",\"free_bytes\":" << free
        << ",\"frames\":" << context_.frames << ",\"pid\":" << getpid()
        << ",\"phase\":\"recording\",\"exit_reason\":\"writer_io_failure\"}\n";
    // This diagnostic belongs to the isolated writer. Failure to save it must
    // never replace the original errno or alter the committed audio checkpoint.
    WriteMetadata("continuous.failure", out.str()); errno = error; return false;
}
bool ContinuousPcmFile::WriteMetadata(const std::string& name, const std::string& text) {
    const std::string temporary = directory_ + "/." + name + ".tmp";
    const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    const bool ok = fchmod(fd, 0600) == 0 && fchown(fd, owner_, geteuid() == owner_ ? static_cast<gid_t>(-1) : owner_) == 0 && WriteBytes(fd, text.data(), text.size()) && fsync(fd) == 0;
    close(fd);
    if (!ok || rename(temporary.c_str(), (directory_ + "/" + name).c_str()) != 0) return false;
    const int dir = open(directory_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dir < 0) return false;
    const bool synced = fsync(dir) == 0; close(dir); return synced;
}
bool ContinuousPcmFile::Open() {
    if (!Valid(context_) || context_.frames != 0 || fd_ >= 0) return false;
    if (mkdir(directory_.c_str(), 0700) != 0 && errno != EEXIST) return false;
    struct stat st {};
    if (lstat(directory_.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) ||
        (st.st_uid != owner_ && st.st_uid != 0) || chmod(directory_.c_str(), 0700) != 0 || chown(directory_.c_str(), owner_, geteuid() == owner_ ? static_cast<gid_t>(-1) : owner_) != 0) return false;
    fd_ = open((directory_ + "/audio.pcm").c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    return fd_ >= 0 && fchown(fd_, owner_, geteuid() == owner_ ? static_cast<gid_t>(-1) : owner_) == 0 && Checkpoint();
}
bool ContinuousPcmFile::Append(const int16_t* samples, uint32_t frames, uint64_t maximumBytes) {
    const int64_t started = ElapsedMs();
    if (fd_ < 0 || !samples || frames == 0 || frames > 48000 || context_.frames > UINT64_MAX / 4 - frames) return false;
    const uint64_t bytes = (context_.frames + frames) * 4;
    if (bytes > maximumBytes || context_.frames + frames > 48'000ULL * 86'400) return Failure("byte_budget", ENOSPC, started);
    if (!WriteBytes(fd_, samples, frames * 4)) return Failure("append", errno, started);
    context_.frames += frames;
    return true;
}
bool ContinuousPcmFile::Checkpoint() {
    const int64_t started = ElapsedMs();
    if (fd_ < 0 || fsync(fd_) != 0) return Failure("flush_audio", errno, started);
    if (!WriteMetadata("continuous.context", FormatContinuousContext(context_))) return Failure("commit_checkpoint", errno, started);
    return true;
}
bool ContinuousPcmFile::Finish(const std::string& reason, bool partial) {
    if (reason.empty() || reason.size() > 32 || reason.find_first_not_of("abcdefghijklmnopqrstuvwxyz_") != std::string::npos) return false;
    // A failed partial write may have left an incomplete PCM frame after the valid prefix.
    if (fd_ < 0 || ftruncate(fd_, static_cast<off_t>(context_.frames * 4)) != 0 || !Checkpoint()) return false;
    return WriteMetadata("continuous.sealed", reason + " " + (partial ? "1\n" : "0\n"));
}
bool ContinuousPcmFile::Recover(const std::string& directory, uid_t owner, const ContinuousRecordingContext& committed) {
    if (!Valid(committed)) return false;
    ContinuousPcmFile file(directory, owner, committed);
    file.fd_ = open((directory + "/audio.pcm").c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    struct stat st {};
    if (file.fd_ < 0 || fstat(file.fd_, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != owner ||
        (st.st_mode & 0077) != 0 || st.st_size < static_cast<off_t>(committed.frames * 4)) return false;
    return file.Finish("interrupted", true);
}
} // namespace ivrdroid
