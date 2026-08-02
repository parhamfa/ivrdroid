#pragma once

#include <string_view>

namespace ivrdroid::protocol {

enum class Command {
    StartMenu,
    Invalid,
};

enum class CurrentState {
    WaitingForSystem,
    BlockedByCall,
    Ready,
    ArmingPrivacy,
    WaitingForCall,
    PlayingMain,
    ListeningDtmf,
    RetryingMenu,
    PlayingSales,
    PlayingSupport,
    PlayingOperator,
    EndingCall,
    Preempting,
    Recovering,
    Error,
    Stopped,
};

enum class LastResult {
    None,
    SessionComplete,
    RemoteHangup,
    RecoveredAndEnded,
    RecoveredAfterReboot,
    EmergencyPreempted,
    ExternalCallPreempted,
    UnverifiedCallPreempted,
    RecoveryHangupSkipped,
    FailedRestore,
    FailedAudio,
    FailedCapture,
    FailedEndCall,
    RejectedRequest,
    RejectedBusy,
    IncompatibleDevice,
    Stopped,
};

Command ParseCommand(std::string_view body);
const char* ToString(CurrentState state);
const char* ToString(LastResult result);

}  // namespace ivrdroid::protocol
