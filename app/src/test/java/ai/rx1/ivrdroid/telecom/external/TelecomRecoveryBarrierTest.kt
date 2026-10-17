package ai.rx1.ivrdroid.telecom.external

import org.junit.Assert.*
import org.junit.Test

class TelecomRecoveryBarrierTest {
    private val boot = "eec4fd23-c002-49c2-9a79-66493859b7f5"
    private val session = "9596b198-2641-441a-a88a-f95856f90eb4"
    private val caller = TelecomCallSnapshot("caller", TelecomCallDirection.INCOMING, TelecomCallState.ACTIVE, session,
        "+12025550111", false, true, nativeId = "TC@1", parentNativeId = "TC@3")
    private val operator = caller.copy(id = "operator", direction = TelecomCallDirection.OUTGOING, nativeId = "TC@2")
    private val conference = caller.copy(id = "conference", direction = TelecomCallDirection.UNKNOWN, ownerSessionId = null,
        nativeId = "TC@3", parentNativeId = null, childrenNativeIds = listOf("TC@1", "TC@2"))
    private val nativeCalls = listOf(NativeObservedCall("TC@1", "ACTIVE", 0, true),
        NativeObservedCall("TC@2", "ACTIVE", 0, true), NativeObservedCall("TC@3", "ACTIVE", 2, false))
    private fun snapshot(now: Long, sequence: Long, calls: List<NativeObservedCall> = nativeCalls) =
        NativeCallSnapshot(boot, session, now, sequence, true, false, calls)

    @Test fun oldControllerCanRetireWhileNextExactCallerRingsAfterResourceRelease() {
        val old = ExternalCallSessionSnapshot(ExternalCallConfig(session, 1,
            "11111111-1111-4111-8111-111111111111", "+12025550111", 30000, boot),
            ExternalCallPhase.DIALING, "old-caller", "old-operator", null, 30000, 1000)
        val waiting = caller.copy(id = "new-caller", ownerSessionId = "new-session", nativeId = "TC@4",
            state = TelecomCallState.RINGING, parentNativeId = null)
        assertTrue(canRetireReleasedController(old, listOf(waiting), false, true, false))
        assertTrue(canRetireReleasedController(old, listOf(waiting), false, false, true))
        assertFalse(canRetireReleasedController(old, listOf(waiting), true, true, false))
        assertFalse(canRetireReleasedController(old, listOf(waiting), false, false, false))
        assertFalse(canRetireReleasedController(old, listOf(waiting, operator.copy(id = "old-operator")), false, true, false))
        val barrier = TelecomRecoveryBarrier()
        val native = snapshot(7000, 1, listOf(NativeObservedCall("TC@4", "RINGING", 0, false))).copy(session = "new-session")
        assertEquals(TelecomReadiness.SETTLING, barrier.observe(native, listOf(waiting), boot, null, 7000))
        assertEquals(TelecomReadiness.READY, barrier.observe(native.copy(elapsedMs = 7500, sequence = 2), listOf(waiting), boot, null, 7500))
    }

    @Test fun callbacksMustAgreeWithIndependentTopologyForEveryOutageLength() {
        // Auditing and network availability are deliberately not inputs to readiness.
        for (outage in listOf(1L, 3L, 10L, 40L, 130L)) for (auditing in listOf(false, true)) {
            for (first in listOf(emptyList(), listOf(caller), listOf(operator), listOf(conference))) {
                val barrier = TelecomRecoveryBarrier()
                val now = 10_000L + outage * 1000
                assertEquals("outage=$outage audit=$auditing first=$first", TelecomReadiness.WAITING_CALLBACKS,
                    barrier.observe(snapshot(now, 1), first, boot, session, now))
                val complete = listOf(operator, conference, caller)
                assertEquals(TelecomReadiness.SETTLING, barrier.observe(snapshot(now + 100, 2), complete, boot, session, now + 100))
                assertEquals(TelecomReadiness.READY, barrier.observe(snapshot(now + 600, 3), complete, boot, session, now + 600))
            }
        }
    }

    @Test fun emptyCallbacksCannotDeclareAnEndButIndependentlyConfirmedIdleCan() {
        val barrier = TelecomRecoveryBarrier()
        assertEquals(TelecomReadiness.WAITING_CALLBACKS, barrier.observe(snapshot(1000, 1), emptyList(), boot, session, 1000))
        assertEquals(TelecomReadiness.SETTLING, barrier.observe(snapshot(2000, 2, emptyList()), emptyList(), boot, session, 2000))
        assertEquals(TelecomReadiness.READY, barrier.observe(snapshot(2500, 3, emptyList()), emptyList(), boot, session, 2500))
    }

    @Test fun replacementObserverCanBecomeReadyWithoutCatchingUpOldSequenceNumbers() {
        val barrier = TelecomRecoveryBarrier()
        assertEquals(TelecomReadiness.SETTLING, barrier.observe(snapshot(2000, 9000, emptyList()), emptyList(), boot, session, 2000))
        assertEquals(TelecomReadiness.READY, barrier.observe(snapshot(2500, 1, emptyList()), emptyList(), boot, session, 2500))
    }

    @Test fun unknownStaleRebootAndBrokenGraphStayUnready() {
        val calls = listOf(caller, operator, conference)
        val barrier = TelecomRecoveryBarrier()
        assertEquals(TelecomReadiness.WAITING_NATIVE, barrier.observe(null, calls, boot, session, 1000))
        assertEquals(TelecomReadiness.WAITING_NATIVE, barrier.observe(snapshot(1000, 1), calls, "another-boot", session, 1000))
        assertEquals(TelecomReadiness.WAITING_NATIVE, barrier.observe(snapshot(1000, 1), calls, boot, session, 3001))
        assertEquals(TelecomReadiness.WAITING_CALLBACKS, barrier.observe(snapshot(4000, 2), listOf(caller, operator, conference.copy(childrenNativeIds = listOf("TC@1"))), boot, session, 4000))
        assertEquals(TelecomReadiness.EMERGENCY, barrier.observe(snapshot(4000, 3).copy(emergency = true), calls, boot, session, 4000))
    }

    @Test fun pinnedPlatformIdentityAndWireRoundTrip() {
        assertEquals("TC@12", NativeTelecomIdentity.from("Call [id: TC@12, state: ACTIVE, details: redacted]"))
        assertNull(NativeTelecomIdentity.from("Call [id: tel:+12025550111, state: ACTIVE]"))
        val native = NativeCallSnapshot.parse("NATIVE2 $boot $session 123 4 1 0 3 TC@1:ACTIVE:0:1 TC@2:ACTIVE:0:1 TC@3:ACTIVE:2:0\n")
        assertEquals(nativeCalls, native.calls)
        assertThrows(IllegalArgumentException::class.java) { NativeCallSnapshot.parse("NATIVE2 $boot $session 123 4 1 0 2 TC@1:ACTIVE:0:0 TC@1:ACTIVE:0:0\n") }
    }
}
