#include "privacy_policy.h"

#include <algorithm>

namespace ivrdroid {

PrivacyStabilityPolicy::PrivacyStabilityPolicy(
    int64_t requiredStableMilliseconds,
    int64_t maximumUnverifiedMilliseconds)
    : requiredStableMilliseconds_(
          std::max<int64_t>(0, requiredStableMilliseconds)),
      maximumUnverifiedMilliseconds_(
          std::max<int64_t>(1, maximumUnverifiedMilliseconds)) {}

void PrivacyStabilityPolicy::Start(int64_t nowMilliseconds) {
    stableSinceMilliseconds_ = -1;
    lastVerifiedPrivateMilliseconds_ = nowMilliseconds;
    started_ = true;
    ready_ = false;
}

PrivacyDecision PrivacyStabilityPolicy::Observe(
    PrivacyObservation observation,
    bool startupRouteReady,
    int64_t nowMilliseconds) {
    if (!started_ || nowMilliseconds < lastVerifiedPrivateMilliseconds_) {
        return PrivacyDecision::Abort;
    }

    if (observation == PrivacyObservation::Contended) {
        stableSinceMilliseconds_ = -1;
        return nowMilliseconds - lastVerifiedPrivateMilliseconds_ >=
                maximumUnverifiedMilliseconds_
            ? PrivacyDecision::Abort
            : PrivacyDecision::Continue;
    }

    lastVerifiedPrivateMilliseconds_ = nowMilliseconds;
    if (ready_) return PrivacyDecision::Continue;

    if (!startupRouteReady) {
        stableSinceMilliseconds_ = -1;
        return PrivacyDecision::Continue;
    }

    if (observation == PrivacyObservation::Corrected ||
        stableSinceMilliseconds_ < 0) {
        stableSinceMilliseconds_ = nowMilliseconds;
    }
    if (nowMilliseconds - stableSinceMilliseconds_ <
        requiredStableMilliseconds_) {
        return PrivacyDecision::Continue;
    }

    ready_ = true;
    return PrivacyDecision::Ready;
}

bool PrivacyStabilityPolicy::ready() const {
    return ready_;
}

}  // namespace ivrdroid
