#include "call_lifetime_policy.h"
#include <cassert>
#include <iostream>

int main() {
    using namespace ivrdroid;
    CallLifetimePolicy policy;
    assert(ParseCallLifetimePolicy("version=1\nmaximum_seconds=3600\n", &policy));
    assert(!ParseCallLifetimePolicy("version=1\nmaximum_seconds=86460\n", &policy));
    assert(!ParseCallLifetimePolicy("version=1\nmaximum_seconds=59\n", &policy));
    const std::string boot = "00000000-0000-4000-8000-000000000001";
    CallLifetime call {"00000000-0000-4000-8000-000000000002", boot, policy, 0, 0};
    assert(!call.Expired(boot, 99999999)); // Ringing does not consume the answered-call allowance.
    assert(call.Answer(123000));
    for (uint64_t outage : {1000, 3000, 10000, 40000, 130000}) {
        CallLifetime restored;
        assert(ParseCallLifetime(FormatCallLifetime(call), &restored));
        assert(!restored.Answer(123000 + outage)); // Restart or transfer cannot reset the clock.
        assert(restored.deadlineElapsedMs == 3723000);
        assert(!restored.Expired(boot, 3722999));
        assert(restored.Expired(boot, 3723000));
        assert(!restored.Expired("another-boot", 99999999));
    }
    // Wall time is absent from the policy and persisted deadline by construction.
    std::cout << "Call lifetime snapshot, expiry, transfer and restart tests passed.\n";
}
