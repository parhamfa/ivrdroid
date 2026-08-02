#include "call_safety_policy.h"

#include <algorithm>

namespace ivrdroid {

SessionCallMonitorPolicy::SessionCallMonitorPolicy(
    int64_t idleConfirmationMilliseconds,
    int64_t unknownMaximumMilliseconds)
    : idleConfirmationMilliseconds_(
          std::max<int64_t>(0, idleConfirmationMilliseconds)),
      unknownMaximumMilliseconds_(
          std::max<int64_t>(1, unknownMaximumMilliseconds)) {}

void SessionCallMonitorPolicy::Start(int64_t nowMilliseconds) {
    lastObservationMilliseconds_ = nowMilliseconds;
    idleSinceMilliseconds_ = -1;
    unknownSinceMilliseconds_ = -1;
    started_ = true;
}

SessionCallDecision SessionCallMonitorPolicy::Observe(
    CallDisposition disposition,
    bool endingCall,
    int64_t nowMilliseconds) {
    if (!started_ || nowMilliseconds < lastObservationMilliseconds_) {
        return SessionCallDecision::UnverifiedPreempt;
    }
    lastObservationMilliseconds_ = nowMilliseconds;

    if (disposition == CallDisposition::Emergency) {
        return SessionCallDecision::EmergencyPreempt;
    }
    if (disposition == CallDisposition::Multiple) {
        return SessionCallDecision::ExternalPreempt;
    }
    if (disposition == CallDisposition::Unknown) {
        idleSinceMilliseconds_ = -1;
        if (unknownSinceMilliseconds_ < 0) {
            unknownSinceMilliseconds_ = nowMilliseconds;
        }
        return nowMilliseconds - unknownSinceMilliseconds_ >=
                unknownMaximumMilliseconds_
            ? SessionCallDecision::UnverifiedPreempt
            : SessionCallDecision::Continue;
    }

    unknownSinceMilliseconds_ = -1;
    if (disposition == CallDisposition::SingleSafe || endingCall) {
        idleSinceMilliseconds_ = -1;
        return SessionCallDecision::Continue;
    }

    if (idleSinceMilliseconds_ < 0) {
        idleSinceMilliseconds_ = nowMilliseconds;
    }
    return nowMilliseconds - idleSinceMilliseconds_ >=
            idleConfirmationMilliseconds_
        ? SessionCallDecision::RemoteEnded
        : SessionCallDecision::Continue;
}

CallRecoveryPolicy::CallRecoveryPolicy(
    int64_t idleConfirmationMilliseconds,
    int64_t audioLagMaximumMilliseconds,
    int64_t singleCallConfirmationMilliseconds,
    int64_t unknownMaximumMilliseconds,
    int64_t hangupRetryMilliseconds,
    int64_t totalMaximumMilliseconds,
    unsigned int maximumHangupAttempts)
    : idleConfirmationMilliseconds_(
          std::max<int64_t>(0, idleConfirmationMilliseconds)),
      audioLagMaximumMilliseconds_(
          std::max(
              idleConfirmationMilliseconds_,
              audioLagMaximumMilliseconds)),
      singleCallConfirmationMilliseconds_(
          std::max<int64_t>(0, singleCallConfirmationMilliseconds)),
      unknownMaximumMilliseconds_(
          std::max<int64_t>(1, unknownMaximumMilliseconds)),
      hangupRetryMilliseconds_(
          std::max<int64_t>(1, hangupRetryMilliseconds)),
      totalMaximumMilliseconds_(
          std::max<int64_t>(1, totalMaximumMilliseconds)),
      maximumHangupAttempts_(std::max(1U, maximumHangupAttempts)) {}

void CallRecoveryPolicy::Start(int64_t nowMilliseconds) {
    startedMilliseconds_ = nowMilliseconds;
    lastObservationMilliseconds_ = nowMilliseconds;
    idleSinceMilliseconds_ = -1;
    singleSinceMilliseconds_ = -1;
    unknownSinceMilliseconds_ = -1;
    lastHangupAttemptMilliseconds_ = -1;
    hangupAttempts_ = 0;
    started_ = true;
}

CallRecoveryDecision CallRecoveryPolicy::Observe(
    CallDisposition disposition,
    AudioCallDisposition audioDisposition,
    int64_t nowMilliseconds) {
    if (!started_ ||
        nowMilliseconds < lastObservationMilliseconds_ ||
        nowMilliseconds < startedMilliseconds_) {
        return CallRecoveryDecision::Fail;
    }
    lastObservationMilliseconds_ = nowMilliseconds;

    if (disposition == CallDisposition::Emergency) {
        return CallRecoveryDecision::EmergencyPreempt;
    }
    if (disposition == CallDisposition::Multiple) {
        return CallRecoveryDecision::ExternalPreempt;
    }

    if (disposition == CallDisposition::Unknown) {
        idleSinceMilliseconds_ = -1;
        singleSinceMilliseconds_ = -1;
        if (unknownSinceMilliseconds_ < 0) {
            unknownSinceMilliseconds_ = nowMilliseconds;
        }
        if (nowMilliseconds - unknownSinceMilliseconds_ >=
            unknownMaximumMilliseconds_) {
            return CallRecoveryDecision::UnverifiedPreempt;
        }
    } else if (disposition == CallDisposition::Idle) {
        singleSinceMilliseconds_ = -1;
        unknownSinceMilliseconds_ = -1;
        if (idleSinceMilliseconds_ < 0) {
            idleSinceMilliseconds_ = nowMilliseconds;
        }
        const int64_t idleDuration =
            nowMilliseconds - idleSinceMilliseconds_;
        if (audioDisposition == AudioCallDisposition::Normal &&
            idleDuration >= idleConfirmationMilliseconds_) {
            return CallRecoveryDecision::Complete;
        }
        if (audioDisposition != AudioCallDisposition::Normal &&
            idleDuration >= audioLagMaximumMilliseconds_) {
            return CallRecoveryDecision::CompleteAfterAudioLag;
        }
    } else {
        idleSinceMilliseconds_ = -1;
        unknownSinceMilliseconds_ = -1;
        if (singleSinceMilliseconds_ < 0) {
            singleSinceMilliseconds_ = nowMilliseconds;
        }
        const bool confirmed =
            nowMilliseconds - singleSinceMilliseconds_ >=
            singleCallConfirmationMilliseconds_;
        const bool firstAttempt = hangupAttempts_ == 0;
        const bool retryDue =
            hangupAttempts_ < maximumHangupAttempts_ &&
            lastHangupAttemptMilliseconds_ >= 0 &&
            nowMilliseconds - lastHangupAttemptMilliseconds_ >=
                hangupRetryMilliseconds_;
        if (confirmed && (firstAttempt || retryDue)) {
            return CallRecoveryDecision::RequestHangup;
        }
        if (hangupAttempts_ >= maximumHangupAttempts_ &&
            lastHangupAttemptMilliseconds_ >= 0 &&
            nowMilliseconds - lastHangupAttemptMilliseconds_ >=
                hangupRetryMilliseconds_) {
            return CallRecoveryDecision::Fail;
        }
    }

    return nowMilliseconds - startedMilliseconds_ >=
            totalMaximumMilliseconds_
        ? CallRecoveryDecision::Fail
        : CallRecoveryDecision::Wait;
}

bool CallRecoveryPolicy::RecordHangupAttempt(
    int64_t nowMilliseconds) {
    if (!started_ ||
        nowMilliseconds < lastObservationMilliseconds_ ||
        hangupAttempts_ >= maximumHangupAttempts_) {
        return false;
    }
    lastHangupAttemptMilliseconds_ = nowMilliseconds;
    ++hangupAttempts_;
    return true;
}

SystemReadinessPolicy::SystemReadinessPolicy(
    int64_t idleConfirmationMilliseconds)
    : idleConfirmationMilliseconds_(
          std::max<int64_t>(0, idleConfirmationMilliseconds)) {}

void SystemReadinessPolicy::Start(int64_t nowMilliseconds) {
    lastObservationMilliseconds_ = nowMilliseconds;
    readySinceMilliseconds_ = -1;
    started_ = true;
}

SystemReadinessDecision SystemReadinessPolicy::Observe(
    CallDisposition disposition,
    AudioCallDisposition audioDisposition,
    int64_t nowMilliseconds) {
    if (!started_ || nowMilliseconds < lastObservationMilliseconds_) {
        readySinceMilliseconds_ = -1;
        return SystemReadinessDecision::WaitingForSystem;
    }
    lastObservationMilliseconds_ = nowMilliseconds;

    if (disposition == CallDisposition::Unknown) {
        readySinceMilliseconds_ = -1;
        return SystemReadinessDecision::WaitingForSystem;
    }
    if (disposition != CallDisposition::Idle) {
        readySinceMilliseconds_ = -1;
        return SystemReadinessDecision::BlockedByCall;
    }
    if (audioDisposition != AudioCallDisposition::Normal) {
        readySinceMilliseconds_ = -1;
        return SystemReadinessDecision::WaitingForSystem;
    }

    if (readySinceMilliseconds_ < 0) {
        readySinceMilliseconds_ = nowMilliseconds;
    }
    return nowMilliseconds - readySinceMilliseconds_ >=
            idleConfirmationMilliseconds_
        ? SystemReadinessDecision::Ready
        : SystemReadinessDecision::WaitingForSystem;
}

}  // namespace ivrdroid
