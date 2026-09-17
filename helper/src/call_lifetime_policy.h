#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace ivrdroid {
struct CallLifetimePolicy { uint64_t version = 0; uint32_t maximumSeconds = 3600; };
bool ParseCallLifetimePolicy(std::string_view text, CallLifetimePolicy* policy);
std::string FormatCallLifetimePolicy(const CallLifetimePolicy& policy);
struct CallLifetime {
    std::string session, boot;
    CallLifetimePolicy policy;
    uint64_t answeredElapsedMs = 0, deadlineElapsedMs = 0;
    bool Answer(uint64_t elapsedMs);
    bool Expired(std::string_view currentBoot, uint64_t elapsedMs) const;
};
std::string FormatCallLifetime(const CallLifetime& call);
bool ParseCallLifetime(std::string_view text, CallLifetime* call);
} // namespace ivrdroid
