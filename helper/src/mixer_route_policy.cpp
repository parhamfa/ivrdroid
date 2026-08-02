#include "mixer_route_policy.h"

namespace ivrdroid {

bool IsValidPreAnswerBaseline(
    const MixerRouteState& candidate,
    const MixerRouteState& auditedBaseline,
    int alternateDout) {
    return
        (candidate.dout == auditedBaseline.dout ||
         candidate.dout == alternateDout) &&
        candidate.mixer == auditedBaseline.mixer &&
        (candidate.speaker == 0 ||
         candidate.speaker == auditedBaseline.speaker) &&
        candidate.mic == auditedBaseline.mic;
}

MixerRouteState PrivatePreAnswerRoute(
    const MixerRouteState& baseline) {
    return {
        baseline.dout,
        baseline.mixer,
        0,
        0,
    };
}

bool IsOwnedPreAnswerRoute(
    const MixerRouteState& current,
    const MixerRouteState& capturedBaseline,
    const MixerRouteState& auditedBaseline) {
    return
        (current.dout == capturedBaseline.dout ||
         current.dout == auditedBaseline.dout) &&
        (current.mixer == capturedBaseline.mixer ||
         current.mixer == auditedBaseline.mixer) &&
        (current.speaker == capturedBaseline.speaker ||
         current.speaker == 0) &&
        (current.mic == capturedBaseline.mic ||
         current.mic == 0);
}

MixerRouteState AuditedPostCallRoute(
    const MixerRouteState& auditedBaseline,
    const MixerRouteState& capturedBaseline) {
    return {
        auditedBaseline.dout,
        auditedBaseline.mixer,
        capturedBaseline.speaker,
        capturedBaseline.mic,
    };
}

}  // namespace ivrdroid
