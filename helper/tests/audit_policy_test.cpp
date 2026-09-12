#include "audit_policy.h"
#include <cassert>
#include <iostream>

int main() {
    ivrdroid::AuditPolicy policy;
    assert(!policy.enabled && policy.version == 0);
    assert(ivrdroid::ParseAuditPolicy("version=3\nenabled=1\nquota=1073741824\n", &policy));
    assert(policy.enabled && policy.version == 3);
    for (const char* invalid : {"", "version=0\nenabled=1\nquota=1073741824\n",
        "version=3\nenabled=2\nquota=1073741824\n", "version=3\nenabled=1\nquota=1\n",
        "version=3\nenabled=1\nquota=1073741824\nextra=1\n"}) {
        assert(!ivrdroid::ParseAuditPolicy(invalid, &policy));
    }
    assert(ivrdroid::AuditStorageFits(0, 0, 2ULL << 30, 1ULL << 30));
    assert(!ivrdroid::AuditStorageFits(1ULL << 30, 0, 2ULL << 30, 1ULL << 30));
    assert(!ivrdroid::AuditStorageFits(UINT64_MAX, UINT64_MAX, UINT64_MAX, 1ULL << 30));
    assert(!ivrdroid::AuditStorageFits(0, 0, ivrdroid::kAuditFilesystemReserve, 1ULL << 30));
    assert(ivrdroid::MixAuditSample(30000, 10000) == 32767);
    assert(ivrdroid::MixAuditSample(-30000, -10000) == -32768);
    assert(ivrdroid::MixAuditSample(1000, 500) == 1500);
    std::cout << "Audit policy, storage reserve and sample mixing tests passed.\n";
}
