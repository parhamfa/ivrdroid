#pragma once

#include "telecom_guard.h"

#include <cstdint>

namespace ivrdroid {

enum class SessionCallDecision {
    Continue,
    RemoteEnded,
    EmergencyPreempt,
    ExternalPreempt,
    UnverifiedPreempt,
};

class SessionCallMonitorPolicy {
public:
    SessionCallMonitorPolicy(
        int64_t idleConfirmationMilliseconds,
        int64_t unknownMaximumMilliseconds);

    void Start(int64_t nowMilliseconds);
    SessionCallDecision Observe(
        CallDisposition disposition,
        bool endingCall,
        int64_t nowMilliseconds);

private:
    int64_t idleConfirmationMilliseconds_;
    int64_t unknownMaximumMilliseconds_;
    int64_t lastObservationMilliseconds_ = -1;
    int64_t idleSinceMilliseconds_ = -1;
    int64_t unknownSinceMilliseconds_ = -1;
    bool started_ = false;
};

enum class CallRecoveryDecision {
    Wait,
    RequestHangup,
    Complete,
    CompleteAfterAudioLag,
    EmergencyPreempt,
    ExternalPreempt,
    UnverifiedPreempt,
    Fail,
};

enum class AudioCallDisposition {
    Normal,
    InCall,
    Unknown,
};

class CallRecoveryPolicy {
public:
    CallRecoveryPolicy(
        int64_t idleConfirmationMilliseconds,
        int64_t audioLagMaximumMilliseconds,
        int64_t singleCallConfirmationMilliseconds,
        int64_t unknownMaximumMilliseconds,
        int64_t hangupRetryMilliseconds,
        int64_t totalMaximumMilliseconds,
        unsigned int maximumHangupAttempts);

    void Start(int64_t nowMilliseconds);
    CallRecoveryDecision Observe(
        CallDisposition disposition,
        AudioCallDisposition audioDisposition,
        int64_t nowMilliseconds);
    bool RecordHangupAttempt(int64_t nowMilliseconds);

private:
    int64_t idleConfirmationMilliseconds_;
    int64_t audioLagMaximumMilliseconds_;
    int64_t singleCallConfirmationMilliseconds_;
    int64_t unknownMaximumMilliseconds_;
    int64_t hangupRetryMilliseconds_;
    int64_t totalMaximumMilliseconds_;
    unsigned int maximumHangupAttempts_;

    int64_t startedMilliseconds_ = -1;
    int64_t lastObservationMilliseconds_ = -1;
    int64_t idleSinceMilliseconds_ = -1;
    int64_t singleSinceMilliseconds_ = -1;
    int64_t unknownSinceMilliseconds_ = -1;
    int64_t lastHangupAttemptMilliseconds_ = -1;
    unsigned int hangupAttempts_ = 0;
    bool started_ = false;
};

enum class SystemReadinessDecision {
    WaitingForSystem,
    BlockedByCall,
    Ready,
};

class SystemReadinessPolicy {
public:
    explicit SystemReadinessPolicy(
        int64_t idleConfirmationMilliseconds);

    void Start(int64_t nowMilliseconds);
    SystemReadinessDecision Observe(
        CallDisposition disposition,
        AudioCallDisposition audioDisposition,
        int64_t nowMilliseconds);

private:
    int64_t idleConfirmationMilliseconds_;
    int64_t lastObservationMilliseconds_ = -1;
    int64_t readySinceMilliseconds_ = -1;
    bool started_ = false;
};

}  // namespace ivrdroid
