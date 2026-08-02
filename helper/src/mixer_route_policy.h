#pragma once

namespace ivrdroid {

struct MixerRouteState {
    int dout = -1;
    int mixer = -1;
    int speaker = -1;
    int mic = -1;
};

bool IsValidPreAnswerBaseline(
    const MixerRouteState& candidate,
    const MixerRouteState& auditedBaseline,
    int alternateDout);
MixerRouteState PrivatePreAnswerRoute(
    const MixerRouteState& baseline);
bool IsOwnedPreAnswerRoute(
    const MixerRouteState& current,
    const MixerRouteState& capturedBaseline,
    const MixerRouteState& auditedBaseline);
MixerRouteState AuditedPostCallRoute(
    const MixerRouteState& auditedBaseline,
    const MixerRouteState& capturedBaseline);

}  // namespace ivrdroid
