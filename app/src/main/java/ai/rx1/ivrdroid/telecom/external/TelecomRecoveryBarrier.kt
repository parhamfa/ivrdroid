package ai.rx1.ivrdroid.telecom.external

/** Native evidence comes from a separate dumpsys observer, not InCallService callbacks. */
data class NativeObservedCall(val id: String, val state: String, val children: Int, val hasParent: Boolean?)
data class NativeCallSnapshot(val boot: String, val session: String, val elapsedMs: Long,
    val sequence: Long, val parsed: Boolean, val emergency: Boolean, val calls: List<NativeObservedCall>) {
    companion object {
        fun parse(wire: String): NativeCallSnapshot {
            require(wire.length <= 4096 && wire.endsWith('\n'))
            val p = wire.trimEnd().split(' ')
            require(p.size >= 8 && p[0] == "NATIVE2" && p[5] in setOf("0", "1") && p[6] in setOf("0", "1"))
            val count = p[7].toInt().also { require(it in 0..16 && p.size == it + 8) }
            val calls = p.drop(8).map { value ->
                val c = value.split(':'); require(c.size == 4 && c[0].matches(Regex("TC@[A-Za-z0-9_-]{1,60}")))
                require(c[1].matches(Regex("[A-Z_]{1,32}")) && c[3] in setOf("0", "1", "?"))
                NativeObservedCall(c[0], c[1], c[2].toInt().also { require(it in -1..16) },
                    if (c[3] == "?") null else c[3] == "1")
            }
            require(calls.map { it.id }.toSet().size == count)
            return NativeCallSnapshot(p[1], p[2], p[3].toLong().also { require(it >= 0) },
                p[4].toLong().also { require(it > 0) }, p[5] == "1", p[6] == "1", calls)
        }
    }
}

enum class TelecomReadiness { WAITING_NATIVE, WAITING_CALLBACKS, WAITING_HANDSHAKE, SETTLING, READY, EMERGENCY }

/** Caller/native agreement must also pass TelecomRecoveryBarrier before retirement. */
fun canRetireReleasedController(old: ExternalCallSessionSnapshot, calls: List<TelecomCallSnapshot>,
    stillLocallyOwned: Boolean, helperIdle: Boolean, nativeOwnsCurrentSession: Boolean): Boolean {
    val oldIds = setOfNotNull(old.callerId, old.operatorId, old.conferenceId)
    return !stillLocallyOwned && (helperIdle || nativeOwnsCurrentSession) &&
        calls.none { it.ownerSessionId == old.config.sessionId || it.id in oldIds }
}

class TelecomRecoveryBarrier(private val settleMs: Long = 500, private val maximumAgeMs: Long = 2000) {
    private var fingerprint: String? = null
    private var settledSince = 0L
    private var firstSequence = 0L

    fun observe(native: NativeCallSnapshot?, calls: List<TelecomCallSnapshot>, boot: String?,
                session: String?, now: Long): TelecomReadiness {
        fun wait(reason: TelecomReadiness): TelecomReadiness { fingerprint = null; return reason }
        if (native == null || !native.parsed || native.boot != boot || native.elapsedMs > now ||
            now - native.elapsedMs > maximumAgeMs || (session != null && native.session != session && native.calls.isNotEmpty()))
            return wait(TelecomReadiness.WAITING_NATIVE)
        if (native.emergency) return wait(TelecomReadiness.EMERGENCY)
        if (calls.any { it.nativeId == null } || calls.mapNotNull { it.nativeId }.toSet().size != calls.size ||
            native.calls.map { it.id }.toSet() != calls.mapNotNull { it.nativeId }.toSet())
            return wait(TelecomReadiness.WAITING_CALLBACKS)
        for (observed in native.calls) {
            val call = calls.single { it.nativeId == observed.id }
            if (observed.hasParent == null || observed.children < 0 ||
                observed.hasParent != (call.parentNativeId != null) || observed.children != call.childrenNativeIds.size ||
                call.childrenNativeIds.any { child -> calls.none { it.nativeId == child && it.parentNativeId == call.nativeId } } ||
                (call.parentNativeId != null && calls.none { it.nativeId == call.parentNativeId && call.nativeId in it.childrenNativeIds }) ||
                normalize(observed.state) != normalize(call.state.name)) return wait(TelecomReadiness.WAITING_CALLBACKS)
        }
        val current = calls.sortedBy { it.nativeId }.joinToString("|") {
            "${it.nativeId}:${it.state}:${it.parentNativeId}:${it.childrenNativeIds.sorted()}:${it.ownerSessionId}"
        }
        if (current != fingerprint || now < settledSince) {
            fingerprint = current; settledSince = now; firstSequence = native.sequence
            return TelecomReadiness.SETTLING
        }
        // Observer sequence numbers restart with the helper. Fresh boot-scoped
        // time and a distinct observation are required, not an ever-growing PID counter.
        return if (now - settledSince >= settleMs && native.sequence != firstSequence && native.elapsedMs > settledSince)
            TelecomReadiness.READY else TelecomReadiness.SETTLING
    }
    private fun normalize(state: String): String = when (state) {
        "ON_HOLD" -> "HOLDING"
        "ANSWERED" -> "ACTIVE"
        "CONNECTING" -> "NEW"
        "SELECT_PHONE_ACCOUNT" -> "DIALING"
        else -> state
    }
}
