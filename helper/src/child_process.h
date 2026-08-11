#pragma once

#include <sys/types.h>

namespace ivrdroid {

enum class ChildProcessState {
    Running,
    ExitedSuccessfully,
    ExitedWithFailure,
    WaitError,
};

ChildProcessState PollChildProcess(pid_t* child);
bool TerminateAndReapChildProcess(pid_t* child, int timeoutMilliseconds = 500);

}  // namespace ivrdroid
