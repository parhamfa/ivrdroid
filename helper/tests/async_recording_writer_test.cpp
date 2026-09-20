#include "async_recording_writer.h"
#include <cassert>
#include <chrono>
#include <fstream>
#include <string>
#include <unistd.h>

int main() {
    using ivrdroid::AsyncRecordingWriter;
    using Clock = std::chrono::steady_clock;
    char pattern[] = "/tmp/ivrdroid-background-writer.XXXXXX";
    const std::string root = mkdtemp(pattern);
    const auto prepare = [](int) { return true; };
    const auto complete = [](int fd, uint64_t bytes, const std::string&, bool captured) { return captured && bytes == 4 && fsync(fd) == 0; };
    auto old = AsyncRecordingWriter::Start(root + "/old", getuid(), prepare,
        [complete](int fd, uint64_t bytes, const std::string& reason, bool captured) {
            sleep(6); // A finalizer longer than the call-control watchdog.
            return complete(fd, bytes, reason, captured);
        });
    assert(old && old->Append("OLD!", 4));
    const auto released = Clock::now();
    old->Stop("caller_hangup", true);
    auto next = AsyncRecordingWriter::Start(root + "/new", getuid(), prepare, complete);
    assert(next && next->Append("NEW!", 4));
    next->Stop("completed", true);
    assert(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - released).count() < 500);
    while (!next->done()) usleep(1000);
    assert(!next->failed() && !old->done());
    // A writer which never reads must cause a bounded recording failure, not a blocked caller.
    auto stalled = AsyncRecordingWriter::Start(root + "/stalled", getuid(), [](int) { sleep(1); return false; }, complete);
    assert(stalled);
    std::string block(4800, 'x');
    const auto started = Clock::now();
    for (int i = 0; i < 10000 && !stalled->failed(); ++i) stalled->Append(block.data(), block.size());
    assert(stalled->failed());
    assert(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count() < 500);
    stalled->Stop("writer_failure", false);
    while (!old->done() || !stalled->done()) usleep(1000);
    assert(!old->failed() && stalled->failed());
    std::ifstream in(root + "/new"); std::string bytes; in >> bytes; assert(bytes == "NEW!");
    old.reset(); next.reset(); stalled.reset();
    unlink((root + "/old").c_str()); unlink((root + "/new").c_str()); unlink((root + "/stalled").c_str()); rmdir(root.c_str());
}
