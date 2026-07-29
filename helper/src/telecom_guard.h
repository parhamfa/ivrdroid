#pragma once

#include <string_view>

namespace ivrdroid {

struct TelecomCallSnapshot {
    bool parsed;
    int liveCallCount;
    bool emergencyCallPresent;
};

TelecomCallSnapshot ParseTelecomCallSnapshot(std::string_view dump);
bool CanForceEndSingleCall(const TelecomCallSnapshot& snapshot);

}  // namespace ivrdroid
