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

using ivrdroid::ConnectivityGuard;
using ivrdroid::ConnectivityGuardOptions;
using ivrdroid::WakeLockState;

enum class ChildMode {
    ExitZero,
    ExitOne,
    Sleep,
    FailSpawn,
};

struct LogLine {
    int priority;
    std::string message;
};

struct Harness {
    ChildMode mode = ChildMode::ExitZero;
    int spawns = 0;
    pid_t lastPid = -1;
    std::vector<LogLine> logs;
};

pid_t Spawn(void* context) {
    auto* harness = static_cast<Harness*>(context);
    ++harness->spawns;
    if (harness->mode == ChildMode::FailSpawn) return -1;
    const pid_t child = fork();
    if (child == 0) {
        switch (harness->mode) {
            case ChildMode::ExitZero: _exit(0);
            case ChildMode::ExitOne: _exit(1);
            case ChildMode::Sleep:
                for (;;) pause();
            case ChildMode::FailSpawn: _exit(2);
        }
        _exit(3);
    }
    harness->lastPid = child;
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

struct Fixture {
    std::string dir;
    std::string lockPath;
    std::string flagPath;
    ConnectivityGuardOptions options;

    Fixture() {
        char pattern[] = "/tmp/ivrdroid-guard-test.XXXXXX";
        const char* created = mkdtemp(pattern);
        assert(created != nullptr);
        dir = created;
        lockPath = dir + "/wake_lock";
        flagPath = dir + "/keep-awake.disabled";
        Touch(lockPath);
        options.wakeLockPath = lockPath.c_str();
        options.disableFlagPath = flagPath.c_str();
        options.retryIntervalMs = 30'000;
        options.refreshIntervalMs = 15 * 60'000;
        options.commandTimeoutMs = 5'000;
    }

    ~Fixture() {
        unlink(lockPath.c_str());
        unlink(flagPath.c_str());
        rmdir(dir.c_str());
    }

    // Truncates the fake lock file so the next write can be checked alone.
    void ClearLock() { Touch(lockPath); }
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
    std::puts("connectivity_guard_test: ok");
    return 0;
}
