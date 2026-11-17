#include "binder_service_dump.h"
#include "service_dump_worker.h"

#include <atomic>
#include <cassert>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <new>
#include <string>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using ivrdroid::ServiceDumpClient;
using ivrdroid::ServiceDumpClientOptions;
using ivrdroid::ServiceDumpResult;

enum Mode : int {
    kModeSequence,
    kModeLarge,
    kModeOverflow,
    kModeSleep,
    kModeReturnFalse,
    kModeDie,
    kModeLateReply,
};

constexpr size_t kLargeBytes = 300 * 1024;
constexpr size_t kMaximumBytes = 512 * 1024;
constexpr size_t kOverflowBytes = kMaximumBytes + 4096;

// Shared with forked fake workers so a long-lived worker sees mode changes.
struct SharedState {
    std::atomic<int> mode;
    std::atomic<int> dumps;
};
SharedState* gShared = nullptr;

struct SpawnContext {
    bool available = true;
    bool failSpawn = false;
    pid_t lastPid = -1;
};

int64_t NowMs() {
    timespec value {};
    clock_gettime(CLOCK_MONOTONIC, &value);
    return static_cast<int64_t>(value.tv_sec) * 1'000 + value.tv_nsec / 1'000'000;
}

bool WriteAll(int fd, const char* data, size_t size) {
    while (size > 0) {
        const ssize_t count = write(fd, data, size);
        if (count < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        data += count;
        size -= static_cast<size_t>(count);
    }
    return true;
}

std::string LargePayload() {
    std::string payload(kLargeBytes, '\0');
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>('a' + i % 26);
    }
    return payload;
}

int gWorkerSequence = 0;

bool FakeDump(const char* service, int fd, void*) {
    gShared->dumps.fetch_add(1);
    switch (gShared->mode.load()) {
        case kModeSequence: {
            const std::string text = std::string(service) + ":" + std::to_string(gWorkerSequence++) + "\n";
            return WriteAll(fd, text.data(), text.size());
        }
        case kModeLarge: {
            const std::string payload = LargePayload();
            return WriteAll(fd, payload.data(), payload.size());
        }
        case kModeOverflow: {
            const std::string payload(kOverflowBytes, 'x');
            return WriteAll(fd, payload.data(), payload.size());
        }
        case kModeSleep:
            WriteAll(fd, "partial", 7);
            sleep(5);
            return true;
        case kModeReturnFalse:
            return false;
        case kModeDie:
            WriteAll(fd, "partial", 7);
            _exit(9);
        case kModeLateReply: {
            // EOF lands before the 200 ms deadline, the reply after it but
            // within the grace. dup2 closes the pipe end without freeing fd.
            usleep(150'000);
            const std::string text = std::string(service) + ":late\n";
            WriteAll(fd, text.data(), text.size());
            const int nullFd = open("/dev/null", O_WRONLY);
            if (nullFd < 0 || dup2(nullFd, fd) < 0) return false;
            close(nullFd);
            usleep(150'000);
            return true;
        }
    }
    return false;
}

pid_t FakeSpawn(int workerSocket, int clientSocket, void* context) {
    auto* spawn = static_cast<SpawnContext*>(context);
    if (spawn->failSpawn) return -1;
    const pid_t child = fork();
    if (child == 0) {
        close(clientSocket);
        ivrdroid::CloseDescriptorsExcept(workerSocket);
        gWorkerSequence = 0;
        _exit(ivrdroid::RunServiceDumpWorker(workerSocket, spawn->available, FakeDump, nullptr));
    }
    spawn->lastPid = child;
    return child;
}

ServiceDumpClientOptions FastOptions() {
    ServiceDumpClientOptions options;
    options.handshakeTimeoutMs = 1'000;
    options.failureThreshold = 3;
    options.initialBackoffMs = 100;
    options.maximumBackoffMs = 400;
    options.spawnBudget = 100;
    return options;
}

int CountOpenDescriptors() {
    int count = 0;
    for (int fd = 0; fd < 4096; ++fd) {
        if (fcntl(fd, F_GETFD) >= 0) ++count;
    }
    return count;
}

bool Reaped(pid_t pid) {
    return waitpid(pid, nullptr, WNOHANG) == -1 && errno == ECHILD;
}

void AssertNoChildren() {
    assert(waitpid(-1, nullptr, WNOHANG) == -1);
    assert(errno == ECHILD);
}

void TestSequentialCaptures() {
    gShared->mode = kModeSequence;
    SpawnContext spawn;
    ServiceDumpClient client(FakeSpawn, &spawn, FastOptions());
    std::string output;
    for (int i = 0; i < 200; ++i) {
        const char* service = i % 2 == 0 ? "telecom" : "audio";
        assert(client.Capture(service, kMaximumBytes, 1'000, &output) == ServiceDumpResult::Ok);
        assert(output == std::string(service) + ":" + std::to_string(i) + "\n");
    }
    assert(client.spawnCount() == 1);
    assert(client.hasWorker());
    client.Shutdown();
    assert(!client.hasWorker());
    AssertNoChildren();
}

void TestLargeOutput() {
    gShared->mode = kModeLarge;
    SpawnContext spawn;
    ServiceDumpClient client(FakeSpawn, &spawn, FastOptions());
    std::string output;
    assert(client.Capture("audio", kMaximumBytes, 2'000, &output) == ServiceDumpResult::Ok);
    assert(output == LargePayload());
    client.Shutdown();
    AssertNoChildren();
}

void TestOverflow() {
    gShared->mode = kModeOverflow;
    SpawnContext spawn;
    ServiceDumpClient client(FakeSpawn, &spawn, FastOptions());
    std::string output;
    assert(client.Capture("telecom", kMaximumBytes, 2'000, &output) == ServiceDumpResult::TooLarge);
    assert(output.empty());
    assert(client.hasWorker());
    gShared->mode = kModeSequence;
    // The worker survived and its reply stream is still in sync.
    assert(client.Capture("telecom", kMaximumBytes, 1'000, &output) == ServiceDumpResult::Ok);
    assert(output == "telecom:0\n");
    assert(client.spawnCount() == 1);
    client.Shutdown();
    AssertNoChildren();
}

void TestReplyGrace() {
    SpawnContext spawn;
    ServiceDumpClient client(FakeSpawn, &spawn, FastOptions());
    std::string output;
    // Warm the worker first so the handshake does not count against the dump.
    gShared->mode = kModeSequence;
    assert(client.Capture("audio", kMaximumBytes, 1'000, &output) == ServiceDumpResult::Ok);
    gShared->mode = kModeLateReply;
    assert(client.Capture("telecom", kMaximumBytes, 200, &output) == ServiceDumpResult::Ok);
    assert(output == "telecom:late\n");
    assert(client.hasWorker());
    assert(client.spawnCount() == 1);
    client.Shutdown();
    AssertNoChildren();
}

void TestSpawnBudget() {
    SpawnContext spawn;
    ServiceDumpClientOptions options = FastOptions();
    options.failureThreshold = 1'000;
    options.spawnBudget = 3;
    options.spawnWindowMs = 600;
    ServiceDumpClient client(FakeSpawn, &spawn, options);
    std::string output;
    const int64_t started = NowMs();
    for (int round = 0; round < 3; ++round) {
        gShared->mode = kModeSequence;
        assert(client.Capture("telecom", kMaximumBytes, 1'000, &output) == ServiceDumpResult::Ok);
        gShared->mode = kModeSleep;
        assert(client.Capture("telecom", kMaximumBytes, 50, &output) == ServiceDumpResult::TimedOut);
        assert(client.spawnCount() == round + 1);
    }
    gShared->mode = kModeSequence;
    for (int i = 0; i < 5; ++i) {
        assert(client.Capture("telecom", kMaximumBytes, 1'000, &output) == ServiceDumpResult::Unavailable);
    }
    assert(client.spawnCount() == 3);
    assert(NowMs() - started < 600);
    usleep(static_cast<useconds_t>((started + 650 - NowMs()) * 1'000));
    assert(client.Capture("telecom", kMaximumBytes, 1'000, &output) == ServiceDumpResult::Ok);
    assert(client.spawnCount() == 4);
    client.Shutdown();
    AssertNoChildren();
}

void TestTimeout() {
    SpawnContext spawn;
    ServiceDumpClient timed(FakeSpawn, &spawn, FastOptions());
    std::string output;
    gShared->mode = kModeSequence;
    assert(timed.Capture("telecom", kMaximumBytes, 1'000, &output) == ServiceDumpResult::Ok);
    gShared->mode = kModeSleep;
    const int64_t started = NowMs();
    assert(timed.Capture("telecom", kMaximumBytes, 200, &output) == ServiceDumpResult::TimedOut);
    const int64_t elapsed = NowMs() - started;
    assert(elapsed >= 190 && elapsed < 1'000);
    assert(!timed.hasWorker());
    assert(Reaped(spawn.lastPid));
    gShared->mode = kModeSequence;
    assert(timed.Capture("audio", kMaximumBytes, 1'000, &output) == ServiceDumpResult::Ok);
    assert(output == "audio:0\n");
    assert(timed.spawnCount() == 2);
    timed.Shutdown();
    AssertNoChildren();
}

void TestDumpFailureAndBackoff() {
    gShared->mode = kModeReturnFalse;
    SpawnContext spawn;
    ServiceDumpClient client(FakeSpawn, &spawn, FastOptions());
    std::string output;
    assert(client.Capture("telecom", kMaximumBytes, 1'000, &output) == ServiceDumpResult::Failed);
    assert(client.hasWorker());
    assert(client.Capture("telecom", kMaximumBytes, 1'000, &output) == ServiceDumpResult::Failed);
    assert(client.hasWorker());
    const pid_t worker = spawn.lastPid;
    assert(client.Capture("telecom", kMaximumBytes, 1'000, &output) == ServiceDumpResult::Failed);
    assert(!client.hasWorker());
    assert(Reaped(worker));
    assert(client.spawnCount() == 1);

    gShared->mode = kModeSequence;
    assert(client.Capture("telecom", kMaximumBytes, 1'000, &output) == ServiceDumpResult::Unavailable);
    assert(client.spawnCount() == 1);
    usleep(150'000);
    assert(client.Capture("telecom", kMaximumBytes, 1'000, &output) == ServiceDumpResult::Ok);
    assert(output == "telecom:0\n");
    assert(client.spawnCount() == 2);
    client.Shutdown();
    AssertNoChildren();
}

void TestWorkerDeath() {
    gShared->mode = kModeDie;
    SpawnContext spawn;
    ServiceDumpClient client(FakeSpawn, &spawn, FastOptions());
    std::string output;
    assert(client.Capture("audio", kMaximumBytes, 1'000, &output) == ServiceDumpResult::Failed);
    assert(!client.hasWorker());
    assert(Reaped(spawn.lastPid));
    gShared->mode = kModeSequence;
    assert(client.Capture("audio", kMaximumBytes, 1'000, &output) == ServiceDumpResult::Ok);
    assert(output == "audio:0\n");
    assert(client.spawnCount() == 2);
    client.Shutdown();
    AssertNoChildren();
}

void TestUnavailableHandshake() {
    gShared->mode = kModeSequence;
    SpawnContext spawn;
    spawn.available = false;
    ServiceDumpClient client(FakeSpawn, &spawn, FastOptions());
    std::string output;
    assert(client.Capture("telecom", kMaximumBytes, 1'000, &output) == ServiceDumpResult::Unavailable);
    assert(client.spawnCount() == 1);
    assert(!client.hasWorker());
    assert(Reaped(spawn.lastPid));
    spawn.available = true;
    assert(client.Capture("telecom", kMaximumBytes, 1'000, &output) == ServiceDumpResult::Unavailable);
    assert(client.spawnCount() == 1);
    usleep(150'000);
    assert(client.Capture("telecom", kMaximumBytes, 1'000, &output) == ServiceDumpResult::Ok);
    assert(client.spawnCount() == 2);
    client.Shutdown();
    AssertNoChildren();
}

void TestSpawnFailure() {
    SpawnContext spawn;
    spawn.failSpawn = true;
    const int before = CountOpenDescriptors();
    ServiceDumpClient client(FakeSpawn, &spawn, FastOptions());
    std::string output;
    assert(client.Capture("telecom", kMaximumBytes, 1'000, &output) == ServiceDumpResult::Unavailable);
    assert(client.spawnCount() == 0);
    assert(!client.hasWorker());
    assert(CountOpenDescriptors() == before);
    client.Shutdown();
    AssertNoChildren();
}

void TestDisallowedService() {
    gShared->mode = kModeSequence;
    gShared->dumps = 0;
    SpawnContext spawn;
    ServiceDumpClient client(FakeSpawn, &spawn, FastOptions());
    std::string output;
    assert(client.Capture("power", kMaximumBytes, 1'000, &output) == ServiceDumpResult::Failed);
    assert(output.empty());
    assert(gShared->dumps == 0);
    assert(client.hasWorker());
    assert(client.Capture("this-name-is-far-too-long-for-the-wire-record", kMaximumBytes, 1'000, &output) ==
        ServiceDumpResult::Failed);
    assert(gShared->dumps == 0);
    assert(client.Capture("telecom", kMaximumBytes, 1'000, &output) == ServiceDumpResult::Ok);
    assert(output == "telecom:0\n");
    assert(client.spawnCount() == 1);
    client.Shutdown();
    AssertNoChildren();
}

void TestForkInheritance() {
    gShared->mode = kModeSequence;
    SpawnContext spawn;
    ServiceDumpClient client(FakeSpawn, &spawn, FastOptions());
    std::string output;
    assert(client.Capture("telecom", kMaximumBytes, 1'000, &output) == ServiceDumpResult::Ok);
    assert(output == "telecom:0\n");
    assert(client.spawnCount() == 1);
    const pid_t parentWorker = spawn.lastPid;

    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        std::string childOutput;
        const bool ok =
            !client.hasWorker() &&
            client.Capture("audio", kMaximumBytes, 1'000, &childOutput) == ServiceDumpResult::Ok &&
            childOutput == "audio:0\n" &&
            client.spawnCount() == 1 &&
            spawn.lastPid != parentWorker;
        client.Shutdown();
        _exit(ok ? 0 : 1);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0) assert(errno == EINTR);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    assert(client.hasWorker());
    assert(client.Capture("telecom", kMaximumBytes, 1'000, &output) == ServiceDumpResult::Ok);
    assert(output == "telecom:1\n");
    assert(client.spawnCount() == 1);
    assert(spawn.lastPid == parentWorker);
    client.Shutdown();
    AssertNoChildren();
}

pid_t WaitForExit(pid_t child) {
    int status = 0;
    while (waitpid(child, &status, 0) < 0) assert(errno == EINTR);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 255;
}

void TestChildExitsWithoutCapture() {
    gShared->mode = kModeSequence;
    SpawnContext spawn;
    ServiceDumpClient client(FakeSpawn, &spawn, FastOptions());
    std::string output;
    assert(client.Capture("telecom", kMaximumBytes, 1'000, &output) == ServiceDumpResult::Ok);
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) _exit(0);
    assert(WaitForExit(child) == 0);
    assert(client.Capture("telecom", kMaximumBytes, 1'000, &output) == ServiceDumpResult::Ok);
    assert(output == "telecom:1\n");
    assert(client.spawnCount() == 1);
    client.Shutdown();
    AssertNoChildren();
}

void TestGrandchild() {
    gShared->mode = kModeSequence;
    SpawnContext spawn;
    ServiceDumpClient client(FakeSpawn, &spawn, FastOptions());
    std::string output;
    assert(client.Capture("telecom", kMaximumBytes, 1'000, &output) == ServiceDumpResult::Ok);
    const pid_t parentWorker = spawn.lastPid;
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        const pid_t grandchild = fork();
        if (grandchild < 0) _exit(2);
        if (grandchild == 0) {
            std::string grandchildOutput;
            const bool ok =
                client.Capture("audio", kMaximumBytes, 1'000, &grandchildOutput) == ServiceDumpResult::Ok &&
                grandchildOutput == "audio:0\n" &&
                client.spawnCount() == 1 &&
                spawn.lastPid != parentWorker;
            client.Shutdown();
            _exit(ok ? 0 : 1);
        }
        _exit(WaitForExit(grandchild));
    }
    assert(WaitForExit(child) == 0);
    assert(client.Capture("telecom", kMaximumBytes, 1'000, &output) == ServiceDumpResult::Ok);
    assert(output == "telecom:1\n");
    assert(client.spawnCount() == 1);
    assert(spawn.lastPid == parentWorker);
    client.Shutdown();
    AssertNoChildren();
}

void TestNoDescriptorOrProcessLeaks() {
    const int before = CountOpenDescriptors();
    SpawnContext spawn;
    ServiceDumpClientOptions options = FastOptions();
    options.failureThreshold = 1'000;
    options.spawnBudget = 1'000;
    ServiceDumpClient client(FakeSpawn, &spawn, options);
    std::string output;
    const Mode pattern[] = {
        kModeSequence, kModeReturnFalse, kModeSequence, kModeOverflow,
        kModeSequence, kModeDie, kModeSequence, kModeLarge,
    };
    for (int i = 0; i < 320; ++i) {
        const Mode mode = pattern[i % 8];
        gShared->mode = mode;
        const char* service = i % 16 == 2 ? "power" : (i % 2 == 0 ? "telecom" : "audio");
        const ServiceDumpResult result = client.Capture(service, kMaximumBytes, 2'000, &output);
        if (std::strcmp(service, "power") == 0 || mode == kModeReturnFalse ||
            mode == kModeDie) {
            assert(result == ServiceDumpResult::Failed);
        } else if (mode == kModeOverflow) {
            assert(result == ServiceDumpResult::TooLarge);
        } else {
            assert(result == ServiceDumpResult::Ok);
        }
    }
    gShared->mode = kModeSleep;
    assert(client.Capture("telecom", kMaximumBytes, 50, &output) == ServiceDumpResult::TimedOut);
    client.Shutdown();
    assert(CountOpenDescriptors() == before);
    AssertNoChildren();
}

void TestCloseDescriptorsExcept() {
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        int keep[2] = {-1, -1};
        int drop[2] = {-1, -1};
        if (pipe(keep) != 0 || pipe(drop) != 0) _exit(2);
        const int extra = dup(drop[0]);
        ivrdroid::CloseDescriptorsExcept(keep[1]);
        const bool ok =
            fcntl(keep[1], F_GETFD) >= 0 &&
            fcntl(keep[0], F_GETFD) < 0 &&
            fcntl(drop[0], F_GETFD) < 0 &&
            fcntl(drop[1], F_GETFD) < 0 &&
            fcntl(extra, F_GETFD) < 0 &&
            fcntl(STDERR_FILENO, F_GETFD) >= 0;
        _exit(ok ? 0 : 1);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0) assert(errno == EINTR);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

void TestAllowList() {
    assert(ivrdroid::IsAllowedDumpService("telecom"));
    assert(ivrdroid::IsAllowedDumpService("audio"));
    assert(!ivrdroid::IsAllowedDumpService("audio2"));
    assert(!ivrdroid::IsAllowedDumpService(""));
    assert(!ivrdroid::IsAllowedDumpService(nullptr));
#ifndef __ANDROID__
    // libbinder_ndk exists only on Android; the dump must refuse cleanly.
    assert(!ivrdroid::InitializeBinderServiceDump());
    assert(!ivrdroid::BinderServiceDump("telecom", STDERR_FILENO, nullptr));
#endif
}

}  // namespace

int main() {
    // SIGPIPE is deliberately left at its default: the client must not need it ignored.
    void* shared = mmap(nullptr, sizeof(SharedState), PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_ANON, -1, 0);
    assert(shared != MAP_FAILED);
    gShared = new (shared) SharedState();

    TestAllowList();
    TestCloseDescriptorsExcept();
    TestSequentialCaptures();
    TestLargeOutput();
    TestOverflow();
    TestTimeout();
    TestDumpFailureAndBackoff();
    TestWorkerDeath();
    TestUnavailableHandshake();
    TestSpawnFailure();
    TestDisallowedService();
    TestForkInheritance();
    TestReplyGrace();
    TestSpawnBudget();
    TestChildExitsWithoutCapture();
    TestGrandchild();
    TestNoDescriptorOrProcessLeaks();
    std::puts("service_dump_worker_test: ok");
    return 0;
}
