#include "child_process.h"

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <ctime>
#include <sys/wait.h>
#include <unistd.h>

namespace ivrdroid {
namespace {

int64_t MonotonicMilliseconds() {
    timespec value {};
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return -1;
    return static_cast<int64_t>(value.tv_sec) * 1'000 + value.tv_nsec / 1'000'000;
}

bool ReapBlocking(pid_t* child) {
    int status = 0;
    while (waitpid(*child, &status, 0) < 0) {
        if (errno != EINTR) return false;
    }
    *child = -1;
    return true;
}

}  // namespace

ChildProcessState PollChildProcess(pid_t* child) {
    if (child == nullptr || *child <= 0) return ChildProcessState::WaitError;
    int status = 0;
    const pid_t result = waitpid(*child, &status, WNOHANG);
    if (result == 0) return ChildProcessState::Running;
    if (result < 0) {
        if (errno == EINTR) return ChildProcessState::Running;
        return ChildProcessState::WaitError;
    }
    *child = -1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0
        ? ChildProcessState::ExitedSuccessfully
        : ChildProcessState::ExitedWithFailure;
}

bool TerminateAndReapChildProcess(pid_t* child, int timeoutMilliseconds) {
    if (child == nullptr || *child <= 0 || timeoutMilliseconds < 0) return false;
    if (kill(*child, SIGTERM) != 0 && errno != ESRCH) {
        kill(*child, SIGKILL);
        ReapBlocking(child);
        return false;
    }

    const int64_t started = MonotonicMilliseconds();
    if (started < 0) {
        kill(*child, SIGKILL);
        ReapBlocking(child);
        return false;
    }
    while (true) {
        const int64_t now = MonotonicMilliseconds();
        if (now < 0 || now - started >= timeoutMilliseconds) break;
        const ChildProcessState state = PollChildProcess(child);
        if (state == ChildProcessState::ExitedSuccessfully ||
            state == ChildProcessState::ExitedWithFailure) {
            return true;
        }
        if (state == ChildProcessState::WaitError) return false;
        usleep(10'000);
    }

    if (kill(*child, SIGKILL) != 0 && errno != ESRCH) return false;
    return ReapBlocking(child);
}

}  // namespace ivrdroid
