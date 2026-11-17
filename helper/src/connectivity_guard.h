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
//   3. a link watch: the Wi-Fi chip has no offloaded background scan, so after
//      the link drops with the screen off Android does not scan again by
//      itself. Once the link has been down for linkDownGraceMs the guard asks
//      for a scan (`cmd wifi start-scan`), every scanIntervalMs for the first
//      scanFastCount requests, then doubling up to refreshIntervalMs. That
//      pacing carries across outages until the link has stayed up for
//      linkStableMs, so a flapping link cannot buy a fresh fast start each
//      time. A missing interface means Wi-Fi is switched off and never
//      triggers a scan. A link that comes back gets the Wi-Fi command again,
//      at most once per linkReassertMinIntervalMs.
struct ConnectivityGuardOptions {
    const char* wakeLockPath = "/sys/power/wake_lock";
    const char* wakeLockName = "ivrdroid-keepawake";
    const char* disableFlagPath = "/data/adb/ivrdroid/keep-awake.disabled";
    int64_t retryIntervalMs = 30'000;          // after the first failed Wi-Fi assert; doubles
    int64_t refreshIntervalMs = 15 * 60'000;   // after a successful one
    int64_t commandTimeoutMs = 5'000;
    const char* linkStatePath = "/sys/class/net/wlan0/operstate";
    int64_t linkPollIntervalMs = 5'000;
    int64_t linkDownGraceMs = 45'000;   // Android's own immediate retries get this long
    int64_t scanIntervalMs = 20'000;    // between the first scanFastCount scan requests
    int scanFastCount = 4;              // after these the interval doubles, capped at refreshIntervalMs
    // The scan pacing starts over only after the link has stayed up this long.
    int64_t linkStableMs = 10 * 60'000;
    // A flapping link must not relaunch the Wi-Fi command on every flap; each
    // launch leaks kernel memory.
    int64_t linkReassertMinIntervalMs = 5 * 60'000;
};

enum class ConnectivityCommand {
    WifiHighPerf,
    WifiScan,
};

// Starts the command in a child process and returns its pid, or -1. Must not block.
using ConnectivityCommandSpawn = pid_t (*)(ConnectivityCommand command, void* context);
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
    int wifiLinkDrops() const { return linkDrops_; }
    // Requests started, including ones whose child could not be spawned.
    int wifiScanRequests() const { return scanRequests_; }
    int wifiScanRequestsFailed() const { return scanFailed_; }
    void ResetStats();
    WakeLockState wakeLockState() const;
    bool commandRunning() const { return childPid_ > 0; }

private:
    enum class Status {
        Unknown,
        Working,
        Failing,
    };

    enum class LinkState {
        Up,
        Down,
        Absent,
    };

    bool RefreshDisabled();
    LinkState ReadLinkState() const;
    void PollLink(int64_t nowMs);
    bool ScanDue(int64_t nowMs) const;
    void StartScan(int64_t nowMs);
    void ReapChild(int64_t nowMs);
    void RecordChildResult(bool succeeded, const char* detail, int64_t nowMs);
    void RecordWifiResult(bool succeeded, const char* detail, int64_t nowMs);
    void MaybeAnnounceHeld();
    void Emit(int priority, const char* format, ...)
        __attribute__((format(printf, 3, 4)));

    ConnectivityCommandSpawn spawn_;
    ConnectivityLog log_;
    void* context_;
    ConnectivityGuardOptions options_;
    pid_t childPid_ = -1;
    ConnectivityCommand childCommand_ = ConnectivityCommand::WifiHighPerf;
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
    bool linkPolled_ = false;
    int64_t lastLinkPollMs_ = 0;
    LinkState linkState_ = LinkState::Up;
    int64_t upSinceMs_ = 0;
    // Outage tracking, only read while linkState_ is Down.
    int64_t downSinceMs_ = 0;
    int outageScans_ = 0;
    int64_t nextScanMs_ = 0;
    // Scan pacing; outlives an outage until the link is stable again.
    int pacedScans_ = 0;
    int64_t scanDelayMs_;
    int64_t scanAllowedMs_ = 0;
    // Set when the link comes back; kept until a Wi-Fi command is started.
    bool assertOnLinkUp_ = false;
    int64_t nextLinkAssertAllowedMs_ = 0;
    int linkDrops_ = 0;
    int scanRequests_ = 0;
    int scanFailed_ = 0;
};

}  // namespace ivrdroid
