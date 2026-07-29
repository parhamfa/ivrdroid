#include "telecom_guard.h"

#include <cassert>
#include <iostream>

int main() {
    const auto idle = ivrdroid::ParseTelecomCallSnapshot(
        "CallsManager:\n"
        "  mCalls:\n"
        "  mCallAudioManager:\n");
    assert(idle.parsed);
    assert(idle.liveCallCount == 0);
    assert(!ivrdroid::CanForceEndSingleCall(idle));

    const auto one = ivrdroid::ParseTelecomCallSnapshot(
        "CallsManager:\n"
        "  mCalls:\n"
        "    [Call id=TC@12, state=ANSWERED, cap=[ mut], prop=[]]\n"
        "  mCallAudioManager:\n");
    assert(one.parsed);
    assert(one.liveCallCount == 1);
    assert(ivrdroid::CanForceEndSingleCall(one));

    const auto two = ivrdroid::ParseTelecomCallSnapshot(
        "CallsManager:\n"
        "  mCalls:\n"
        "    [Call id=TC@12, state=ANSWERED, cap=[ mut], prop=[]]\n"
        "    [Call id=TC@13, state=RINGING, cap=[], prop=[]]\n"
        "  mCallAudioManager:\n");
    assert(two.liveCallCount == 2);
    assert(!ivrdroid::CanForceEndSingleCall(two));

    const auto emergency = ivrdroid::ParseTelecomCallSnapshot(
        "CallsManager:\n"
        "  mCalls:\n"
        "    [Call id=TC@14, state=ANSWERED, cap=[], prop=[emerg]]\n"
        "  mCallAudioManager:\n");
    assert(emergency.emergencyCallPresent);
    assert(!ivrdroid::CanForceEndSingleCall(emergency));

    const auto legacy = ivrdroid::ParseTelecomCallSnapshot(
        "CallsManager:\n"
        "  mCalls:\n"
        "    Call TC@15: state=ACTIVE\n"
        "      isEmergencyCall: false\n"
        "  mCallAudioManager:\n");
    assert(legacy.liveCallCount == 1);
    assert(ivrdroid::CanForceEndSingleCall(legacy));

    const auto malformed = ivrdroid::ParseTelecomCallSnapshot("mCalls:\n");
    assert(!malformed.parsed);
    assert(!ivrdroid::CanForceEndSingleCall(malformed));

    std::cout << "Telecom guard tests passed." << std::endl;
    return 0;
}
