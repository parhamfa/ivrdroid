#include "call_control_protocol.h"

#include <charconv>
#include <limits>
#include <vector>

namespace ivrdroid::call_control {
namespace {

constexpr std::string_view kVersion = "IVRDROID_CALL_CONTROL_V2";

std::vector<std::string_view> Split(std::string_view wire) {
    std::vector<std::string_view> tokens;
    size_t start = 0;
    while (start <= wire.size()) {
        const size_t separator = wire.find(' ', start);
        const size_t end = separator == std::string_view::npos
            ? wire.size()
            : separator;
        if (end == start) return {};
        tokens.push_back(wire.substr(start, end - start));
        if (separator == std::string_view::npos) break;
        start = separator + 1;
    }
    return tokens;
}

template <typename Integer>
bool ParseCanonicalUnsigned(std::string_view value, Integer* output, bool allowZero) {
    static_assert(std::numeric_limits<Integer>::is_integer);
    if (output == nullptr || value.empty() ||
        (value.size() > 1 && value.front() == '0')) {
        return false;
    }
    Integer parsed = 0;
    const auto result = std::from_chars(
        value.data(),
        value.data() + value.size(),
        parsed);
    if (result.ec != std::errc() || result.ptr != value.data() + value.size() ||
        (!allowZero && parsed == 0)) {
        return false;
    }
    *output = parsed;
    return true;
}

bool Prepare(std::string_view wire, std::vector<std::string_view>* tokens) {
    if (tokens == nullptr || wire.empty() || wire.size() > kMaximumWireBytes ||
        wire.back() != '\n' || wire.find('\n') != wire.size() - 1 ||
        wire.find('\r') != std::string_view::npos) {
        return false;
    }
    wire.remove_suffix(1);
    *tokens = Split(wire);
    return !tokens->empty() && tokens->front() == kVersion;
}

StatusKind ParseStatusKind(std::string_view value) {
    if (value == "ACK") return StatusKind::Ack;
    if (value == "CALLER_HELD") return StatusKind::CallerHeld;
    if (value == "DIALING") return StatusKind::Dialing;
    if (value == "OPERATOR_ANSWERED") return StatusKind::OperatorAnswered;
    if (value == "MERGING") return StatusKind::Merging;
    if (value == "CONFERENCED") return StatusKind::Conferenced;
    if (value == "COMPLETED") return StatusKind::Completed;
    if (value == "NOT_CONNECTED") return StatusKind::NotConnected;
    if (value == "SYSTEM_FAILURE") return StatusKind::SystemFailure;
    return StatusKind::Invalid;
}

}  // namespace

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

bool IsPhoneNumber(std::string_view value) {
    if (!value.empty() && value.front() == '+') value.remove_prefix(1);
    if (value.size() < 8 || value.size() > 15) return false;
    for (const char character : value) {
        if (character < '0' || character > '9') return false;
    }
    return true;
}

bool IsReason(std::string_view value) {
    if (value == "-") return true;
    if (value.empty() || value.size() > 64 ||
        value.front() < 'A' || value.front() > 'Z') {
        return false;
    }
    for (const char character : value) {
        if (!((character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') ||
              character == '_')) {
            return false;
        }
    }
    return true;
}

Request ParseRequest(std::string_view wire) {
    std::vector<std::string_view> tokens;
    if (!Prepare(wire, &tokens) || tokens.size() < 2) return {};

    Request request;
    if (tokens[1] == "DIAL") {
        if (tokens.size() != 10 || !IsCanonicalUuid(tokens[2]) ||
            !ParseCanonicalUnsigned(tokens[3], &request.revisionId, false) ||
            !IsCanonicalUuid(tokens[4]) ||
            !ParseCanonicalUnsigned(tokens[5], &request.sequence, false) ||
            !IsCanonicalUuid(tokens[6]) ||
            !ParseCanonicalUnsigned(
                tokens[7],
                &request.elapsedMilliseconds,
                true) ||
            !IsPhoneNumber(tokens[8]) ||
            !ParseCanonicalUnsigned(
                tokens[9],
                &request.answerTimeoutMilliseconds,
                false) ||
            request.answerTimeoutMilliseconds < kMinimumAnswerTimeoutMilliseconds ||
            request.answerTimeoutMilliseconds > kMaximumAnswerTimeoutMilliseconds ||
            request.answerTimeoutMilliseconds % 1'000 != 0) {
            return {};
        }
        request.kind = RequestKind::Dial;
        request.phoneNumber = std::string(tokens[8]);
    } else if (tokens[1] == "RECORDER_READY") {
        if (tokens.size() != 8 || !IsCanonicalUuid(tokens[2]) ||
            !ParseCanonicalUnsigned(tokens[3], &request.revisionId, false) ||
            !IsCanonicalUuid(tokens[4]) ||
            !ParseCanonicalUnsigned(tokens[5], &request.sequence, false) ||
            !IsCanonicalUuid(tokens[6]) ||
            !ParseCanonicalUnsigned(
                tokens[7],
                &request.elapsedMilliseconds,
                true)) {
            return {};
        }
        request.kind = RequestKind::RecorderReady;
    } else if (tokens[1] == "CANCEL") {
        if (tokens.size() != 9 || !IsCanonicalUuid(tokens[2]) ||
            !ParseCanonicalUnsigned(tokens[3], &request.revisionId, false) ||
            !IsCanonicalUuid(tokens[4]) ||
            !ParseCanonicalUnsigned(tokens[5], &request.sequence, false) ||
            !IsCanonicalUuid(tokens[6]) ||
            !ParseCanonicalUnsigned(
                tokens[7],
                &request.elapsedMilliseconds,
                true) ||
            !IsReason(tokens[8]) || tokens[8] == "-") {
            return {};
        }
        request.kind = RequestKind::Cancel;
        request.reason = std::string(tokens[8]);
    } else {
        return {};
    }
    request.sessionUuid = std::string(tokens[2]);
    request.blockUuid = std::string(tokens[4]);
    request.bootUuid = std::string(tokens[6]);
    return request;
}

Status ParseStatus(std::string_view wire) {
    std::vector<std::string_view> tokens;
    if (!Prepare(wire, &tokens) || tokens.size() != 9) return {};
    Status status;
    status.kind = ParseStatusKind(tokens[1]);
    if (status.kind == StatusKind::Invalid || !IsCanonicalUuid(tokens[2]) ||
        !ParseCanonicalUnsigned(tokens[3], &status.revisionId, false) ||
        !IsCanonicalUuid(tokens[4]) ||
        !ParseCanonicalUnsigned(tokens[5], &status.sequence, false) ||
        !IsCanonicalUuid(tokens[6]) ||
        !ParseCanonicalUnsigned(tokens[7], &status.elapsedMilliseconds, true) ||
        !IsReason(tokens[8])) {
        return {};
    }
    status.sessionUuid = std::string(tokens[2]);
    status.blockUuid = std::string(tokens[4]);
    status.bootUuid = std::string(tokens[6]);
    status.reason = std::string(tokens[8]);
    return status;
}

std::string EncodeRequest(const Request& request) {
    if (request.kind == RequestKind::Invalid ||
        !IsCanonicalUuid(request.sessionUuid) || request.revisionId == 0 ||
        !IsCanonicalUuid(request.blockUuid) || request.sequence == 0 ||
        !IsCanonicalUuid(request.bootUuid)) {
        return {};
    }
    std::string result(kVersion);
    result.push_back(' ');
    result.append(ToString(request.kind));
    result.push_back(' ');
    result.append(request.sessionUuid);
    result.push_back(' ');
    result.append(std::to_string(request.revisionId));
    result.push_back(' ');
    result.append(request.blockUuid);
    result.push_back(' ');
    result.append(std::to_string(request.sequence));
    result.push_back(' ');
    result.append(request.bootUuid);
    result.push_back(' ');
    result.append(std::to_string(request.elapsedMilliseconds));
    if (request.kind == RequestKind::Dial) {
        if (!IsPhoneNumber(request.phoneNumber) ||
            request.answerTimeoutMilliseconds < kMinimumAnswerTimeoutMilliseconds ||
            request.answerTimeoutMilliseconds > kMaximumAnswerTimeoutMilliseconds ||
            request.answerTimeoutMilliseconds % 1'000 != 0) {
            return {};
        }
        result.push_back(' ');
        result.append(request.phoneNumber);
        result.push_back(' ');
        result.append(std::to_string(request.answerTimeoutMilliseconds));
    }
    if (request.kind == RequestKind::Cancel) {
        if (!IsReason(request.reason) || request.reason == "-") return {};
        result.push_back(' ');
        result.append(request.reason);
    } else if (!request.reason.empty()) {
        return {};
    }
    result.push_back('\n');
    return result.size() <= kMaximumWireBytes ? result : std::string();
}

const char* ToString(RequestKind kind) {
    switch (kind) {
        case RequestKind::Dial:
            return "DIAL";
        case RequestKind::RecorderReady:
            return "RECORDER_READY";
        case RequestKind::Cancel:
            return "CANCEL";
        case RequestKind::Invalid:
            return "INVALID";
    }
    return "INVALID";
}

const char* ToString(StatusKind kind) {
    switch (kind) {
        case StatusKind::Ack:
            return "ACK";
        case StatusKind::CallerHeld:
            return "CALLER_HELD";
        case StatusKind::Dialing:
            return "DIALING";
        case StatusKind::OperatorAnswered:
            return "OPERATOR_ANSWERED";
        case StatusKind::Merging:
            return "MERGING";
        case StatusKind::Conferenced:
            return "CONFERENCED";
        case StatusKind::Completed:
            return "COMPLETED";
        case StatusKind::NotConnected:
            return "NOT_CONNECTED";
        case StatusKind::SystemFailure:
            return "SYSTEM_FAILURE";
        case StatusKind::Invalid:
            return "INVALID";
    }
    return "INVALID";
}

}  // namespace ivrdroid::call_control
