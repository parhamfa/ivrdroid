#include "call_safety_policy.h"

#include <cassert>
#include <iostream>

namespace {

using ivrdroid::CallDisposition;
using ivrdroid::CallRecoveryDecision;
using ivrdroid::CallRecoveryPolicy;
using ivrdroid::AudioCallDisposition;
using ivrdroid::SessionCallDecision;
using ivrdroid::SessionCallMonitorPolicy;
using ivrdroid::SystemReadinessDecision;
using ivrdroid::SystemReadinessPolicy;

void SessionMonitorRequiresConfirmedRemoteEnd() {
    SessionCallMonitorPolicy policy(500, 3'000);
    policy.Start(1'000);
    assert(
        policy.Observe(CallDisposition::Idle, false, 1'000) ==
        SessionCallDecision::Continue);
    assert(
        policy.Observe(CallDisposition::Idle, false, 1'499) ==
        SessionCallDecision::Continue);
    assert(
        policy.Observe(CallDisposition::Idle, false, 1'500) ==
        SessionCallDecision::RemoteEnded);

    SessionCallMonitorPolicy reset(500, 3'000);
    reset.Start(2'000);
    assert(
        reset.Observe(CallDisposition::Idle, false, 2'000) ==
        SessionCallDecision::Continue);
    assert(
        reset.Observe(CallDisposition::SingleSafe, false, 2'400) ==
        SessionCallDecision::Continue);
    assert(
        reset.Observe(CallDisposition::Idle, false, 2'500) ==
        SessionCallDecision::Continue);
    assert(
        reset.Observe(CallDisposition::Idle, false, 3'000) ==
        SessionCallDecision::RemoteEnded);
}

void SessionMonitorYieldsToHigherPriorityCalls() {
    SessionCallMonitorPolicy emergency(500, 3'000);
    emergency.Start(3'000);
    assert(
        emergency.Observe(CallDisposition::Emergency, false, 3'001) ==
        SessionCallDecision::EmergencyPreempt);

    SessionCallMonitorPolicy multiple(500, 3'000);
    multiple.Start(4'000);
    assert(
        multiple.Observe(CallDisposition::Multiple, false, 4'001) ==
        SessionCallDecision::ExternalPreempt);
}

void SessionMonitorIgnoresExpectedIdleWhileEnding() {
    SessionCallMonitorPolicy policy(500, 3'000);
    policy.Start(5'000);
    assert(
        policy.Observe(CallDisposition::Idle, true, 6'000) ==
        SessionCallDecision::Continue);
    assert(
        policy.Observe(CallDisposition::Idle, false, 6'001) ==
        SessionCallDecision::Continue);
}

void SessionMonitorBoundsUnknownState() {
    SessionCallMonitorPolicy policy(500, 3'000);
    assert(
        policy.Observe(CallDisposition::SingleSafe, false, 1) ==
        SessionCallDecision::UnverifiedPreempt);
    policy.Start(7'000);
    assert(
        policy.Observe(CallDisposition::Unknown, false, 7'000) ==
        SessionCallDecision::Continue);
    assert(
        policy.Observe(CallDisposition::Unknown, false, 9'999) ==
        SessionCallDecision::Continue);
    assert(
        policy.Observe(CallDisposition::Unknown, false, 10'000) ==
        SessionCallDecision::UnverifiedPreempt);
}

CallRecoveryPolicy NewRecoveryPolicy() {
    return CallRecoveryPolicy(
        300,
        5'000,
        100,
        3'000,
        2'000,
        8'000,
        2);
}

void RecoveryConfirmsIdleAndBoundsAudioLag() {
    auto normal = NewRecoveryPolicy();
    normal.Start(10'000);
    assert(
        normal.Observe(
            CallDisposition::Idle,
            AudioCallDisposition::Normal,
            10'000) ==
        CallRecoveryDecision::Wait);
    assert(
        normal.Observe(
            CallDisposition::Idle,
            AudioCallDisposition::Normal,
            10'299) ==
        CallRecoveryDecision::Wait);
    assert(
        normal.Observe(
            CallDisposition::Idle,
            AudioCallDisposition::Normal,
            10'300) ==
        CallRecoveryDecision::Complete);

    auto lagged = NewRecoveryPolicy();
    lagged.Start(20'000);
    assert(
        lagged.Observe(
            CallDisposition::Idle,
            AudioCallDisposition::InCall,
            20'000) ==
        CallRecoveryDecision::Wait);
    assert(
        lagged.Observe(
            CallDisposition::Idle,
            AudioCallDisposition::InCall,
            24'999) ==
        CallRecoveryDecision::Wait);
    assert(
        lagged.Observe(
            CallDisposition::Idle,
            AudioCallDisposition::InCall,
            25'000) ==
        CallRecoveryDecision::CompleteAfterAudioLag);

    auto unknownAudio = NewRecoveryPolicy();
    unknownAudio.Start(26'000);
    assert(
        unknownAudio.Observe(
            CallDisposition::Idle,
            AudioCallDisposition::Unknown,
            26'000) ==
        CallRecoveryDecision::Wait);
    assert(
        unknownAudio.Observe(
            CallDisposition::Idle,
            AudioCallDisposition::Unknown,
            31'000) ==
        CallRecoveryDecision::CompleteAfterAudioLag);
}

void RecoveryConfirmsAndRetriesOneSafeHangup() {
    auto policy = NewRecoveryPolicy();
    policy.Start(30'000);
    assert(
        policy.Observe(
            CallDisposition::SingleSafe,
            AudioCallDisposition::InCall,
            30'000) ==
        CallRecoveryDecision::Wait);
    assert(
        policy.Observe(
            CallDisposition::SingleSafe,
            AudioCallDisposition::InCall,
            30'100) ==
        CallRecoveryDecision::RequestHangup);
    assert(policy.RecordHangupAttempt(30'100));
    assert(
        policy.Observe(
            CallDisposition::SingleSafe,
            AudioCallDisposition::InCall,
            32'099) ==
        CallRecoveryDecision::Wait);
    assert(
        policy.Observe(
            CallDisposition::SingleSafe,
            AudioCallDisposition::InCall,
            32'100) ==
        CallRecoveryDecision::RequestHangup);
    assert(policy.RecordHangupAttempt(32'100));
    assert(
        policy.Observe(
            CallDisposition::SingleSafe,
            AudioCallDisposition::InCall,
            34'100) ==
        CallRecoveryDecision::Fail);
    assert(!policy.RecordHangupAttempt(34'100));
}

void RecoveryPreemptsEmergencyAndMultipleCalls() {
    auto emergency = NewRecoveryPolicy();
    emergency.Start(40'000);
    assert(
        emergency.Observe(
            CallDisposition::Emergency,
            AudioCallDisposition::InCall,
            40'001) ==
        CallRecoveryDecision::EmergencyPreempt);

    auto multiple = NewRecoveryPolicy();
    multiple.Start(50'000);
    assert(
        multiple.Observe(
            CallDisposition::Multiple,
            AudioCallDisposition::InCall,
            50'001) ==
        CallRecoveryDecision::ExternalPreempt);
}

void RecoveryBoundsUnknownStateAndResetsEvidence() {
    auto unknown = NewRecoveryPolicy();
    unknown.Start(60'000);
    assert(
        unknown.Observe(
            CallDisposition::Unknown,
            AudioCallDisposition::Unknown,
            60'000) ==
        CallRecoveryDecision::Wait);
    assert(
        unknown.Observe(
            CallDisposition::Unknown,
            AudioCallDisposition::Unknown,
            62'999) ==
        CallRecoveryDecision::Wait);
    assert(
        unknown.Observe(
            CallDisposition::Unknown,
            AudioCallDisposition::Unknown,
            63'000) ==
        CallRecoveryDecision::UnverifiedPreempt);

    auto reset = NewRecoveryPolicy();
    reset.Start(70'000);
    assert(
        reset.Observe(
            CallDisposition::Idle,
            AudioCallDisposition::Normal,
            70'000) ==
        CallRecoveryDecision::Wait);
    assert(
        reset.Observe(
            CallDisposition::SingleSafe,
            AudioCallDisposition::InCall,
            70'200) ==
        CallRecoveryDecision::Wait);
    assert(
        reset.Observe(
            CallDisposition::Idle,
            AudioCallDisposition::Normal,
            70'300) ==
        CallRecoveryDecision::Wait);
    assert(
        reset.Observe(
            CallDisposition::Idle,
            AudioCallDisposition::Normal,
            70'600) ==
        CallRecoveryDecision::Complete);
}

void ReadinessRequiresStableIdleNormalState() {
    SystemReadinessPolicy policy(500);
    policy.Start(80'000);
    assert(
        policy.Observe(
            CallDisposition::Idle,
            AudioCallDisposition::Normal,
            80'000) ==
        SystemReadinessDecision::WaitingForSystem);
    assert(
        policy.Observe(
            CallDisposition::Idle,
            AudioCallDisposition::Normal,
            80'499) ==
        SystemReadinessDecision::WaitingForSystem);
    assert(
        policy.Observe(
            CallDisposition::Idle,
            AudioCallDisposition::Normal,
            80'500) ==
        SystemReadinessDecision::Ready);
}

void ReadinessBlocksCallsAndResetsEvidence() {
    SystemReadinessPolicy policy(500);
    policy.Start(90'000);
    assert(
        policy.Observe(
            CallDisposition::Idle,
            AudioCallDisposition::Normal,
            90'000) ==
        SystemReadinessDecision::WaitingForSystem);
    assert(
        policy.Observe(
            CallDisposition::Emergency,
            AudioCallDisposition::InCall,
            90'400) ==
        SystemReadinessDecision::BlockedByCall);
    assert(
        policy.Observe(
            CallDisposition::Idle,
            AudioCallDisposition::Normal,
            90'500) ==
        SystemReadinessDecision::WaitingForSystem);
    assert(
        policy.Observe(
            CallDisposition::Idle,
            AudioCallDisposition::Normal,
            91'000) ==
        SystemReadinessDecision::Ready);

    SystemReadinessPolicy unknown(500);
    unknown.Start(100'000);
    assert(
        unknown.Observe(
            CallDisposition::Unknown,
            AudioCallDisposition::Unknown,
            100'001) ==
        SystemReadinessDecision::WaitingForSystem);
    assert(
        unknown.Observe(
            CallDisposition::Multiple,
            AudioCallDisposition::InCall,
            100'002) ==
        SystemReadinessDecision::BlockedByCall);
}

}  // namespace

int main() {
    SessionMonitorRequiresConfirmedRemoteEnd();
    SessionMonitorYieldsToHigherPriorityCalls();
    SessionMonitorIgnoresExpectedIdleWhileEnding();
    SessionMonitorBoundsUnknownState();
    RecoveryConfirmsIdleAndBoundsAudioLag();
    RecoveryConfirmsAndRetriesOneSafeHangup();
    RecoveryPreemptsEmergencyAndMultipleCalls();
    RecoveryBoundsUnknownStateAndResetsEvidence();
    ReadinessRequiresStableIdleNormalState();
    ReadinessBlocksCallsAndResetsEvidence();
    std::cout << "Call safety policy tests passed." << std::endl;
    return 0;
}
