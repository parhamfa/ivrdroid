#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <sys/types.h>

namespace ivrdroid {
// A bounded pipe separates capture/control from disk writes and finalization.
// The child closes all inherited descriptors and drops privileges before work.
class AsyncRecordingWriter {
public:
    using Prepare = std::function<bool(int)>;
    using Finish = std::function<bool(int, uint64_t, const std::string&, bool)>;
    static std::unique_ptr<AsyncRecordingWriter> Start(const std::string& path, uid_t uid, Prepare prepare, Finish finish);
    ~AsyncRecordingWriter();
    bool Append(const void* data, size_t bytes);
    void Stop(const std::string& reason, bool captured);
    bool done() const;
    bool failed() const;
private:
    struct State;
    AsyncRecordingWriter(State* state, int fd) : state_(state), fd_(fd) {}
    State* state_;
    int fd_;
};
} // namespace ivrdroid
