#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ivrdroid {

struct TelecomObservedCall {
    std::string id;
    std::string state;
    int children = -1;
    bool parentKnown = false;
    bool hasParent = false;
};
struct TelecomCallSnapshot {
    bool parsed;
    int liveCallCount;
    bool emergencyCallPresent;
    std::string singleCallIdentity;
    std::vector<TelecomObservedCall> calls {};
};

struct OwnedCallTopology {
    std::string caller, operatorCall, conference;
};
// Only the exact two-leg graph is supported. Count alone is never ownership proof.
bool MatchesOwnedConference(const TelecomCallSnapshot& snapshot, const OwnedCallTopology& owned);
// Recover only the parent of two already identified owned legs; never infer a leg by count.
bool ResolveOwnedConference(const TelecomCallSnapshot& snapshot, OwnedCallTopology* owned);
bool ContainsOnlyOwnedCalls(const TelecomCallSnapshot& snapshot, const OwnedCallTopology& owned);
bool IsLiveTelecomState(std::string_view state);
std::string FormatNativeCallSnapshot(const TelecomCallSnapshot& snapshot, std::string_view boot,
    std::string_view session, uint64_t elapsedMs, uint64_t sequence);
bool ParseNativeCallSnapshot(std::string_view wire, std::string_view expectedBoot, std::string_view expectedSession,
    uint64_t nowMs, TelecomCallSnapshot* snapshot);

enum class CallDisposition {
    Idle,
    SingleSafe,
    Emergency,
    Multiple,
    Unknown,
};

TelecomCallSnapshot ParseTelecomCallSnapshot(std::string_view dump);
CallDisposition ClassifyCallDisposition(const TelecomCallSnapshot& snapshot);
uint64_t StableCallIdentityHash(const TelecomCallSnapshot& snapshot);
bool CanForceEndSingleCall(const TelecomCallSnapshot& snapshot);

}  // namespace ivrdroid
