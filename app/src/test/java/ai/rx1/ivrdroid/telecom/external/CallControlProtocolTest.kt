package ai.rx1.ivrdroid.telecom.external

import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test

class CallControlProtocolTest {
    @Test
    fun parsesFrozenHelperDialFixture() {
        val request = CallControlProtocol.parseRequest(resource("call_control/dial_v1.txt"))
            as CallControlRequest.Dial
        assertEquals(91, request.revisionId)
        assertEquals(1, request.sequence)
        assertEquals(1_000, request.elapsedMs)
        assertEquals("03136644636", request.phoneNumber)
        assertEquals(30_000, request.answerTimeoutMs)
    }

    @Test
    fun roundTripsFrozenAppStatusFixture() {
        val fixture = resource("call_control/status_v1.txt")
        assertEquals(fixture, CallControlProtocol.formatStatus(CallControlProtocol.parseStatus(fixture)))
    }

    @Test
    fun parsesMonotonicRecorderReadyFixture() {
        val request = CallControlProtocol.parseRequest(resource("call_control/recorder_ready_v1.txt"))
            as CallControlRequest.RecorderReady
        assertEquals(2, request.sequence)
        assertEquals(2_000, request.elapsedMs)
    }

    @Test
    fun parsesFrozenHelperCancelFixtureWithBoundedReason() {
        val request = CallControlProtocol.parseRequest(resource("call_control/cancel_v1.txt"))
            as CallControlRequest.Cancel
        assertEquals("HELPER_CANCELLED", request.reason)
        assertThrows(Exception::class.java) {
            CallControlProtocol.parseRequest(resource("call_control/cancel_v1.txt").replace(" HELPER_CANCELLED\n", " -\n"))
        }
    }

    @Test
    fun roundTripsOutcomeAwareAnswerTimeoutFixtures() {
        val cancelFixture = resource("call_control/answer_timeout_cancel_v1.txt")
        val cancel = CallControlProtocol.parseRequest(cancelFixture) as CallControlRequest.Cancel
        assertEquals("ANSWER_TIMEOUT", cancel.reason)
        assertEquals(2, cancel.sequence)

        val terminalFixture = resource("call_control/answer_timeout_not_connected_v1.txt")
        val terminal = CallControlProtocol.parseStatus(terminalFixture)
        assertEquals(CallControlStatus.NOT_CONNECTED, terminal.status)
        assertEquals("ANSWER_TIMEOUT", terminal.reason)
        assertEquals(terminalFixture, CallControlProtocol.formatStatus(terminal))
    }

    @Test
    fun rejectsNonCanonicalOrInjectedRecords() {
        val dial = resource("call_control/dial_v1.txt")
        listOf(
            dial.dropLast(1),
            dial + "TRAILING\n",
            dial.replace(" 91 ", " 0 "),
            dial.replace(" 91 ", " 091 "),
            dial.replace(" 1 33333333", " 0 33333333"),
            dial.replace(" 1 33333333", " 01 33333333"),
            dial.replace(" 1000 03136644636 ", " -1 03136644636 "),
            dial.replace(" 1000 03136644636 ", " 01000 03136644636 "),
            dial.replace(" 03136644636 ", " tel:03136644636 "),
            dial.replace(" 30000\n", " 30001\n"),
        ).forEach { malformed ->
            assertThrows(Exception::class.java) { CallControlProtocol.parseRequest(malformed) }
        }
    }

    @Test
    fun firstAppStatusIsOneAndSequenceZeroIsRejected() {
        val first = CallControlStatusRecord(
            CallControlStatus.ACK,
            "11111111-1111-4111-8111-111111111111",
            91,
            "22222222-2222-4222-8222-222222222222",
            1,
            "33333333-3333-4333-8333-333333333333",
            1_000,
            "-",
        )
        assertEquals(first, CallControlProtocol.parseStatus(CallControlProtocol.formatStatus(first)))
        assertThrows(Exception::class.java) {
            CallControlProtocol.parseStatus(CallControlProtocol.formatStatus(first).replace(" 1 33333333", " 0 33333333"))
        }
    }

    private fun resource(name: String): String = requireNotNull(javaClass.classLoader?.getResource(name)).readText()
}
