#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ivrdroid::conversation_handoff {

constexpr size_t kMaximumWireBytes = 512;
constexpr int64_t kAcknowledgementTimeoutMilliseconds = 10'000;

enum class Result {
    Ok,
    Failed,
    Invalid,
};

struct Acknowledgement {
    std::string callUuid;
    uint64_t revisionId = 0;
    std::string blockUuid;
    std::string recordingUuid;
    uint32_t segmentIndex = 0;
    std::string bootUuid;
    uint64_t elapsedMilliseconds = 0;
    Result result = Result::Invalid;
    std::string reason;
};

Acknowledgement ParseAcknowledgement(std::string_view wire);
std::string EncodeAcknowledgement(const Acknowledgement& acknowledgement);
const char* ToString(Result result);

enum class Decision {
    Awaiting,
    Accepted,
    Failed,
    TimedOut,
    IgnoredForeign,
    Stale,
    ProtocolFailure,
};

class Policy {
public:
    Policy(
        std::string callUuid,
        uint64_t revisionId,
        std::string blockUuid,
        std::string recordingUuid,
        std::string bootUuid);

    bool MarkSegmentFinalized(uint32_t segmentIndex, int64_t nowMilliseconds);
    Decision Observe(
        const Acknowledgement& acknowledgement,
        int64_t nowMilliseconds);
    Decision CheckDeadline(int64_t nowMilliseconds) const;

    bool valid() const { return valid_; }
    bool pending() const { return pending_; }
    uint32_t pendingSegmentIndex() const { return pendingSegmentIndex_; }
    uint32_t nextExpectedSegmentIndex() const { return nextExpectedSegmentIndex_; }

private:
    bool Correlates(const Acknowledgement& acknowledgement) const;

    std::string callUuid_;
    uint64_t revisionId_ = 0;
    std::string blockUuid_;
    std::string recordingUuid_;
    std::string bootUuid_;
    bool valid_ = false;
    bool pending_ = false;
    uint32_t pendingSegmentIndex_ = 0;
    uint32_t nextExpectedSegmentIndex_ = 0;
    int64_t deadlineMilliseconds_ = 0;
    bool hasLastElapsed_ = false;
    uint64_t lastElapsedMilliseconds_ = 0;
};

}  // namespace ivrdroid::conversation_handoff
