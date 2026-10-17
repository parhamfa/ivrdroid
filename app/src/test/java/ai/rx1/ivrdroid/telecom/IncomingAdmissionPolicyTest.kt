package ai.rx1.ivrdroid.telecom

import ai.rx1.ivrdroid.telecom.external.*
import org.junit.Assert.*
import org.junit.Test

class IncomingAdmissionPolicyTest {
    private val caller = TelecomCallSnapshot("caller", TelecomCallDirection.INCOMING, TelecomCallState.RINGING,
        "new-session", null, false, false, nativeId = "TC@2")
    private fun native(now: Long) = NativeCallSnapshot("boot", "new-session", now, now + 1, true, false,
        listOf(NativeObservedCall("TC@2", "RINGING", 0, false)))
    private fun allowed(now: Long = 7000, call: TelecomCallSnapshot = caller, observation: NativeCallSnapshot? = native(now),
        armed: Boolean = true, intent: Boolean = false, enabled: Boolean = true) =
        IncomingAdmissionPolicy.mayAnswer("new-session", "boot", call, observation, now, armed, intent, enabled)

    @Test fun waitsThroughLongRecoveryAndAnswersAtFirstReadyCallback() {
        for (delay in listOf(0L, 250L, 7000L, 20000L, 60000L)) {
            for (now in 0..delay step 250) assertFalse(allowed(now, armed = false))
            assertTrue(allowed(delay)) // No five-second expiry and no invented seven-second delay.
            assertFalse(allowed(delay + 250, intent = true))
        }
    }
    @Test fun refusesChangedIdentityAmbiguousOrStaleNativeEvidence() {
        val observed = native(7000)
        assertFalse(allowed(observation = observed.copy(boot = "previous-boot")))
        assertFalse(allowed(observation = observed.copy(session = "previous-session")))
        assertFalse(allowed(observation = observed.copy(elapsedMs = 1)))
        assertFalse(allowed(observation = observed.copy(elapsedMs = 7001)))
        assertFalse(allowed(observation = observed.copy(parsed = false)))
        assertFalse(allowed(observation = observed.copy(calls = observed.calls + observed.calls)))
        assertFalse(allowed(call = caller.copy(nativeId = "TC@3")))
        assertFalse(allowed(call = caller.copy(ownerSessionId = "previous-session")))
        assertFalse(allowed(observation = observed.copy(calls = listOf(observed.calls.single().copy(hasParent = null)))))
    }
    @Test fun stopsForKillSwitchOtherAnswerDisappearanceAndEmergency() {
        assertFalse(allowed(enabled = false))
        assertFalse(allowed(call = caller.copy(state = TelecomCallState.ACTIVE)))
        assertFalse(allowed(call = caller.copy(state = TelecomCallState.DISCONNECTED)))
        assertFalse(allowed(observation = native(7000).copy(calls = emptyList())))
        assertFalse(allowed(call = caller.copy(emergency = true)))
        assertFalse(allowed(observation = native(7000).copy(emergency = true)))
    }
    @Test fun persistedAnswerIntentIsNotReplayedAfterProcessDeath() {
        assertFalse(allowed(intent = true))
        assertTrue(allowed(intent = false))
    }
}
