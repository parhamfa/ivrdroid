#include "async_recording_writer.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace ivrdroid {
struct AsyncRecordingWriter::State {
    std::atomic<bool> stopped {false}, captured {false}, done {false}, failed {false};
    char reason[33] {};
};
namespace {
std::vector<pid_t> children;
bool WriteAll(int fd, const void* data, size_t bytes) {
    auto* at = static_cast<const char*>(data);
    while (bytes) {
        const auto count = write(fd, at, bytes);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        at += count; bytes -= count;
    }
    return true;
}
}
std::unique_ptr<AsyncRecordingWriter> AsyncRecordingWriter::Start(const std::string& path, uid_t uid, Prepare prepare, Finish finish) {
    children.erase(std::remove_if(children.begin(), children.end(), [](pid_t pid) {
        return waitpid(pid, nullptr, WNOHANG) != 0;
    }), children.end());
    if (children.size() >= 8) return nullptr;
    int pipeFd[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pipeFd) != 0) return nullptr;
    if (fcntl(pipeFd[1], F_SETFL, O_NONBLOCK) != 0) {
        close(pipeFd[0]); close(pipeFd[1]); return nullptr;
    }
    void* memory = mmap(nullptr, sizeof(State), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) { close(pipeFd[0]); close(pipeFd[1]); return nullptr; }
    auto* state = new(memory) State();
    const pid_t child = fork();
    if (child == 0) {
        setpriority(PRIO_PROCESS, 0, 10);
        // This child may outlive this helper. It must retain neither the helper
        // lock nor any telephone, mixer, capture, guardian or other call fd.
        const int maximum = static_cast<int>(sysconf(_SC_OPEN_MAX));
        for (int fd = 3; fd < maximum; ++fd) if (fd != pipeFd[0]) close(fd);
        signal(SIGPIPE, SIG_IGN);
        bool healthy = geteuid() == uid || (setgroups(0, nullptr) == 0 && setgid(uid) == 0 && setuid(uid) == 0);
        int output = healthy ? open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600) : -1;
        healthy = output >= 0 && fchmod(output, 0600) == 0 && prepare(output);
        uint64_t bytes = 0;
        char buffer[16 * 1024];
        while (healthy) {
            const auto count = read(pipeFd[0], buffer, sizeof(buffer));
            if (count < 0 && errno == EINTR) continue;
            if (count == 0) break;
            if (count < 0 || !WriteAll(output, buffer, count)) { healthy = false; break; }
            bytes += count;
        }
        if (!healthy) state->failed.store(true);
        close(pipeFd[0]);
        const bool captured = state->stopped.load(std::memory_order_acquire) && state->captured.load() && healthy;
        const bool finished = finish(output, bytes, captured ? state->reason : "writer_failure", captured);
        if (output >= 0) close(output);
        state->failed.store(!finished); state->done.store(true, std::memory_order_release);
        _exit(finished ? 0 : 1);
    }
    close(pipeFd[0]);
    if (child < 0) {
        close(pipeFd[1]); munmap(memory, sizeof(State)); return nullptr;
    }
    children.push_back(child);
    return std::unique_ptr<AsyncRecordingWriter>(new AsyncRecordingWriter(state, pipeFd[1]));
}
AsyncRecordingWriter::~AsyncRecordingWriter() {
    if (fd_ >= 0) Stop("writer_failure", false);
    munmap(state_, sizeof(State));
}
bool AsyncRecordingWriter::Append(const void* data, size_t bytes) {
    if (fd_ < 0 || failed()) return false;
    auto* at = static_cast<const char*>(data);
    while (bytes) {
        const auto count = send(fd_, at, bytes, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { state_->failed.store(true); return false; }
        at += count; bytes -= count;
    }
    return true;
}
void AsyncRecordingWriter::Stop(const std::string& reason, bool captured) {
    if (fd_ < 0) return;
    std::strncpy(state_->reason, reason.c_str(), sizeof(state_->reason) - 1);
    state_->captured.store(captured && !state_->failed.load());
    state_->stopped.store(true, std::memory_order_release);
    close(fd_); fd_ = -1;
}
bool AsyncRecordingWriter::done() const { return state_->done.load(std::memory_order_acquire); }
bool AsyncRecordingWriter::failed() const { return state_->failed.load(); }
} // namespace ivrdroid
