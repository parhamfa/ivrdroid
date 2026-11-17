#pragma once

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ivrdroid {

// A long-lived worker that holds one binder connection and writes fresh
// service dumps into pipes on request. Old vendor kernels leak kernel memory
// on every binder open, so one binder client per poll is not acceptable.
//
// Protocol over a private AF_UNIX stream socketpair:
//   worker -> client: one handshake byte, 'R' (ready) or 'U' (unavailable).
//   client -> worker: one ServiceDumpRequest with exactly one SCM_RIGHTS fd,
//                     the write end of a fresh pipe.
//   worker -> client: after the dump call and closing the fd, 'K' or 'E'.
using ServiceDumpFunction = bool (*)(const char* service, int fd, void* context);
// Forks and returns the child pid, or -1. The child must close clientSocket and
// run the worker on workerSocket; it must never return.
using ServiceDumpSpawnFunction = pid_t (*)(int workerSocket, int clientSocket, void* context);

constexpr size_t kServiceDumpNameBytes = 32;

struct ServiceDumpRequest {
    char magic[4];
    char service[kServiceDumpNameBytes];
};

constexpr char kServiceDumpMagic[4] = {'I', 'V', 'D', 'W'};
constexpr char kServiceDumpReady = 'R';
constexpr char kServiceDumpUnavailable = 'U';
constexpr char kServiceDumpSucceeded = 'K';
constexpr char kServiceDumpFailed = 'E';

bool IsAllowedDumpService(const char* service);
// Closes every descriptor >= 3 except keepFd.
void CloseDescriptorsExcept(int keepFd);
// Returns the process exit code: 0 on socket EOF or error, non-zero when
// unavailable or on a malformed request.
int RunServiceDumpWorker(
    int socketFd,
    bool available,
    ServiceDumpFunction dump,
    void* context);

enum class ServiceDumpResult {
    Ok,
    TimedOut,
    Failed,
    Unavailable,
    // The dump exceeded maximumBytes; the worker stays alive and in sync.
    TooLarge,
};

struct ServiceDumpClientOptions {
    int handshakeTimeoutMs = 1'000;
    int failureThreshold = 3;
    int initialBackoffMs = 5'000;
    int maximumBackoffMs = 60'000;
    // Extra time for the reply byte once the dump pipe reached EOF.
    int replyGraceMs = 250;
    // At most spawnBudget spawn attempts within any spawnWindowMs, regardless of
    // successes in between.
    int spawnBudget = 3;
    int spawnWindowMs = 60'000;
};

class ServiceDumpClient {
public:
    ServiceDumpClient(
        ServiceDumpSpawnFunction spawn,
        void* context,
        ServiceDumpClientOptions options = ServiceDumpClientOptions());
    ServiceDumpClient(const ServiceDumpClient&) = delete;
    ServiceDumpClient& operator=(const ServiceDumpClient&) = delete;

    ServiceDumpResult Capture(
        const char* service,
        size_t maximumBytes,
        int timeoutMs,
        std::string* output);
    // Kills and reaps the worker only when this process spawned it.
    void Shutdown();

    int spawnCount() const;
    bool hasWorker() const;

private:
    void ForgetInheritedWorker();
    bool StartWorker();
    void KillWorker();
    ServiceDumpResult Exchange(
        const char* service,
        size_t maximumBytes,
        int timeoutMs,
        std::string* output);
    void RecordFailure();
    void ApplyBackoff();
    bool SpawnAllowed(int64_t now);

    ServiceDumpSpawnFunction spawn_;
    void* context_;
    ServiceDumpClientOptions options_;
    pid_t ownerPid_ = -1;
    pid_t workerPid_ = -1;
    int socket_ = -1;
    int consecutiveFailures_ = 0;
    int nextBackoffMs_;
    int64_t backoffUntilMs_ = 0;
    int spawnCount_ = 0;
    std::vector<int64_t> spawnAttemptsMs_;
};

}  // namespace ivrdroid
