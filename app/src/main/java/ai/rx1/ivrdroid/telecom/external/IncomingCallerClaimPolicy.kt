package ai.rx1.ivrdroid.telecom.external

import java.security.MessageDigest

data class RegisteredIncomingCaller(
    val sessionId: String,
    val bootId: String,
    val currentCallId: String,
    val handleDigest: String?,
    val registeredElapsedMs: Long,
    val confirmedByInCall: Boolean,
)

data class LiveIncomingCallerCandidate(
    val callId: String,
    val handleDigest: String?,
    val direction: TelecomCallDirection,
    val state: TelecomCallState,
    val emergency: Boolean,
)

sealed interface CallerClaimDecision {
    data class Claimed(val callId: String) : CallerClaimDecision
    data class Rejected(val reason: String) : CallerClaimDecision
}

/**
 * Correlates CallScreeningService and InCallService without assuming their Call.Details fields
 * are identical. A mismatched framework identifier is accepted only for the sole live call, with
 * matching boot-scoped caller evidence, during the initial claim window or an already verified
 * signed external-call request.
 */
object IncomingCallerClaimPolicy {
    const val INITIAL_CLAIM_WINDOW_MS = 30_000L

    fun decide(
        registration: RegisteredIncomingCaller,
        calls: List<LiveIncomingCallerCandidate>,
        nowElapsedMs: Long,
        signedSessionAuthorized: Boolean,
    ): CallerClaimDecision {
        val present = calls.filter {
            it.state !in setOf(TelecomCallState.DISCONNECTING, TelecomCallState.DISCONNECTED)
        }
        if (present.isEmpty()) return CallerClaimDecision.Rejected("CALLER_NOT_VISIBLE")
        if (present.size != 1) return CallerClaimDecision.Rejected("CALLER_TOPOLOGY_AMBIGUOUS")

        val candidate = present.single()
        if (candidate.direction != TelecomCallDirection.INCOMING) {
            return CallerClaimDecision.Rejected("CALLER_DIRECTION_INVALID")
        }
        if (candidate.state !in setOf(
                TelecomCallState.NEW,
                TelecomCallState.RINGING,
                TelecomCallState.ACTIVE,
                TelecomCallState.HOLDING,
            )
        ) {
            return CallerClaimDecision.Rejected("CALLER_STATE_INVALID")
        }
        if (candidate.emergency) return CallerClaimDecision.Rejected("CALLER_EMERGENCY")

        if (candidate.callId != registration.currentCallId) {
            val age = nowElapsedMs - registration.registeredElapsedMs
            if (!signedSessionAuthorized && !registration.confirmedByInCall &&
                age !in 0..INITIAL_CLAIM_WINDOW_MS
            ) {
                return CallerClaimDecision.Rejected("CALLER_CLAIM_EXPIRED")
            }
            if (registration.handleDigest == null ||
                candidate.handleDigest != registration.handleDigest
            ) {
                return CallerClaimDecision.Rejected("CALLER_IDENTITY_MISMATCH")
            }
        }
        return CallerClaimDecision.Claimed(candidate.callId)
    }
}

/** Boot-scoped and number-masked evidence; the raw caller handle is never persisted. */
object CallerHandleEvidence {
    fun digest(rawHandle: String?, bootId: String): String? {
        val raw = rawHandle?.trim()?.takeIf { it.isNotEmpty() } ?: return null
        val digits = raw.filter { it in '0'..'9' }
        if (digits.length !in 8..15) return null
        val suffix = digits.takeLast(10)
        val source = "$bootId\n$suffix"
        return MessageDigest.getInstance("SHA-256")
            .digest(source.toByteArray(Charsets.UTF_8))
            .joinToString("") { "%02x".format(it) }
    }
}
