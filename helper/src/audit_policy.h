#pragma once
#include <cstdint>
#include <string_view>

namespace ivrdroid {
constexpr uint32_t kAuditFrameCount = 1200;
constexpr uint32_t kAuditSegmentFrames = 720000;
constexpr uint64_t kAuditFilesystemReserve = 576ULL * 1024 * 1024;
struct AuditPolicy {
    uint64_t version = 0;
    bool enabled = false;
    uint64_t quotaBytes = 1024ULL * 1024 * 1024;
};
bool ParseAuditPolicy(std::string_view value, AuditPolicy* policy);
bool AuditStorageFits(uint64_t spoolBytes, uint64_t inboxBytes, uint64_t freeBytes, uint64_t quotaBytes);
int16_t MixAuditSample(int16_t remote, int16_t prompt);
} // namespace ivrdroid
