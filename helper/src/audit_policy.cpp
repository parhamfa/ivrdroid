#include "audit_policy.h"
#include <algorithm>
#include <charconv>
#include <string>

namespace ivrdroid {
bool ParseAuditPolicy(std::string_view value, AuditPolicy* policy) {
    if (policy == nullptr || value.size() > 160) return false;
    AuditPolicy result;
    const char* keys[] = {"version=", "enabled=", "quota="};
    uint64_t values[3] = {};
    for (int index = 0; index < 3; ++index) {
        const std::string_view key(keys[index]);
        if (value.substr(0, key.size()) != key) return false;
        value.remove_prefix(key.size());
        const size_t end = value.find('\n');
        if (end == std::string_view::npos || end == 0) return false;
        const auto parsed = std::from_chars(value.data(), value.data() + end, values[index]);
        if (parsed.ec != std::errc() || parsed.ptr != value.data() + end) return false;
        value.remove_prefix(end + 1);
    }
    if (!value.empty() || values[0] > INT64_MAX || values[1] > 1 ||
        (values[1] == 1 && values[0] == 0) || values[2] < 64ULL * 1024 * 1024 || values[2] > 4ULL * 1024 * 1024 * 1024) return false;
    result.version = values[0]; result.enabled = values[1] == 1; result.quotaBytes = values[2];
    *policy = result;
    return true;
}
bool AuditStorageFits(uint64_t spoolBytes, uint64_t inboxBytes, uint64_t freeBytes, uint64_t quotaBytes) {
    constexpr uint64_t segmentBytes = 44 + kAuditSegmentFrames * 4ULL;
    return quotaBytes >= segmentBytes && spoolBytes <= quotaBytes - segmentBytes &&
        inboxBytes <= quotaBytes - segmentBytes - spoolBytes && freeBytes >= kAuditFilesystemReserve + segmentBytes;
}
int16_t MixAuditSample(int16_t remote, int16_t prompt) {
    return static_cast<int16_t>(std::clamp(static_cast<int>(remote) + static_cast<int>(prompt), -32768, 32767));
}
} // namespace ivrdroid
