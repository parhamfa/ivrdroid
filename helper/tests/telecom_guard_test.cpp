#include "telecom_guard.h"

#include <cassert>
#include <iostream>

int main() {
    using ivrdroid::CallDisposition;

    const auto idle = ivrdroid::ParseTelecomCallSnapshot(
        "CallsManager:\n"
        "  mCalls:\n"
        "  mCallAudioManager:\n");
    assert(idle.parsed);
    assert(idle.liveCallCount == 0);
    assert(
        ivrdroid::ClassifyCallDisposition(idle) ==
        CallDisposition::Idle);
    assert(!ivrdroid::CanForceEndSingleCall(idle));

    const auto one = ivrdroid::ParseTelecomCallSnapshot(
        "CallsManager:\n"
        "  mCalls:\n"
        "    [Call id=TC@12, state=ANSWERED, cap=[ mut], prop=[]]\n"
        "  mCallAudioManager:\n");
    assert(one.parsed);
    assert(one.liveCallCount == 1);
    assert(one.singleCallIdentity == "TC@12");
    assert(ivrdroid::StableCallIdentityHash(one) != 0);
    assert(
        ivrdroid::ClassifyCallDisposition(one) ==
        CallDisposition::SingleSafe);
    assert(ivrdroid::CanForceEndSingleCall(one));

    const auto two = ivrdroid::ParseTelecomCallSnapshot(
        "CallsManager:\n"
        "  mCalls:\n"
        "    [Call id=TC@12, state=ANSWERED, cap=[ mut], prop=[]]\n"
        "    [Call id=TC@13, state=RINGING, cap=[], prop=[]]\n"
        "  mCallAudioManager:\n");
    assert(two.liveCallCount == 2);
    assert(two.singleCallIdentity.empty());
    assert(ivrdroid::StableCallIdentityHash(two) == 0);
    assert(
        ivrdroid::ClassifyCallDisposition(two) ==
        CallDisposition::Multiple);
    assert(!ivrdroid::CanForceEndSingleCall(two));

    const auto emergency = ivrdroid::ParseTelecomCallSnapshot(
        "CallsManager:\n"
        "  mCalls:\n"
        "    [Call id=TC@14, state=ANSWERED, cap=[], prop=[emerg]]\n"
        "  mCallAudioManager:\n");
    assert(emergency.emergencyCallPresent);
    assert(ivrdroid::StableCallIdentityHash(emergency) == 0);
    assert(
        ivrdroid::ClassifyCallDisposition(emergency) ==
        CallDisposition::Emergency);
    assert(!ivrdroid::CanForceEndSingleCall(emergency));

    const auto legacy = ivrdroid::ParseTelecomCallSnapshot(
        "CallsManager:\n"
        "  mCalls:\n"
        "    Call TC@15: state=ACTIVE\n"
        "      isEmergencyCall: false\n"
        "  mCallAudioManager:\n");
    assert(legacy.liveCallCount == 1);
    assert(legacy.singleCallIdentity == "TC@15");
    assert(ivrdroid::CanForceEndSingleCall(legacy));

    const auto snakeCaseFalse = ivrdroid::ParseTelecomCallSnapshot(
        "CallsManager:\n"
        "  mCalls:\n"
        "    [Call id=TC@16, state=ACTIVE, emergency_call=false]\n"
        "  mCallAudioManager:\n");
    assert(!snakeCaseFalse.emergencyCallPresent);
    assert(
        ivrdroid::ClassifyCallDisposition(snakeCaseFalse) ==
        CallDisposition::SingleSafe);
    assert(ivrdroid::CanForceEndSingleCall(snakeCaseFalse));

    const auto explicitTrue = ivrdroid::ParseTelecomCallSnapshot(
        "CallsManager:\n"
        "  mCalls:\n"
        "    Call TC@17: state=ACTIVE\n"
        "      isEmergencyCall = true\n"
        "  mCallAudioManager:\n");
    assert(explicitTrue.emergencyCallPresent);
    assert(
        ivrdroid::ClassifyCallDisposition(explicitTrue) ==
        CallDisposition::Emergency);
    assert(!ivrdroid::CanForceEndSingleCall(explicitTrue));

    const auto memberTrue = ivrdroid::ParseTelecomCallSnapshot(
        "CallsManager:\n"
        "  mCalls:\n"
        "    [Call id=TC@18, state=ACTIVE, mIsEmergencyCall=true]\n"
        "  mCallAudioManager:\n");
    assert(memberTrue.emergencyCallPresent);
    assert(
        ivrdroid::ClassifyCallDisposition(memberTrue) ==
        CallDisposition::Emergency);
    assert(!ivrdroid::CanForceEndSingleCall(memberTrue));

    const auto capabilityOnly = ivrdroid::ParseTelecomCallSnapshot(
        "CallsManager:\n"
        "  mCalls:\n"
        "    [Call id=TC@19, state=ACTIVE, supportsEmergencyCall=true]\n"
        "  mCallAudioManager:\n");
    assert(!capabilityOnly.emergencyCallPresent);
    assert(
        ivrdroid::ClassifyCallDisposition(capabilityOnly) ==
        CallDisposition::SingleSafe);
    assert(ivrdroid::CanForceEndSingleCall(capabilityOnly));

    const auto ambiguousEmergency = ivrdroid::ParseTelecomCallSnapshot(
        "CallsManager:\n"
        "  mCalls:\n"
        "    [Call id=TC@20, state=ACTIVE, emergency metadata unavailable]\n"
        "  mCallAudioManager:\n");
    assert(ambiguousEmergency.emergencyCallPresent);
    assert(
        ivrdroid::ClassifyCallDisposition(ambiguousEmergency) ==
        CallDisposition::Emergency);
    assert(!ivrdroid::CanForceEndSingleCall(ambiguousEmergency));

    const auto missingIdentity = ivrdroid::ParseTelecomCallSnapshot(
        "CallsManager:\n"
        "  mCalls:\n"
        "    [Call id=TC@, state=ANSWERED, cap=[], prop=[]]\n"
        "  mCallAudioManager:\n");
    assert(
        ivrdroid::ClassifyCallDisposition(missingIdentity) ==
        CallDisposition::Unknown);

    const auto malformed = ivrdroid::ParseTelecomCallSnapshot("mCalls:\n");
    assert(!malformed.parsed);
    assert(
        ivrdroid::ClassifyCallDisposition(malformed) ==
        CallDisposition::Unknown);
    assert(!ivrdroid::CanForceEndSingleCall(malformed));

    const ivrdroid::TelecomCallSnapshot impossible {
        true,
        -1,
        false,
        {},
    };
    assert(
        ivrdroid::ClassifyCallDisposition(impossible) ==
        CallDisposition::Unknown);

    const ivrdroid::OwnedCallTopology owned {"TC@a", "TC@b", "TC@c"};
    auto conference = ivrdroid::ParseTelecomCallSnapshot(
        "mCalls:\n"
        " [Call id=TC@a, state=ACTIVE, childs(0), has_parent(true)]\n"
        " [Call id=TC@b, state=ON_HOLD, childs(0), has_parent(true)]\n"
        " [Call id=TC@c, state=ACTIVE, childs(2), has_parent(false)]\n"
        "mCallAudioManager:\n");
    assert(ivrdroid::MatchesOwnedConference(conference, owned));
    ivrdroid::OwnedCallTopology awaitingParent {owned.caller, owned.operatorCall, {}};
    assert(ivrdroid::ResolveOwnedConference(conference, &awaitingParent));
    assert(awaitingParent.conference == owned.conference);
    ivrdroid::OwnedCallTopology missingOperator {owned.caller, {}, {}};
    assert(!ivrdroid::ResolveOwnedConference(conference, &missingOperator));
    const auto wire = ivrdroid::FormatNativeCallSnapshot(conference, "boot", "session", 5000, 1);
    ivrdroid::TelecomCallSnapshot recovered {};
    assert(ivrdroid::ParseNativeCallSnapshot(wire, "boot", "session", 5100, &recovered));
    assert(ivrdroid::MatchesOwnedConference(recovered, owned));
    assert(!ivrdroid::ParseNativeCallSnapshot(wire, "boot", "session", 7100, &recovered));
    assert(!ivrdroid::ParseNativeCallSnapshot(wire, "other-boot", "session", 5100, &recovered));
    for (int index = 0; index < 3; ++index) {
        auto incomplete = conference;
        incomplete.calls.erase(incomplete.calls.begin() + index); --incomplete.liveCallCount;
        assert(!ivrdroid::MatchesOwnedConference(incomplete, owned));
        assert(ivrdroid::ContainsOnlyOwnedCalls(incomplete, owned));
    }
    auto foreign = conference; foreign.calls[1].id = "TC@unrelated";
    assert(!ivrdroid::ContainsOnlyOwnedCalls(foreign, owned));
    awaitingParent.conference.clear();
    assert(!ivrdroid::ResolveOwnedConference(foreign, &awaitingParent));
    auto unsafe = conference; unsafe.emergencyCallPresent = true;
    assert(!ivrdroid::ContainsOnlyOwnedCalls(unsafe, owned));
    unsafe = conference; unsafe.calls[2].children = 1;
    assert(!ivrdroid::MatchesOwnedConference(unsafe, owned));
    unsafe = conference; unsafe.calls[0].parentKnown = false;
    assert(!ivrdroid::MatchesOwnedConference(unsafe, owned));
    std::cout << "Telecom guard ownership, topology and freshness tests passed." << std::endl;
    return 0;
}
