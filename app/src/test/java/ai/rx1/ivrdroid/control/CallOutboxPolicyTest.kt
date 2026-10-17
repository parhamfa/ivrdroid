package ai.rx1.ivrdroid.control

import org.junit.Assert.*
import org.junit.Test

class CallOutboxPolicyTest {
    private val sent = PendingCallEvent("call-1", "2026-09-19T10:00:00Z", null, "IVR_HANDLED", 21,
        emptyList(), "REMOTE_HANGUP", 30, generation = 1)
    @Test fun acknowledgementCannotDeleteActiveCallOrUpdatedSnapshot() {
        val active = sent.copy(result = "IN_PROGRESS")
        assertEquals(listOf(active), CallOutboxPolicy.afterAcknowledgement(listOf(active), listOf(active), setOf(sent.callId)))
        val event = PendingCallSubEvent("2026-09-19T10:00:31Z", "NOT_CONNECTED", "22222222-2222-4222-8222-222222222222", "ANSWER_TIMEOUT")
        val updated = sent.copy(events = listOf(event), generation = 2)
        assertEquals(listOf(updated), CallOutboxPolicy.afterAcknowledgement(listOf(updated), listOf(sent), setOf(sent.callId)))
        // Even a recreated journal with the same counter must match the exact submitted payload.
        val recreated = updated.copy(generation = 1)
        assertEquals(listOf(recreated), CallOutboxPolicy.afterAcknowledgement(listOf(recreated), listOf(sent), setOf(sent.callId)))
    }
    @Test fun deletesOnlyAcceptedUnchangedTerminalSnapshot() {
        val newerCall = sent.copy(callId = "call-2")
        assertEquals(listOf(newerCall), CallOutboxPolicy.afterAcknowledgement(listOf(sent, newerCall), listOf(sent), setOf(sent.callId)))
        assertEquals(listOf(sent), CallOutboxPolicy.afterAcknowledgement(listOf(sent), listOf(sent), emptySet()))
    }
}
