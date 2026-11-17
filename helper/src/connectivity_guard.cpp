#include "connectivity_guard.h"

#include <sys/stat.h>
#include <sys/wait.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace ivrdroid {

namespace {

constexpr int kLogInfo = 4;
constexpr int kLogWarn = 5;

}  // namespace

const char* WakeLockStateName(WakeLockState state) {
    switch (state) {
        case WakeLockState::Held: return "held";
        case WakeLockState::Failed: return "failed";
        case WakeLockState::Disabled: return "disabled";
        case WakeLockState::Unknown: return "unknown";
    }
    return "unknown";
}

ConnectivityGuard::ConnectivityGuard(
    ConnectivityCommandSpawn spawn,
    ConnectivityLog log,
    void* context,
    ConnectivityGuardOptions options)
    : spawn_(spawn),
      log_(log),
      context_(context),
      options_(options),
      nextRetryDelayMs_(options.retryIntervalMs),
      scanDelayMs_(options.scanIntervalMs) {}

void ConnectivityGuard::Emit(int priority, const char* format, ...) {
    if (log_ == nullptr) return;
    char message[256] = {};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    log_(priority, message, context_);
}

// The flag only stops further asserts. It does not release a lock that is
// already held and does not undo the Wi-Fi setting; the operator does that by
// hand (echo ivrdroid-keepawake > /sys/power/wake_unlock and
// cmd wifi force-hi-perf-mode disabled).
bool ConnectivityGuard::RefreshDisabled() {
    struct stat info {};
    const bool present = stat(options_.disableFlagPath, &info) == 0;
    if (present != disabled_) {
        disabled_ = present;
        if (present) {
            Emit(kLogInfo,
                "Keep-awake disable flag %s found; no longer asserting the lock or Wi-Fi setting.",
                options_.disableFlagPath);
        } else {
            Emit(kLogInfo,
                "Keep-awake disable flag %s removed; asserting the lock and Wi-Fi setting again.",
                options_.disableFlagPath);
            // Resume right away rather than after a stale interval; the
            // failure backoff is kept.
            scheduled_ = false;
        }
    }
    return disabled_;
}

void ConnectivityGuard::AssertWakeLock() {
    if (RefreshDisabled()) return;
    bool ok = false;
    int error = 0;
    const int fd = open(options_.wakeLockPath, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        error = errno;
    } else {
        const size_t length = std::strlen(options_.wakeLockName);
        ssize_t written;
        do {
            written = write(fd, options_.wakeLockName, length);
        } while (written < 0 && errno == EINTR);
        if (written == static_cast<ssize_t>(length)) {
            ok = true;
        } else {
            error = written < 0 ? errno : EIO;
        }
        close(fd);
    }
    if (ok) {
        if (lockStatus_ == Status::Failing) Emit(kLogInfo, "Keep-awake lock recovered.");
        lockStatus_ = Status::Working;
        MaybeAnnounceHeld();
    } else {
        if (lockStatus_ != Status::Failing) {
            Emit(kLogWarn,
                "Could not write the keep-awake lock to %s: %s.",
                options_.wakeLockPath,
                std::strerror(error));
        }
        lockStatus_ = Status::Failing;
    }
}

void ConnectivityGuard::MaybeAnnounceHeld() {
    if (announcedHeld_) return;
    if (lockStatus_ != Status::Working || wifiStatus_ != Status::Working) return;
    announcedHeld_ = true;
    Emit(kLogInfo, "Keep-awake lock held and Wi-Fi power saving switched off.");
}

void ConnectivityGuard::RecordWifiResult(bool succeeded, const char* detail, int64_t nowMs) {
    scheduled_ = true;
    if (succeeded) {
        ++wifiSucceeded_;
        nextAssertMs_ = nowMs + options_.refreshIntervalMs;
        nextRetryDelayMs_ = options_.retryIntervalMs;
        if (wifiStatus_ == Status::Failing) {
            Emit(kLogInfo, "Wi-Fi power saving switch-off recovered.");
        }
        wifiStatus_ = Status::Working;
        MaybeAnnounceHeld();
    } else {
        ++wifiFailed_;
        // Each launch leaks kernel memory, so a permanent failure must not
        // relaunch at a fixed short interval.
        const int64_t delay = nextRetryDelayMs_ < options_.refreshIntervalMs
            ? nextRetryDelayMs_
            : options_.refreshIntervalMs;
        nextAssertMs_ = nowMs + delay;
        nextRetryDelayMs_ = delay <= options_.refreshIntervalMs / 2
            ? delay * 2
            : options_.refreshIntervalMs;
        if (wifiStatus_ != Status::Failing) {
            Emit(kLogWarn,
                "Could not switch off Wi-Fi power saving (%s); retrying with backoff.",
                detail);
        }
        wifiStatus_ = Status::Failing;
    }
}

ConnectivityGuard::LinkState ConnectivityGuard::ReadLinkState() const {
    const int fd = open(options_.linkStatePath, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return LinkState::Absent;
    char buffer[32] = {};
    ssize_t count;
    do {
        count = read(fd, buffer, sizeof(buffer) - 1);
    } while (count < 0 && errno == EINTR);
    close(fd);
    // An empty read is unknown, like a missing file.
    if (count <= 0) return LinkState::Absent;
    size_t length = static_cast<size_t>(count);
    while (length > 0 && std::isspace(static_cast<unsigned char>(buffer[length - 1]))) --length;
    return length == 2 && std::memcmp(buffer, "up", 2) == 0 ? LinkState::Up : LinkState::Down;
}

void ConnectivityGuard::PollLink(int64_t nowMs) {
    if (linkPolled_ && nowMs - lastLinkPollMs_ < options_.linkPollIntervalMs) return;
    linkPolled_ = true;
    lastLinkPollMs_ = nowMs;
    const LinkState state = ReadLinkState();
    if (state == linkState_) return;
    const LinkState previous = linkState_;
    linkState_ = state;
    if (previous == LinkState::Up && nowMs - upSinceMs_ >= options_.linkStableMs) {
        pacedScans_ = 0;
        scanDelayMs_ = options_.scanIntervalMs;
        scanAllowedMs_ = 0;
    }
    // Entering Down starts a fresh outage; Up or Absent ends it, and the
    // outage fields are only read while Down.
    if (state == LinkState::Down) {
        downSinceMs_ = nowMs;
        outageScans_ = 0;
        nextScanMs_ = std::max(nowMs + options_.linkDownGraceMs, scanAllowedMs_);
        return;
    }
    if (state == LinkState::Up) {
        upSinceMs_ = nowMs;
        if (previous == LinkState::Down && outageScans_ > 0) {
            Emit(kLogInfo,
                "Wi-Fi link back after %lld s (%d scan requests).",
                static_cast<long long>((nowMs - downSinceMs_) / 1'000),
                outageScans_);
        }
        // A driver reload resets power saving, so assert again soon; the
        // failure backoff is kept.
        assertOnLinkUp_ = true;
    }
    // Absent means Wi-Fi is switched off; unknown never triggers a scan.
}

bool ConnectivityGuard::ScanDue(int64_t nowMs) const {
    return linkState_ == LinkState::Down && nowMs >= nextScanMs_;
}

void ConnectivityGuard::StartScan(int64_t nowMs) {
    if (outageScans_ == 0) {
        ++linkDrops_;
        Emit(kLogWarn,
            "Wi-Fi link down for %lld s; asking Android to scan.",
            static_cast<long long>((nowMs - downSinceMs_) / 1'000));
    }
    ++outageScans_;
    ++scanRequests_;
    ++pacedScans_;
    // Each launch leaks kernel memory, so a link that stays down, or keeps
    // dropping, is asked less and less often.
    if (pacedScans_ >= options_.scanFastCount) {
        scanDelayMs_ = scanDelayMs_ <= options_.refreshIntervalMs / 2
            ? scanDelayMs_ * 2
            : options_.refreshIntervalMs;
    }
    scanAllowedMs_ = nowMs + scanDelayMs_;
    nextScanMs_ = scanAllowedMs_;
    const pid_t pid = spawn_ != nullptr ? spawn_(ConnectivityCommand::WifiScan, context_) : -1;
    if (pid <= 0) {
        ++scanFailed_;
        return;
    }
    childPid_ = pid;
    childCommand_ = ConnectivityCommand::WifiScan;
    childStartedMs_ = nowMs;
}

// A scan result only feeds the scan counters; it says nothing about the
// high-performance setting.
void ConnectivityGuard::RecordChildResult(bool succeeded, const char* detail, int64_t nowMs) {
    if (childCommand_ == ConnectivityCommand::WifiScan) {
        if (!succeeded) ++scanFailed_;
        return;
    }
    RecordWifiResult(succeeded, detail, nowMs);
}

void ConnectivityGuard::ReapChild(int64_t nowMs) {
    if (childPid_ <= 0) return;
    int status = 0;
    const pid_t result = waitpid(childPid_, &status, WNOHANG);
    if (result == childPid_) {
        childPid_ = -1;
        char detail[48] = {};
        if (WIFEXITED(status)) {
            std::snprintf(detail, sizeof(detail), "exit %d", WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            std::snprintf(detail, sizeof(detail), "signal %d", WTERMSIG(status));
        } else {
            std::snprintf(detail, sizeof(detail), "status %d", status);
        }
        RecordChildResult(WIFEXITED(status) && WEXITSTATUS(status) == 0, detail, nowMs);
        return;
    }
    if (result < 0 && errno != EINTR) {
        // Someone else reaped it; the outcome is unknowable, so count a failure.
        childPid_ = -1;
        RecordChildResult(false, "child lost", nowMs);
        return;
    }
    if (nowMs - childStartedMs_ >= options_.commandTimeoutMs) {
        const pid_t pid = childPid_;
        childPid_ = -1;
        kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        RecordChildResult(false, "timed out", nowMs);
    }
}

void ConnectivityGuard::Tick(int64_t nowMs) {
    ReapChild(nowMs);
    // Tracked while disabled too, so outages are timed right once enabled.
    PollLink(nowMs);
    if (RefreshDisabled()) return;
    if (childPid_ > 0) return;
    // One child at a time; the scan goes first and the assert follows later.
    if (ScanDue(nowMs)) {
        StartScan(nowMs);
        return;
    }
    // The floor runs from the last link-triggered attempt, so an ordinary
    // refresh never delays the first link-up toggle after a quiet period.
    if (assertOnLinkUp_ && nowMs >= nextLinkAssertAllowedMs_) {
        assertOnLinkUp_ = false;
        scheduled_ = false;
        nextLinkAssertAllowedMs_ = nowMs + options_.linkReassertMinIntervalMs;
    }
    if (scheduled_ && nowMs < nextAssertMs_) return;
    AssertWakeLock();
    // AssertWakeLock re-checks the flag; it may have appeared in between.
    if (disabled_) return;
    const pid_t pid = spawn_ != nullptr ? spawn_(ConnectivityCommand::WifiHighPerf, context_) : -1;
    if (pid <= 0) {
        RecordWifiResult(false, "could not start the command", nowMs);
        return;
    }
    childPid_ = pid;
    childCommand_ = ConnectivityCommand::WifiHighPerf;
    childStartedMs_ = nowMs;
    // Any assert satisfies a pending link-up one.
    assertOnLinkUp_ = false;
}

void ConnectivityGuard::Shutdown() {
    if (childPid_ <= 0) return;
    const pid_t pid = childPid_;
    childPid_ = -1;
    kill(pid, SIGKILL);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
}

void ConnectivityGuard::ResetStats() {
    wifiSucceeded_ = 0;
    wifiFailed_ = 0;
    linkDrops_ = 0;
    scanRequests_ = 0;
    scanFailed_ = 0;
}

WakeLockState ConnectivityGuard::wakeLockState() const {
    if (disabled_) return WakeLockState::Disabled;
    switch (lockStatus_) {
        case Status::Working: return WakeLockState::Held;
        case Status::Failing: return WakeLockState::Failed;
        case Status::Unknown: return WakeLockState::Unknown;
    }
    return WakeLockState::Unknown;
}

}  // namespace ivrdroid
