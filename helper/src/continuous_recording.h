#pragma once

#include <cstdint>
#include <string>
#include <sys/types.h>

namespace ivrdroid {

struct ContinuousRecordingContext {
    std::string kind, id, callId, bootId, blockId;
    uint64_t revision = 0, policy = 0, frames = 0, processStart = 0;
    int64_t wallMs = 0, elapsedMs = 0;
    pid_t pid = -1;
};

std::string FormatContinuousContext(const ContinuousRecordingContext& context);
bool ParseContinuousContext(const std::string& text, ContinuousRecordingContext* context);

// Used exclusively by writer children. Nothing in this class runs on call-control threads.
class ContinuousPcmFile {
public:
    ContinuousPcmFile(std::string directory, uid_t owner, ContinuousRecordingContext context);
    ~ContinuousPcmFile();
    bool Open();
    bool Append(const int16_t* samples, uint32_t frames, uint64_t maximumBytes);
    bool Checkpoint();
    bool Finish(const std::string& reason, bool partial);
    uint64_t frames() const { return context_.frames; }
    static bool Recover(const std::string& directory, uid_t owner, const ContinuousRecordingContext& committed);
private:
    bool WriteMetadata(const std::string& name, const std::string& text);
    bool Failure(const char* operation, int error, int64_t startedMs);
    std::string directory_;
    uid_t owner_;
    ContinuousRecordingContext context_;
    int fd_ = -1;
};

} // namespace ivrdroid
