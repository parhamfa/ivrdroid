#pragma once

#include "call_control_protocol.h"
#include "telecom_guard.h"

#include <cstdint>
#include <string>

namespace ivrdroid {

enum class ExternalCallStage {
    AwaitingApp,
    Dialing,
    OperatorAnswered,
    RecorderReady,
    Merging,
    Conferenced,
    Terminal,
};

enum class ExternalCallDecision {
    IgnoredForeign,
    Duplicate,
    DialingHeartbeat,
    StartRecorder,
    Merging,
    Conferenced,
    Completed,
    CallerHangup,
    NotConnected,
    SystemFailure,
    CleanupTimeout,
    ProtocolFailure,
    ControlTimeout,
    SetupTimeout,
    AnswerTimeout,
    MergeTimeout,
};

bool AllowsConversationPersistenceStart(ExternalCallStage stage);

class ExternalCallPolicy {
public:
    ExternalCallPolicy(
        std::string sessionUuid,
        uint64_t revisionId,
        std::string blockUuid,
        std::string bootUuid,
        uint32_t answerTimeoutMilliseconds,
        int64_t startedAtMilliseconds);

    ExternalCallDecision Observe(
        const call_control::Status& status,
        int64_t nowMilliseconds);
    bool MarkRecorderReady(int64_t nowMilliseconds);
    ExternalCallDecision CheckDeadline(int64_t nowMilliseconds) const;

    ExternalCallStage stage() const { return stage_; }
    uint64_t lastSequence() const { return lastSequence_; }
    uint64_t lastElapsedMilliseconds() const { return lastElapsedMilliseconds_; }
    call_control::StatusKind lastKind() const { return lastKind_; }
    const std::string& lastReason() const { return lastReason_; }
    bool terminal() const { return stage_ == ExternalCallStage::Terminal; }

private:
    bool Correlates(const call_control::Status& status) const;
    bool IsExactDuplicate(const call_control::Status& status) const;
    ExternalCallDecision FailProtocol();

    std::string sessionUuid_;
    uint64_t revisionId_ = 0;
    std::string blockUuid_;
    std::string bootUuid_;
    int64_t answerDeadlineMilliseconds_ = 0;
    int64_t setupDeadlineMilliseconds_ = 0;
    uint32_t answerTimeoutMilliseconds_ = 0;
    int64_t heartbeatDeadlineMilliseconds_ = 0;
    int64_t mergeDeadlineMilliseconds_ = 0;
    ExternalCallStage stage_ = ExternalCallStage::AwaitingApp;
    uint64_t lastSequence_ = 0;
    uint64_t lastElapsedMilliseconds_ = 0;
    call_control::StatusKind lastKind_ = call_control::StatusKind::Invalid;
    std::string lastReason_;
    int highestPhaseRank_ = -1;
};

struct ExternalCallTerminalExpectation {
    call_control::StatusKind kind = call_control::StatusKind::Invalid;
    std::string reason;
};

enum class ExternalCallTeardownDecision {
    Heartbeat,
    Duplicate,
    IgnoredForeign,
    MatchedTerminal,
    CleanupTimeout,
    MismatchedTerminal,
    ProtocolFailure,
};

class ExternalCallTeardownPolicy {
public:
    ExternalCallTeardownPolicy(
        std::string sessionUuid,
        uint64_t revisionId,
        std::string blockUuid,
        std::string bootUuid,
        uint64_t lastSequence,
        uint64_t lastElapsedMilliseconds,
        call_control::StatusKind lastKind,
        std::string lastReason,
        ExternalCallTerminalExpectation expectation);

    ExternalCallTeardownDecision Observe(
        const call_control::Status& status);
    uint64_t lastSequence() const { return lastSequence_; }
    uint64_t lastElapsedMilliseconds() const { return lastElapsedMilliseconds_; }

private:
    bool Correlates(const call_control::Status& status) const;
    bool IsExactDuplicate(const call_control::Status& status) const;

    std::string sessionUuid_;
    uint64_t revisionId_ = 0;
    std::string blockUuid_;
    std::string bootUuid_;
    uint64_t lastSequence_ = 0;
    uint64_t lastElapsedMilliseconds_ = 0;
    call_control::StatusKind lastKind_ = call_control::StatusKind::Invalid;
    std::string lastReason_;
    ExternalCallTerminalExpectation expectation_;
    bool terminal_ = false;
};

enum class ExternalCallerSafeDecision {
    Waiting,
    Stable,
    Unsafe,
    TimedOut,
};

class ExternalCallerSafePolicy {
public:
    ExternalCallerSafePolicy(
        uint64_t expectedCallIdentityHash,
        int64_t startedAtMilliseconds,
        int64_t stableMilliseconds,
        int64_t maximumMilliseconds);

    ExternalCallerSafeDecision Observe(
        CallDisposition disposition,
        uint64_t identityHash,
        int64_t nowMilliseconds);

private:
    uint64_t expectedCallIdentityHash_ = 0;
    int64_t startedAtMilliseconds_ = 0;
    int64_t stableSinceMilliseconds_ = -1;
    int64_t lastObservationMilliseconds_ = 0;
    int64_t stableMilliseconds_ = 0;
    int64_t maximumMilliseconds_ = 0;
};

class ExternalCallGuardianBudget {
public:
    ExternalCallGuardianBudget(
        int64_t startedAtMilliseconds,
        int64_t totalBudgetMilliseconds);
    void EnterConference(int64_t nowMilliseconds);
    void LeaveConference(int64_t nowMilliseconds);
    int64_t deadlineMilliseconds() const;
    bool suspended() const { return suspended_; }

private:
    int64_t deadlineMilliseconds_ = 0;
    int64_t remainingMilliseconds_ = 0;
    bool suspended_ = false;
};

enum class ExternalCallRuntimeResult {
    Completed,
    CallerHangup,
    NotConnected,
    SystemFailure,
    CleanupTimeout,
    Failed,
};

enum class ExternalCallRuntimeAction {
    Branch,
    CallerHangup,
    Fail,
};

bool RequiresOriginalCallerSafeConfirmation(
    ExternalCallRuntimeResult result);

struct ExternalCallRuntimeRoute {
    ExternalCallRuntimeAction action = ExternalCallRuntimeAction::Fail;
    uint32_t programCounter = 0;
};

ExternalCallRuntimeRoute RouteExternalCallResult(
    ExternalCallRuntimeResult result,
    uint32_t completedPc,
    uint32_t notConnectedPc,
    uint32_t systemFailurePc);

call_control::Request BuildExternalCallDialRequest(
    const std::string& callUuid,
    uint64_t revisionId,
    const std::string& blockUuid,
    const std::string& phoneNumber,
    uint32_t answerTimeoutMilliseconds,
    const std::string& bootUuid,
    uint64_t sequence,
    uint64_t elapsedMilliseconds);

}  // namespace ivrdroid
