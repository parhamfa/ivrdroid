#pragma once

#include <string_view>

namespace ivrdroid::protocol {

enum class Command {
    StartMenu,
    Invalid,
};

enum class CurrentState {
    Ready,
    WaitingForCall,
    PlayingMain,
    ListeningDtmf,
    RetryingMenu,
    PlayingSales,
    PlayingSupport,
    PlayingOperator,
    EndingCall,
    Recovering,
    Error,
    Stopped,
};

enum class LastResult {
    None,
    SessionComplete,
    RemoteHangup,
    RecoveredAndEnded,
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
