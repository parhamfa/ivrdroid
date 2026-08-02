#include "helper_protocol.h"

#include <cassert>
#include <iostream>
#include <string>

int main() {
    using ivrdroid::protocol::Command;
    using ivrdroid::protocol::CurrentState;
    using ivrdroid::protocol::LastResult;
    assert(ivrdroid::protocol::ParseCommand("START_MENU\n") == Command::StartMenu);
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
