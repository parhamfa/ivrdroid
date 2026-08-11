#include "recording_policy.h"

#include <cassert>
#include <iostream>

int main() {
    using ivrdroid::RecordingCallEvent;
    using ivrdroid::RecordingStopDecision;

    ivrdroid::RecordingCapacity legacyCapacity;
    assert(ivrdroid::ParseRecordingCapacity("123 4\n", &legacyCapacity));
    assert(legacyCapacity.version == 1);
    assert(legacyCapacity.voicemailBytes == 123);
    assert(legacyCapacity.voicemailCount == 4);
    ivrdroid::RecordingCapacity v2Capacity;
    assert(ivrdroid::ParseRecordingCapacity(
        "IVRDROID_RECORDING_CAPACITY_V2 123 4 567 8 1073741824\n",
        &v2Capacity));
    assert(v2Capacity.version == 2);
    assert(v2Capacity.conversationBytes == 567);
    assert(v2Capacity.conversationCount == 8);
    assert(v2Capacity.filesystemFreeBytes == 1'073'741'824);
    assert(!ivrdroid::ParseRecordingCapacity(
        "IVRDROID_RECORDING_CAPACITY_V2 123 4 567 8\n",
        &v2Capacity));
    assert(!ivrdroid::ParseRecordingCapacity("0123 4\n", &legacyCapacity));

    assert(ivrdroid::DecideRecordingStop('#', '#', 1'200, 48'000, RecordingCallEvent::Active) ==
        RecordingStopDecision::FinishKey);
    assert(ivrdroid::DecideRecordingStop(0, '#', 1'200, 48'000, RecordingCallEvent::Active) ==
        RecordingStopDecision::Continue);
    assert(ivrdroid::DecideRecordingStop('#', 0, 48'000, 48'000, RecordingCallEvent::Active) ==
        RecordingStopDecision::MaximumDuration);
    assert(ivrdroid::DecideRecordingStop('#', 0, 2'400, 48'000, RecordingCallEvent::Hangup) ==
        RecordingStopDecision::FinalizeHangup);
    assert(ivrdroid::DecideRecordingStop('#', 0, 2'400, 48'000, RecordingCallEvent::Emergency) ==
        RecordingStopDecision::EmergencyPreempt);
    assert(ivrdroid::DecideRecordingStop('#', 0, 2'400, 48'000, RecordingCallEvent::Multiple) ==
        RecordingStopDecision::ExternalPreempt);
    assert(ivrdroid::DecideRecordingStop('#', 0, 2'400, 48'000, RecordingCallEvent::Unverified) ==
        RecordingStopDecision::UnverifiedPreempt);

    // Finish-key frames stay in the trim buffer and are never committed.
    assert(ivrdroid::BufferedFramesToCommit(RecordingStopDecision::FinishKey, 4) == 0);
    assert(ivrdroid::BufferedFramesToCommit(RecordingStopDecision::MaximumDuration, 4) == 4);
    assert(ivrdroid::BufferedFramesToCommit(RecordingStopDecision::FinalizeHangup, 3) == 3);

    assert(ivrdroid::RecordingStorageFits(100, 100, 100, 1'000, 512));
    assert(!ivrdroid::RecordingStorageFits(400, 100, 100, 1'000, 512));
    assert(!ivrdroid::RecordingStorageFits(100, 100, 100, 150, 512));
    assert(ivrdroid::ConversationStorageFits(
        1024,
        1024,
        34'560'044,
        ivrdroid::kConversationFilesystemReserveBytes + 34'560'044,
        ivrdroid::kConversationFilesystemReserveBytes + 34'560'044));
    assert(!ivrdroid::ConversationStorageFits(
        1024,
        1024,
        34'560'044,
        ivrdroid::kConversationFilesystemReserveBytes + 34'560'043,
        ivrdroid::kConversationFilesystemReserveBytes + 34'560'044));
    assert(!ivrdroid::ConversationStorageFits(
        ivrdroid::kConversationSpoolLimitBytes - 10,
        0,
        11,
        UINT64_MAX,
        UINT64_MAX));
    assert(ivrdroid::DecideConversationRotation(
        ivrdroid::kConversationSegmentMaximumFrames,
        true,
        1'200) == ivrdroid::ConversationRotationDecision::FinalizeBoundary);
    assert(ivrdroid::DecideConversationRotation(
        ivrdroid::kConversationSegmentMaximumFrames,
        true,
        0) ==
        ivrdroid::ConversationRotationDecision::RetainCurrentForFailure);
    assert(ivrdroid::DecideConversationRotation(
        ivrdroid::kConversationSegmentMaximumFrames,
        false,
        0) ==
        ivrdroid::ConversationRotationDecision::RetainCurrentForFailure);

    assert(ivrdroid::ShouldRemovePartialRecording(
        ".abc.wav.partial", true, 0, 0, 10'123));
    assert(ivrdroid::ShouldRemovePartialRecording(
        ".abc.json.tmp", true, 10'123, 0, 10'123));
    assert(!ivrdroid::ShouldRemovePartialRecording(
        ".abc.wav.partial", false, 0, 0, 10'123));
    assert(!ivrdroid::ShouldRemovePartialRecording(
        ".abc.wav.partial", true, 20'000, 0, 10'123));
    assert(!ivrdroid::ShouldRemovePartialRecording(
        "abc.wav", true, 0, 0, 10'123));

    assert(ivrdroid::IsConversationStopReason("segment_boundary", false));
    assert(ivrdroid::IsConversationStopReason("operator_hangup", false));
    assert(ivrdroid::IsConversationStopReason("caller_hangup", false));
    assert(ivrdroid::IsConversationStopReason("recording_failure", true));
    assert(!ivrdroid::IsConversationStopReason("recording_failure", false));
    assert(!ivrdroid::IsConversationStopReason("system_failure", true));
    assert(ivrdroid::ConversationSegmentStem(
        "11111111-1111-4111-8111-111111111111", 0) ==
        "11111111-1111-4111-8111-111111111111.00000");
    assert(ivrdroid::ConversationSegmentStem(
        "11111111-1111-4111-8111-111111111111", 65'535) ==
        "11111111-1111-4111-8111-111111111111.65535");
    assert(ivrdroid::ConversationSegmentStem(
        "11111111-1111-4111-8111-111111111111", 65'536).empty());
    std::string conferenceTimestamp;
    std::string firstFrameTimestamp;
    assert(ivrdroid::FormatUtcTimestamp(
        1'786'190'400, 123, &conferenceTimestamp));
    assert(ivrdroid::FormatUtcTimestamp(
        1'786'190'400, 456, &firstFrameTimestamp));
    assert(conferenceTimestamp == "2026-08-08T12:00:00.123Z");
    assert(firstFrameTimestamp == "2026-08-08T12:00:00.456Z");
    assert(firstFrameTimestamp > conferenceTimestamp);

    ivrdroid::RecordingHeartbeatWatchdog watchdog(
        ivrdroid::kRecordingHeartbeatTimeoutMilliseconds);
    watchdog.Heartbeat(1'000);
    assert(!watchdog.Expired(3'999));
    watchdog.Heartbeat(3'500);
    assert(!watchdog.Expired(6'499));
    assert(watchdog.Expired(6'500));
    ivrdroid::RecordingHeartbeatWatchdog finalization(
        ivrdroid::kRecordingFinalizationTimeoutMilliseconds);
    finalization.Heartbeat(10'000);
    assert(!finalization.Expired(39'999));
    assert(finalization.Expired(40'000));

    // Each explicit record step receives its own bounded sequence slot.
    for (uint32_t sequence = 0; sequence < ivrdroid::kMaximumRecordingSteps; ++sequence) {
        assert(sequence < 64);
    }
    std::cout << "Recording policy tests passed." << std::endl;
    return 0;
}
