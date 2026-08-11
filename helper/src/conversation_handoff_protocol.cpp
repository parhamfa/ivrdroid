#include "conversation_handoff_protocol.h"

#include "call_control_protocol.h"
#include "recording_policy.h"

#include <charconv>
#include <limits>
#include <utility>
#include <vector>

namespace ivrdroid::conversation_handoff {
namespace {

constexpr std::string_view kVersion = "IVRDROID_CONVERSATION_HANDOFF_V1";

std::vector<std::string_view> Split(std::string_view wire) {
    std::vector<std::string_view> tokens;
    size_t start = 0;
    while (start <= wire.size()) {
        const size_t separator = wire.find(' ', start);
        const size_t end = separator == std::string_view::npos
            ? wire.size()
            : separator;
        if (end == start) return {};
        tokens.push_back(wire.substr(start, end - start));
        if (separator == std::string_view::npos) break;
        start = separator + 1;
    }
    return tokens;
}

template <typename Integer>
bool ParseCanonicalUnsigned(
    std::string_view value,
    Integer* output,
    bool allowZero) {
    static_assert(std::numeric_limits<Integer>::is_integer);
    if (output == nullptr || value.empty() ||
        (value.size() > 1 && value.front() == '0')) {
        return false;
    }
    Integer parsed = 0;
    const auto result = std::from_chars(
        value.data(),
        value.data() + value.size(),
        parsed);
    if (result.ec != std::errc() || result.ptr != value.data() + value.size() ||
        (!allowZero && parsed == 0)) {
        return false;
    }
    *output = parsed;
    return true;
}

Result ParseResult(std::string_view value) {
    if (value == "OK") return Result::Ok;
    if (value == "FAILED") return Result::Failed;
    return Result::Invalid;
}

}  // namespace

Acknowledgement ParseAcknowledgement(std::string_view wire) {
    if (wire.empty() || wire.size() > kMaximumWireBytes ||
        wire.back() != '\n' || wire.find('\n') != wire.size() - 1 ||
        wire.find('\r') != std::string_view::npos) {
        return {};
    }
    wire.remove_suffix(1);
    const std::vector<std::string_view> tokens = Split(wire);
    if (tokens.size() != 10 || tokens[0] != kVersion) return {};

    Acknowledgement acknowledgement;
    acknowledgement.result = ParseResult(tokens[8]);
    if (!call_control::IsCanonicalUuid(tokens[1]) ||
        !ParseCanonicalUnsigned(tokens[2], &acknowledgement.revisionId, false) ||
        !call_control::IsCanonicalUuid(tokens[3]) ||
        !call_control::IsCanonicalUuid(tokens[4]) ||
        !ParseCanonicalUnsigned(tokens[5], &acknowledgement.segmentIndex, true) ||
        acknowledgement.segmentIndex > kConversationMaximumSegmentIndex ||
        !call_control::IsCanonicalUuid(tokens[6]) ||
        !ParseCanonicalUnsigned(
            tokens[7],
            &acknowledgement.elapsedMilliseconds,
            true) ||
        acknowledgement.result == Result::Invalid ||
        !call_control::IsReason(tokens[9]) ||
        (acknowledgement.result == Result::Ok && tokens[9] != "-") ||
        (acknowledgement.result == Result::Failed && tokens[9] == "-")) {
        return {};
    }
    acknowledgement.callUuid = std::string(tokens[1]);
    acknowledgement.blockUuid = std::string(tokens[3]);
    acknowledgement.recordingUuid = std::string(tokens[4]);
    acknowledgement.bootUuid = std::string(tokens[6]);
    acknowledgement.reason = std::string(tokens[9]);
    return acknowledgement;
}

std::string EncodeAcknowledgement(const Acknowledgement& acknowledgement) {
    if (!call_control::IsCanonicalUuid(acknowledgement.callUuid) ||
        acknowledgement.revisionId == 0 ||
        !call_control::IsCanonicalUuid(acknowledgement.blockUuid) ||
        !call_control::IsCanonicalUuid(acknowledgement.recordingUuid) ||
        acknowledgement.segmentIndex > kConversationMaximumSegmentIndex ||
        !call_control::IsCanonicalUuid(acknowledgement.bootUuid) ||
        acknowledgement.result == Result::Invalid ||
        !call_control::IsReason(acknowledgement.reason) ||
        (acknowledgement.result == Result::Ok && acknowledgement.reason != "-") ||
        (acknowledgement.result == Result::Failed && acknowledgement.reason == "-")) {
        return {};
    }
    std::string wire(kVersion);
    wire.push_back(' ');
    wire.append(acknowledgement.callUuid);
    wire.push_back(' ');
    wire.append(std::to_string(acknowledgement.revisionId));
    wire.push_back(' ');
    wire.append(acknowledgement.blockUuid);
    wire.push_back(' ');
    wire.append(acknowledgement.recordingUuid);
    wire.push_back(' ');
    wire.append(std::to_string(acknowledgement.segmentIndex));
    wire.push_back(' ');
    wire.append(acknowledgement.bootUuid);
    wire.push_back(' ');
    wire.append(std::to_string(acknowledgement.elapsedMilliseconds));
    wire.push_back(' ');
    wire.append(ToString(acknowledgement.result));
    wire.push_back(' ');
    wire.append(acknowledgement.reason);
    wire.push_back('\n');
    return wire.size() <= kMaximumWireBytes ? wire : std::string();
}

const char* ToString(Result result) {
    switch (result) {
        case Result::Ok:
            return "OK";
        case Result::Failed:
            return "FAILED";
        case Result::Invalid:
            return "INVALID";
    }
    return "INVALID";
}

Policy::Policy(
    std::string callUuid,
    uint64_t revisionId,
    std::string blockUuid,
    std::string recordingUuid,
    std::string bootUuid)
    : callUuid_(std::move(callUuid)),
      revisionId_(revisionId),
      blockUuid_(std::move(blockUuid)),
      recordingUuid_(std::move(recordingUuid)),
      bootUuid_(std::move(bootUuid)) {
    valid_ = call_control::IsCanonicalUuid(callUuid_) && revisionId_ > 0 &&
        call_control::IsCanonicalUuid(blockUuid_) &&
        call_control::IsCanonicalUuid(recordingUuid_) &&
        call_control::IsCanonicalUuid(bootUuid_);
}

bool Policy::Correlates(const Acknowledgement& acknowledgement) const {
    return acknowledgement.result != Result::Invalid &&
        acknowledgement.callUuid == callUuid_ &&
        acknowledgement.revisionId == revisionId_ &&
        acknowledgement.blockUuid == blockUuid_ &&
        acknowledgement.recordingUuid == recordingUuid_ &&
        acknowledgement.bootUuid == bootUuid_;
}

bool Policy::MarkSegmentFinalized(
    uint32_t segmentIndex,
    int64_t nowMilliseconds) {
    if (!valid_ || pending_ || nowMilliseconds < 0 ||
        segmentIndex != nextExpectedSegmentIndex_ ||
        segmentIndex > kConversationMaximumSegmentIndex) {
        return false;
    }
    pending_ = true;
    pendingSegmentIndex_ = segmentIndex;
    deadlineMilliseconds_ = nowMilliseconds + kAcknowledgementTimeoutMilliseconds;
    return deadlineMilliseconds_ >= nowMilliseconds;
}

Decision Policy::Observe(
    const Acknowledgement& acknowledgement,
    int64_t nowMilliseconds) {
    if (!valid_ || acknowledgement.result == Result::Invalid ||
        nowMilliseconds < 0) {
        return Decision::ProtocolFailure;
    }
    if (!Correlates(acknowledgement)) return Decision::IgnoredForeign;
    if (!pending_) {
        return acknowledgement.segmentIndex < nextExpectedSegmentIndex_
            ? Decision::Stale
            : Decision::ProtocolFailure;
    }
    if (nowMilliseconds > deadlineMilliseconds_) return Decision::TimedOut;
    if (acknowledgement.segmentIndex < pendingSegmentIndex_) {
        return Decision::Stale;
    }
    if (acknowledgement.segmentIndex != pendingSegmentIndex_ ||
        (hasLastElapsed_ &&
         acknowledgement.elapsedMilliseconds < lastElapsedMilliseconds_)) {
        return Decision::ProtocolFailure;
    }
    if (acknowledgement.result == Result::Failed) return Decision::Failed;

    pending_ = false;
    hasLastElapsed_ = true;
    lastElapsedMilliseconds_ = acknowledgement.elapsedMilliseconds;
    if (nextExpectedSegmentIndex_ <= kConversationMaximumSegmentIndex) {
        ++nextExpectedSegmentIndex_;
    }
    return Decision::Accepted;
}

Decision Policy::CheckDeadline(int64_t nowMilliseconds) const {
    if (!valid_ || nowMilliseconds < 0) return Decision::ProtocolFailure;
    return pending_ && nowMilliseconds > deadlineMilliseconds_
        ? Decision::TimedOut
        : Decision::Awaiting;
}

}  // namespace ivrdroid::conversation_handoff
