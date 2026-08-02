#include "helper_protocol.h"

namespace ivrdroid::protocol {

Command ParseCommand(std::string_view body) {
    return body == "START_MENU\n" ? Command::StartMenu : Command::Invalid;
}

const char* ToString(CurrentState state) {
    switch (state) {
        case CurrentState::WaitingForSystem:
            return "WAITING_FOR_SYSTEM";
        case CurrentState::BlockedByCall:
            return "BLOCKED_BY_CALL";
        case CurrentState::Ready:
            return "READY";
        case CurrentState::ArmingPrivacy:
            return "ARMING_PRIVACY";
        case CurrentState::WaitingForCall:
            return "WAITING_FOR_CALL";
        case CurrentState::PlayingMain:
            return "PLAYING_MAIN";
        case CurrentState::ListeningDtmf:
            return "LISTENING_DTMF";
        case CurrentState::RetryingMenu:
            return "RETRYING_MENU";
        case CurrentState::PlayingSales:
            return "PLAYING_SALES";
        case CurrentState::PlayingSupport:
            return "PLAYING_SUPPORT";
        case CurrentState::PlayingOperator:
            return "PLAYING_OPERATOR";
        case CurrentState::EndingCall:
            return "ENDING_CALL";
        case CurrentState::Preempting:
            return "PREEMPTING";
        case CurrentState::Recovering:
            return "RECOVERING";
        case CurrentState::Error:
            return "ERROR";
        case CurrentState::Stopped:
            return "STOPPED";
    }
    return "ERROR";
}

const char* ToString(LastResult result) {
    switch (result) {
        case LastResult::None:
            return "NONE";
        case LastResult::SessionComplete:
            return "SESSION_COMPLETE";
        case LastResult::RemoteHangup:
            return "REMOTE_HANGUP";
        case LastResult::RecoveredAndEnded:
            return "RECOVERED_AND_ENDED";
        case LastResult::RecoveredAfterReboot:
            return "RECOVERED_AFTER_REBOOT";
        case LastResult::EmergencyPreempted:
            return "EMERGENCY_PREEMPTED";
        case LastResult::ExternalCallPreempted:
            return "EXTERNAL_CALL_PREEMPTED";
        case LastResult::UnverifiedCallPreempted:
            return "UNVERIFIED_CALL_PREEMPTED";
        case LastResult::RecoveryHangupSkipped:
            return "RECOVERY_HANGUP_SKIPPED";
        case LastResult::FailedRestore:
            return "FAILED_RESTORE";
        case LastResult::FailedAudio:
            return "FAILED_AUDIO";
        case LastResult::FailedCapture:
            return "FAILED_CAPTURE";
        case LastResult::FailedEndCall:
            return "FAILED_END_CALL";
        case LastResult::RejectedRequest:
            return "REJECTED_REQUEST";
        case LastResult::RejectedBusy:
            return "REJECTED_BUSY";
        case LastResult::IncompatibleDevice:
            return "INCOMPATIBLE_DEVICE";
        case LastResult::Stopped:
            return "STOPPED";
    }
    return "FAILED_AUDIO";
}

}  // namespace ivrdroid::protocol
