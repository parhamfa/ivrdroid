package ai.rx1.ivrdroid.telecom.external

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class OutgoingOperatorOwnershipPolicyTest {
    private val sessionId = "11111111-1111-4111-8111-111111111111"
    private val blockId = "22222222-2222-4222-8222-222222222222"
    private val bootId = "33333333-3333-4333-8333-333333333333"
    private val number = "03136644636"
    private val config = ExternalCallConfig(sessionId, 91, blockId, number, 30_000, bootId)
    private val instruction = SignedExternalCallInstruction(91, blockId, number, 30_000)

    @Test
    fun journaledSignedIdentitySurvivesTheMutableBridgeAdvancingToCancel() {
        val cleanup = snapshot(ExternalCallPhase.CLEANUP_SYSTEM_FAILURE)

        assertTrue(
            OutgoingOperatorOwnershipPolicy.allows(
                cleanup,
                sessionId,
                number,
                bootId,
                instruction,
            ),
        )
    }

    @Test
    fun terminalOrMismatchedIdentityCannotOwnAnOutgoingLeg() {
        assertFalse(
            OutgoingOperatorOwnershipPolicy.allows(
                snapshot(ExternalCallPhase.SYSTEM_FAILURE),
                sessionId,
                number,
                bootId,
                instruction,
            ),
        )
        assertFalse(
            OutgoingOperatorOwnershipPolicy.allows(
                snapshot(ExternalCallPhase.CLEANUP_SYSTEM_FAILURE),
                sessionId,
                number,
                bootId,
                instruction.copy(phoneNumber = "+989120000000"),
            ),
        )
    }

    private fun snapshot(phase: ExternalCallPhase) = ExternalCallSessionSnapshot(
        config = config,
        phase = phase,
        callerId = "caller",
        operatorId = null,
        conferenceId = null,
        deadlineElapsedMs = 10_000,
        updatedElapsedMs = 1_000,
        reason = if (phase == ExternalCallPhase.SYSTEM_FAILURE) "HELPER_CANCELLED" else "-",
        cleanupHeartbeatStatus = if (phase == ExternalCallPhase.CLEANUP_SYSTEM_FAILURE) {
            CallControlStatus.CALLER_HELD
        } else {
            null
        },
    )
}
