#include "call_lifetime_policy.h"
#include "call_control_protocol.h"
#include <charconv>
#include <sstream>

namespace ivrdroid {
bool ParseCallLifetimePolicy(std::string_view text, CallLifetimePolicy* policy) {
    if (!policy || text.size() > 160) return false;
    uint64_t values[2] {};
    const char* keys[] = {"version=", "maximum_seconds="};
    for (int i = 0; i < 2; ++i) {
        const std::string_view key(keys[i]);
        if (text.substr(0, key.size()) != key) return false;
        text.remove_prefix(key.size());
        const size_t end = text.find('\n');
        if (end == std::string_view::npos || end == 0) return false;
        const auto result = std::from_chars(text.data(), text.data() + end, values[i]);
        if (result.ec != std::errc() || result.ptr != text.data() + end) return false;
        text.remove_prefix(end + 1);
    }
    if (!text.empty() || values[0] > INT64_MAX || values[1] < 60 || values[1] > 86400 || values[1] % 60 != 0) return false;
    *policy = {values[0], static_cast<uint32_t>(values[1])}; return true;
}
std::string FormatCallLifetimePolicy(const CallLifetimePolicy& policy) {
    return "version=" + std::to_string(policy.version) + "\nmaximum_seconds=" + std::to_string(policy.maximumSeconds) + "\n";
}
bool CallLifetime::Answer(uint64_t elapsedMs) {
    if (answeredElapsedMs != 0) return false; // Transfers and restarts can never move the deadline.
    if (elapsedMs == 0 || elapsedMs > UINT64_MAX - policy.maximumSeconds * 1000ULL) return false;
    answeredElapsedMs = elapsedMs; deadlineElapsedMs = elapsedMs + policy.maximumSeconds * 1000ULL; return true;
}
bool CallLifetime::Expired(std::string_view currentBoot, uint64_t elapsedMs) const {
    return boot == currentBoot && answeredElapsedMs != 0 && deadlineElapsedMs >= answeredElapsedMs && elapsedMs >= deadlineElapsedMs;
}
std::string FormatCallLifetime(const CallLifetime& call) {
    return "CALL1 " + call.session + " " + call.boot + " " + std::to_string(call.policy.version) + " " +
        std::to_string(call.policy.maximumSeconds) + " " + std::to_string(call.answeredElapsedMs) + " " + std::to_string(call.deadlineElapsedMs) + "\n";
}
bool ParseCallLifetime(std::string_view text, CallLifetime* call) {
    if (!call || text.size() > 512) return false;
    CallLifetime value; std::string magic, extra; std::istringstream in{std::string(text)};
    if (!(in >> magic >> value.session >> value.boot >> value.policy.version >> value.policy.maximumSeconds >> value.answeredElapsedMs >> value.deadlineElapsedMs) ||
        (in >> extra) || magic != "CALL1" || !call_control::IsCanonicalUuid(value.session) || !call_control::IsCanonicalUuid(value.boot) ||
        value.policy.maximumSeconds < 60 || value.policy.maximumSeconds > 86400 || value.policy.maximumSeconds % 60 ||
        value.answeredElapsedMs == 0 || value.answeredElapsedMs > UINT64_MAX - value.policy.maximumSeconds * 1000ULL ||
        value.deadlineElapsedMs != value.answeredElapsedMs + value.policy.maximumSeconds * 1000ULL) return false;
    *call = value; return true;
}
} // namespace ivrdroid
