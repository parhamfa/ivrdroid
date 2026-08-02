#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace ivrdroid {

struct TelecomCallSnapshot {
    bool parsed;
    int liveCallCount;
    bool emergencyCallPresent;
    std::string singleCallIdentity;
};

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
