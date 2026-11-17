#include "service_dump_worker.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <unistd.h>
#include <vector>

namespace ivrdroid {
namespace {

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

// Bounds the descriptor sweep when the soft limit is unlimited or huge.
constexpr long kMaximumDescriptorScan = 65'536;

int64_t MonotonicMilliseconds() {
    timespec value {};
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return static_cast<int64_t>(value.tv_sec) * 1'000 + value.tv_nsec / 1'000'000;
}

bool SetCloseOnExec(int fd) {
    const int flags = fcntl(fd, F_GETFD);
    return flags >= 0 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

bool SetNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void SuppressSigpipe(int fd) {
#ifdef SO_NOSIGPIPE
    const int enabled = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#else
    (void)fd;
#endif
}

bool CreateSocketPair(int descriptors[2]) {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) != 0) return false;
    if (!SetCloseOnExec(descriptors[0]) || !SetCloseOnExec(descriptors[1])) {
        close(descriptors[0]);
        close(descriptors[1]);
        return false;
    }
    SuppressSigpipe(descriptors[0]);
    SuppressSigpipe(descriptors[1]);
    return true;
}

bool CreateDumpPipe(int descriptors[2]) {
    if (pipe(descriptors) != 0) return false;
    if (!SetCloseOnExec(descriptors[0]) ||
        !SetCloseOnExec(descriptors[1]) ||
        !SetNonBlocking(descriptors[0])) {
        close(descriptors[0]);
        close(descriptors[1]);
        return false;
    }
    return true;
}

bool SendByte(int fd, char value) {
    while (true) {
        const ssize_t count = send(fd, &value, 1, kSendFlags);
        if (count == 1) return true;
        if (count < 0 && errno == EINTR) continue;
        return false;
    }
}

enum class WaitStatus {
    Ready,
    TimedOut,
    Error,
};

WaitStatus WaitReadable(int fd, int64_t deadline) {
    while (true) {
        const int64_t remaining = deadline - MonotonicMilliseconds();
        if (remaining <= 0) return WaitStatus::TimedOut;
        pollfd descriptor {fd, POLLIN, 0};
        const int timeout = remaining > std::numeric_limits<int>::max()
            ? std::numeric_limits<int>::max()
            : static_cast<int>(remaining);
        const int result = poll(&descriptor, 1, timeout);
        if (result < 0) {
            if (errno == EINTR) continue;
            return WaitStatus::Error;
        }
        if (result == 0) continue;
        if ((descriptor.revents & POLLNVAL) != 0) return WaitStatus::Error;
        return WaitStatus::Ready;
    }
}

enum class ByteStatus {
    Received,
    TimedOut,
    Closed,
    Error,
};

ByteStatus ReceiveByte(int fd, int64_t deadline, char* value) {
    while (true) {
        const WaitStatus wait = WaitReadable(fd, deadline);
        if (wait == WaitStatus::TimedOut) return ByteStatus::TimedOut;
        if (wait == WaitStatus::Error) return ByteStatus::Error;
        const ssize_t count = recv(fd, value, 1, MSG_DONTWAIT);
        if (count == 1) return ByteStatus::Received;
        if (count == 0) return ByteStatus::Closed;
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
        return ByteStatus::Error;
    }
}

bool SendRequest(int socketFd, const ServiceDumpRequest& request, int fd) {
    union {
        cmsghdr header;
        char buffer[CMSG_SPACE(sizeof(int))];
    } control {};
    iovec vector {
        const_cast<ServiceDumpRequest*>(&request),
        sizeof(request),
    };
    msghdr message {};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control.buffer;
    message.msg_controllen = sizeof(control.buffer);
    cmsghdr* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(header), &fd, sizeof(fd));

    ssize_t sent = -1;
    do {
        sent = sendmsg(socketFd, &message, kSendFlags);
    } while (sent < 0 && errno == EINTR);
    if (sent <= 0) return false;

    // The descriptor travels with the first byte; any remainder goes plainly.
    const auto* cursor = reinterpret_cast<const char*>(&request) + sent;
    size_t remaining = sizeof(request) - static_cast<size_t>(sent);
    while (remaining > 0) {
        const ssize_t count = send(socketFd, cursor, remaining, kSendFlags);
        if (count < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (count == 0) return false;
        cursor += count;
        remaining -= static_cast<size_t>(count);
    }
    return true;
}

enum class ReceiveStatus {
    Request,
    EndOfStream,
    Malformed,
};

ReceiveStatus ReceiveRequest(int socketFd, ServiceDumpRequest* request, int* fd) {
    *fd = -1;
    auto* bytes = reinterpret_cast<char*>(request);
    size_t received = 0;
    bool malformed = false;
    while (received < sizeof(*request)) {
        union {
            cmsghdr header;
            char buffer[CMSG_SPACE(sizeof(int))];
        } control {};
        iovec vector {bytes + received, sizeof(*request) - received};
        msghdr message {};
        message.msg_iov = &vector;
        message.msg_iovlen = 1;
        message.msg_control = control.buffer;
        message.msg_controllen = sizeof(control.buffer);
        const ssize_t count = recvmsg(socketFd, &message, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            if (*fd >= 0) close(*fd);
            *fd = -1;
            return received == 0 ? ReceiveStatus::EndOfStream : ReceiveStatus::Malformed;
        }
        for (cmsghdr* header = CMSG_FIRSTHDR(&message);
             header != nullptr;
             header = CMSG_NXTHDR(&message, header)) {
            if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS) {
                malformed = true;
                continue;
            }
            const size_t payload = header->cmsg_len - CMSG_LEN(0);
            for (size_t offset = 0; offset + sizeof(int) <= payload; offset += sizeof(int)) {
                int descriptor = -1;
                std::memcpy(&descriptor, CMSG_DATA(header) + offset, sizeof(descriptor));
                if (descriptor < 0) continue;
                if (*fd < 0 && received == 0) {
                    *fd = descriptor;
                } else {
                    close(descriptor);
                    malformed = true;
                }
            }
        }
        if ((message.msg_flags & MSG_CTRUNC) != 0) malformed = true;
        received += static_cast<size_t>(count);
    }
    if (malformed || *fd < 0) {
        if (*fd >= 0) close(*fd);
        *fd = -1;
        return ReceiveStatus::Malformed;
    }
    return ReceiveStatus::Request;
}

}  // namespace

bool IsAllowedDumpService(const char* service) {
    return service != nullptr &&
        (std::strcmp(service, "telecom") == 0 || std::strcmp(service, "audio") == 0);
}

void CloseDescriptorsExcept(int keepFd) {
    // Prefer the process's own descriptor list: a full numeric sweep costs tens
    // of thousands of syscalls on every worker start.
    DIR* directory = opendir("/proc/self/fd");
    if (directory == nullptr) directory = opendir("/dev/fd");
    if (directory != nullptr) {
        std::vector<int> descriptors;
        while (const dirent* entry = readdir(directory)) {
            char* end = nullptr;
            const long value = std::strtol(entry->d_name, &end, 10);
            if (end == entry->d_name || *end != '\0' || value < 3 ||
                value > std::numeric_limits<int>::max()) {
                continue;
            }
            descriptors.push_back(static_cast<int>(value));
        }
        // Close only after closedir so the listing's own descriptor is never
        // closed underneath it.
        closedir(directory);
        for (const int fd : descriptors) {
            if (fd != keepFd) close(fd);
        }
        return;
    }
    long maximum = sysconf(_SC_OPEN_MAX);
    if (maximum < 0 || maximum > kMaximumDescriptorScan) maximum = kMaximumDescriptorScan;
    for (long fd = 3; fd < maximum; ++fd) {
        if (fd != keepFd) close(static_cast<int>(fd));
    }
}

int RunServiceDumpWorker(
    int socketFd,
    bool available,
    ServiceDumpFunction dump,
    void* context) {
    SuppressSigpipe(socketFd);
    const bool ready = available && dump != nullptr;
    if (!SendByte(socketFd, ready ? kServiceDumpReady : kServiceDumpUnavailable)) {
        return ready ? 0 : 3;
    }
    if (!ready) return 3;

    while (true) {
        ServiceDumpRequest request {};
        int fd = -1;
        const ReceiveStatus status = ReceiveRequest(socketFd, &request, &fd);
        if (status == ReceiveStatus::EndOfStream) return 0;
        if (status == ReceiveStatus::Malformed) return 2;
        if (std::memcmp(request.magic, kServiceDumpMagic, sizeof(kServiceDumpMagic)) != 0) {
            close(fd);
            return 2;
        }
        char reply = kServiceDumpFailed;
        if (std::memchr(request.service, '\0', sizeof(request.service)) != nullptr &&
            IsAllowedDumpService(request.service)) {
            reply = dump(request.service, fd, context)
                ? kServiceDumpSucceeded
                : kServiceDumpFailed;
        }
        close(fd);
        if (!SendByte(socketFd, reply)) return 0;
    }
}

ServiceDumpClient::ServiceDumpClient(
    ServiceDumpSpawnFunction spawn,
    void* context,
    ServiceDumpClientOptions options)
    : spawn_(spawn),
      context_(context),
      options_(options),
      nextBackoffMs_(options.initialBackoffMs) {}

int ServiceDumpClient::spawnCount() const {
    return spawnCount_;
}

bool ServiceDumpClient::hasWorker() const {
    return socket_ >= 0 && workerPid_ > 0 && ownerPid_ == getpid();
}

void ServiceDumpClient::ForgetInheritedWorker() {
    if (ownerPid_ < 0 || ownerPid_ == getpid()) return;
    // A forked child inherited this object. The worker belongs to the parent:
    // never signal or wait for it, only drop the inherited descriptor.
    if (socket_ >= 0) close(socket_);
    socket_ = -1;
    workerPid_ = -1;
    consecutiveFailures_ = 0;
    nextBackoffMs_ = options_.initialBackoffMs;
    backoffUntilMs_ = 0;
    spawnCount_ = 0;
    spawnAttemptsMs_.clear();
    ownerPid_ = -1;
}

void ServiceDumpClient::KillWorker() {
    if (workerPid_ > 0) {
        kill(workerPid_, SIGKILL);
        while (waitpid(workerPid_, nullptr, 0) < 0 && errno == EINTR) {}
    }
    workerPid_ = -1;
    if (socket_ >= 0) close(socket_);
    socket_ = -1;
}

void ServiceDumpClient::ApplyBackoff() {
    backoffUntilMs_ = MonotonicMilliseconds() + nextBackoffMs_;
    nextBackoffMs_ = nextBackoffMs_ > options_.maximumBackoffMs / 2
        ? options_.maximumBackoffMs
        : nextBackoffMs_ * 2;
    consecutiveFailures_ = 0;
}

bool ServiceDumpClient::SpawnAllowed(int64_t now) {
    spawnAttemptsMs_.erase(
        std::remove_if(
            spawnAttemptsMs_.begin(),
            spawnAttemptsMs_.end(),
            [&](int64_t attempt) { return now - attempt >= options_.spawnWindowMs; }),
        spawnAttemptsMs_.end());
    return static_cast<int>(spawnAttemptsMs_.size()) < options_.spawnBudget;
}

void ServiceDumpClient::RecordFailure() {
    ++consecutiveFailures_;
    if (consecutiveFailures_ < options_.failureThreshold) return;
    KillWorker();
    ApplyBackoff();
}

bool ServiceDumpClient::StartWorker() {
    int sockets[2] = {-1, -1};
    if (spawn_ == nullptr || !CreateSocketPair(sockets)) return false;
    const pid_t child = spawn_(sockets[1], sockets[0], context_);
    close(sockets[1]);
    if (child <= 0) {
        close(sockets[0]);
        return false;
    }
    ++spawnCount_;
    workerPid_ = child;
    socket_ = sockets[0];

    char handshake = 0;
    const ByteStatus status = ReceiveByte(
        socket_,
        MonotonicMilliseconds() + options_.handshakeTimeoutMs,
        &handshake);
    if (status != ByteStatus::Received || handshake != kServiceDumpReady) {
        KillWorker();
        return false;
    }
    return true;
}

ServiceDumpResult ServiceDumpClient::Exchange(
    const char* service,
    size_t maximumBytes,
    int timeoutMs,
    std::string* output) {
    ServiceDumpRequest request {};
    std::memcpy(request.magic, kServiceDumpMagic, sizeof(kServiceDumpMagic));
    if (service == nullptr) return ServiceDumpResult::Failed;
    const size_t length = strnlen(service, sizeof(request.service));
    if (length >= sizeof(request.service)) return ServiceDumpResult::Failed;
    std::memcpy(request.service, service, length);

    int pipeFds[2] = {-1, -1};
    if (!CreateDumpPipe(pipeFds)) return ServiceDumpResult::Failed;
    const int64_t deadline = MonotonicMilliseconds() + timeoutMs;
    const bool sent = SendRequest(socket_, request, pipeFds[1]);
    close(pipeFds[1]);
    if (!sent) {
        close(pipeFds[0]);
        KillWorker();
        return ServiceDumpResult::Failed;
    }

    ServiceDumpResult failure = ServiceDumpResult::Ok;
    bool complete = false;
    bool tooLarge = false;
    std::array<char, 16 * 1024> buffer {};
    while (!complete && failure == ServiceDumpResult::Ok) {
        const WaitStatus wait = WaitReadable(pipeFds[0], deadline);
        if (wait == WaitStatus::TimedOut) {
            failure = ServiceDumpResult::TimedOut;
            break;
        }
        if (wait == WaitStatus::Error) {
            failure = ServiceDumpResult::Failed;
            break;
        }
        while (true) {
            const ssize_t count = read(pipeFds[0], buffer.data(), buffer.size());
            if (count > 0) {
                // Past the limit, keep draining to EOF so the worker's reply
                // stays in sync and the worker survives.
                if (tooLarge) continue;
                if (output->size() + static_cast<size_t>(count) > maximumBytes) {
                    tooLarge = true;
                    output->clear();
                    continue;
                }
                output->append(buffer.data(), static_cast<size_t>(count));
                continue;
            }
            if (count == 0) {
                complete = true;
                break;
            }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            failure = ServiceDumpResult::Failed;
            break;
        }
    }
    close(pipeFds[0]);
    if (failure != ServiceDumpResult::Ok) {
        KillWorker();
        return failure;
    }

    char reply = 0;
    const int64_t replyDeadline =
        std::max(deadline, MonotonicMilliseconds() + options_.replyGraceMs);
    const ByteStatus status = ReceiveByte(socket_, replyDeadline, &reply);
    if (status == ByteStatus::TimedOut) {
        KillWorker();
        return ServiceDumpResult::TimedOut;
    }
    if (status != ByteStatus::Received) {
        KillWorker();
        return ServiceDumpResult::Failed;
    }
    if (reply == kServiceDumpSucceeded || reply == kServiceDumpFailed) {
        if (tooLarge) return ServiceDumpResult::TooLarge;
        return reply == kServiceDumpSucceeded
            ? ServiceDumpResult::Ok
            : ServiceDumpResult::Failed;
    }
    KillWorker();
    return ServiceDumpResult::Failed;
}

ServiceDumpResult ServiceDumpClient::Capture(
    const char* service,
    size_t maximumBytes,
    int timeoutMs,
    std::string* output) {
    output->clear();
    ForgetInheritedWorker();
    ownerPid_ = getpid();

    if (socket_ < 0) {
        if (backoffUntilMs_ != 0 && MonotonicMilliseconds() < backoffUntilMs_) {
            return ServiceDumpResult::Unavailable;
        }
        backoffUntilMs_ = 0;
        // Bounds respawns even when successes keep resetting the failure
        // counter, e.g. alternating Ok and timeout.
        const int64_t now = MonotonicMilliseconds();
        if (!SpawnAllowed(now)) return ServiceDumpResult::Unavailable;
        spawnAttemptsMs_.push_back(now);
        if (!StartWorker()) {
            // A worker that cannot start is a backoff event on its own; retrying
            // on every poll would recreate the per-poll process launches.
            ApplyBackoff();
            return ServiceDumpResult::Unavailable;
        }
    }

    const ServiceDumpResult result = Exchange(service, maximumBytes, timeoutMs, output);
    if (result == ServiceDumpResult::Ok) {
        consecutiveFailures_ = 0;
        nextBackoffMs_ = options_.initialBackoffMs;
        return result;
    }
    // An oversized dump is the service's output, not a worker fault.
    if (result == ServiceDumpResult::TooLarge) return result;
    RecordFailure();
    return result;
}

void ServiceDumpClient::Shutdown() {
    ForgetInheritedWorker();
    KillWorker();
}

}  // namespace ivrdroid
