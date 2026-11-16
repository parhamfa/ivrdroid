#pragma once

#include <sys/types.h>

#include <cstdint>

namespace ivrdroid {

// Keeps the tablet reachable while the screen is off:
//   1. a named kernel wake lock stops the CPU from suspending, because the
//      vendor Wi-Fi driver cannot re-join a lost network while suspended;
//   2. a periodic Wi-Fi command (in production `cmd wifi force-hi-perf-mode`
//      toggled disabled then enabled) switches off Wi-Fi power saving, which
//      delays and drops inbound packets. The setting lives in system_server,
//      so it is asserted again after every refresh interval.
// Each toggle opens binder twice, and old vendor kernels leak kernel memory on
// every binder open, so the command runs rarely (two opens per 15-minute
// refresh, 192 a day) and never in a loop. Consecutive failures back off:
// retryIntervalMs, then 2x, 4x, ... capped at refreshIntervalMs.
struct ConnectivityGuardOptions {
    const char* wakeLockPath = "/sys/power/wake_lock";
    const char* wakeLockName = "ivrdroid-keepawake";
    const char* disableFlagPath = "/data/adb/ivrdroid/keep-awake.disabled";
    int64_t retryIntervalMs = 30'000;          // after the first failed Wi-Fi assert; doubles
    int64_t refreshIntervalMs = 15 * 60'000;   // after a successful one
    int64_t commandTimeoutMs = 5'000;
};

// Starts the Wi-Fi command in a child process and returns its pid, or -1. Must not block.
using ConnectivityCommandSpawn = pid_t (*)(void* context);
// priority uses the android log numbering (4 = INFO, 5 = WARN); may be null.
using ConnectivityLog = void (*)(int priority, const char* message, void* context);

enum class WakeLockState {
    Unknown,
    Held,
    Failed,
    Disabled,
};

const char* WakeLockStateName(WakeLockState state);

class ConnectivityGuard {
public:
    ConnectivityGuard(
        ConnectivityCommandSpawn spawn,
        ConnectivityLog log,
        void* context,
        ConnectivityGuardOptions options = ConnectivityGuardOptions());
    ConnectivityGuard(const ConnectivityGuard&) = delete;
    ConnectivityGuard& operator=(const ConnectivityGuard&) = delete;

    // Writes the wake lock name unless the disable flag exists.
    void AssertWakeLock();
    // Reaps a finished command and starts the next one when due. Blocks only to
    // reap a command it has just killed for running past commandTimeoutMs.
    void Tick(int64_t nowMs);
    // Kills and reaps a running command. Deliberately leaves the wake lock held.
    void Shutdown();

    int wifiAssertsSucceeded() const { return wifiSucceeded_; }
    int wifiAssertsFailed() const { return wifiFailed_; }
    void ResetStats();
    WakeLockState wakeLockState() const;
    bool commandRunning() const { return childPid_ > 0; }

private:
    enum class Status {
        Unknown,
        Working,
        Failing,
    };

    bool RefreshDisabled();
    void ReapChild(int64_t nowMs);
    void RecordWifiResult(bool succeeded, const char* detail, int64_t nowMs);
    void MaybeAnnounceHeld();
    void Emit(int priority, const char* format, ...)
        __attribute__((format(printf, 3, 4)));

    ConnectivityCommandSpawn spawn_;
    ConnectivityLog log_;
    void* context_;
    ConnectivityGuardOptions options_;
    pid_t childPid_ = -1;
    int64_t childStartedMs_ = 0;
    bool scheduled_ = false;
    int64_t nextAssertMs_ = 0;
    bool disabled_ = false;
    Status lockStatus_ = Status::Unknown;
    Status wifiStatus_ = Status::Unknown;
    bool announcedHeld_ = false;
    int wifiSucceeded_ = 0;
    int wifiFailed_ = 0;
    // Delay after the next failure; doubles per consecutive failure.
    int64_t nextRetryDelayMs_;
};

}  // namespace ivrdroid
