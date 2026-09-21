#include "helper_protocol.h"

#include <charconv>

namespace {

bool IsLowerHexSha256(std::string_view value) {
    if (value.size() != 64) return false;
    for (const char character : value) {
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool ParseRevision(std::string_view value, uint64_t* revision) {
    if (value.empty()) return false;
    uint64_t result = 0;
    const auto parsed = std::from_chars(
        value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc() ||
        parsed.ptr != value.data() + value.size() ||
        result == 0) {
        return false;
    }
    *revision = result;
    return true;
}

bool IsCanonicalUuid(std::string_view value) {
    if (value.size() != 36) return false;
    for (size_t index = 0; index < value.size(); ++index) {
        const bool hyphen = index == 8 || index == 13 || index == 18 || index == 23;
        const char character = value[index];
        if (hyphen ? character != '-' :
            !((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return value[14] >= '1' && value[14] <= '5' &&
        (value[19] == '8' || value[19] == '9' ||
         value[19] == 'a' || value[19] == 'b');
}

}  // namespace

namespace ivrdroid::protocol {

Command ParseCommand(std::string_view body) {
    return ParseCommandRequest(body).command;
}

CommandRequest ParseCommandRequest(std::string_view body) {
    CommandRequest request;
    if (body == "START_MENU\n") {
        request.command = Command::StartMenu;
        return request;
    }
    if (body.empty() || body.back() != '\n' || body.find('\n') != body.size() - 1) {
        return request;
    }
    body.remove_suffix(1);
    constexpr std::string_view stagePrefix = "STAGE_REVISION ";
    constexpr std::string_view activatePrefix = "ACTIVATE_STAGED ";
    constexpr std::string_view startPrefix = "START_MENU ";
    if (body.substr(0, startPrefix.size()) == startPrefix) {
        body.remove_prefix(startPrefix.size());
        if (!IsCanonicalUuid(body)) return CommandRequest {};
        request.command = Command::StartMenu;
        request.callUuid = std::string(body);
        return request;
    }
    if (body.substr(0, stagePrefix.size()) == stagePrefix) {
        body.remove_prefix(stagePrefix.size());
        const size_t separator = body.find(' ');
        if (separator == std::string_view::npos ||
            body.find(' ', separator + 1) != std::string_view::npos ||
            !ParseRevision(body.substr(0, separator), &request.revisionId) ||
            !IsLowerHexSha256(body.substr(separator + 1))) {
            return CommandRequest {};
        }
        request.command = Command::StageRevision;
        request.manifestSha256 = std::string(body.substr(separator + 1));
        return request;
    }
    if (body.substr(0, activatePrefix.size()) == activatePrefix) {
        body.remove_prefix(activatePrefix.size());
        if (!ParseRevision(body, &request.revisionId)) return CommandRequest {};
        request.command = Command::ActivateStaged;
    }
    return request;
}

const char* ToString(CurrentState state) {
    switch (state) {
        case CurrentState::WaitingForSystem:
            return "WAITING_FOR_SYSTEM";
        case CurrentState::BlockedByCall:
            return "BLOCKED_BY_CALL";
        case CurrentState::Ready:
            return "READY";
        case CurrentState::StagingRevision:
            return "STAGING_REVISION";
        case CurrentState::ActivatingRevision:
            return "ACTIVATING_REVISION";
        case CurrentState::ArmingPrivacy:
            return "ARMING_PRIVACY";
        case CurrentState::WaitingForCall:
            return "WAITING_FOR_CALL";
        case CurrentState::PlayingMain:
            return "PLAYING_MAIN";
        case CurrentState::ListeningDtmf:
            return "LISTENING_DTMF";
        case CurrentState::RecordingMessage:
            return "RECORDING_MESSAGE";
        case CurrentState::CallingOperator:
            return "CALLING_OPERATOR";
        case CurrentState::RecordingConversation:
            return "RECORDING_CONVERSATION";
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
        case LastResult::MaxCallDuration:
            return "MAX_CALL_DURATION";
        case LastResult::RecoveredOperatorHangup:
            return "RECOVERED_OPERATOR_HANGUP";
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
        case LastResult::RevisionStaged:
            return "REVISION_STAGED";
        case LastResult::RevisionActivated:
            return "REVISION_ACTIVATED";
        case LastResult::RevisionRejected:
            return "REVISION_REJECTED";
        case LastResult::IncompatibleDevice:
            return "INCOMPATIBLE_DEVICE";
        case LastResult::Stopped:
            return "STOPPED";
    }
    return "FAILED_AUDIO";
}

std::string FormatCallOutcome(
    LastResult result,
    std::string_view callUuid,
    std::string_view bootUuid,
    int64_t elapsedMs) {
    if (!IsCanonicalUuid(callUuid)) return {};
    // Only call outcomes belong in END1. A command discarded during cleanup
    // still has the just-finished call's UUID in scope, but cannot end that call.
    switch (result) {
        case LastResult::SessionComplete:
        case LastResult::RemoteHangup:
        case LastResult::MaxCallDuration:
        case LastResult::RecoveredOperatorHangup:
        case LastResult::RecoveredAndEnded:
        case LastResult::RecoveredAfterReboot:
        case LastResult::EmergencyPreempted:
        case LastResult::ExternalCallPreempted:
        case LastResult::UnverifiedCallPreempted:
        case LastResult::RecoveryHangupSkipped:
        case LastResult::FailedRestore:
        case LastResult::FailedAudio:
        case LastResult::FailedCapture:
        case LastResult::FailedEndCall:
            break;
        default:
            return {};
    }
    return "END1 " + std::string(callUuid) + " " + std::string(bootUuid) + " " +
        std::to_string(elapsedMs) + " " + ToString(result) + "\n";
}

}  // namespace ivrdroid::protocol
