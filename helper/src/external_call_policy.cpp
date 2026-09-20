#include "external_call_policy.h"

#include <limits>

namespace ivrdroid {
namespace {

int64_t SaturatingAdd(int64_t value, int64_t delta) {
    if (delta > 0 && value > std::numeric_limits<int64_t>::max() - delta) {
        return std::numeric_limits<int64_t>::max();
    }
    return value + delta;
}

int PhaseRank(call_control::StatusKind kind) {
    using call_control::StatusKind;
    switch (kind) {
        case StatusKind::Ack:
            return 0;
        case StatusKind::CallerHeld:
            return 1;
        case StatusKind::Dialing:
            return 2;
        case StatusKind::OperatorAnswered:
            return 3;
        case StatusKind::Merging:
            return 4;
        case StatusKind::Conferenced:
            return 5;
        default:
            return -1;
    }
}

bool IsTerminalStatus(call_control::StatusKind kind) {
    return kind == call_control::StatusKind::Completed ||
        kind == call_control::StatusKind::NotConnected ||
        kind == call_control::StatusKind::SystemFailure;
}

}  // namespace

bool AllowsConversationPersistenceStart(ExternalCallStage stage) {
    return stage == ExternalCallStage::Conferenced;
}

ExternalCallPolicy::ExternalCallPolicy(
    std::string sessionUuid,
    uint64_t revisionId,
    std::string blockUuid,
    std::string bootUuid,
    uint32_t answerTimeoutMilliseconds,
    int64_t startedAtMilliseconds)
    : sessionUuid_(std::move(sessionUuid)),
      revisionId_(revisionId),
      blockUuid_(std::move(blockUuid)),
      bootUuid_(std::move(bootUuid)),
      setupDeadlineMilliseconds_(SaturatingAdd(
          startedAtMilliseconds,
          call_control::kSetupMaximumMilliseconds)),
      answerTimeoutMilliseconds_(answerTimeoutMilliseconds),
      heartbeatDeadlineMilliseconds_(SaturatingAdd(
          startedAtMilliseconds,
          call_control::kHeartbeatMaximumAgeMilliseconds)) {}

bool ExternalCallPolicy::Correlates(
    const call_control::Status& status) const {
    return status.sessionUuid == sessionUuid_ &&
        status.revisionId == revisionId_ &&
        status.blockUuid == blockUuid_ &&
        status.bootUuid == bootUuid_;
}

bool ExternalCallPolicy::IsExactDuplicate(
    const call_control::Status& status) const {
    return status.sequence == lastSequence_ &&
        status.elapsedMilliseconds == lastElapsedMilliseconds_ &&
        status.kind == lastKind_ &&
        status.reason == lastReason_;
}

ExternalCallDecision ExternalCallPolicy::FailProtocol() {
    stage_ = ExternalCallStage::Terminal;
    return ExternalCallDecision::ProtocolFailure;
}

ExternalCallDecision ExternalCallPolicy::Observe(
    const call_control::Status& status,
    int64_t nowMilliseconds) {
    using call_control::StatusKind;
    if (status.kind == StatusKind::Invalid || !Correlates(status)) {
        return ExternalCallDecision::IgnoredForeign;
    }
    if (terminal()) {
        // Ended attempts do not reopen because callbacks or a terminal heartbeat
        // arrive late. Correlation above still rejects a different session.
        return ExternalCallDecision::Duplicate;
    }
    if (nowMilliseconds >= heartbeatDeadlineMilliseconds_ && !(stage_ == ExternalCallStage::Conferenced &&
        independentConferenceAt_ >= 0 && nowMilliseconds >= independentConferenceAt_ && nowMilliseconds - independentConferenceAt_ <= 2000)) {
        stage_ = ExternalCallStage::Terminal;
        return ExternalCallDecision::ControlTimeout;
    }
    if (status.sequence == lastSequence_ && lastSequence_ != 0) {
        return IsExactDuplicate(status)
            ? ExternalCallDecision::Duplicate
            : FailProtocol();
    }
    if (status.sequence <= lastSequence_ ||
        (lastSequence_ != 0 &&
         status.elapsedMilliseconds < lastElapsedMilliseconds_)) {
        return FailProtocol();
    }

    const int phaseRank = PhaseRank(status.kind);
    if (phaseRank >= 0) {
        if (phaseRank < highestPhaseRank_) return FailProtocol();
        if (status.kind == StatusKind::Merging &&
            stage_ != ExternalCallStage::RecorderReady &&
            stage_ != ExternalCallStage::Merging) {
            return FailProtocol();
        }
        if (status.kind == StatusKind::Conferenced &&
            stage_ != ExternalCallStage::Merging &&
            stage_ != ExternalCallStage::Conferenced) {
            return FailProtocol();
        }
        highestPhaseRank_ = phaseRank;
    }

    lastSequence_ = status.sequence;
    lastElapsedMilliseconds_ = status.elapsedMilliseconds;
    lastKind_ = status.kind;
    lastReason_ = status.reason;
    heartbeatDeadlineMilliseconds_ = SaturatingAdd(
        nowMilliseconds,
        call_control::kHeartbeatMaximumAgeMilliseconds);

    switch (status.kind) {
        case StatusKind::Ack:
        case StatusKind::CallerHeld:
        case StatusKind::Dialing:
            if (status.kind == StatusKind::Dialing &&
                answerDeadlineMilliseconds_ == 0) {
                answerDeadlineMilliseconds_ = SaturatingAdd(
                    nowMilliseconds,
                    answerTimeoutMilliseconds_);
            }
            stage_ = ExternalCallStage::Dialing;
            return ExternalCallDecision::DialingHeartbeat;
        case StatusKind::OperatorAnswered:
            if (stage_ != ExternalCallStage::OperatorAnswered &&
                stage_ != ExternalCallStage::RecorderReady) {
                stage_ = ExternalCallStage::OperatorAnswered;
                return ExternalCallDecision::StartRecorder;
            }
            return ExternalCallDecision::DialingHeartbeat;
        case StatusKind::Merging:
            if (stage_ == ExternalCallStage::RecorderReady) {
                mergeDeadlineMilliseconds_ = SaturatingAdd(
                    nowMilliseconds,
                    call_control::kMergeMaximumMilliseconds);
            }
            stage_ = ExternalCallStage::Merging;
            return ExternalCallDecision::Merging;
        case StatusKind::Conferenced:
            stage_ = ExternalCallStage::Conferenced;
            return ExternalCallDecision::Conferenced;
        case StatusKind::Completed:
            // The caller may leave before the operator answers. Android publishes
            // this completion only after removing the owned operator/conference.
            if (status.reason == "CALLER_HANGUP") {
                stage_ = ExternalCallStage::Terminal;
                return ExternalCallDecision::CallerHangup;
            }
            if (stage_ != ExternalCallStage::Conferenced) return FailProtocol();
            stage_ = ExternalCallStage::Terminal;
            if (status.reason == "OPERATOR_HANGUP") {
                return ExternalCallDecision::Completed;
            }
            return ExternalCallDecision::ProtocolFailure;
        case StatusKind::NotConnected:
            if (stage_ == ExternalCallStage::Conferenced) {
                return FailProtocol();
            }
            stage_ = ExternalCallStage::Terminal;
            return ExternalCallDecision::NotConnected;
        case StatusKind::SystemFailure:
            stage_ = ExternalCallStage::Terminal;
            return status.reason == "CLEANUP_TIMEOUT"
                ? ExternalCallDecision::CleanupTimeout
                : ExternalCallDecision::SystemFailure;
        case StatusKind::Invalid:
            return FailProtocol();
    }
    return FailProtocol();
}

bool ExternalCallPolicy::MarkRecorderReady(int64_t nowMilliseconds) {
    if (stage_ != ExternalCallStage::OperatorAnswered ||
        nowMilliseconds >= heartbeatDeadlineMilliseconds_) {
        return false;
    }
    stage_ = ExternalCallStage::RecorderReady;
    return true;
}

ExternalCallTeardownPolicy::ExternalCallTeardownPolicy(
    std::string sessionUuid,
    uint64_t revisionId,
    std::string blockUuid,
    std::string bootUuid,
    uint64_t lastSequence,
    uint64_t lastElapsedMilliseconds,
    call_control::StatusKind lastKind,
    std::string lastReason,
    ExternalCallTerminalExpectation expectation)
    : sessionUuid_(std::move(sessionUuid)),
      revisionId_(revisionId),
      blockUuid_(std::move(blockUuid)),
      bootUuid_(std::move(bootUuid)),
      lastSequence_(lastSequence),
      lastElapsedMilliseconds_(lastElapsedMilliseconds),
      lastKind_(lastKind),
      lastReason_(std::move(lastReason)),
      expectation_(std::move(expectation)) {}

bool ExternalCallTeardownPolicy::Correlates(
    const call_control::Status& status) const {
    return status.kind != call_control::StatusKind::Invalid &&
        status.sessionUuid == sessionUuid_ &&
        status.revisionId == revisionId_ &&
        status.blockUuid == blockUuid_ &&
        status.bootUuid == bootUuid_;
}

bool ExternalCallTeardownPolicy::IsExactDuplicate(
    const call_control::Status& status) const {
    return status.sequence == lastSequence_ &&
        status.elapsedMilliseconds == lastElapsedMilliseconds_ &&
        status.kind == lastKind_ && status.reason == lastReason_;
}

ExternalCallTeardownDecision ExternalCallTeardownPolicy::Observe(
    const call_control::Status& status) {
    if (!IsTerminalStatus(expectation_.kind) ||
        expectation_.reason.empty() ||
        !call_control::IsReason(expectation_.reason) ||
        expectation_.reason == "-") {
        return ExternalCallTeardownDecision::ProtocolFailure;
    }
    if (!Correlates(status)) {
        return ExternalCallTeardownDecision::IgnoredForeign;
    }
    if (terminal_) return (status.kind == lastKind_ && status.reason == lastReason_)
        ? ExternalCallTeardownDecision::Duplicate : ExternalCallTeardownDecision::IgnoredForeign;
    const bool callerEnded = status.kind == call_control::StatusKind::Completed && status.reason == "CALLER_HANGUP";
    if (status.sequence == lastSequence_ && lastSequence_ != 0) {
        if (!IsExactDuplicate(status)) return ExternalCallTeardownDecision::ProtocolFailure;
        if (callerEnded || (status.kind == expectation_.kind && status.reason == expectation_.reason)) {
            terminal_ = true;
            return ExternalCallTeardownDecision::MatchedTerminal;
        }
        return ExternalCallTeardownDecision::Duplicate;
    }
    if (status.sequence <= lastSequence_ ||
        status.elapsedMilliseconds < lastElapsedMilliseconds_) {
        return ExternalCallTeardownDecision::ProtocolFailure;
    }

    lastSequence_ = status.sequence;
    lastElapsedMilliseconds_ = status.elapsedMilliseconds;
    lastKind_ = status.kind;
    lastReason_ = status.reason;
    if (status.kind == call_control::StatusKind::SystemFailure &&
        status.reason == "CLEANUP_TIMEOUT") {
        terminal_ = true;
        return ExternalCallTeardownDecision::CleanupTimeout;
    }
    if (IsTerminalStatus(status.kind)) {
        terminal_ = true;
        return callerEnded || (status.kind == expectation_.kind &&
                status.reason == expectation_.reason)
            ? ExternalCallTeardownDecision::MatchedTerminal
            : ExternalCallTeardownDecision::MismatchedTerminal;
    }
    return status.reason == "-"
        ? ExternalCallTeardownDecision::Heartbeat
        : ExternalCallTeardownDecision::ProtocolFailure;
}

ExternalCallerSafePolicy::ExternalCallerSafePolicy(
    uint64_t expectedCallIdentityHash,
    int64_t startedAtMilliseconds,
    int64_t stableMilliseconds,
    int64_t maximumMilliseconds)
    : expectedCallIdentityHash_(expectedCallIdentityHash),
      startedAtMilliseconds_(startedAtMilliseconds),
      lastObservationMilliseconds_(startedAtMilliseconds),
      stableMilliseconds_(stableMilliseconds),
      maximumMilliseconds_(maximumMilliseconds) {}

ExternalCallerSafeDecision ExternalCallerSafePolicy::Observe(
    CallDisposition disposition,
    uint64_t identityHash,
    int64_t nowMilliseconds) {
    if (expectedCallIdentityHash_ == 0 || stableMilliseconds_ <= 0 ||
        maximumMilliseconds_ < stableMilliseconds_ ||
        nowMilliseconds < startedAtMilliseconds_ ||
        nowMilliseconds < lastObservationMilliseconds_) {
        return ExternalCallerSafeDecision::Unsafe;
    }
    lastObservationMilliseconds_ = nowMilliseconds;
    if (nowMilliseconds - startedAtMilliseconds_ >= maximumMilliseconds_) {
        return ExternalCallerSafeDecision::TimedOut;
    }
    if (disposition == CallDisposition::Unknown) {
        stableSinceMilliseconds_ = -1;
        return ExternalCallerSafeDecision::Waiting;
    }
    if (disposition != CallDisposition::SingleSafe ||
        identityHash != expectedCallIdentityHash_) {
        stableSinceMilliseconds_ = -1;
        return ExternalCallerSafeDecision::Unsafe;
    }
    if (stableSinceMilliseconds_ < 0) {
        stableSinceMilliseconds_ = nowMilliseconds;
    }
    return nowMilliseconds - stableSinceMilliseconds_ >= stableMilliseconds_
        ? ExternalCallerSafeDecision::Stable
        : ExternalCallerSafeDecision::Waiting;
}

ExternalCallDecision ExternalCallPolicy::CheckDeadline(
    int64_t nowMilliseconds) const {
    if (terminal()) return ExternalCallDecision::ProtocolFailure;
    if (nowMilliseconds >= heartbeatDeadlineMilliseconds_ && !(stage_ == ExternalCallStage::Conferenced &&
        independentConferenceAt_ >= 0 && nowMilliseconds >= independentConferenceAt_ && nowMilliseconds - independentConferenceAt_ <= 2000)) {
        return ExternalCallDecision::ControlTimeout;
    }
    if (answerDeadlineMilliseconds_ == 0 &&
        (stage_ == ExternalCallStage::AwaitingApp ||
         stage_ == ExternalCallStage::Dialing) &&
        nowMilliseconds >= setupDeadlineMilliseconds_) {
        return ExternalCallDecision::SetupTimeout;
    }
    if (answerDeadlineMilliseconds_ > 0 &&
        stage_ == ExternalCallStage::Dialing &&
        nowMilliseconds >= answerDeadlineMilliseconds_) {
        return ExternalCallDecision::AnswerTimeout;
    }
    if (stage_ == ExternalCallStage::Merging &&
        mergeDeadlineMilliseconds_ > 0 &&
        nowMilliseconds >= mergeDeadlineMilliseconds_) {
        return ExternalCallDecision::MergeTimeout;
    }
    if (stage_ == ExternalCallStage::Conferenced) {
        return ExternalCallDecision::Conferenced;
    }
    if (stage_ == ExternalCallStage::OperatorAnswered) {
        return ExternalCallDecision::StartRecorder;
    }
    if (stage_ == ExternalCallStage::Merging) {
        return ExternalCallDecision::Merging;
    }
    return ExternalCallDecision::DialingHeartbeat;
}

ExternalCallGuardianBudget::ExternalCallGuardianBudget(
    int64_t startedAtMilliseconds,
    int64_t totalBudgetMilliseconds)
    : deadlineMilliseconds_(SaturatingAdd(
          startedAtMilliseconds,
          totalBudgetMilliseconds > 0 ? totalBudgetMilliseconds : 1)) {}

void ExternalCallGuardianBudget::EnterConference(int64_t nowMilliseconds) {
    if (suspended_) return;
    remainingMilliseconds_ = nowMilliseconds < deadlineMilliseconds_
        ? deadlineMilliseconds_ - nowMilliseconds
        : 0;
    suspended_ = true;
}

void ExternalCallGuardianBudget::LeaveConference(int64_t nowMilliseconds) {
    if (!suspended_) return;
    deadlineMilliseconds_ = SaturatingAdd(
        nowMilliseconds,
        remainingMilliseconds_);
    suspended_ = false;
}

int64_t ExternalCallGuardianBudget::deadlineMilliseconds() const {
    return suspended_
        ? std::numeric_limits<int64_t>::max()
        : deadlineMilliseconds_;
}

ExternalCallRuntimeRoute RouteExternalCallResult(
    ExternalCallRuntimeResult result,
    uint32_t completedPc,
    uint32_t notConnectedPc,
    uint32_t systemFailurePc) {
    switch (result) {
        case ExternalCallRuntimeResult::Completed:
            return {ExternalCallRuntimeAction::Branch, completedPc};
        case ExternalCallRuntimeResult::NotConnected:
            return {ExternalCallRuntimeAction::Branch, notConnectedPc};
        case ExternalCallRuntimeResult::SystemFailure:
            return {ExternalCallRuntimeAction::Branch, systemFailurePc};
        case ExternalCallRuntimeResult::CallerHangup:
            return {ExternalCallRuntimeAction::CallerHangup, 0};
        case ExternalCallRuntimeResult::CleanupTimeout:
        case ExternalCallRuntimeResult::Failed:
            return {ExternalCallRuntimeAction::Fail, 0};
    }
    return {ExternalCallRuntimeAction::Fail, 0};
}

bool RequiresOriginalCallerSafeConfirmation(
    ExternalCallRuntimeResult result) {
    switch (result) {
        case ExternalCallRuntimeResult::Completed:
        case ExternalCallRuntimeResult::NotConnected:
        case ExternalCallRuntimeResult::SystemFailure:
            return true;
        case ExternalCallRuntimeResult::CallerHangup:
        case ExternalCallRuntimeResult::CleanupTimeout:
        case ExternalCallRuntimeResult::Failed:
            return false;
    }
    return false;
}

call_control::Request BuildExternalCallDialRequest(
    const std::string& callUuid,
    uint64_t revisionId,
    const std::string& blockUuid,
    const std::string& phoneNumber,
    uint32_t answerTimeoutMilliseconds,
    const std::string& bootUuid,
    uint64_t sequence,
    uint64_t elapsedMilliseconds) {
    call_control::Request request;
    request.kind = call_control::RequestKind::Dial;
    request.sessionUuid = callUuid;
    request.revisionId = revisionId;
    request.blockUuid = blockUuid;
    request.phoneNumber = phoneNumber;
    request.answerTimeoutMilliseconds = answerTimeoutMilliseconds;
    request.bootUuid = bootUuid;
    request.sequence = sequence;
    request.elapsedMilliseconds = elapsedMilliseconds;
    if (call_control::EncodeRequest(request).empty()) return {};
    return request;
}

}  // namespace ivrdroid
