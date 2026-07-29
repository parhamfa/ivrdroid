#include "telecom_guard.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace ivrdroid {
namespace {

std::string Trim(std::string_view value) {
    const size_t first = value.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) return {};
    const size_t last = value.find_last_not_of(" \t\r");
    return std::string(value.substr(first, last - first + 1));
}

std::string Lower(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

bool IndicatesEmergency(std::string_view rawLine) {
    const std::string line = Lower(Trim(rawLine));
    if (line.find("emerg") == std::string::npos) return false;
    return line.find("=true") != std::string::npos ||
        line.find(": true") != std::string::npos ||
        line.find("emergency_call") != std::string::npos ||
        line.find("prop=[emerg") != std::string::npos ||
        line.find(" prop=[") != std::string::npos;
}

}  // namespace

TelecomCallSnapshot ParseTelecomCallSnapshot(std::string_view dump) {
    TelecomCallSnapshot result {false, 0, false};
    bool inCalls = false;
    bool foundCallsStart = false;

    size_t offset = 0;
    while (offset <= dump.size()) {
        const size_t end = dump.find('\n', offset);
        const std::string_view line = dump.substr(
            offset,
            end == std::string_view::npos ? dump.size() - offset : end - offset);
        const std::string trimmed = Trim(line);

        if (!inCalls && trimmed == "mCalls:") {
            inCalls = true;
            foundCallsStart = true;
        } else if (inCalls && trimmed == "mCallAudioManager:") {
            result.parsed = foundCallsStart;
            break;
        } else if (inCalls) {
            if (trimmed.rfind("Call TC@", 0) == 0 ||
                trimmed.rfind("[Call id=TC@", 0) == 0) {
                ++result.liveCallCount;
            }
            result.emergencyCallPresent =
                result.emergencyCallPresent || IndicatesEmergency(trimmed);
        }

        if (end == std::string_view::npos) break;
        offset = end + 1;
    }
    return result;
}

bool CanForceEndSingleCall(const TelecomCallSnapshot& snapshot) {
    return snapshot.parsed &&
        snapshot.liveCallCount == 1 &&
        !snapshot.emergencyCallPresent;
}

}  // namespace ivrdroid
