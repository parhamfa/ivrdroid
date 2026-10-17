#include "telecom_guard.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <charconv>
#include <sstream>
#include <set>

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
                TelecomObservedCall observed;
                observed.id = identity;
                const auto stateStart = trimmed.find("state=");
                if (stateStart != std::string::npos) {
                    const auto end = trimmed.find_first_of(", ]", stateStart + 6);
                    observed.state = trimmed.substr(stateStart + 6, end - stateStart - 6);
                }
                const auto children = trimmed.find("childs(");
                if (children != std::string::npos) {
                    const auto end = trimmed.find(')', children + 7);
                    if (end != std::string::npos) {
                        const auto parsed = std::from_chars(trimmed.data() + children + 7, trimmed.data() + end, observed.children);
                        if (parsed.ec != std::errc() || parsed.ptr != trimmed.data() + end || observed.children < 0 || observed.children > 16) observed.children = -1;
                    }
                }
                observed.parentKnown = trimmed.find("has_parent(true)") != std::string::npos || trimmed.find("has_parent(false)") != std::string::npos;
                observed.hasParent = trimmed.find("has_parent(true)") != std::string::npos;
                result.calls.push_back(observed);
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

bool IsLiveTelecomState(std::string_view state) {
    return state == "NEW" || state == "CONNECTING" || state == "SELECT_PHONE_ACCOUNT" || state == "DIALING" ||
        state == "RINGING" || state == "ANSWERED" || state == "ACTIVE" || state == "ON_HOLD" || state == "HOLDING";
}
bool ContainsOnlyOwnedCalls(const TelecomCallSnapshot& snapshot, const OwnedCallTopology& owned) {
    if (!snapshot.parsed || snapshot.emergencyCallPresent || snapshot.calls.empty() ||
        snapshot.calls.size() != static_cast<size_t>(snapshot.liveCallCount) || owned.caller.empty()) return false;
    std::set<std::string> seen;
    for (const auto& call : snapshot.calls) {
        if (call.id.empty() || !seen.insert(call.id).second ||
            (call.id != owned.caller && (owned.operatorCall.empty() || call.id != owned.operatorCall) &&
             (owned.conference.empty() || call.id != owned.conference)) ||
            (!IsLiveTelecomState(call.state) && call.state != "DISCONNECTING" && call.state != "DISCONNECTED")) return false;
    }
    return true;
}
bool ResolveOwnedConference(const TelecomCallSnapshot& snapshot, OwnedCallTopology* owned) {
    if (!owned || owned->caller.empty() || owned->operatorCall.empty() || !owned->conference.empty()) return false;
    for (const auto& call : snapshot.calls) {
        if (call.id == owned->caller || call.id == owned->operatorCall) continue;
        auto candidate = *owned;
        candidate.conference = call.id;
        if (MatchesOwnedConference(snapshot, candidate)) { *owned = candidate; return true; }
    }
    return false;
}

bool MatchesOwnedConference(const TelecomCallSnapshot& snapshot, const OwnedCallTopology& owned) {
    if (owned.caller == owned.operatorCall || owned.caller == owned.conference || owned.operatorCall == owned.conference ||
        owned.operatorCall.empty() || owned.conference.empty() || snapshot.calls.size() != 3 || !ContainsOnlyOwnedCalls(snapshot, owned)) return false;
    for (const auto& call : snapshot.calls) {
        if (!call.parentKnown || !IsLiveTelecomState(call.state)) return false;
        if (call.id == owned.conference) {
            if (call.hasParent || call.children != 2 || call.state != "ACTIVE") return false;
        } else if (!call.hasParent || call.children != 0 ||
                   (call.state != "ACTIVE" && call.state != "ON_HOLD" && call.state != "HOLDING")) return false;
    }
    return true;
}
std::string FormatNativeCallSnapshot(const TelecomCallSnapshot& snapshot, std::string_view boot,
    std::string_view session, uint64_t elapsedMs, uint64_t sequence) {
    std::ostringstream output;
    output << "NATIVE2 " << boot << ' ' << session << ' ' << elapsedMs << ' ' << sequence << ' '
        << (snapshot.parsed ? 1 : 0) << ' ' << (snapshot.emergencyCallPresent ? 1 : 0) << ' ' << snapshot.calls.size();
    if (snapshot.calls.size() > 16) return "";
    for (const auto& call : snapshot.calls) {
        if (call.id.empty() || call.id.size() > 64 || call.id.find_first_not_of("TC@0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_-") != std::string::npos) return "";
        output << ' ' << call.id << ':' << (call.state.empty() ? "UNKNOWN" : call.state) << ':'
            << call.children << ':' << (call.parentKnown ? (call.hasParent ? "1" : "0") : "?");
    }
    return output.str() + "\n";
}
bool ParseNativeCallSnapshot(std::string_view wire, std::string_view expectedBoot, std::string_view expectedSession,
    uint64_t nowMs, TelecomCallSnapshot* snapshot) {
    if (!snapshot || wire.size() > 4096) return false;
    std::istringstream input{std::string(wire)};
    std::string magic, boot, session, token, extra;
    uint64_t elapsed = 0, sequence = 0; int parsed = 0, emergency = 0, count = 0;
    if (!(input >> magic >> boot >> session >> elapsed >> sequence >> parsed >> emergency >> count) ||
        magic != "NATIVE2" || boot != expectedBoot || session != expectedSession || elapsed > nowMs || nowMs - elapsed > 2000 ||
        sequence == 0 || parsed != 1 || (emergency != 0 && emergency != 1) || count < 0 || count > 16) return false;
    TelecomCallSnapshot result {true, count, emergency == 1, {}, {}};
    std::set<std::string> ids;
    for (int i = 0; i < count; ++i) {
        if (!(input >> token)) return false;
        const auto a = token.find(':'), b = token.find(':', a + 1), c = token.find(':', b + 1);
        if (a == std::string::npos || b == std::string::npos || c == std::string::npos) return false;
        TelecomObservedCall call;
        call.id = token.substr(0, a); call.state = token.substr(a + 1, b - a - 1);
        const auto number = std::from_chars(token.data() + b + 1, token.data() + c, call.children);
        if (number.ec != std::errc() || number.ptr != token.data() + c || call.children < -1 || call.children > 16 ||
            call.id.rfind("TC@", 0) != 0 || call.id.size() <= 3 || call.id.size() > 64 ||
            call.id.substr(3).find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_-") != std::string::npos ||
            call.state.empty() || call.state.size() > 32 || call.state.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ_") != std::string::npos ||
            !ids.insert(call.id).second) return false;
        const auto parent = token.substr(c + 1);
        if (parent != "?" && parent != "0" && parent != "1") return false;
        call.parentKnown = parent != "?"; call.hasParent = parent == "1";
        result.calls.push_back(call);
    }
    if (input >> extra) return false;
    if (count == 1) result.singleCallIdentity = result.calls[0].id;
    *snapshot = std::move(result); return true;
}

bool IsOnlyRingingCaller(const TelecomCallSnapshot& snapshot, std::string_view caller) {
    if (!snapshot.parsed || snapshot.emergencyCallPresent || snapshot.liveCallCount != 1 || snapshot.calls.size() != 1) return false;
    const auto& call = snapshot.calls.front();
    return call.id == caller && call.state == "RINGING" && call.children == 0 && call.parentKnown && !call.hasParent;
}

}  // namespace ivrdroid
