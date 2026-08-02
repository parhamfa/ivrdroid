#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace ivrdroid {

constexpr size_t kBootIdLength = 36;

struct PersistentSessionSnapshot {
    uint32_t magic;
    uint32_t version;
    uint64_t sessionId;
    uint64_t callIdentityHash;
    int32_t dout;
    int32_t mixer;
    int32_t speaker;
    int32_t mic;
    std::array<char, kBootIdLength> bootId;
    uint32_t checksum;
};

enum class SnapshotBootRelation {
    SameBoot,
    PreviousBoot,
    Invalid,
};

bool IsValidBootId(std::string_view bootId);
bool BuildSessionSnapshot(
    std::string_view bootId,
    uint64_t sessionId,
    uint64_t callIdentityHash,
    int32_t dout,
    int32_t mixer,
    int32_t speaker,
    int32_t mic,
    PersistentSessionSnapshot* snapshot);
bool ValidateSessionSnapshot(
    const PersistentSessionSnapshot& snapshot);
SnapshotBootRelation ClassifySnapshotBoot(
    const PersistentSessionSnapshot& snapshot,
    std::string_view currentBootId);

}  // namespace ivrdroid
