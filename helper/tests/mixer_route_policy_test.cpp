#include "mixer_route_policy.h"

#include <cassert>
#include <iostream>

namespace {

constexpr ivrdroid::MixerRouteState kAuditedBaseline {
    0,
    0,
    1,
    0,
};

void AcceptsBothObservedSpeakerStates() {
    assert(ivrdroid::IsValidPreAnswerBaseline(
        {0, 0, 1, 0},
        kAuditedBaseline,
        1));
    assert(ivrdroid::IsValidPreAnswerBaseline(
        {0, 0, 0, 0},
        kAuditedBaseline,
        1));
    assert(ivrdroid::IsValidPreAnswerBaseline(
        {1, 0, 0, 0},
        kAuditedBaseline,
        1));
}

void RejectsRoutingOrMicrophoneChanges() {
    assert(!ivrdroid::IsValidPreAnswerBaseline(
        {2, 0, 1, 0},
        kAuditedBaseline,
        1));
    assert(!ivrdroid::IsValidPreAnswerBaseline(
        {0, 1, 1, 0},
        kAuditedBaseline,
        1));
    assert(!ivrdroid::IsValidPreAnswerBaseline(
        {0, 0, 1, 1},
        kAuditedBaseline,
        1));
    assert(!ivrdroid::IsValidPreAnswerBaseline(
        {0, 0, 2, 0},
        kAuditedBaseline,
        1));
}

void BuildsPrivacyFromTheAuditedBaseline() {
    const auto privateRoute =
        ivrdroid::PrivatePreAnswerRoute(kAuditedBaseline);
    assert(privateRoute.dout == kAuditedBaseline.dout);
    assert(privateRoute.mixer == kAuditedBaseline.mixer);
    assert(privateRoute.speaker == 0);
    assert(privateRoute.mic == 0);
}

void DoesNotCarryAColdRouteThroughAnswer() {
    const ivrdroid::MixerRouteState coldBaseline {1, 0, 0, 0};
    assert(ivrdroid::IsValidPreAnswerBaseline(
        coldBaseline,
        kAuditedBaseline,
        1));

    const auto privateRoute =
        ivrdroid::PrivatePreAnswerRoute(kAuditedBaseline);
    assert(privateRoute.dout == kAuditedBaseline.dout);
    assert(privateRoute.dout != coldBaseline.dout);
    assert(privateRoute.mixer == kAuditedBaseline.mixer);
    assert(privateRoute.speaker == 0);
    assert(privateRoute.mic == 0);
}

void RecognizesEveryOwnedPreAnswerTransition() {
    const ivrdroid::MixerRouteState coldBaseline {1, 0, 1, 0};

    assert(ivrdroid::IsOwnedPreAnswerRoute(
        coldBaseline,
        coldBaseline,
        kAuditedBaseline));
    assert(ivrdroid::IsOwnedPreAnswerRoute(
        {1, 0, 0, 0},
        coldBaseline,
        kAuditedBaseline));
    assert(ivrdroid::IsOwnedPreAnswerRoute(
        {0, 0, 0, 0},
        coldBaseline,
        kAuditedBaseline));

    assert(!ivrdroid::IsOwnedPreAnswerRoute(
        {2, 0, 0, 0},
        coldBaseline,
        kAuditedBaseline));
    assert(!ivrdroid::IsOwnedPreAnswerRoute(
        {0, 1, 0, 0},
        coldBaseline,
        kAuditedBaseline));
    assert(!ivrdroid::IsOwnedPreAnswerRoute(
        {0, 0, 1, 1},
        coldBaseline,
        kAuditedBaseline));
}

void RestoresAuditedRoutingAndCapturedEndpointState() {
    const auto postCallRoute =
        ivrdroid::AuditedPostCallRoute(
            kAuditedBaseline,
            {1, 0, 0, 0});
    assert(postCallRoute.dout == kAuditedBaseline.dout);
    assert(postCallRoute.mixer == kAuditedBaseline.mixer);
    assert(postCallRoute.speaker == 0);
    assert(postCallRoute.mic == 0);
}

}  // namespace

int main() {
    AcceptsBothObservedSpeakerStates();
    RejectsRoutingOrMicrophoneChanges();
    BuildsPrivacyFromTheAuditedBaseline();
    DoesNotCarryAColdRouteThroughAnswer();
    RecognizesEveryOwnedPreAnswerTransition();
    RestoresAuditedRoutingAndCapturedEndpointState();
    std::cout << "Mixer route policy tests passed." << std::endl;
    return 0;
}
