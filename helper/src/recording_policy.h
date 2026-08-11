#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ivrdroid {

constexpr int kRecordingHeartbeatTimeoutMilliseconds = 3'000;
constexpr int kRecordingFinalizationTimeoutMilliseconds = 30'000;
constexpr uint32_t kMaximumRecordingSteps = 64;
constexpr uint32_t kConversationSegmentMaximumMilliseconds = 180'000;
constexpr uint32_t kConversationMaximumSegmentIndex = 65'535;
constexpr uint64_t kConversationSegmentMaximumFrames = 8'640'000;
constexpr uint64_t kConversationSpoolLimitBytes = 4ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr uint64_t kConversationFilesystemReserveBytes = 512ULL * 1024ULL * 1024ULL;

struct RecordingCapacity {
    uint32_t version = 0;
    uint64_t voicemailBytes = 0;
    uint32_t voicemailCount = 0;
    uint64_t conversationBytes = 0;
    uint32_t conversationCount = 0;
    uint64_t filesystemFreeBytes = 0;
};

bool ParseRecordingCapacity(
    std::string_view wire,
    RecordingCapacity* capacity);

enum class RecordingCallEvent {
    Active,
    Hangup,
    Emergency,
    Multiple,
    Unverified,
};

enum class RecordingStopDecision {
    Continue,
    FinishKey,
    MaximumDuration,
    FinalizeHangup,
    EmergencyPreempt,
    ExternalPreempt,
    UnverifiedPreempt,
};

RecordingStopDecision DecideRecordingStop(
    char configuredFinishKey,
    char detectedDigit,
    uint64_t capturedFrames,
    uint64_t maximumFrames,
    RecordingCallEvent callEvent);

size_t BufferedFramesToCommit(
    RecordingStopDecision decision,
    size_t bufferedFrames);

bool RecordingStorageFits(
    uint64_t encryptedSpoolBytes,
    uint64_t inboxBytes,
    uint64_t requiredBytes,
    uint64_t availableFilesystemBytes,
    uint64_t spoolLimitBytes);

bool ConversationStorageFits(
    uint64_t encryptedConversationBytes,
    uint64_t inboxConversationBytes,
    uint64_t requiredBytes,
    uint64_t observedFilesystemBytes,
    uint64_t publishedFilesystemBytes);

enum class ConversationRotationDecision {
    FinalizeBoundary,
    RetainCurrentForFailure,
};

ConversationRotationDecision DecideConversationRotation(
    uint64_t currentSegmentFrames,
    bool nextSegmentOpened,
    uint64_t nextSegmentFrames);

bool ShouldRemovePartialRecording(
    std::string_view name,
    bool regularFile,
    uint32_t ownerUid,
    uint32_t rootUid,
    uint32_t appUid);

bool IsConversationStopReason(
    std::string_view reason,
    bool partial);

std::string ConversationSegmentStem(
    std::string_view recordingUuid,
    uint32_t segmentIndex);

bool FormatUtcTimestamp(
    int64_t epochSeconds,
    int32_t milliseconds,
    std::string* output);

class RecordingHeartbeatWatchdog {
public:
    explicit RecordingHeartbeatWatchdog(int64_t timeoutMilliseconds);
    void Heartbeat(int64_t nowMilliseconds);
    bool Expired(int64_t nowMilliseconds) const;

private:
    int64_t timeoutMilliseconds_;
    int64_t deadlineMilliseconds_ = 0;
};

}  // namespace ivrdroid
