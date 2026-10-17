package ai.rx1.ivrdroid.telecom

import ai.rx1.ivrdroid.telecom.external.NativeCallSnapshot
import ai.rx1.ivrdroid.telecom.external.TelecomCallDirection
import ai.rx1.ivrdroid.telecom.external.TelecomCallSnapshot
import ai.rx1.ivrdroid.telecom.external.TelecomCallState

/** The gate has no recovery-duration timeout. Evidence, identity and live state decide. */
object IncomingAdmissionPolicy {
    fun mayAnswer(session: String, boot: String, caller: TelecomCallSnapshot, native: NativeCallSnapshot?,
        now: Long, privacyArmed: Boolean, answerRequested: Boolean, policyAllowed: Boolean): Boolean {
        if (!policyAllowed || !privacyArmed || answerRequested || caller.ownerSessionId != session ||
            caller.direction != TelecomCallDirection.INCOMING || caller.state != TelecomCallState.RINGING ||
            caller.emergency || caller.nativeId == null || caller.parentNativeId != null || caller.childrenNativeIds.isNotEmpty()) return false
        if (native == null || !native.parsed || native.emergency || native.session != session || native.boot != boot ||
            native.elapsedMs > now || now - native.elapsedMs > 2000) return false
        val observed = native.calls.singleOrNull() ?: return false
        return observed.id == caller.nativeId && observed.state == "RINGING" && observed.children == 0 && observed.hasParent == false
    }
}
