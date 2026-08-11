package ai.rx1.ivrdroid.control

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class CallEventPayloadTest {
    private val subEvent = PendingCallSubEvent(
        "2026-08-09T12:00:01Z",
        "CONFERENCED",
        "22222222-2222-4222-8222-222222222222",
        "-",
    )
    private val initial = PendingCallEvent(
        "11111111-1111-4111-8111-111111111111",
        "2026-08-09T12:00:00Z",
        "+989121234567",
        "ACCEPT_ALL",
        86,
        emptyList(),
        "IN_PROGRESS",
        0,
        listOf(subEvent),
    )

    @Test
    fun serializesBoundedSanitizedExternalEventsForUpload() {
        val payload = CallEventPayload.encode(initial)
        val nested = payload.getJSONArray("events")
        assertEquals(listOf(subEvent), CallEventPayload.decodeEvents(nested))
        val eventText = nested.toString()
        assertTrue(eventText.contains("CONFERENCED"))
        assertFalse(eventText.contains(initial.caller!!))
        assertFalse(eventText.contains("telecom"))
        assertFalse(eventText.contains("03136644636"))
    }

    @Test
    fun terminalCallReplacementPreservesCapturedExternalHistory() {
        val completed = initial.copy(result = "SESSION_COMPLETE", durationSeconds = 30, events = emptyList())
        val merged = CallEventPayload.preserveEvents(initial, completed)
        assertEquals("SESSION_COMPLETE", merged.result)
        assertEquals(listOf(subEvent), merged.events)
    }
}
