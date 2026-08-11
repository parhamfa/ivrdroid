#include "child_process.h"

#include <cassert>
#include <cerrno>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

int main() {
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        signal(SIGTERM, SIG_IGN);
        while (true) pause();
    }

    assert(ivrdroid::PollChildProcess(&child) ==
        ivrdroid::ChildProcessState::Running);
    assert(ivrdroid::TerminateAndReapChildProcess(&child, 20));
    assert(child == -1);
    assert(waitpid(-1, nullptr, WNOHANG) == -1);
    assert(errno == ECHILD);
    return 0;
}
