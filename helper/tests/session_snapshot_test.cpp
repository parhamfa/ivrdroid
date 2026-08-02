#include "session_snapshot.h"

#include <cassert>
#include <iostream>

namespace {

constexpr char kBootOne[] =
    "01234567-89ab-cdef-0123-456789abcdef";
constexpr char kBootTwo[] =
    "fedcba98-7654-3210-fedc-ba9876543210";

ivrdroid::PersistentSessionSnapshot BuildValidSnapshot() {
    ivrdroid::PersistentSessionSnapshot snapshot {};
    assert(ivrdroid::BuildSessionSnapshot(
        kBootOne,
        42,
        84,
        1,
        1,
        1,
        1,
        &snapshot));
    return snapshot;
}

void ValidatesStrictBootIds() {
    assert(ivrdroid::IsValidBootId(kBootOne));
    assert(!ivrdroid::IsValidBootId(""));
    assert(!ivrdroid::IsValidBootId(
        "01234567-89AB-CDEF-0123-456789ABCDEF"));
    assert(!ivrdroid::IsValidBootId(
        "0123456789ab-cdef-0123-456789abcdef"));
    assert(!ivrdroid::IsValidBootId(
        "g1234567-89ab-cdef-0123-456789abcdef"));
}

void BuildsAndClassifiesSnapshots() {
    const auto snapshot = BuildValidSnapshot();
    assert(ivrdroid::ValidateSessionSnapshot(snapshot));
    assert(
        ivrdroid::ClassifySnapshotBoot(snapshot, kBootOne) ==
        ivrdroid::SnapshotBootRelation::SameBoot);
    assert(
        ivrdroid::ClassifySnapshotBoot(snapshot, kBootTwo) ==
        ivrdroid::SnapshotBootRelation::PreviousBoot);
}

void RejectsCorruptionAndInvalidConstruction() {
    auto corrupted = BuildValidSnapshot();
    corrupted.mic = 0;
    assert(!ivrdroid::ValidateSessionSnapshot(corrupted));
    assert(
        ivrdroid::ClassifySnapshotBoot(corrupted, kBootOne) ==
        ivrdroid::SnapshotBootRelation::Invalid);

    auto oldVersion = BuildValidSnapshot();
    --oldVersion.version;
    assert(!ivrdroid::ValidateSessionSnapshot(oldVersion));

    ivrdroid::PersistentSessionSnapshot snapshot {};
    assert(!ivrdroid::BuildSessionSnapshot(
        kBootOne,
        0,
        84,
        1,
        1,
        1,
        1,
        &snapshot));
    assert(!ivrdroid::BuildSessionSnapshot(
        "invalid",
        1,
        84,
        1,
        1,
        1,
        1,
        &snapshot));
    assert(!ivrdroid::BuildSessionSnapshot(
        kBootOne,
        1,
        84,
        1,
        1,
        1,
        1,
        nullptr));
    assert(!ivrdroid::BuildSessionSnapshot(
        kBootOne,
        1,
        0,
        1,
        1,
        1,
        1,
        &snapshot));
}

}  // namespace

int main() {
    ValidatesStrictBootIds();
    BuildsAndClassifiesSnapshots();
    RejectsCorruptionAndInvalidConstruction();
    std::cout << "Session snapshot tests passed." << std::endl;
    return 0;
}
