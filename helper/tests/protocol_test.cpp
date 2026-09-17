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
        "prompt_barge_in=1;session_audit=1");
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
    std::cout << "Helper protocol tests passed." << std::endl;
    return 0;
}
