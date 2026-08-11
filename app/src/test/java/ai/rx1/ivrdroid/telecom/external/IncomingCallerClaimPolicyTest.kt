package ai.rx1.ivrdroid.telecom.external

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Test

class IncomingCallerClaimPolicyTest {
    private val sessionId = "11111111-1111-4111-8111-111111111111"
    private val bootId = "33333333-3333-4333-8333-333333333333"
    private val digest = requireNotNull(CallerHandleEvidence.digest("09120000000", bootId))

    @Test
    fun crossServiceIdentifierMismatchClaimsTheSoleMatchingIncomingCall() {
        val result = IncomingCallerClaimPolicy.decide(
            registration("screening-key"),
            listOf(candidate("incall-key")),
            nowElapsedMs = 1_500,
            signedSessionAuthorized = false,
        )

        assertEquals(CallerClaimDecision.Claimed("incall-key"), result)
    }

    @Test
    fun confirmedSessionCanRecoverAReboundIdentifierAfterInitialWindow() {
        val result = IncomingCallerClaimPolicy.decide(
            registration("old-incall-key", confirmed = true),
            listOf(candidate("rebound-incall-key")),
            nowElapsedMs = 90_000,
            signedSessionAuthorized = false,
        )

        assertEquals(CallerClaimDecision.Claimed("rebound-incall-key"), result)
    }

    @Test
    fun expiredUnverifiedClaimFailsClosed() {
        val result = IncomingCallerClaimPolicy.decide(
            registration("screening-key"),
            listOf(candidate("incall-key")),
            nowElapsedMs = 31_001,
            signedSessionAuthorized = false,
        )

        assertEquals(CallerClaimDecision.Rejected("CALLER_CLAIM_EXPIRED"), result)
    }

    @Test
    fun mismatchedCallerEvidenceFailsClosed() {
        val foreignDigest = requireNotNull(CallerHandleEvidence.digest("09120000001", bootId))
        val result = IncomingCallerClaimPolicy.decide(
            registration("screening-key"),
            listOf(candidate("incall-key", handleDigest = foreignDigest)),
            nowElapsedMs = 1_500,
            signedSessionAuthorized = true,
        )

        assertEquals(CallerClaimDecision.Rejected("CALLER_IDENTITY_MISMATCH"), result)
    }

    @Test
    fun extraEmergencyOrUnrelatedCallsAreNeverClaimed() {
        val ambiguous = IncomingCallerClaimPolicy.decide(
            registration("screening-key"),
            listOf(candidate("incall-key"), candidate("unrelated-key")),
            nowElapsedMs = 1_500,
            signedSessionAuthorized = true,
        )
        assertEquals(CallerClaimDecision.Rejected("CALLER_TOPOLOGY_AMBIGUOUS"), ambiguous)

        val emergency = IncomingCallerClaimPolicy.decide(
            registration("screening-key"),
            listOf(candidate("incall-key", emergency = true)),
            nowElapsedMs = 1_500,
            signedSessionAuthorized = true,
        )
        assertEquals(CallerClaimDecision.Rejected("CALLER_EMERGENCY"), emergency)
    }

    @Test
    fun handleEvidenceMasksFormattingAndLocalInternationalPrefix() {
        val local = CallerHandleEvidence.digest("0912 000-0000", bootId)
        val international = CallerHandleEvidence.digest("+98 (912) 000 0000", bootId)

        assertEquals(local, international)
        assertNotEquals(local, CallerHandleEvidence.digest("09120000001", bootId))
        assertNull(CallerHandleEvidence.digest("private", bootId))
    }

    private fun registration(
        callId: String,
        confirmed: Boolean = false,
    ) = RegisteredIncomingCaller(
        sessionId,
        bootId,
        callId,
        digest,
        registeredElapsedMs = 1_000,
        confirmedByInCall = confirmed,
    )

    private fun candidate(
        callId: String,
        handleDigest: String? = digest,
        emergency: Boolean = false,
    ) = LiveIncomingCallerCandidate(
        callId,
        handleDigest,
        TelecomCallDirection.INCOMING,
        TelecomCallState.ACTIVE,
        emergency,
    )
}
