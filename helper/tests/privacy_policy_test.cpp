#include "privacy_policy.h"

#include <cassert>
#include <iostream>

namespace {

using ivrdroid::PrivacyDecision;
using ivrdroid::PrivacyObservation;
using ivrdroid::PrivacyStabilityPolicy;

void RequiresAContinuousStableWindow() {
    PrivacyStabilityPolicy policy(500, 250);
    policy.Start(1'000);

    assert(
        policy.Observe(PrivacyObservation::Private, true, 1'000) ==
        PrivacyDecision::Continue);
    assert(
        policy.Observe(PrivacyObservation::Private, true, 1'499) ==
        PrivacyDecision::Continue);
    assert(
        policy.Observe(PrivacyObservation::Private, true, 1'500) ==
        PrivacyDecision::Ready);
    assert(policy.ready());
}

void CorrectionsAndRouteChangesResetStartupStability() {
    PrivacyStabilityPolicy policy(500, 250);
    policy.Start(2'000);

    assert(
        policy.Observe(PrivacyObservation::Private, true, 2'000) ==
        PrivacyDecision::Continue);
    assert(
        policy.Observe(PrivacyObservation::Corrected, true, 2'300) ==
        PrivacyDecision::Continue);
    assert(
        policy.Observe(PrivacyObservation::Private, true, 2'799) ==
        PrivacyDecision::Continue);
    assert(
        policy.Observe(PrivacyObservation::Private, true, 2'800) ==
        PrivacyDecision::Ready);

    PrivacyStabilityPolicy routePolicy(500, 250);
    routePolicy.Start(3'000);
    assert(
        routePolicy.Observe(PrivacyObservation::Private, false, 3'300) ==
        PrivacyDecision::Continue);
    assert(
        routePolicy.Observe(PrivacyObservation::Private, true, 3'799) ==
        PrivacyDecision::Continue);
    assert(
        routePolicy.Observe(PrivacyObservation::Private, true, 4'299) ==
        PrivacyDecision::Ready);
}

void ToleratesBriefContentionButAbortsSustainedLoss() {
    PrivacyStabilityPolicy policy(500, 250);
    policy.Start(4'000);

    assert(
        policy.Observe(PrivacyObservation::Contended, false, 4'249) ==
        PrivacyDecision::Continue);
    assert(
        policy.Observe(PrivacyObservation::Contended, false, 4'250) ==
        PrivacyDecision::Abort);

    PrivacyStabilityPolicy recovered(500, 250);
    recovered.Start(5'000);
    assert(
        recovered.Observe(PrivacyObservation::Contended, false, 5'200) ==
        PrivacyDecision::Continue);
    assert(
        recovered.Observe(PrivacyObservation::Corrected, true, 5'205) ==
        PrivacyDecision::Continue);
    assert(
        recovered.Observe(PrivacyObservation::Contended, false, 5'454) ==
        PrivacyDecision::Continue);
}

void KeepsRuntimeReadyAcrossBriefRewrites() {
    PrivacyStabilityPolicy policy(500, 250);
    policy.Start(6'000);
    assert(
        policy.Observe(PrivacyObservation::Private, true, 6'000) ==
        PrivacyDecision::Continue);
    assert(
        policy.Observe(PrivacyObservation::Private, true, 6'500) ==
        PrivacyDecision::Ready);
    assert(
        policy.Observe(PrivacyObservation::Contended, false, 6'600) ==
        PrivacyDecision::Continue);
    assert(
        policy.Observe(PrivacyObservation::Corrected, false, 6'605) ==
        PrivacyDecision::Continue);
    assert(policy.ready());
}

void AbortsSustainedRuntimePrivacyLoss() {
    PrivacyStabilityPolicy policy(500, 250);
    policy.Start(7'000);
    assert(
        policy.Observe(PrivacyObservation::Private, true, 7'000) ==
        PrivacyDecision::Continue);
    assert(
        policy.Observe(PrivacyObservation::Private, true, 7'500) ==
        PrivacyDecision::Ready);
    assert(
        policy.Observe(PrivacyObservation::Contended, false, 7'749) ==
        PrivacyDecision::Continue);
    assert(
        policy.Observe(PrivacyObservation::Contended, false, 7'750) ==
        PrivacyDecision::Abort);
}

void RejectsUseBeforeStartAndBackwardTime() {
    PrivacyStabilityPolicy policy(500, 250);
    assert(
        policy.Observe(PrivacyObservation::Private, true, 100) ==
        PrivacyDecision::Abort);
    policy.Start(1'000);
    assert(
        policy.Observe(PrivacyObservation::Private, true, 999) ==
        PrivacyDecision::Abort);
}

}  // namespace

int main() {
    RequiresAContinuousStableWindow();
    CorrectionsAndRouteChangesResetStartupStability();
    ToleratesBriefContentionButAbortsSustainedLoss();
    KeepsRuntimeReadyAcrossBriefRewrites();
    AbortsSustainedRuntimePrivacyLoss();
    RejectsUseBeforeStartAndBackwardTime();
    std::cout << "Privacy policy tests passed." << std::endl;
    return 0;
}
