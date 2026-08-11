#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace ivrdroid::protocol {

inline constexpr char kCapabilities[] =
    "runtime=1,2,3,4;recording=1;call_control=1;conversation_recording=1;prompt_barge_in=1";

enum class Command {
    StartMenu,
    StageRevision,
    ActivateStaged,
    Invalid,
};

struct CommandRequest {
    Command command = Command::Invalid;
    uint64_t revisionId = 0;
    std::string manifestSha256;
    std::string callUuid;
};

enum class CurrentState {
    WaitingForSystem,
    BlockedByCall,
    Ready,
    StagingRevision,
    ActivatingRevision,
    ArmingPrivacy,
    WaitingForCall,
    PlayingMain,
    ListeningDtmf,
    RecordingMessage,
    CallingOperator,
    RecordingConversation,
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
    RevisionStaged,
    RevisionActivated,
    RevisionRejected,
    IncompatibleDevice,
    Stopped,
};

Command ParseCommand(std::string_view body);
CommandRequest ParseCommandRequest(std::string_view body);
const char* ToString(CurrentState state);
const char* ToString(LastResult result);

}  // namespace ivrdroid::protocol
