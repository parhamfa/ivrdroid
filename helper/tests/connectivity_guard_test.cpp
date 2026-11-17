#include "connectivity_guard.h"

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

using ivrdroid::ConnectivityCommand;
using ivrdroid::ConnectivityGuard;
using ivrdroid::ConnectivityGuardOptions;
using ivrdroid::WakeLockState;

enum class ChildMode {
    ExitZero,
    ExitOne,
    Sleep,
    FailSpawn,
    // Runs until the harness release file exists, then exits 0.
    WaitForRelease,
};

struct LogLine {
    int priority;
    std::string message;
};

// spawns and lastPid count only the high-performance command; scans are kept apart.
struct Harness {
    ChildMode mode = ChildMode::ExitZero;
    ChildMode scanMode = ChildMode::ExitZero;
    int spawns = 0;
    int scans = 0;
    pid_t lastPid = -1;
    pid_t lastScanPid = -1;
    std::string releasePath;
    std::vector<LogLine> logs;
};

pid_t Spawn(ConnectivityCommand command, void* context) {
    auto* harness = static_cast<Harness*>(context);
    const bool scan = command == ConnectivityCommand::WifiScan;
    const ChildMode mode = scan ? harness->scanMode : harness->mode;
    ++(scan ? harness->scans : harness->spawns);
    if (mode == ChildMode::FailSpawn) return -1;
    const pid_t child = fork();
    if (child == 0) {
        switch (mode) {
            case ChildMode::ExitZero: _exit(0);
            case ChildMode::ExitOne: _exit(1);
            case ChildMode::Sleep:
                for (;;) pause();
            case ChildMode::FailSpawn: _exit(2);
            case ChildMode::WaitForRelease:
                while (access(harness->releasePath.c_str(), F_OK) != 0) usleep(1'000);
                _exit(0);
        }
        _exit(3);
    }
    (scan ? harness->lastScanPid : harness->lastPid) = child;
    return child;
}

void Record(int priority, const char* message, void* context) {
    static_cast<Harness*>(context)->logs.push_back({priority, message});
}

int CountLogs(const Harness& harness, int priority, const char* fragment) {
    int count = 0;
    for (const auto& line : harness.logs) {
        if (line.priority == priority && line.message.find(fragment) != std::string::npos) ++count;
    }
    return count;
}

// Waits for the fake child to exit without reaping it, so the next Tick sees it.
void WaitUntilExited(pid_t pid) {
    siginfo_t info {};
    while (waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOWAIT) != 0) {
        assert(errno == EINTR);
    }
}

std::string ReadFile(const std::string& path) {
    std::string content;
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return content;
    char buffer[256];
    ssize_t count;
    while ((count = read(fd, buffer, sizeof(buffer))) > 0) content.append(buffer, static_cast<size_t>(count));
    close(fd);
    return content;
}

void Touch(const std::string& path) {
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    assert(fd >= 0);
    close(fd);
}

void WriteText(const std::string& path, const char* text) {
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    assert(fd >= 0);
    const size_t length = std::strlen(text);
    assert(write(fd, text, length) == static_cast<ssize_t>(length));
    close(fd);
}

struct Fixture {
    std::string dir;
    std::string lockPath;
    std::string flagPath;
    std::string linkPath;
    ConnectivityGuardOptions options;

    Fixture() {
        char pattern[] = "/tmp/ivrdroid-guard-test.XXXXXX";
        const char* created = mkdtemp(pattern);
        assert(created != nullptr);
        dir = created;
        lockPath = dir + "/wake_lock";
        flagPath = dir + "/keep-awake.disabled";
        linkPath = dir + "/operstate";
        Touch(lockPath);
        SetLink("up\n");
        options.wakeLockPath = lockPath.c_str();
        options.disableFlagPath = flagPath.c_str();
        options.retryIntervalMs = 30'000;
        options.refreshIntervalMs = 15 * 60'000;
        options.commandTimeoutMs = 5'000;
        options.linkStatePath = linkPath.c_str();
        options.linkPollIntervalMs = 5'000;
        options.linkDownGraceMs = 45'000;
        options.scanIntervalMs = 20'000;
        options.scanFastCount = 4;
    }

    ~Fixture() {
        unlink(lockPath.c_str());
        unlink(flagPath.c_str());
        unlink(linkPath.c_str());
        rmdir(dir.c_str());
    }

    // Truncates the fake lock file so the next write can be checked alone.
    void ClearLock() { Touch(lockPath); }
    void SetLink(const char* state) { WriteText(linkPath, state); }
    // The interface disappears when Wi-Fi is switched off.
    void RemoveLink() { unlink(linkPath.c_str()); }
};

void TestFirstTickAndRefresh() {
    Fixture fixture;
    Harness harness;
    ConnectivityGuard guard(Spawn, Record, &harness, fixture.options);
    assert(guard.wakeLockState() == WakeLockState::Unknown);

    guard.Tick(1'000);
    assert(ReadFile(fixture.lockPath) == "ivrdroid-keepawake");
    assert(harness.spawns == 1);
    assert(guard.commandRunning());
    assert(guard.wakeLockState() == WakeLockState::Held);

    WaitUntilExited(harness.lastPid);
    guard.Tick(1'100);
    assert(!guard.commandRunning());
    assert(guard.wifiAssertsSucceeded() == 1);
    assert(guard.wifiAssertsFailed() == 0);
    errno = 0;
    assert(waitpid(harness.lastPid, nullptr, WNOHANG) < 0 && errno == ECHILD);

    for (int64_t now = 1'100; now < 1'100 + fixture.options.refreshIntervalMs; now += 10'000) {
        guard.Tick(now);
    }
    assert(harness.spawns == 1);

    fixture.ClearLock();
    guard.Tick(1'100 + fixture.options.refreshIntervalMs);
    assert(harness.spawns == 2);
    assert(ReadFile(fixture.lockPath) == "ivrdroid-keepawake");
    guard.Tick(1'200 + fixture.options.refreshIntervalMs);
    assert(harness.spawns == 2);
    WaitUntilExited(harness.lastPid);
    guard.Tick(1'300 + fixture.options.refreshIntervalMs);
    assert(guard.wifiAssertsSucceeded() == 2);

    assert(CountLogs(harness, 4, "Keep-awake lock held and Wi-Fi power saving switched off.") == 1);
    assert(CountLogs(harness, 5, "") == 0);
}

void TestFailureRetryAndRecovery() {
    Fixture fixture;
    Harness harness;
    harness.mode = ChildMode::ExitOne;
    ConnectivityGuard guard(Spawn, Record, &harness, fixture.options);

    guard.Tick(0);
    WaitUntilExited(harness.lastPid);
    guard.Tick(100);
    assert(guard.wifiAssertsFailed() == 1);
    guard.Tick(100 + fixture.options.retryIntervalMs - 1);
    assert(harness.spawns == 1);
    guard.Tick(100 + fixture.options.retryIntervalMs);
    assert(harness.spawns == 2);
    WaitUntilExited(harness.lastPid);
    guard.Tick(200 + fixture.options.retryIntervalMs);
    assert(guard.wifiAssertsFailed() == 2);
    // One WARN for the transition, not one per failure.
    assert(CountLogs(harness, 5, "Wi-Fi power saving") == 1);
    assert(CountLogs(harness, 4, "switched off.") == 0);

    harness.mode = ChildMode::ExitZero;
    // The second consecutive failure doubles the delay.
    guard.Tick(200 + 3 * fixture.options.retryIntervalMs - 1);
    assert(harness.spawns == 2);
    guard.Tick(200 + 3 * fixture.options.retryIntervalMs);
    assert(harness.spawns == 3);
    WaitUntilExited(harness.lastPid);
    guard.Tick(300 + 3 * fixture.options.retryIntervalMs);
    assert(guard.wifiAssertsSucceeded() == 1);
    assert(CountLogs(harness, 4, "Wi-Fi power saving switch-off recovered.") == 1);
    assert(CountLogs(harness, 4, "Keep-awake lock held and Wi-Fi power saving switched off.") == 1);
}

void TestHungChildKilled() {
    Fixture fixture;
    Harness harness;
    harness.mode = ChildMode::Sleep;
    ConnectivityGuard guard(Spawn, Record, &harness, fixture.options);

    guard.Tick(0);
    const pid_t pid = harness.lastPid;
    guard.Tick(fixture.options.commandTimeoutMs - 1);
    assert(guard.commandRunning());
    guard.Tick(fixture.options.commandTimeoutMs);
    assert(!guard.commandRunning());
    assert(guard.wifiAssertsFailed() == 1);
    errno = 0;
    assert(waitpid(pid, nullptr, WNOHANG) < 0 && errno == ECHILD);
    assert(CountLogs(harness, 5, "timed out") == 1);
    // The retry is scheduled from the kill.
    guard.Tick(fixture.options.commandTimeoutMs + fixture.options.retryIntervalMs - 1);
    assert(harness.spawns == 1);
    guard.Tick(fixture.options.commandTimeoutMs + fixture.options.retryIntervalMs);
    assert(harness.spawns == 2);
    guard.Shutdown();
}

void TestSpawnFailure() {
    Fixture fixture;
    Harness harness;
    harness.mode = ChildMode::FailSpawn;
    ConnectivityGuard guard(Spawn, Record, &harness, fixture.options);

    guard.Tick(0);
    assert(harness.spawns == 1);
    assert(!guard.commandRunning());
    assert(guard.wifiAssertsFailed() == 1);
    guard.Tick(1);
    assert(harness.spawns == 1);
    guard.Tick(fixture.options.retryIntervalMs);
    assert(harness.spawns == 2);
    assert(guard.wifiAssertsFailed() == 2);
    assert(CountLogs(harness, 5, "Wi-Fi power saving") == 1);

    ConnectivityGuard nullSpawn(nullptr, nullptr, nullptr, fixture.options);
    nullSpawn.Tick(0);
    assert(nullSpawn.wifiAssertsFailed() == 1);
}

void TestDisableFlag() {
    Fixture fixture;
    Harness harness;
    ConnectivityGuard guard(Spawn, Record, &harness, fixture.options);
    unlink(fixture.lockPath.c_str());
    Touch(fixture.flagPath);

    guard.AssertWakeLock();
    for (int64_t now = 0; now < 3 * fixture.options.refreshIntervalMs; now += 10'000) {
        guard.Tick(now);
    }
    struct stat info {};
    assert(stat(fixture.lockPath.c_str(), &info) != 0);
    assert(harness.spawns == 0);
    assert(guard.wakeLockState() == WakeLockState::Disabled);
    assert(CountLogs(harness, 4, "found") == 1);

    Touch(fixture.lockPath);
    unlink(fixture.flagPath.c_str());
    const int64_t resume = 3 * fixture.options.refreshIntervalMs;
    guard.Tick(resume);
    assert(CountLogs(harness, 4, "removed") == 1);
    assert(harness.spawns == 1);
    assert(ReadFile(fixture.lockPath) == "ivrdroid-keepawake");
    assert(guard.wakeLockState() == WakeLockState::Held);

    // A flag that appears while the command runs still lets the child be reaped.
    Touch(fixture.flagPath);
    const pid_t pid = harness.lastPid;
    WaitUntilExited(pid);
    guard.Tick(resume + 100);
    assert(!guard.commandRunning());
    assert(guard.wifiAssertsSucceeded() == 1);
    errno = 0;
    assert(waitpid(pid, nullptr, WNOHANG) < 0 && errno == ECHILD);
    assert(CountLogs(harness, 4, "found") == 2);
    guard.Tick(resume + 2 * fixture.options.refreshIntervalMs);
    assert(harness.spawns == 1);
}

void TestUnwritableLock() {
    Fixture fixture;
    Harness harness;
    ConnectivityGuardOptions options = fixture.options;
    const std::string missing = fixture.dir + "/missing/wake_lock";
    options.wakeLockPath = missing.c_str();
    options.refreshIntervalMs = options.retryIntervalMs;
    ConnectivityGuard guard(Spawn, Record, &harness, options);

    int64_t now = 0;
    for (int i = 0; i < 5; ++i) {
        guard.AssertWakeLock();
        guard.Tick(now);
        assert(harness.spawns == i + 1);
        WaitUntilExited(harness.lastPid);
        guard.Tick(now + 100);
        now += 100 + options.retryIntervalMs;
    }
    assert(guard.wakeLockState() == WakeLockState::Failed);
    assert(CountLogs(harness, 5, "keep-awake lock") == 1);
    assert(CountLogs(harness, 4, "switched off.") == 0);

    const std::string parent = fixture.dir + "/missing";
    assert(mkdir(parent.c_str(), 0700) == 0);
    Touch(missing);
    guard.AssertWakeLock();
    guard.AssertWakeLock();
    assert(guard.wakeLockState() == WakeLockState::Held);
    assert(CountLogs(harness, 4, "Keep-awake lock recovered.") == 1);
    assert(CountLogs(harness, 4, "Keep-awake lock held and Wi-Fi power saving switched off.") == 1);
    unlink(missing.c_str());
    rmdir(parent.c_str());
}

void TestChildLost() {
    Fixture fixture;
    Harness harness;
    ConnectivityGuard guard(Spawn, Record, &harness, fixture.options);
    guard.Tick(0);
    const pid_t pid = harness.lastPid;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) assert(errno == EINTR);
    guard.Tick(100);
    assert(!guard.commandRunning());
    assert(guard.wifiAssertsFailed() == 1);
    assert(guard.wifiAssertsSucceeded() == 0);
    assert(CountLogs(harness, 5, "child lost") == 1);
    guard.Tick(100 + fixture.options.retryIntervalMs - 1);
    assert(harness.spawns == 1);
    guard.Tick(100 + fixture.options.retryIntervalMs);
    assert(harness.spawns == 2);
    WaitUntilExited(harness.lastPid);
    guard.Tick(200 + fixture.options.retryIntervalMs);
    assert(guard.wifiAssertsSucceeded() == 1);
}

// Runs one assert that ends at now + 10 and returns the time of the result.
int64_t RunOnce(ConnectivityGuard& guard, Harness& harness, int64_t now, int expectedSpawns) {
    guard.Tick(now);
    assert(harness.spawns == expectedSpawns);
    WaitUntilExited(harness.lastPid);
    guard.Tick(now + 10);
    assert(!guard.commandRunning());
    return now + 10;
}

void TestBackoff() {
    Fixture fixture;
    Harness harness;
    harness.mode = ChildMode::ExitOne;
    ConnectivityGuard guard(Spawn, Record, &harness, fixture.options);
    const int64_t expected[] = {30, 60, 120, 240, 480, 900, 900};

    int spawns = 1;
    int64_t resultAt = RunOnce(guard, harness, 0, spawns);
    for (const int64_t seconds : expected) {
        const int64_t due = resultAt + seconds * 1'000;
        guard.Tick(due - 1);
        assert(harness.spawns == spawns);
        resultAt = RunOnce(guard, harness, due, ++spawns);
    }
    assert(guard.wifiAssertsFailed() == 8);
    assert(CountLogs(harness, 5, "retrying with backoff") == 1);

    // A success resets the backoff: the next failure retries after 30 s.
    harness.mode = ChildMode::ExitZero;
    resultAt = RunOnce(guard, harness, resultAt + 900'000, ++spawns);
    assert(guard.wifiAssertsSucceeded() == 1);
    harness.mode = ChildMode::ExitOne;
    resultAt = RunOnce(guard, harness, resultAt + fixture.options.refreshIntervalMs, ++spawns);
    guard.Tick(resultAt + 30'000 - 1);
    assert(harness.spawns == spawns);
    guard.Tick(resultAt + 30'000);
    assert(harness.spawns == spawns + 1);
    WaitUntilExited(harness.lastPid);
    guard.Tick(resultAt + 30'010);

    // Removing the disable flag asserts at once but keeps the backoff: after
    // that third consecutive failure the next delay is 120 s, not 30 s.
    Touch(fixture.flagPath);
    guard.Tick(resultAt + 30'020);
    unlink(fixture.flagPath.c_str());
    ++spawns;
    resultAt = RunOnce(guard, harness, resultAt + 30'030, ++spawns);
    guard.Tick(resultAt + 120'000 - 1);
    assert(harness.spawns == spawns);
    guard.Tick(resultAt + 120'000);
    assert(harness.spawns == spawns + 1);
    guard.Shutdown();
}

void TestShutdownReaps() {
    Fixture fixture;
    Harness harness;
    harness.mode = ChildMode::Sleep;
    ConnectivityGuard guard(Spawn, Record, &harness, fixture.options);
    guard.Tick(0);
    const pid_t pid = harness.lastPid;
    assert(guard.commandRunning());
    guard.Shutdown();
    assert(!guard.commandRunning());
    errno = 0;
    assert(waitpid(pid, nullptr, WNOHANG) < 0 && errno == ECHILD);
    // Shutdown leaves the lock alone.
    assert(ReadFile(fixture.lockPath) == "ivrdroid-keepawake");
    guard.Shutdown();
}

void TestStatsReset() {
    Fixture fixture;
    Harness harness;
    ConnectivityGuardOptions options = fixture.options;
    options.refreshIntervalMs = 1'000;
    options.retryIntervalMs = 1'000;
    ConnectivityGuard guard(Spawn, Record, &harness, options);

    int64_t now = 0;
    for (int i = 0; i < 3; ++i) {
        guard.Tick(now);
        WaitUntilExited(harness.lastPid);
        guard.Tick(now + 10);
        now += 1'010;
    }
    harness.mode = ChildMode::ExitOne;
    for (int i = 0; i < 2; ++i) {
        guard.Tick(now);
        WaitUntilExited(harness.lastPid);
        guard.Tick(now + 10);
        now += 1'010;
    }
    assert(guard.wifiAssertsSucceeded() == 3);
    assert(guard.wifiAssertsFailed() == 2);
    guard.ResetStats();
    assert(guard.wifiAssertsSucceeded() == 0);
    assert(guard.wifiAssertsFailed() == 0);
    assert(guard.wakeLockState() == WakeLockState::Held);
    assert(std::strcmp(ivrdroid::WakeLockStateName(guard.wakeLockState()), "held") == 0);
    assert(std::strcmp(ivrdroid::WakeLockStateName(WakeLockState::Disabled), "disabled") == 0);
}

// Completes the first high-performance assert at 0..10; the next is due at 10 + refreshIntervalMs.
void Settle(ConnectivityGuard& guard, Harness& harness) {
    guard.Tick(0);
    assert(harness.spawns == 1);
    WaitUntilExited(harness.lastPid);
    guard.Tick(10);
    assert(guard.wifiAssertsSucceeded() == 1);
}

// Expects a scan request at exactly now and reaps it at now + 10.
void ScanAt(ConnectivityGuard& guard, Harness& harness, int64_t now) {
    const int before = harness.scans;
    guard.Tick(now - 1);
    assert(harness.scans == before);
    guard.Tick(now);
    assert(harness.scans == before + 1);
    if (guard.commandRunning()) {
        WaitUntilExited(harness.lastScanPid);
        guard.Tick(now + 10);
        assert(!guard.commandRunning());
    }
}

void TestLinkUpNeverScans() {
    Fixture fixture;
    Harness harness;
    ConnectivityGuard guard(Spawn, Record, &harness, fixture.options);
    Settle(guard, harness);
    for (int64_t now = 5'000; now < 2 * fixture.options.refreshIntervalMs; now += 5'000) {
        guard.Tick(now);
        if (guard.commandRunning()) {
            WaitUntilExited(harness.lastPid);
            guard.Tick(now + 10);
        }
    }
    assert(harness.scans == 0);
    assert(guard.wifiLinkDrops() == 0);
    assert(guard.wifiScanRequests() == 0);
    assert(CountLogs(harness, 5, "link") == 0);
}

void TestScanAfterGrace() {
    Fixture fixture;
    Harness harness;
    ConnectivityGuard guard(Spawn, Record, &harness, fixture.options);
    Settle(guard, harness);
    fixture.SetLink("down\n");
    for (int64_t now = 5'000; now < 50'000; now += 1'000) guard.Tick(now);
    assert(harness.scans == 0);
    assert(guard.wifiLinkDrops() == 0);

    ScanAt(guard, harness, 50'000);
    assert(guard.wifiLinkDrops() == 1);
    assert(guard.wifiScanRequests() == 1);
    assert(guard.wifiScanRequestsFailed() == 0);
    assert(CountLogs(harness, 5, "Wi-Fi link down for 45 s; asking Android to scan.") == 1);
    guard.Tick(69'999);
    assert(harness.scans == 1);
    ScanAt(guard, harness, 70'000);
    assert(guard.wifiLinkDrops() == 1);
    assert(CountLogs(harness, 5, "Wi-Fi link down") == 1);
    assert(harness.spawns == 1);
    assert(guard.wifiAssertsSucceeded() == 1);
}

void TestScanSpacing() {
    Fixture fixture;
    Harness harness;
    // A failing spawn never occupies the child slot, so only scans use it.
    harness.mode = ChildMode::FailSpawn;
    ConnectivityGuardOptions options = fixture.options;
    options.refreshIntervalMs = 160'000;
    ConnectivityGuard guard(Spawn, Record, &harness, options);
    fixture.SetLink("down\n");
    guard.Tick(0);
    const int64_t gaps[] = {20, 20, 20, 40, 80, 160, 160, 160};

    int64_t at = options.linkDownGraceMs;
    ScanAt(guard, harness, at);
    for (const int64_t seconds : gaps) {
        at += seconds * 1'000;
        ScanAt(guard, harness, at);
    }
    assert(harness.scans == 9);
    assert(guard.wifiScanRequests() == 9);
    assert(guard.wifiScanRequestsFailed() == 0);
    assert(guard.wifiLinkDrops() == 1);
}

void TestShortBlipNoScan() {
    Fixture fixture;
    Harness harness;
    ConnectivityGuard guard(Spawn, Record, &harness, fixture.options);
    Settle(guard, harness);
    // After the link-triggered floor that the assert at 0 started.
    fixture.SetLink("down\n");
    guard.Tick(65'000);
    guard.Tick(90'000);
    assert(harness.spawns == 1);
    fixture.SetLink("up\n");
    guard.Tick(95'000);
    // The assert is due at once although the refresh is 15 minutes away.
    assert(harness.spawns == 2);
    WaitUntilExited(harness.lastPid);
    guard.Tick(95'010);
    assert(guard.wifiAssertsSucceeded() == 2);
    for (int64_t now = 100'000; now < 260'000; now += 5'000) guard.Tick(now);
    assert(harness.scans == 0);
    assert(guard.wifiLinkDrops() == 0);
    assert(CountLogs(harness, 4, "link back") == 0);
    assert(CountLogs(harness, 5, "link down") == 0);
}

void TestOutageEndsAndRepeats() {
    Fixture fixture;
    Harness harness;
    ConnectivityGuard guard(Spawn, Record, &harness, fixture.options);
    Settle(guard, harness);
    fixture.SetLink("down\n");
    guard.Tick(5'000);
    ScanAt(guard, harness, 50'000);
    ScanAt(guard, harness, 70'000);
    ScanAt(guard, harness, 90'000);
    ScanAt(guard, harness, 110'000);
    ScanAt(guard, harness, 150'000);
    assert(harness.spawns == 1);

    fixture.SetLink("up\n");
    guard.Tick(155'000);
    assert(CountLogs(harness, 4, "Wi-Fi link back after 150 s (5 scan requests).") == 1);
    assert(harness.spawns == 2);
    WaitUntilExited(harness.lastPid);
    guard.Tick(155'010);
    for (int64_t now = 160'000; now < 760'000; now += 5'000) guard.Tick(now);
    assert(harness.scans == 5);

    // After ten minutes of stable link a new outage counts a second drop and
    // starts from the fast spacing again.
    fixture.SetLink("dormant\n");
    guard.Tick(760'000);
    ScanAt(guard, harness, 805'000);
    assert(guard.wifiLinkDrops() == 2);
    assert(CountLogs(harness, 5, "Wi-Fi link down for 45 s") == 2);
    ScanAt(guard, harness, 825'000);
    fixture.SetLink("up\n");
    guard.Tick(830'000);
    assert(CountLogs(harness, 4, "Wi-Fi link back after 70 s (2 scan requests).") == 1);
    assert(CountLogs(harness, 4, "link back") == 2);
    assert(harness.spawns == 3);
    assert(guard.wifiScanRequests() == 7);
    guard.Shutdown();
}

void TestLinkAbsent() {
    Fixture fixture;
    Harness harness;
    ConnectivityGuard guard(Spawn, Record, &harness, fixture.options);
    Settle(guard, harness);
    fixture.RemoveLink();
    for (int64_t now = 5'000; now < 300'000; now += 5'000) guard.Tick(now);
    assert(harness.scans == 0);
    assert(harness.spawns == 1);

    // Down, then absent before the grace ends: the next outage starts over.
    fixture.SetLink("down\n");
    guard.Tick(300'000);
    fixture.RemoveLink();
    guard.Tick(340'000);
    fixture.SetLink("down\n");
    guard.Tick(345'000);
    guard.Tick(350'000);
    assert(harness.scans == 0);
    ScanAt(guard, harness, 390'000);
    assert(guard.wifiLinkDrops() == 1);

    // Absent after scans ends the outage silently; Absent to Up asserts at once.
    fixture.RemoveLink();
    guard.Tick(395'000);
    guard.Tick(500'000);
    assert(harness.scans == 1);
    assert(harness.spawns == 1);
    fixture.SetLink("up");
    guard.Tick(505'000);
    assert(harness.spawns == 2);
    assert(CountLogs(harness, 4, "link back") == 0);
    guard.Shutdown();
}

void TestDisableFlagStopsScans() {
    Fixture fixture;
    Harness harness;
    Touch(fixture.flagPath);
    fixture.SetLink("down\n");
    ConnectivityGuard guard(Spawn, Record, &harness, fixture.options);
    for (int64_t now = 0; now < 2 * fixture.options.refreshIntervalMs; now += 5'000) guard.Tick(now);
    assert(harness.scans == 0);
    assert(harness.spawns == 0);
    assert(guard.wifiLinkDrops() == 0);
    assert(guard.wifiScanRequests() == 0);
}

void TestSingleChild() {
    Fixture fixture;
    {
        // A running scan holds back a due assert; a killed scan touches only the scan counters.
        Harness harness;
        harness.scanMode = ChildMode::Sleep;
        ConnectivityGuardOptions options = fixture.options;
        options.refreshIntervalMs = 60'000;
        options.linkDownGraceMs = 53'000;
        ConnectivityGuard guard(Spawn, Record, &harness, options);
        Settle(guard, harness);
        fixture.SetLink("down\n");
        guard.Tick(5'000);
        guard.Tick(58'000);
        assert(harness.scans == 1);
        const pid_t scanPid = harness.lastScanPid;
        guard.Tick(60'010);
        assert(harness.spawns == 1);
        assert(guard.commandRunning());
        guard.Tick(62'999);
        assert(guard.commandRunning());
        guard.Tick(63'000);
        errno = 0;
        assert(waitpid(scanPid, nullptr, WNOHANG) < 0 && errno == ECHILD);
        assert(guard.wifiScanRequestsFailed() == 1);
        assert(guard.wifiAssertsFailed() == 0);
        assert(guard.wifiAssertsSucceeded() == 1);
        assert(CountLogs(harness, 5, "timed out") == 0);
        // The assert that was due goes ahead on the same Tick.
        assert(harness.spawns == 2);
        WaitUntilExited(harness.lastPid);
        guard.Tick(63'010);
        assert(guard.wifiAssertsSucceeded() == 2);

        // Shutdown kills a running scan as well.
        guard.Tick(78'000);
        assert(harness.scans == 2);
        const pid_t secondScan = harness.lastScanPid;
        guard.Shutdown();
        assert(!guard.commandRunning());
        errno = 0;
        assert(waitpid(secondScan, nullptr, WNOHANG) < 0 && errno == ECHILD);
    }
    {
        // A running assert holds back a due scan; a scan result leaves the backoff alone.
        Harness harness;
        harness.mode = ChildMode::Sleep;
        fixture.SetLink("down\n");
        ConnectivityGuardOptions options = fixture.options;
        options.linkDownGraceMs = 2'000;
        ConnectivityGuard guard(Spawn, Record, &harness, options);
        guard.Tick(0);
        assert(harness.spawns == 1);
        guard.Tick(2'000);
        assert(harness.scans == 0);
        guard.Tick(5'000);
        assert(guard.wifiAssertsFailed() == 1);
        assert(harness.scans == 1);
        WaitUntilExited(harness.lastScanPid);
        guard.Tick(5'010);
        assert(guard.wifiScanRequests() == 1);
        assert(guard.wifiScanRequestsFailed() == 0);
        assert(guard.wifiAssertsFailed() == 1);
        assert(guard.wifiAssertsSucceeded() == 0);
        ScanAt(guard, harness, 25'000);
        guard.Tick(34'999);
        assert(harness.spawns == 1);
        guard.Tick(35'000);
        assert(harness.spawns == 2);
        assert(CountLogs(harness, 5, "Wi-Fi power saving") == 1);
        guard.Shutdown();
    }
}

void TestScanSpawnFailure() {
    Fixture fixture;
    Harness harness;
    harness.scanMode = ChildMode::FailSpawn;
    ConnectivityGuard guard(Spawn, Record, &harness, fixture.options);
    Settle(guard, harness);
    fixture.SetLink("down\n");
    guard.Tick(5'000);
    guard.Tick(50'000);
    assert(harness.scans == 1);
    assert(!guard.commandRunning());
    assert(guard.wifiScanRequests() == 1);
    assert(guard.wifiScanRequestsFailed() == 1);
    assert(guard.wifiLinkDrops() == 1);
    guard.Tick(50'001);
    guard.Tick(69'999);
    assert(harness.scans == 1);
    guard.Tick(70'000);
    assert(harness.scans == 2);
    assert(guard.wifiScanRequestsFailed() == 2);
    assert(guard.wifiAssertsFailed() == 0);
    assert(guard.wifiAssertsSucceeded() == 1);
}

void TestLinkPollThrottled() {
    Fixture fixture;
    Harness harness;
    ConnectivityGuard guard(Spawn, Record, &harness, fixture.options);
    Settle(guard, harness);
    fixture.SetLink("down\n");
    // Not polled until 5 s after the poll at 0, so the grace runs from 5'000.
    guard.Tick(4'999);
    guard.Tick(5'000);
    ScanAt(guard, harness, 50'000);
    ScanAt(guard, harness, 70'000);

    // ScanAt last polled at 69'999, past the link-triggered floor.
    fixture.SetLink("up\n");
    guard.Tick(74'998);
    assert(harness.spawns == 1);
    assert(CountLogs(harness, 4, "link back") == 0);
    guard.Tick(74'999);
    assert(harness.spawns == 2);
    assert(CountLogs(harness, 4, "Wi-Fi link back after 69 s (2 scan requests).") == 1);
    guard.Shutdown();
}

void TestScanStatsReset() {
    Fixture fixture;
    Harness harness;
    harness.scanMode = ChildMode::ExitOne;
    ConnectivityGuard guard(Spawn, Record, &harness, fixture.options);
    Settle(guard, harness);
    fixture.SetLink("down\n");
    guard.Tick(5'000);
    ScanAt(guard, harness, 50'000);
    harness.scanMode = ChildMode::ExitZero;
    ScanAt(guard, harness, 70'000);
    assert(guard.wifiLinkDrops() == 1);
    assert(guard.wifiScanRequests() == 2);
    assert(guard.wifiScanRequestsFailed() == 1);
    guard.ResetStats();
    assert(guard.wifiLinkDrops() == 0);
    assert(guard.wifiScanRequests() == 0);
    assert(guard.wifiScanRequestsFailed() == 0);
    // The outage itself goes on: no second drop and no second warning.
    ScanAt(guard, harness, 90'000);
    assert(guard.wifiLinkDrops() == 0);
    assert(guard.wifiScanRequests() == 1);
    assert(CountLogs(harness, 5, "Wi-Fi link down") == 1);
}

// Up while an assert runs: the flag outlives that child and pulls the next
// assert in, but no sooner than linkReassertMinIntervalMs after the last one.
void TestLinkUpDuringAssert() {
    Fixture fixture;
    ConnectivityGuardOptions options = fixture.options;
    options.commandTimeoutMs = 20'000;
    const std::string release = fixture.dir + "/release";
    {
        // The floor has passed when the running child is reaped.
        Harness harness;
        harness.releasePath = release;
        options.linkReassertMinIntervalMs = 1'000;
        ConnectivityGuard guard(Spawn, Record, &harness, options);
        Settle(guard, harness);
        harness.mode = ChildMode::WaitForRelease;
        fixture.SetLink("down\n");
        guard.Tick(5'000);
        fixture.SetLink("up\n");
        guard.Tick(10'000);
        assert(harness.spawns == 2);
        fixture.SetLink("down\n");
        guard.Tick(15'000);
        fixture.SetLink("up\n");
        guard.Tick(20'000);
        assert(harness.spawns == 2);
        harness.mode = ChildMode::ExitZero;
        Touch(release);
        WaitUntilExited(harness.lastPid);
        // The success schedules the next refresh 15 minutes out; the flag wins.
        guard.Tick(20'010);
        assert(guard.wifiAssertsSucceeded() == 2);
        assert(harness.spawns == 3);
        WaitUntilExited(harness.lastPid);
        guard.Tick(20'020);
        unlink(release.c_str());
    }
    {
        // The floor has not passed: the assert waits until exactly the allowed time.
        Harness harness;
        harness.releasePath = release;
        options.linkReassertMinIntervalMs = 5 * 60'000;
        ConnectivityGuard guard(Spawn, Record, &harness, options);
        Settle(guard, harness);
        harness.mode = ChildMode::WaitForRelease;
        fixture.SetLink("down\n");
        guard.Tick(5'000);
        fixture.SetLink("up\n");
        // The assert at 0 was a refresh, so this first link-up is not delayed.
        guard.Tick(10'000);
        assert(harness.spawns == 2);
        fixture.SetLink("down\n");
        guard.Tick(15'000);
        fixture.SetLink("up\n");
        guard.Tick(20'000);
        harness.mode = ChildMode::ExitZero;
        Touch(release);
        WaitUntilExited(harness.lastPid);
        guard.Tick(20'010);
        assert(guard.wifiAssertsSucceeded() == 2);
        assert(harness.spawns == 2);
        guard.Tick(309'999);
        assert(harness.spawns == 2);
        guard.Tick(310'000);
        assert(harness.spawns == 3);
        guard.Shutdown();
        unlink(release.c_str());
    }
}

// Runs a tick every 5 s from first to last, writing the link state that
// downAt reports, reaping each child at once and recording the spawn times
// of both commands.
template <typename DownAt>
void RunLinkPattern(ConnectivityGuard& guard, Fixture& fixture, Harness& harness,
    int64_t first, int64_t last, DownAt downAt,
    std::vector<int64_t>* assertTimes, std::vector<int64_t>* scanTimes) {
    for (int64_t now = first; now <= last; now += 5'000) {
        fixture.SetLink(downAt(now) ? "down\n" : "up\n");
        const int spawns = harness.spawns;
        const int scans = harness.scans;
        guard.Tick(now);
        if (harness.spawns != spawns) assertTimes->push_back(now);
        if (harness.scans != scans) scanTimes->push_back(now);
        if (guard.commandRunning()) {
            WaitUntilExited(harness.spawns != spawns ? harness.lastPid : harness.lastScanPid);
            guard.Tick(now + 10);
        }
    }
}

void TestFlappingLinkRateLimited() {
    Fixture fixture;
    Harness harness;
    ConnectivityGuard guard(Spawn, Record, &harness, fixture.options);
    Settle(guard, harness);
    std::vector<int64_t> asserts;
    std::vector<int64_t> scans;
    // Down and up on alternate polls: up at 10 s, 20 s, ...
    RunLinkPattern(guard, fixture, harness, 5'000, 600'000,
        [](int64_t now) { return (now / 5'000) % 2 == 1; }, &asserts, &scans);
    // The first link-up at once, then one per five minutes, not one per flap.
    assert((asserts == std::vector<int64_t>{10'000, 310'000}));
    assert(scans.empty());
}

// The reviewer's pattern: down 50 s, up 10 s, for 72 minutes. Cycle k starts
// at c = 60 s + 60k s; the link is down at c .. c + 45 s and up at c + 50 s,
// c + 55 s. Relative to the first cycle the pacing gives scans at 45, 105,
// 165, 225 s (20 s apart, kept apart by the grace), then 285 (+40), 405 (+80
// falls in an up phase, next grace), 585 (+160), 945 (+320), 1605 (+640),
// 2505, 3405, 4305 (+900 cap): 12 scans, against 72 when every outage started
// the pacing afresh. Link-ups toggle at 50 s and then every 300 s while the
// flag is pending: 50, 350, ..., 4250 s, 15 toggles; each success pushes the
// refresh past the next one, so no refresh runs.
void TestDownFiftyUpTen() {
    Fixture fixture;
    Harness harness;
    ConnectivityGuard guard(Spawn, Record, &harness, fixture.options);
    Settle(guard, harness);
    std::vector<int64_t> asserts;
    std::vector<int64_t> scans;
    const int64_t start = 60'000;
    RunLinkPattern(guard, fixture, harness, 5'000, start + 72 * 60'000 - 5'000,
        [&](int64_t now) { return now >= start && (now - start) % 60'000 < 50'000; },
        &asserts, &scans);

    std::vector<int64_t> expectedScans;
    for (const int64_t seconds : {45, 105, 165, 225, 285, 405, 585, 945, 1605, 2505, 3405, 4305}) {
        expectedScans.push_back(start + seconds * 1'000);
    }
    assert(scans == expectedScans);
    assert(guard.wifiScanRequests() == 12);
    assert(guard.wifiLinkDrops() == 12);
    std::vector<int64_t> expectedAsserts;
    for (int64_t seconds = 50; seconds <= 4'250; seconds += 300) {
        expectedAsserts.push_back(start + seconds * 1'000);
    }
    assert(expectedAsserts.size() == 15);
    assert(asserts == expectedAsserts);
}

void TestAbsentKeepsPacing() {
    Fixture fixture;
    Harness harness;
    ConnectivityGuard guard(Spawn, Record, &harness, fixture.options);
    Settle(guard, harness);
    fixture.SetLink("down\n");
    guard.Tick(5'000);
    ScanAt(guard, harness, 50'000);
    ScanAt(guard, harness, 70'000);
    ScanAt(guard, harness, 90'000);
    ScanAt(guard, harness, 110'000);
    ScanAt(guard, harness, 150'000);
    // The next scan is allowed at 230 s; a fresh pacing would scan at 205 s.
    fixture.RemoveLink();
    guard.Tick(155'000);
    fixture.SetLink("down\n");
    guard.Tick(160'000);
    guard.Tick(205'000);
    assert(harness.scans == 5);
    ScanAt(guard, harness, 230'000);
    assert(guard.wifiLinkDrops() == 2);

    // Even a long spell without Wi-Fi is not a stable link: after the next
    // scan the spacing goes on doubling (320 s) instead of 20 s.
    fixture.RemoveLink();
    guard.Tick(235'000);
    fixture.SetLink("down\n");
    // The refresh falls due in the gap; let it finish.
    guard.Tick(1'000'000);
    assert(harness.spawns == 2);
    WaitUntilExited(harness.lastPid);
    guard.Tick(1'000'010);
    ScanAt(guard, harness, 1'045'000);
    guard.Tick(1'065'000);
    assert(harness.scans == 7);
    ScanAt(guard, harness, 1'365'000);
}

void TestRefreshDoesNotDelayLinkUp() {
    Fixture fixture;
    Harness harness;
    ConnectivityGuardOptions options = fixture.options;
    options.refreshIntervalMs = 100'000;
    ConnectivityGuard guard(Spawn, Record, &harness, options);
    Settle(guard, harness);
    guard.Tick(100'010);
    assert(harness.spawns == 2);
    WaitUntilExited(harness.lastPid);
    guard.Tick(100'020);
    fixture.SetLink("down\n");
    guard.Tick(105'010);
    fixture.SetLink("up\n");
    guard.Tick(110'010);
    assert(harness.spawns == 3);
    guard.Shutdown();
}

void TestFailedLinkUpSpawnMovesFloor() {
    Fixture fixture;
    Harness harness;
    ConnectivityGuard guard(Spawn, Record, &harness, fixture.options);
    Settle(guard, harness);
    harness.mode = ChildMode::FailSpawn;
    fixture.SetLink("down\n");
    guard.Tick(5'000);
    fixture.SetLink("up\n");
    guard.Tick(10'000);
    assert(harness.spawns == 2);
    assert(guard.wifiAssertsFailed() == 1);
    fixture.SetLink("down\n");
    guard.Tick(15'000);
    fixture.SetLink("up\n");
    guard.Tick(20'000);
    assert(harness.spawns == 2);
}

void TestScheduledAssertClearsLinkFlag() {
    Fixture fixture;
    Harness harness;
    ConnectivityGuardOptions options = fixture.options;
    options.refreshIntervalMs = 200'000;
    ConnectivityGuard guard(Spawn, Record, &harness, options);
    Settle(guard, harness);
    fixture.SetLink("down\n");
    guard.Tick(5'000);
    fixture.SetLink("up\n");
    guard.Tick(10'000);
    assert(harness.spawns == 2);
    WaitUntilExited(harness.lastPid);
    guard.Tick(10'010);
    // Pending until the floor at 310 s; the refresh at 210'010 satisfies it.
    fixture.SetLink("down\n");
    guard.Tick(15'000);
    fixture.SetLink("up\n");
    guard.Tick(20'000);
    assert(harness.spawns == 2);
    guard.Tick(210'010);
    assert(harness.spawns == 3);
    WaitUntilExited(harness.lastPid);
    guard.Tick(210'020);
    for (int64_t now = 211'000; now < 400'000; now += 1'000) guard.Tick(now);
    assert(harness.spawns == 3);
}

void TestDisabledStillTracksLink() {
    Fixture fixture;
    Harness harness;
    ConnectivityGuard guard(Spawn, Record, &harness, fixture.options);
    Settle(guard, harness);
    Touch(fixture.flagPath);
    fixture.SetLink("down\n");
    guard.Tick(5'000);
    fixture.SetLink("up\n");
    guard.Tick(10'000);
    fixture.SetLink("down\n");
    guard.Tick(15'000);
    unlink(fixture.flagPath.c_str());
    guard.Tick(20'000);
    assert(harness.spawns == 2);
    WaitUntilExited(harness.lastPid);
    guard.Tick(20'010);
    // The second outage has its own grace, from 15 s.
    ScanAt(guard, harness, 60'000);
    assert(guard.wifiLinkDrops() == 1);
    assert(CountLogs(harness, 5, "Wi-Fi link down for 45 s") == 1);
}

void TestScanChildLost() {
    Fixture fixture;
    Harness harness;
    ConnectivityGuard guard(Spawn, Record, &harness, fixture.options);
    Settle(guard, harness);
    fixture.SetLink("down\n");
    guard.Tick(5'000);
    guard.Tick(50'000);
    assert(harness.scans == 1);
    int status = 0;
    while (waitpid(harness.lastScanPid, &status, 0) < 0) assert(errno == EINTR);
    guard.Tick(50'010);
    assert(!guard.commandRunning());
    assert(guard.wifiScanRequestsFailed() == 1);
    assert(guard.wifiAssertsFailed() == 0);
    assert(guard.wifiAssertsSucceeded() == 1);
    assert(CountLogs(harness, 5, "child lost") == 0);
    // Wi-Fi switched off: no scans or link-ups, so only the refresh remains.
    fixture.RemoveLink();
    guard.Tick(55'000);
    guard.Tick(fixture.options.refreshIntervalMs + 9);
    assert(harness.spawns == 1);
    guard.Tick(fixture.options.refreshIntervalMs + 10);
    assert(harness.spawns == 2);
    guard.Shutdown();
}

void TestEmptyLinkStateIsUnknown() {
    Fixture fixture;
    Harness harness;
    ConnectivityGuard guard(Spawn, Record, &harness, fixture.options);
    Settle(guard, harness);
    fixture.SetLink("");
    for (int64_t now = 5'000; now < 300'000; now += 5'000) guard.Tick(now);
    assert(harness.scans == 0);
    assert(guard.wifiLinkDrops() == 0);
}

}  // namespace

int main() {
    TestFirstTickAndRefresh();
    TestFailureRetryAndRecovery();
    TestHungChildKilled();
    TestSpawnFailure();
    TestDisableFlag();
    TestUnwritableLock();
    TestChildLost();
    TestBackoff();
    TestShutdownReaps();
    TestStatsReset();
    TestLinkUpNeverScans();
    TestScanAfterGrace();
    TestScanSpacing();
    TestShortBlipNoScan();
    TestOutageEndsAndRepeats();
    TestLinkAbsent();
    TestDisableFlagStopsScans();
    TestSingleChild();
    TestScanSpawnFailure();
    TestLinkPollThrottled();
    TestScanStatsReset();
    TestLinkUpDuringAssert();
    TestFlappingLinkRateLimited();
    TestDownFiftyUpTen();
    TestAbsentKeepsPacing();
    TestRefreshDoesNotDelayLinkUp();
    TestFailedLinkUpSpawnMovesFloor();
    TestScheduledAssertClearsLinkFlag();
    TestDisabledStillTracksLink();
    TestScanChildLost();
    TestEmptyLinkStateIsUnknown();
    std::puts("connectivity_guard_test: ok");
    return 0;
}
