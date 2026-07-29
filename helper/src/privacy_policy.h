#pragma once

#include <cstdint>

namespace ivrdroid {

enum class PrivacyObservation {
    Private,
    Corrected,
    Contended,
};

enum class PrivacyDecision {
    Continue,
    Ready,
    Abort,
};

class PrivacyStabilityPolicy {
public:
    PrivacyStabilityPolicy(
        int64_t requiredStableMilliseconds,
        int64_t maximumUnverifiedMilliseconds);

    void Start(int64_t nowMilliseconds);
    PrivacyDecision Observe(
        PrivacyObservation observation,
        bool startupRouteReady,
        int64_t nowMilliseconds);

    bool ready() const;

private:
    int64_t requiredStableMilliseconds_;
    int64_t maximumUnverifiedMilliseconds_;
    int64_t stableSinceMilliseconds_ = -1;
    int64_t lastVerifiedPrivateMilliseconds_ = -1;
    bool started_ = false;
    bool ready_ = false;
};

}  // namespace ivrdroid
