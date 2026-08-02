#include "session_snapshot.h"

#include <cstddef>
#include <cstring>
#include <type_traits>

namespace ivrdroid {
namespace {

constexpr uint32_t kSnapshotMagic = 0x49565231U;  // IVR1
constexpr uint32_t kSnapshotVersion = 5;
constexpr uint32_t kFnvOffsetBasis = 2'166'136'261U;
constexpr uint32_t kFnvPrime = 16'777'619U;

static_assert(std::is_standard_layout_v<PersistentSessionSnapshot>);
static_assert(offsetof(PersistentSessionSnapshot, checksum) == 76);
static_assert(sizeof(PersistentSessionSnapshot) == 80);

uint32_t SnapshotChecksum(
    const PersistentSessionSnapshot& snapshot) {
    uint32_t checksum = kFnvOffsetBasis;
    const auto* bytes =
        reinterpret_cast<const uint8_t*>(&snapshot);
    for (size_t index = 0;
         index < offsetof(PersistentSessionSnapshot, checksum);
         ++index) {
        checksum ^= bytes[index];
        checksum *= kFnvPrime;
    }
    return checksum;
}

bool IsLowerHex(char value) {
    return
        (value >= '0' && value <= '9') ||
        (value >= 'a' && value <= 'f');
}

std::string_view SnapshotBootId(
    const PersistentSessionSnapshot& snapshot) {
    return std::string_view(
        snapshot.bootId.data(),
        snapshot.bootId.size());
}

}  // namespace

bool IsValidBootId(std::string_view bootId) {
    if (bootId.size() != kBootIdLength) return false;
    for (size_t index = 0; index < bootId.size(); ++index) {
        const bool hyphen =
            index == 8 || index == 13 || index == 18 || index == 23;
        if (hyphen ? bootId[index] != '-' : !IsLowerHex(bootId[index])) {
            return false;
        }
    }
    return true;
}

bool BuildSessionSnapshot(
    std::string_view bootId,
    uint64_t sessionId,
    uint64_t callIdentityHash,
    int32_t dout,
    int32_t mixer,
    int32_t speaker,
    int32_t mic,
    PersistentSessionSnapshot* snapshot) {
    if (snapshot == nullptr ||
        sessionId == 0 ||
        callIdentityHash == 0 ||
        !IsValidBootId(bootId)) {
        return false;
    }

    *snapshot = {};
    snapshot->magic = kSnapshotMagic;
    snapshot->version = kSnapshotVersion;
    snapshot->sessionId = sessionId;
    snapshot->callIdentityHash = callIdentityHash;
    snapshot->dout = dout;
    snapshot->mixer = mixer;
    snapshot->speaker = speaker;
    snapshot->mic = mic;
    std::memcpy(
        snapshot->bootId.data(),
        bootId.data(),
        snapshot->bootId.size());
    snapshot->checksum = SnapshotChecksum(*snapshot);
    return true;
}

bool ValidateSessionSnapshot(
    const PersistentSessionSnapshot& snapshot) {
    return
        snapshot.magic == kSnapshotMagic &&
        snapshot.version == kSnapshotVersion &&
        snapshot.sessionId != 0 &&
        snapshot.callIdentityHash != 0 &&
        IsValidBootId(SnapshotBootId(snapshot)) &&
        snapshot.checksum == SnapshotChecksum(snapshot);
}

SnapshotBootRelation ClassifySnapshotBoot(
    const PersistentSessionSnapshot& snapshot,
    std::string_view currentBootId) {
    if (!ValidateSessionSnapshot(snapshot) ||
        !IsValidBootId(currentBootId)) {
        return SnapshotBootRelation::Invalid;
    }
    return SnapshotBootId(snapshot) == currentBootId
        ? SnapshotBootRelation::SameBoot
        : SnapshotBootRelation::PreviousBoot;
}

}  // namespace ivrdroid
