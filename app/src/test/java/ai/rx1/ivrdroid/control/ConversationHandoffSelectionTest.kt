package ai.rx1.ivrdroid.control

import org.json.JSONObject
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.Instant

class ConversationHandoffSelectionTest {
    private val current = ConversationHandoffIdentity(
        "11111111-1111-4111-8111-111111111111",
        91,
        "22222222-2222-4222-8222-222222222222",
    )
    private val recordingId = "44444444-4444-4444-8444-444444444444"

    @Test
    fun onlyAnExactlyCorrelatedReceiptCanAffectTheActiveConference() {
        val exact = JSONObject()
            .put("version", 2)
            .put("kind", "conversation")
            .put("call_id", current.callId)
            .put("revision_id", current.revisionId)
            .put("block_id", current.blockId)
            .put("recording_id", recordingId)
            .put("sequence", 0)
            .put("segment_index", 0)
        assertTrue(ConversationHandoffSelection.matchesCurrentReceipt(exact, current, recordingId, 0))

        assertFalse(
            ConversationHandoffSelection.matchesCurrentReceipt(
                JSONObject(exact.toString()).put("call_id", "55555555-5555-4555-8555-555555555555"),
                current,
                recordingId,
                0,
            ),
        )
        assertFalse(
            ConversationHandoffSelection.matchesCurrentReceipt(
                JSONObject(exact.toString()).put("revision_id", 92),
                current,
                recordingId,
                0,
            ),
        )
        assertFalse(ConversationHandoffSelection.matchesCurrentReceipt(JSONObject(), current, recordingId, 0))
    }

    @Test
    fun helperCaptureAtOrAfterPrecommittedConferenceEvidenceIsAccepted() {
        val evidence = Instant.parse("2026-08-09T12:00:00Z")
        assertTrue(ConversationCaptureBoundary.permits(evidence, evidence))
        assertTrue(ConversationCaptureBoundary.permits(evidence, evidence.plusMillis(1)))
        assertFalse(ConversationCaptureBoundary.permits(evidence, evidence.minusMillis(1)))
    }
}
