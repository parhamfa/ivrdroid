#include "recording_policy.h"

#include "call_control_protocol.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <ctime>
#include <limits>
#include <vector>

namespace ivrdroid {
namespace {

std::vector<std::string_view> SplitCapacity(std::string_view value) {
    std::vector<std::string_view> tokens;
    size_t start = 0;
    while (start <= value.size()) {
        const size_t separator = value.find(' ', start);
        const size_t end = separator == std::string_view::npos
            ? value.size()
            : separator;
        if (end == start) return {};
        tokens.push_back(value.substr(start, end - start));
        if (separator == std::string_view::npos) break;
        start = separator + 1;
    }
    return tokens;
}

template <typename Integer>
bool ParseCapacityInteger(std::string_view value, Integer* output) {
    if (output == nullptr || value.empty() ||
        (value.size() > 1 && value.front() == '0')) {
        return false;
    }
    Integer parsed = 0;
    const auto result = std::from_chars(
        value.data(),
        value.data() + value.size(),
        parsed);
    if (result.ec != std::errc() || result.ptr != value.data() + value.size()) {
        return false;
    }
    *output = parsed;
    return true;
}

}  // namespace

bool ParseRecordingCapacity(
    std::string_view wire,
    RecordingCapacity* capacity) {
    if (capacity == nullptr || wire.empty() || wire.size() > 256 ||
        wire.back() != '\n' || wire.find('\n') != wire.size() - 1 ||
        wire.find('\r') != std::string_view::npos) {
        return false;
    }
    wire.remove_suffix(1);
    const std::vector<std::string_view> tokens = SplitCapacity(wire);
    RecordingCapacity parsed;
    if (tokens.size() == 2) {
        parsed.version = 1;
        if (!ParseCapacityInteger(tokens[0], &parsed.voicemailBytes) ||
            !ParseCapacityInteger(tokens[1], &parsed.voicemailCount)) {
            return false;
        }
    } else if (tokens.size() == 6 &&
               tokens[0] == "IVRDROID_RECORDING_CAPACITY_V2") {
        parsed.version = 2;
        if (!ParseCapacityInteger(tokens[1], &parsed.voicemailBytes) ||
            !ParseCapacityInteger(tokens[2], &parsed.voicemailCount) ||
            !ParseCapacityInteger(tokens[3], &parsed.conversationBytes) ||
            !ParseCapacityInteger(tokens[4], &parsed.conversationCount) ||
            !ParseCapacityInteger(tokens[5], &parsed.filesystemFreeBytes)) {
            return false;
        }
    } else {
        return false;
    }
    *capacity = parsed;
    return true;
}

RecordingStopDecision DecideRecordingStop(
    char configuredFinishKey,
    char detectedDigit,
    uint64_t capturedFrames,
    uint64_t maximumFrames,
    RecordingCallEvent callEvent) {
    switch (callEvent) {
        case RecordingCallEvent::Hangup:
            return RecordingStopDecision::FinalizeHangup;
        case RecordingCallEvent::Emergency:
            return RecordingStopDecision::EmergencyPreempt;
        case RecordingCallEvent::Multiple:
            return RecordingStopDecision::ExternalPreempt;
        case RecordingCallEvent::Unverified:
            return RecordingStopDecision::UnverifiedPreempt;
        case RecordingCallEvent::Active:
            break;
    }
    if (configuredFinishKey != 0 && detectedDigit == configuredFinishKey) {
        return RecordingStopDecision::FinishKey;
    }
    if (maximumFrames > 0 && capturedFrames >= maximumFrames) {
        return RecordingStopDecision::MaximumDuration;
    }
    return RecordingStopDecision::Continue;
}

size_t BufferedFramesToCommit(
    RecordingStopDecision decision,
    size_t bufferedFrames) {
    return decision == RecordingStopDecision::MaximumDuration ||
        decision == RecordingStopDecision::FinalizeHangup
        ? bufferedFrames
        : 0;
}

bool RecordingStorageFits(
    uint64_t encryptedSpoolBytes,
    uint64_t inboxBytes,
    uint64_t requiredBytes,
    uint64_t availableFilesystemBytes,
    uint64_t spoolLimitBytes) {
    if (requiredBytes == 0 || requiredBytes > spoolLimitBytes ||
        encryptedSpoolBytes > spoolLimitBytes || inboxBytes > spoolLimitBytes ||
        encryptedSpoolBytes > spoolLimitBytes - inboxBytes ||
        requiredBytes > spoolLimitBytes - encryptedSpoolBytes - inboxBytes) {
        return false;
    }
    return availableFilesystemBytes >= requiredBytes &&
        availableFilesystemBytes - requiredBytes >= requiredBytes;
}

bool ConversationStorageFits(
    uint64_t encryptedConversationBytes,
    uint64_t inboxConversationBytes,
    uint64_t requiredBytes,
    uint64_t observedFilesystemBytes,
    uint64_t publishedFilesystemBytes) {
    if (requiredBytes == 0 || requiredBytes > kConversationSpoolLimitBytes ||
        encryptedConversationBytes > kConversationSpoolLimitBytes ||
        inboxConversationBytes > kConversationSpoolLimitBytes ||
        encryptedConversationBytes >
            kConversationSpoolLimitBytes - inboxConversationBytes ||
        requiredBytes > kConversationSpoolLimitBytes -
            encryptedConversationBytes - inboxConversationBytes) {
        return false;
    }
    const uint64_t available = std::min(
        observedFilesystemBytes,
        publishedFilesystemBytes);
    return available >= requiredBytes &&
        available - requiredBytes >= kConversationFilesystemReserveBytes;
}

ConversationRotationDecision DecideConversationRotation(
    uint64_t currentSegmentFrames,
    bool nextSegmentOpened,
    uint64_t nextSegmentFrames) {
    return currentSegmentFrames == kConversationSegmentMaximumFrames &&
        nextSegmentOpened && nextSegmentFrames > 0
        ? ConversationRotationDecision::FinalizeBoundary
        : ConversationRotationDecision::RetainCurrentForFailure;
}

bool ShouldRemovePartialRecording(
    std::string_view name,
    bool regularFile,
    uint32_t ownerUid,
    uint32_t rootUid,
    uint32_t appUid) {
    const bool partialName =
        !name.empty() && name.front() == '.' &&
        (name.find(".partial") != std::string_view::npos ||
         name.find(".tmp") != std::string_view::npos);
    return partialName && regularFile &&
        (ownerUid == rootUid || ownerUid == appUid);
}

bool IsConversationStopReason(
    std::string_view reason,
    bool partial) {
    if (reason == "recording_failure") return partial;
    return !partial &&
        (reason == "segment_boundary" ||
         reason == "operator_hangup" ||
         reason == "caller_hangup");
}

std::string ConversationSegmentStem(
    std::string_view recordingUuid,
    uint32_t segmentIndex) {
    if (!call_control::IsCanonicalUuid(recordingUuid) ||
        segmentIndex > kConversationMaximumSegmentIndex) {
        return {};
    }
    char suffix[8] = {};
    const int length = std::snprintf(
        suffix,
        sizeof(suffix),
        ".%05u",
        segmentIndex);
    if (length != 6) return {};
    return std::string(recordingUuid) + suffix;
}

bool FormatUtcTimestamp(
    int64_t epochSeconds,
    int32_t milliseconds,
    std::string* output) {
    if (output == nullptr || epochSeconds <= 0 ||
        milliseconds < 0 || milliseconds > 999) {
        return false;
    }
    const time_t seconds = static_cast<time_t>(epochSeconds);
    if (static_cast<int64_t>(seconds) != epochSeconds) return false;
    tm utc {};
    char base[20] = {};
    if (gmtime_r(&seconds, &utc) == nullptr ||
        std::strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &utc) != 19) {
        return false;
    }
    char timestamp[25] = {};
    const int length = std::snprintf(
        timestamp,
        sizeof(timestamp),
        "%s.%03dZ",
        base,
        milliseconds);
    if (length != 24) return false;
    *output = timestamp;
    return true;
}

RecordingHeartbeatWatchdog::RecordingHeartbeatWatchdog(
    int64_t timeoutMilliseconds)
    : timeoutMilliseconds_(timeoutMilliseconds) {
    if (timeoutMilliseconds_ <= 0) timeoutMilliseconds_ = 1;
}

void RecordingHeartbeatWatchdog::Heartbeat(int64_t nowMilliseconds) {
    if (nowMilliseconds > std::numeric_limits<int64_t>::max() - timeoutMilliseconds_) {
        deadlineMilliseconds_ = std::numeric_limits<int64_t>::max();
    } else {
        deadlineMilliseconds_ = nowMilliseconds + timeoutMilliseconds_;
    }
}

bool RecordingHeartbeatWatchdog::Expired(int64_t nowMilliseconds) const {
    return deadlineMilliseconds_ == 0 || nowMilliseconds >= deadlineMilliseconds_;
}

}  // namespace ivrdroid
