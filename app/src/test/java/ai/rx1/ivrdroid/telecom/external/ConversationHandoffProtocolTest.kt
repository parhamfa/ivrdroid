package ai.rx1.ivrdroid.telecom.external

import ai.rx1.ivrdroid.control.ConversationHandoffIdentity
import ai.rx1.ivrdroid.control.ConversationHandoffResult
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Test

class ConversationHandoffProtocolTest {
    private val identity = ConversationHandoffIdentity(
        "11111111-1111-4111-8111-111111111111",
        91,
        "22222222-2222-4222-8222-222222222222",
    )
    private val session = ConversationHandoffSession(
        identity,
        "33333333-3333-4333-8333-333333333333",
    )

    @Test
    fun roundTripsFrozenCrossLanguageFixture() {
        val fixture = resource("call_control/conversation_handoff_v1.txt")
        assertEquals(fixture, ConversationHandoffProtocol.format(ConversationHandoffProtocol.parse(fixture)))
    }

    @Test
    fun rejectsWrongResultReasonAndNonCanonicalSequenceOrTime() {
        val fixture = resource("call_control/conversation_handoff_v1.txt")
        listOf(
            fixture.replace(" OK -\n", " OK STORAGE_FAILED\n"),
            fixture.replace(" OK -\n", " FAILED -\n"),
            fixture.replace(" 0 33333333", " -1 33333333"),
            fixture.replace(" 0 33333333", " 00 33333333"),
            fixture.replace(" 2500 OK", " -1 OK"),
            fixture.replace(" 2500 OK", " 02500 OK"),
            fixture.dropLast(1),
        ).forEach { malformed ->
            assertThrows(Exception::class.java) { ConversationHandoffProtocol.parse(malformed) }
        }
    }

    @Test
    fun controllerWaitsForCompletePairRetriesAckAndSignalsFailure() {
        val outcomes = ArrayDeque<ConversationHandoffResult?>().apply {
            add(null) // WAV-before-receipt: no complete pair yet.
            add(
                ConversationHandoffResult(
                    identity,
                    "44444444-4444-4444-8444-444444444444",
                    0,
                    true,
                    "-",
                ),
            )
            add(
                ConversationHandoffResult(
                    identity,
                    "44444444-4444-4444-8444-444444444444",
                    1,
                    false,
                    "APP_HANDOFF_FAILED",
                ),
            )
        }
        val published = mutableListOf<ConversationHandoffRecord>()
        val failures = mutableListOf<ConversationHandoffIdentity>()
        var rejectFirstPublish = true
        val controller = ConversationHandoffController(
            ingest = { outcomes.removeFirst() },
            publish = {
                published += it
                if (rejectFirstPublish) {
                    rejectFirstPublish = false
                    false
                } else {
                    true
                }
            },
            onFailure = failures::add,
        )

        controller.tick(session, 1_000)
        assertNull(controller.pendingForTest())
        controller.tick(session, 1_100)
        assertEquals(0, controller.pendingForTest()?.segmentIndex)
        controller.tick(session, 1_200)
        assertNull(controller.pendingForTest())
        controller.tick(session, 1_300) // ACK visibility window prevents replacing segment zero.
        assertEquals(2, published.size)
        controller.tick(session, 1_500)
        assertEquals(ConversationHandoffResultKind.FAILED, published.last().result)
        assertEquals(listOf(identity), failures)
    }

    private fun resource(name: String): String = requireNotNull(javaClass.classLoader?.getResource(name)).readText()
}
