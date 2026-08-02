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

std::string WithoutWhitespace(std::string value) {
    value.erase(
        std::remove_if(
            value.begin(),
            value.end(),
            [](unsigned char character) {
                return std::isspace(character) != 0;
            }),
        value.end());
    return value;
}

bool HasBooleanMarker(
    const std::string& compactLine,
    std::string_view key,
    std::string_view value) {
    for (const char separator : {'=', ':'}) {
        const std::string marker =
            std::string(key) + separator + std::string(value);
        size_t offset = 0;
        while (offset < compactLine.size()) {
            const size_t position = compactLine.find(marker, offset);
            if (position == std::string::npos) break;
            if (position == 0 ||
                (!std::isalnum(static_cast<unsigned char>(
                     compactLine[position - 1])) &&
                 compactLine[position - 1] != '_')) {
                return true;
            }
            offset = position + 1;
        }
    }
    return false;
}

bool HasEmergencyWordBoundary(const std::string& line) {
    size_t offset = 0;
    while (offset < line.size()) {
        const size_t position = line.find("emerg", offset);
        if (position == std::string::npos) return false;
        if (position == 0 ||
            (!std::isalnum(static_cast<unsigned char>(line[position - 1])) &&
             line[position - 1] != '_')) {
            return true;
        }
        offset = position + 1;
    }
    return false;
}

bool IndicatesEmergency(std::string_view rawLine) {
    const std::string line = Lower(Trim(rawLine));
    if (line.find("emerg") == std::string::npos) return false;
    const std::string compact = WithoutWhitespace(line);

    if (compact.find("prop=[emerg") != std::string::npos ||
        compact.find("properties=[[emerg") != std::string::npos) {
        return true;
    }

    constexpr std::string_view keys[] = {
        "misemergencycall",
        "isemergencycall",
        "is_emergency_call",
        "isemergency",
        "emergency_call",
        "emergencycall",
        "emergency",
    };
    for (const std::string_view key : keys) {
        if (HasBooleanMarker(compact, key, "true")) return true;
    }
    for (const std::string_view key : keys) {
        if (HasBooleanMarker(compact, key, "false")) return false;
    }

    // An unrecognized emergency marker must fail closed. A false negative
    // here could let the appliance alter or terminate an emergency call.
    return HasEmergencyWordBoundary(line);
}

std::string ExtractCallIdentity(const std::string& line) {
    size_t start = std::string::npos;
    size_t end = std::string::npos;
    if (line.rfind("[Call id=", 0) == 0) {
        start = line.find("id=") + 3;
        end = line.find_first_of(", ]", start);
    } else if (line.rfind("Call TC@", 0) == 0) {
        start = 5;
        end = line.find(':', start);
    }
    if (start == std::string::npos ||
        end == std::string::npos ||
        end <= start) {
        return {};
    }
    const std::string identity = line.substr(start, end - start);
    return identity.size() > 3 && identity.rfind("TC@", 0) == 0
        ? identity
        : std::string();
}

}  // namespace

TelecomCallSnapshot ParseTelecomCallSnapshot(std::string_view dump) {
    TelecomCallSnapshot result {false, 0, false, {}};
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
                const std::string identity =
                    ExtractCallIdentity(trimmed);
                if (result.liveCallCount == 1) {
                    result.singleCallIdentity = identity;
                } else {
                    result.singleCallIdentity.clear();
                }
            }
            result.emergencyCallPresent =
                result.emergencyCallPresent || IndicatesEmergency(trimmed);
        }

        if (end == std::string_view::npos) break;
        offset = end + 1;
    }
    return result;
}

CallDisposition ClassifyCallDisposition(
    const TelecomCallSnapshot& snapshot) {
    if (!snapshot.parsed || snapshot.liveCallCount < 0) {
        return CallDisposition::Unknown;
    }
    if (snapshot.emergencyCallPresent) {
        return CallDisposition::Emergency;
    }
    if (snapshot.liveCallCount == 0) {
        return CallDisposition::Idle;
    }
    if (snapshot.liveCallCount == 1) {
        return snapshot.singleCallIdentity.empty()
            ? CallDisposition::Unknown
            : CallDisposition::SingleSafe;
    }
    return CallDisposition::Multiple;
}

uint64_t StableCallIdentityHash(
    const TelecomCallSnapshot& snapshot) {
    if (ClassifyCallDisposition(snapshot) !=
        CallDisposition::SingleSafe) {
        return 0;
    }
    constexpr uint64_t kOffsetBasis = 14'695'981'039'346'656'037ULL;
    constexpr uint64_t kPrime = 1'099'511'628'211ULL;
    uint64_t hash = kOffsetBasis;
    for (const unsigned char value : snapshot.singleCallIdentity) {
        hash ^= value;
        hash *= kPrime;
    }
    return hash;
}

bool CanForceEndSingleCall(const TelecomCallSnapshot& snapshot) {
    return ClassifyCallDisposition(snapshot) == CallDisposition::SingleSafe;
}

}  // namespace ivrdroid
