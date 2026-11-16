#include "connectivity_guard.h"

#include <sys/stat.h>
#include <sys/wait.h>

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
      nextRetryDelayMs_(options.retryIntervalMs) {}

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
        RecordWifiResult(WIFEXITED(status) && WEXITSTATUS(status) == 0, detail, nowMs);
        return;
    }
    if (result < 0 && errno != EINTR) {
        // Someone else reaped it; the outcome is unknowable, so count a failure.
        childPid_ = -1;
        RecordWifiResult(false, "child lost", nowMs);
        return;
    }
    if (nowMs - childStartedMs_ >= options_.commandTimeoutMs) {
        const pid_t pid = childPid_;
        childPid_ = -1;
        kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        RecordWifiResult(false, "timed out", nowMs);
    }
}

void ConnectivityGuard::Tick(int64_t nowMs) {
    ReapChild(nowMs);
    if (RefreshDisabled()) return;
    if (childPid_ > 0) return;
    if (scheduled_ && nowMs < nextAssertMs_) return;
    AssertWakeLock();
    // AssertWakeLock re-checks the flag; it may have appeared in between.
    if (disabled_) return;
    const pid_t pid = spawn_ != nullptr ? spawn_(context_) : -1;
    if (pid <= 0) {
        RecordWifiResult(false, "could not start the command", nowMs);
        return;
    }
    childPid_ = pid;
    childStartedMs_ = nowMs;
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
