#include "external_call_policy.h"

#include <cassert>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>

namespace {

constexpr char kSession[] = "11111111-1111-4111-8111-111111111111";
constexpr char kBlock[] = "22222222-2222-4222-8222-222222222222";
constexpr char kBoot[] = "33333333-3333-4333-8333-333333333333";

ivrdroid::call_control::Status Status(
    ivrdroid::call_control::StatusKind kind,
    uint64_t sequence,
    uint64_t elapsed = 0,
    const char* reason = "-") {
    return {
        kind,
        kSession,
        17,
        kBlock,
        sequence,
        kBoot,
        elapsed,
        reason,
    };
}

ivrdroid::ExternalCallPolicy Policy() {
    return ivrdroid::ExternalCallPolicy(
        kSession,
        17,
        kBlock,
        kBoot,
        30'000,
        1'000);
}

std::string ReadFixture(const char* path) {
    std::ifstream input(path);
    std::ostringstream value;
    value << input.rdbuf();
    assert(input.good() || input.eof());
    return value.str();
}

}  // namespace

int main(int argc, char** argv) {
    using ivrdroid::ExternalCallDecision;
    using ivrdroid::ExternalCallStage;
    using ivrdroid::call_control::StatusKind;

    // A caller may leave at every pre-conference stage. Replaying the exact
    // terminal status also satisfies a teardown that started after it arrived.
    for (int phase = 0; phase < 7; ++phase) {
        auto early = Policy();
        uint64_t sequence = 0;
        if (phase >= 1) early.Observe(Status(StatusKind::Ack, ++sequence), 1100);
        if (phase >= 2) early.Observe(Status(StatusKind::CallerHeld, ++sequence), 1150);
        if (phase >= 3) early.Observe(Status(StatusKind::Dialing, ++sequence), 1200);
        if (phase >= 4) {
            early.Observe(Status(StatusKind::OperatorAnswered, ++sequence), 1250);
            assert(early.MarkRecorderReady(1260));
        }
        if (phase >= 5) early.Observe(Status(StatusKind::Merging, ++sequence), 1300);
        if (phase >= 6) early.Observe(Status(StatusKind::Conferenced, ++sequence), 1350);
        auto end = Status(StatusKind::Completed, ++sequence, 1400, "CALLER_HANGUP");
        assert(early.Observe(end, 1400) == ExternalCallDecision::CallerHangup);
        assert(early.Observe(end, 1401) == ExternalCallDecision::Duplicate);
        auto repeated = end; repeated.sequence++; repeated.elapsedMilliseconds++;
        assert(early.Observe(repeated, 1402) == ExternalCallDecision::Duplicate);
        assert(early.Observe(Status(StatusKind::Dialing, 1), 1403) == ExternalCallDecision::Duplicate);
        ivrdroid::ExternalCallTeardownPolicy teardown(kSession, 17, kBlock, kBoot,
            early.lastSequence(), early.lastElapsedMilliseconds(), early.lastKind(), early.lastReason(),
            {StatusKind::Completed, "CALLER_HANGUP"});
        assert(teardown.Observe(end) == ivrdroid::ExternalCallTeardownDecision::MatchedTerminal);
        assert(teardown.Observe(repeated) == ivrdroid::ExternalCallTeardownDecision::Duplicate);
        ivrdroid::ExternalCallTeardownPolicy cancelRaced(kSession, 17, kBlock, kBoot,
            early.lastSequence(), early.lastElapsedMilliseconds(), early.lastKind(), early.lastReason(),
            {StatusKind::SystemFailure, "HELPER_CANCELLED"});
        assert(cancelRaced.Observe(end) == ivrdroid::ExternalCallTeardownDecision::MatchedTerminal);
    }
    auto prematureOperator = Policy();
    assert(prematureOperator.Observe(Status(StatusKind::Completed, 1, 1100, "OPERATOR_HANGUP"), 1100)
        == ExternalCallDecision::ProtocolFailure);

    auto call = Policy();
    assert(call.Observe(Status(StatusKind::Ack, 1), 1'100) ==
        ExternalCallDecision::DialingHeartbeat);
    assert(call.Observe(Status(StatusKind::Dialing, 2, 10), 1'200) ==
        ExternalCallDecision::DialingHeartbeat);
    assert(call.Observe(Status(StatusKind::OperatorAnswered, 3, 20), 1'300) ==
        ExternalCallDecision::StartRecorder);
    assert(call.stage() == ExternalCallStage::OperatorAnswered);
    assert(call.MarkRecorderReady(1'350));
    assert(call.stage() == ExternalCallStage::RecorderReady);
    assert(call.Observe(Status(StatusKind::Merging, 4, 30), 1'400) ==
        ExternalCallDecision::Merging);
    assert(call.Observe(Status(StatusKind::Merging, 5, 40), 2'300) ==
        ExternalCallDecision::Merging);
    assert(call.Observe(Status(StatusKind::Conferenced, 6, 50), 2'400) ==
        ExternalCallDecision::Conferenced);
    assert(call.CheckDeadline(1'000'000) == ExternalCallDecision::ControlTimeout);

    auto completed = Policy();
    assert(completed.Observe(Status(StatusKind::OperatorAnswered, 1), 1'100) ==
        ExternalCallDecision::StartRecorder);
    assert(completed.MarkRecorderReady(1'101));
    assert(completed.Observe(Status(StatusKind::Merging, 2), 1'200) ==
        ExternalCallDecision::Merging);
    assert(completed.Observe(Status(StatusKind::Conferenced, 3), 1'300) ==
        ExternalCallDecision::Conferenced);
    assert(completed.Observe(Status(
        StatusKind::Completed, 4, 300, "OPERATOR_HANGUP"), 1'400) ==
        ExternalCallDecision::Completed);
    assert(completed.Observe(Status(StatusKind::Completed, 5, 310, "CALLER_HANGUP"), 1'410) ==
        ExternalCallDecision::Duplicate);

    auto callerEnded = Policy();
    assert(callerEnded.Observe(Status(StatusKind::OperatorAnswered, 1), 1'100) ==
        ExternalCallDecision::StartRecorder);
    assert(callerEnded.MarkRecorderReady(1'101));
    assert(callerEnded.Observe(Status(StatusKind::Merging, 2), 1'200) ==
        ExternalCallDecision::Merging);
    assert(callerEnded.Observe(Status(StatusKind::Conferenced, 3), 1'300) ==
        ExternalCallDecision::Conferenced);
    assert(callerEnded.Observe(Status(
        StatusKind::Completed, 4, 300, "CALLER_HANGUP"), 1'400) ==
        ExternalCallDecision::CallerHangup);
    assert(callerEnded.Observe(Status(StatusKind::Completed, 5, 310, "OPERATOR_HANGUP"), 1'410) ==
        ExternalCallDecision::Duplicate);

    auto answeredDeadline = Policy();
    assert(answeredDeadline.Observe(Status(StatusKind::Dialing, 1), 1'500) ==
        ExternalCallDecision::DialingHeartbeat);
    uint64_t answerSequence = 2;
    for (int64_t now = 4'000; now <= 29'000; now += 2'500) {
        assert(answeredDeadline.Observe(
            Status(StatusKind::Dialing, answerSequence++, now - 1'500), now) ==
            ExternalCallDecision::DialingHeartbeat);
    }
    assert(answeredDeadline.CheckDeadline(31'500) ==
        ExternalCallDecision::AnswerTimeout);
    assert(answeredDeadline.Observe(
        Status(StatusKind::NotConnected, answerSequence, 30'000), 31'400) ==
        ExternalCallDecision::NotConnected);

    auto delayedSetup = Policy();
    assert(delayedSetup.Observe(Status(StatusKind::Ack, 1), 3'000) ==
        ExternalCallDecision::DialingHeartbeat);
    assert(delayedSetup.Observe(Status(StatusKind::CallerHeld, 2), 5'999) ==
        ExternalCallDecision::DialingHeartbeat);
    assert(delayedSetup.Observe(Status(StatusKind::CallerHeld, 3), 8'998) ==
        ExternalCallDecision::DialingHeartbeat);
    assert(delayedSetup.CheckDeadline(10'999) ==
        ExternalCallDecision::DialingHeartbeat);
    assert(delayedSetup.CheckDeadline(11'000) ==
        ExternalCallDecision::SetupTimeout);

    auto lateDialing = Policy();
    assert(lateDialing.Observe(Status(StatusKind::Ack, 1), 3'000) ==
        ExternalCallDecision::DialingHeartbeat);
    assert(lateDialing.Observe(Status(StatusKind::CallerHeld, 2), 5'500) ==
        ExternalCallDecision::DialingHeartbeat);
    assert(lateDialing.Observe(Status(StatusKind::CallerHeld, 3), 8'000) ==
        ExternalCallDecision::DialingHeartbeat);
    assert(lateDialing.Observe(Status(StatusKind::Dialing, 4), 10'500) ==
        ExternalCallDecision::DialingHeartbeat);
    uint64_t lateSequence = 5;
    for (int64_t now = 13'000; now <= 38'000; now += 2'500) {
        assert(lateDialing.Observe(
            Status(StatusKind::Dialing, lateSequence++, now - 10'500), now) ==
            ExternalCallDecision::DialingHeartbeat);
    }
    assert(lateDialing.CheckDeadline(40'500) ==
        ExternalCallDecision::AnswerTimeout);

    auto recorderGate = Policy();
    assert(recorderGate.Observe(Status(StatusKind::OperatorAnswered, 1), 1'100) ==
        ExternalCallDecision::StartRecorder);
    assert(recorderGate.Observe(Status(StatusKind::Merging, 2), 1'200) ==
        ExternalCallDecision::ProtocolFailure);

    auto mergeTimeout = Policy();
    assert(mergeTimeout.Observe(Status(StatusKind::OperatorAnswered, 1), 1'100) ==
        ExternalCallDecision::StartRecorder);
    assert(mergeTimeout.MarkRecorderReady(1'101));
    assert(mergeTimeout.Observe(Status(StatusKind::Merging, 2), 1'200) ==
        ExternalCallDecision::Merging);
    uint64_t mergeSequence = 3;
    for (int64_t now = 3'000; now <= 10'000; now += 2'000) {
        assert(mergeTimeout.Observe(
            Status(StatusKind::Merging, mergeSequence++, now - 1'200), now) ==
            ExternalCallDecision::Merging);
    }
    assert(mergeTimeout.CheckDeadline(11'200) == ExternalCallDecision::MergeTimeout);

    auto duplicate = Policy();
    const auto dialing = Status(StatusKind::Dialing, 1, 100);
    assert(duplicate.Observe(dialing, 1'100) ==
        ExternalCallDecision::DialingHeartbeat);
    assert(duplicate.Observe(dialing, 3'000) == ExternalCallDecision::Duplicate);
    assert(duplicate.CheckDeadline(4'100) == ExternalCallDecision::ControlTimeout);

    auto replay = Policy();
    assert(replay.Observe(Status(StatusKind::Dialing, 2, 100), 1'100) ==
        ExternalCallDecision::DialingHeartbeat);
    assert(replay.Observe(Status(StatusKind::Ack, 1, 101), 1'200) ==
        ExternalCallDecision::ProtocolFailure);

    auto foreign = Policy();
    auto wrong = Status(StatusKind::Ack, 1);
    wrong.blockUuid = "44444444-4444-4444-8444-444444444444";
    assert(foreign.Observe(wrong, 1'100) == ExternalCallDecision::IgnoredForeign);
    assert(foreign.CheckDeadline(4'000) == ExternalCallDecision::ControlTimeout);

    auto heartbeat = Policy();
    assert(heartbeat.Observe(Status(StatusKind::Ack, 1), 1'100) ==
        ExternalCallDecision::DialingHeartbeat);
    assert(heartbeat.CheckDeadline(4'099) == ExternalCallDecision::DialingHeartbeat);
    assert(heartbeat.CheckDeadline(4'100) == ExternalCallDecision::ControlTimeout);

    auto preMergeDisconnect = Policy();
    assert(preMergeDisconnect.Observe(
        Status(StatusKind::OperatorAnswered, 1), 1'100) ==
        ExternalCallDecision::StartRecorder);
    assert(preMergeDisconnect.MarkRecorderReady(1'101));
    assert(preMergeDisconnect.Observe(Status(StatusKind::Merging, 2), 1'200) ==
        ExternalCallDecision::Merging);
    assert(preMergeDisconnect.Observe(Status(StatusKind::NotConnected, 3), 1'300) ==
        ExternalCallDecision::NotConnected);

    auto unsafeCleanup = Policy();
    assert(unsafeCleanup.Observe(Status(StatusKind::OperatorAnswered, 1), 1'100) ==
        ExternalCallDecision::StartRecorder);
    assert(unsafeCleanup.Observe(Status(
        StatusKind::SystemFailure, 2, 100, "CLEANUP_TIMEOUT"), 1'200) ==
        ExternalCallDecision::CleanupTimeout);

    ivrdroid::ExternalCallTeardownPolicy answerCleanup(
        kSession,
        17,
        kBlock,
        kBoot,
        7,
        5'000,
        StatusKind::Dialing,
        "-",
        {StatusKind::NotConnected, "ANSWER_TIMEOUT"});
    assert(answerCleanup.Observe(Status(StatusKind::Dialing, 7, 5'000)) ==
        ivrdroid::ExternalCallTeardownDecision::Duplicate);
    assert(answerCleanup.Observe(Status(StatusKind::Dialing, 8, 6'000)) ==
        ivrdroid::ExternalCallTeardownDecision::Heartbeat);
    assert(answerCleanup.Observe(Status(
        StatusKind::NotConnected, 9, 7'000, "ANSWER_TIMEOUT")) ==
        ivrdroid::ExternalCallTeardownDecision::MatchedTerminal);

    ivrdroid::ExternalCallTeardownPolicy mismatchedCleanup(
        kSession,
        17,
        kBlock,
        kBoot,
        7,
        5'000,
        StatusKind::Dialing,
        "-",
        {StatusKind::NotConnected, "ANSWER_TIMEOUT"});
    assert(mismatchedCleanup.Observe(Status(
        StatusKind::SystemFailure, 8, 6'000, "HELPER_CANCELLED")) ==
        ivrdroid::ExternalCallTeardownDecision::MismatchedTerminal);

    ivrdroid::ExternalCallTeardownPolicy cleanupTimeout(
        kSession,
        17,
        kBlock,
        kBoot,
        7,
        5'000,
        StatusKind::Dialing,
        "-",
        {StatusKind::NotConnected, "ANSWER_TIMEOUT"});
    assert(cleanupTimeout.Observe(Status(
        StatusKind::SystemFailure, 8, 6'000, "CLEANUP_TIMEOUT")) ==
        ivrdroid::ExternalCallTeardownDecision::CleanupTimeout);

    assert(argc == 3);
    const auto sharedAnswerCancel = ivrdroid::call_control::ParseRequest(
        ReadFixture(argv[1]));
    const auto sharedAnswerTerminal = ivrdroid::call_control::ParseStatus(
        ReadFixture(argv[2]));
    assert(sharedAnswerCancel.kind ==
        ivrdroid::call_control::RequestKind::Cancel);
    assert(sharedAnswerCancel.reason == "ANSWER_TIMEOUT");
    assert(sharedAnswerTerminal.kind == StatusKind::NotConnected);
    assert(sharedAnswerTerminal.reason == "ANSWER_TIMEOUT");
    ivrdroid::ExternalCallTeardownPolicy sharedAnswerCleanup(
        sharedAnswerTerminal.sessionUuid,
        sharedAnswerTerminal.revisionId,
        sharedAnswerTerminal.blockUuid,
        sharedAnswerTerminal.bootUuid,
        sharedAnswerTerminal.sequence - 1,
        sharedAnswerCancel.elapsedMilliseconds,
        StatusKind::Dialing,
        "-",
        {StatusKind::NotConnected, "ANSWER_TIMEOUT"});
    assert(sharedAnswerCleanup.Observe(sharedAnswerTerminal) ==
        ivrdroid::ExternalCallTeardownDecision::MatchedTerminal);
    auto sharedMismatchedTerminal = sharedAnswerTerminal;
    sharedMismatchedTerminal.kind = StatusKind::SystemFailure;
    sharedMismatchedTerminal.reason = "HELPER_CANCELLED";
    ivrdroid::ExternalCallTeardownPolicy sharedMismatchedCleanup(
        sharedAnswerTerminal.sessionUuid,
        sharedAnswerTerminal.revisionId,
        sharedAnswerTerminal.blockUuid,
        sharedAnswerTerminal.bootUuid,
        sharedAnswerTerminal.sequence - 1,
        sharedAnswerCancel.elapsedMilliseconds,
        StatusKind::Dialing,
        "-",
        {StatusKind::NotConnected, "ANSWER_TIMEOUT"});
    assert(sharedMismatchedCleanup.Observe(sharedMismatchedTerminal) ==
        ivrdroid::ExternalCallTeardownDecision::MismatchedTerminal);

    constexpr uint64_t kOriginalCaller = 0x123456789abcdef0ULL;
    ivrdroid::ExternalCallerSafePolicy callerSafe(
        kOriginalCaller,
        10'000,
        1'000,
        2'500);
    assert(callerSafe.Observe(
        ivrdroid::CallDisposition::SingleSafe,
        kOriginalCaller,
        10'100) == ivrdroid::ExternalCallerSafeDecision::Waiting);
    assert(callerSafe.Observe(
        ivrdroid::CallDisposition::SingleSafe,
        kOriginalCaller,
        11'099) == ivrdroid::ExternalCallerSafeDecision::Waiting);
    assert(callerSafe.Observe(
        ivrdroid::CallDisposition::SingleSafe,
        kOriginalCaller,
        11'100) == ivrdroid::ExternalCallerSafeDecision::Stable);
    ivrdroid::ExternalCallerSafePolicy transientUnknown(
        kOriginalCaller,
        12'000,
        1'000,
        3'000);
    assert(transientUnknown.Observe(
        ivrdroid::CallDisposition::SingleSafe,
        kOriginalCaller,
        12'100) == ivrdroid::ExternalCallerSafeDecision::Waiting);
    assert(transientUnknown.Observe(
        ivrdroid::CallDisposition::Unknown,
        0,
        12'600) == ivrdroid::ExternalCallerSafeDecision::Waiting);
    assert(transientUnknown.Observe(
        ivrdroid::CallDisposition::SingleSafe,
        kOriginalCaller,
        12'700) == ivrdroid::ExternalCallerSafeDecision::Waiting);
    assert(transientUnknown.Observe(
        ivrdroid::CallDisposition::SingleSafe,
        kOriginalCaller,
        13'699) == ivrdroid::ExternalCallerSafeDecision::Waiting);
    assert(transientUnknown.Observe(
        ivrdroid::CallDisposition::SingleSafe,
        kOriginalCaller,
        13'700) == ivrdroid::ExternalCallerSafeDecision::Stable);
    ivrdroid::ExternalCallerSafePolicy unknownTimeout(
        kOriginalCaller,
        14'000,
        1'000,
        2'500);
    assert(unknownTimeout.Observe(
        ivrdroid::CallDisposition::Unknown,
        0,
        14'100) == ivrdroid::ExternalCallerSafeDecision::Waiting);
    assert(unknownTimeout.Observe(
        ivrdroid::CallDisposition::Unknown,
        0,
        16'500) == ivrdroid::ExternalCallerSafeDecision::TimedOut);
    ivrdroid::ExternalCallerSafePolicy wrongCaller(
        kOriginalCaller,
        20'000,
        1'000,
        2'500);
    assert(wrongCaller.Observe(
        ivrdroid::CallDisposition::SingleSafe,
        0xfeedULL,
        20'100) == ivrdroid::ExternalCallerSafeDecision::Unsafe);
    ivrdroid::ExternalCallerSafePolicy multipleCalls(
        kOriginalCaller,
        30'000,
        1'000,
        2'500);
    assert(multipleCalls.Observe(
        ivrdroid::CallDisposition::Multiple,
        0,
        30'100) == ivrdroid::ExternalCallerSafeDecision::Unsafe);
    ivrdroid::ExternalCallerSafePolicy idleCaller(
        kOriginalCaller,
        40'000,
        1'000,
        2'500);
    assert(idleCaller.Observe(
        ivrdroid::CallDisposition::Idle,
        0,
        40'100) == ivrdroid::ExternalCallerSafeDecision::Unsafe);
    ivrdroid::ExternalCallerSafePolicy emergencyCaller(
        kOriginalCaller,
        50'000,
        1'000,
        2'500);
    assert(emergencyCaller.Observe(
        ivrdroid::CallDisposition::Emergency,
        0,
        50'100) == ivrdroid::ExternalCallerSafeDecision::Unsafe);

    ivrdroid::ExternalCallGuardianBudget budget(1'000, 20'000);
    assert(budget.deadlineMilliseconds() == 21'000);
    budget.EnterConference(6'000);
    assert(budget.suspended());
    assert(budget.deadlineMilliseconds() == std::numeric_limits<int64_t>::max());
    budget.EnterConference(50'000);
    budget.LeaveConference(106'000);
    assert(!budget.suspended());
    assert(budget.deadlineMilliseconds() == 121'000);

    using ivrdroid::ExternalCallRuntimeAction;
    using ivrdroid::ExternalCallRuntimeResult;
    assert(!ivrdroid::AllowsConversationPersistenceStart(
        ivrdroid::ExternalCallStage::OperatorAnswered));
    assert(!ivrdroid::AllowsConversationPersistenceStart(
        ivrdroid::ExternalCallStage::RecorderReady));
    assert(!ivrdroid::AllowsConversationPersistenceStart(
        ivrdroid::ExternalCallStage::Merging));
    assert(ivrdroid::AllowsConversationPersistenceStart(
        ivrdroid::ExternalCallStage::Conferenced));
    assert(!ivrdroid::AllowsConversationPersistenceStart(
        ivrdroid::ExternalCallStage::Terminal));
    assert(ivrdroid::RouteExternalCallResult(
        ExternalCallRuntimeResult::Completed, 7, 8, 9).programCounter == 7);
    assert(ivrdroid::RouteExternalCallResult(
        ExternalCallRuntimeResult::NotConnected, 7, 8, 9).programCounter == 8);
    assert(ivrdroid::RouteExternalCallResult(
        ExternalCallRuntimeResult::SystemFailure, 7, 8, 9).programCounter == 9);
    assert(ivrdroid::RouteExternalCallResult(
        ExternalCallRuntimeResult::CallerHangup, 7, 8, 9).action ==
        ExternalCallRuntimeAction::CallerHangup);
    assert(ivrdroid::RouteExternalCallResult(
        ExternalCallRuntimeResult::CleanupTimeout, 7, 8, 9).action ==
        ExternalCallRuntimeAction::Fail);
    assert(ivrdroid::RequiresOriginalCallerSafeConfirmation(
        ExternalCallRuntimeResult::Completed));
    assert(ivrdroid::RequiresOriginalCallerSafeConfirmation(
        ExternalCallRuntimeResult::NotConnected));
    assert(ivrdroid::RequiresOriginalCallerSafeConfirmation(
        ExternalCallRuntimeResult::SystemFailure));
    assert(!ivrdroid::RequiresOriginalCallerSafeConfirmation(
        ExternalCallRuntimeResult::CallerHangup));
    assert(!ivrdroid::RequiresOriginalCallerSafeConfirmation(
        ExternalCallRuntimeResult::CleanupTimeout));
    assert(!ivrdroid::RequiresOriginalCallerSafeConfirmation(
        ExternalCallRuntimeResult::Failed));

    const auto dial = ivrdroid::BuildExternalCallDialRequest(
        kSession,
        17,
        kBlock,
        "+982112345678",
        30'000,
        kBoot,
        1,
        12'345);
    assert(dial.kind == ivrdroid::call_control::RequestKind::Dial);
    assert(dial.sessionUuid == kSession);
    assert(ivrdroid::call_control::EncodeRequest(dial) ==
        std::string("IVRDROID_CALL_CONTROL_V2 DIAL ") + kSession + " 17 " +
        kBlock + " 1 " + kBoot + " 12345 +982112345678 30000\n");

    for (const int outage : {1, 3, 10, 40, 130}) {
        auto recovery = Policy();
        recovery.Observe(Status(StatusKind::OperatorAnswered, 1), 1100);
        assert(recovery.MarkRecorderReady(1101));
        recovery.Observe(Status(StatusKind::Merging, 2), 1200);
        recovery.Observe(Status(StatusKind::Conferenced, 3), 1300);
        // No app callbacks, status heartbeat, recording ACK or network response.
        for (int64_t now = 1400; now <= 1400 + outage * 1000; now += 500) {
            recovery.ConfirmIndependentConference(now);
            assert(recovery.CheckDeadline(now) == ExternalCallDecision::Conferenced);
        }
        const int64_t resumed = 1400 + outage * 1000;
        assert(recovery.Observe(Status(StatusKind::Conferenced, 4, resumed), resumed) == ExternalCallDecision::Conferenced);
        // If independent evidence also disappears, old proof is insufficient.
        assert(recovery.CheckDeadline(resumed + 4000) == ExternalCallDecision::ControlTimeout);
    }
    std::cout << "External-call policy tests passed." << std::endl;
    return 0;
}
