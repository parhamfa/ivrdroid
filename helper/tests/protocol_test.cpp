#include "helper_protocol.h"

#include <cassert>
#include <iostream>
#include <string>

int main() {
    using ivrdroid::protocol::Command;
    using ivrdroid::protocol::CurrentState;
    using ivrdroid::protocol::LastResult;
    assert(std::string(ivrdroid::protocol::kCapabilities) ==
        "runtime=1,2,3,4;recording=1;call_control=2;conversation_recording=1;"
        "prompt_barge_in=1;session_audit=1;call_admission=1;capture_receipt=2");
    assert(ivrdroid::protocol::ParseCommand("START_MENU\n") == Command::StartMenu);
    const auto start = ivrdroid::protocol::ParseCommandRequest(
        "START_MENU 11111111-1111-4111-8111-111111111111\n");
    assert(start.command == Command::StartMenu);
    assert(start.callUuid == "11111111-1111-4111-8111-111111111111");
    assert(ivrdroid::protocol::ParseCommand("START_MENU not-a-uuid\n") == Command::Invalid);
    const auto stage = ivrdroid::protocol::ParseCommandRequest(
        "STAGE_REVISION 42 aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n");
    assert(stage.command == Command::StageRevision);
    assert(stage.revisionId == 42);
    assert(stage.manifestSha256 == std::string(64, 'a'));
    const auto activate = ivrdroid::protocol::ParseCommandRequest("ACTIVATE_STAGED 42\n");
    assert(activate.command == Command::ActivateStaged);
    assert(activate.revisionId == 42);
    assert(ivrdroid::protocol::ParseCommand("ACTIVATE_STAGED 0\n") == Command::Invalid);
    assert(ivrdroid::protocol::ParseCommand("STAGE_REVISION 1 AAAA\n") == Command::Invalid);
    assert(ivrdroid::protocol::ParseCommand("START_MENU") == Command::Invalid);
    assert(ivrdroid::protocol::ParseCommand("PLAY_MAIN\n") == Command::Invalid);
    assert(ivrdroid::protocol::ParseCommand("START_MENU\nignored") == Command::Invalid);
    assert(
        ivrdroid::protocol::ParseCommand(std::string(65, 'A')) ==
        Command::Invalid);
    assert(
        std::string(ivrdroid::protocol::ToString(
            CurrentState::WaitingForSystem)) ==
        "WAITING_FOR_SYSTEM");
    assert(
        std::string(ivrdroid::protocol::ToString(
            CurrentState::BlockedByCall)) ==
        "BLOCKED_BY_CALL");
    assert(
        std::string(ivrdroid::protocol::ToString(
            CurrentState::ArmingPrivacy)) ==
        "ARMING_PRIVACY");
    assert(
        std::string(ivrdroid::protocol::ToString(
            CurrentState::Preempting)) ==
        "PREEMPTING");
    assert(
        std::string(ivrdroid::protocol::ToString(
            CurrentState::CallingOperator)) ==
        "CALLING_OPERATOR");
    assert(
        std::string(ivrdroid::protocol::ToString(
            CurrentState::RecordingConversation)) ==
        "RECORDING_CONVERSATION");
    assert(
        std::string(ivrdroid::protocol::ToString(
            LastResult::RecoveredAfterReboot)) ==
        "RECOVERED_AFTER_REBOOT");
    assert(
        std::string(ivrdroid::protocol::ToString(
            LastResult::EmergencyPreempted)) ==
        "EMERGENCY_PREEMPTED");
    assert(
        std::string(ivrdroid::protocol::ToString(
            LastResult::ExternalCallPreempted)) ==
        "EXTERNAL_CALL_PREEMPTED");
    assert(
        std::string(ivrdroid::protocol::ToString(
            LastResult::UnverifiedCallPreempted)) ==
        "UNVERIFIED_CALL_PREEMPTED");
    const std::string call = "13f6ea80-353e-46e3-a9c8-327aac21fa7c";
    const std::string boot = "14f06e76-ee5d-47fc-bd2b-eb8ddcc6fbd3";
    const auto outcome = [&](LastResult result) {
        return ivrdroid::protocol::FormatCallOutcome(result, call, boot, 59058179);
    };
    const std::string callerHangup = "END1 " + call + " " + boot + " 59058179 REMOTE_HANGUP\n";
    assert(outcome(LastResult::RemoteHangup) == callerHangup);
    // Regression: cleanup rejected a queued command after the guardian had
    // published REMOTE_HANGUP. Neither per-call nor shared END1 may be replaced.
    for (const LastResult commandResult : {LastResult::RejectedBusy, LastResult::RejectedRequest,
             LastResult::None, LastResult::RevisionStaged, LastResult::RevisionActivated,
             LastResult::RevisionRejected, LastResult::IncompatibleDevice, LastResult::Stopped}) {
        assert(outcome(commandResult).empty());
    }
    for (const LastResult callResult : {LastResult::SessionComplete, LastResult::RemoteHangup,
             LastResult::MaxCallDuration, LastResult::RecoveredOperatorHangup,
             LastResult::RecoveredAndEnded, LastResult::RecoveredAfterReboot,
             LastResult::EmergencyPreempted, LastResult::ExternalCallPreempted,
             LastResult::UnverifiedCallPreempted, LastResult::RecoveryHangupSkipped,
             LastResult::FailedRestore, LastResult::FailedAudio, LastResult::FailedCapture,
             LastResult::FailedEndCall}) {
        assert(outcome(callResult) == "END1 " + call + " " + boot + " 59058179 " +
            ivrdroid::protocol::ToString(callResult) + "\n");
    }
    assert(ivrdroid::protocol::FormatCallOutcome(LastResult::RemoteHangup, "-", boot, 1).empty());
    assert(ivrdroid::protocol::FormatCallOutcome(LastResult::RemoteHangup, "", boot, 1).empty());
    assert(outcome(static_cast<LastResult>(999)).empty());
    assert(std::string(ivrdroid::protocol::ToString(LastResult::RejectedBusy)) == "REJECTED_BUSY");
    std::cout << "Helper protocol tests passed." << std::endl;
    return 0;
}
