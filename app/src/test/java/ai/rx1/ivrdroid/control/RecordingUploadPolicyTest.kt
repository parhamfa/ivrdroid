package ai.rx1.ivrdroid.control

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class RecordingUploadPolicyTest {
    private val recording = PendingRecording(
        recordingId = "11111111-1111-4111-8111-111111111111",
        callId = "22222222-2222-4222-8222-222222222222",
        revisionId = 85,
        blockId = "33333333-3333-4333-8333-333333333333",
        sequence = 2,
        capturedAt = "2026-08-08T00:00:00Z",
        durationMs = 10_000,
        stopReason = "finish_key",
        sizeBytes = RecordingUploadPolicy.CHUNK_BYTES.toLong() * 2 + 17,
        sha256 = "a".repeat(64),
    )

    @Test
    fun conversationSegmentUsesCompoundIdentityAndRejectsAnotherSegment() {
        val segment = recording.copy(
            kind = "conversation",
            sequence = 7,
            segmentIndex = 7,
            stopReason = "segment_boundary",
        )
        val request = RecordingUploadPolicy.createRequest(segment)
        assertEquals(7, request.getInt("segment_index"))
        assertEquals(false, request.getBoolean("partial"))
        assertEquals(
            setOf(
                "kind",
                "recording_id",
                "call_id",
                "revision_id",
                "block_id",
                "sequence",
                "segment_index",
                "captured_at",
                "duration_ms",
                "stop_reason",
                "expected_size_bytes",
                "source_sha256",
                "partial",
            ),
            request.keys().asSequence().toSet(),
        )

        val accepted = receipt().put("segment_index", 7)
        RecordingUploadPolicy.verify(segment, accepted)
        assertThrows(IllegalArgumentException::class.java) {
            RecordingUploadPolicy.verify(segment, JSONObject(accepted.toString()).put("segment_index", 8))
        }
        assertEquals(
            "/api/device/v1/conversation-recordings/${segment.recordingId}/segments/7",
            RecordingApiPaths.segment(segment),
        )
    }

    @Test
    fun conversationOrchestrationBeginsLogicalFirstAndFinalizesOnlyTerminalSegment() {
        val first = recording.copy(
            kind = "conversation",
            sequence = 0,
            segmentIndex = 0,
            stopReason = "segment_boundary",
        )
        val logicalRequest = ConversationUploadPolicy.logicalCreateRequest(first)
        assertEquals(first.recordingId, logicalRequest.getString("recording_id"))
        assertEquals(first.capturedAt, logicalRequest.getString("captured_at"))
        val logicalUploading = JSONObject()
            .put("id", first.recordingId)
            .put("status", "uploading")
            .put("segment_count", 0)
            .put("duration_ms", 0)
            .put("acknowledged", false)
        assertEquals(false, ConversationUploadPolicy.verifyLogical(first, logicalUploading))

        val verifiedSegment = receipt(status = "verified", offset = first.sizeBytes, acknowledged = true)
            .put("segment_index", 0)
        val verified = RecordingUploadPolicy.verify(first, verifiedSegment)
        assertEquals(RecordingUploadAction.AcknowledgeAndDelete, RecordingUploadPolicy.nextAction(first, verified, false))
        assertTrue(RecordingUploadPolicy.isAcknowledgedTerminalStatus(first, "verified"))
        assertTrue(RecordingUploadPolicy.isAcknowledgedTerminalStatus(first, "assembled"))

        val terminal = first.copy(stopReason = "operator_hangup")
        assertEquals(
            RecordingUploadAction.FinalizeConversation,
            RecordingUploadPolicy.nextAction(terminal, RecordingUploadPolicy.verify(terminal, verifiedSegment), false),
        )
        val logicalReady = JSONObject(logicalUploading.toString())
            .put("status", "ready")
            .put("segment_count", 1)
            .put("duration_ms", terminal.durationMs)
            .put("acknowledged", true)
        ConversationUploadPolicy.verifyCompleted(terminal, logicalReady)

        val resumedSecond = terminal.copy(sequence = 1, segmentIndex = 1)
        val onePriorSegment = JSONObject(logicalUploading.toString()).put("segment_count", 1)
        assertEquals(false, ConversationUploadPolicy.verifyLogical(resumedSecond, onePriorSegment))
    }

    @Test
    fun requestKeepsDurableCallAndRecordingCorrelation() {
        val body = RecordingUploadPolicy.createRequest(recording)
        assertEquals(recording.recordingId, body.getString("recording_id"))
        assertEquals(recording.callId, body.getString("call_id"))
        assertEquals(recording.revisionId, body.getLong("revision_id"))
        assertEquals(recording.blockId, body.getString("block_id"))
        assertEquals(recording.sequence, body.getInt("sequence"))
        assertEquals(recording.sha256, body.getString("source_sha256"))
    }

    @Test
    fun resumesAtServerOffsetAndUsesFixedOneMiBChunks() {
        val verified = RecordingUploadPolicy.verify(
            recording,
            receipt(offset = RecordingUploadPolicy.CHUNK_BYTES.toLong()),
        )
        val action = RecordingUploadPolicy.nextAction(recording, verified, false)
        assertEquals(
            RecordingUploadAction.UploadChunk(
                RecordingUploadPolicy.CHUNK_BYTES.toLong(),
                RecordingUploadPolicy.CHUNK_BYTES,
            ),
            action,
        )
        RecordingUploadPolicy.requireAdvanced(
            action as RecordingUploadAction.UploadChunk,
            RecordingUploadPolicy.verify(recording, receipt(offset = RecordingUploadPolicy.CHUNK_BYTES.toLong() * 2)),
        )
        val tail = RecordingUploadPolicy.nextAction(
            recording,
            RecordingUploadPolicy.verify(recording, receipt(offset = RecordingUploadPolicy.CHUNK_BYTES.toLong() * 2)),
            false,
        )
        assertEquals(RecordingUploadAction.UploadChunk(RecordingUploadPolicy.CHUNK_BYTES.toLong() * 2, 17), tail)
    }

    @Test
    fun duplicateReadyReceiptDeletesOnlyAfterVerifiedAcknowledgement() {
        val ready = RecordingUploadPolicy.verify(
            recording,
            receipt(status = "ready", offset = 0, acknowledged = true),
        )
        assertEquals(RecordingUploadAction.AcknowledgeAndDelete, RecordingUploadPolicy.nextAction(recording, ready, false))

        val uploaded = RecordingUploadPolicy.verify(recording, receipt(offset = recording.sizeBytes))
        assertEquals(RecordingUploadAction.Complete, RecordingUploadPolicy.nextAction(recording, uploaded, false))
    }

    @Test
    fun incomingCallPausesBeforeUploadCompletionOrLocalDeletion() {
        val uploading = RecordingUploadPolicy.verify(recording, receipt(offset = recording.sizeBytes))
        assertEquals(RecordingUploadAction.PauseForCall, RecordingUploadPolicy.nextAction(recording, uploading, true))
        val ready = RecordingUploadPolicy.verify(recording, receipt(status = "ready", offset = 0, acknowledged = true))
        assertEquals(RecordingUploadAction.PauseForCall, RecordingUploadPolicy.nextAction(recording, ready, true))
    }

    @Test
    fun rejectsTamperedReceiptsAndUnsafeOffsets() {
        listOf(
            receipt().put("id", "99999999-9999-4999-8999-999999999999"),
            receipt().put("source_sha256", "b".repeat(64)),
            receipt().put("expected_size_bytes", recording.sizeBytes - 1),
            receipt(offset = recording.sizeBytes + 1),
            receipt(status = "ready", acknowledged = false),
            receipt(status = "uploading", acknowledged = true),
        ).forEach { tampered ->
            assertThrows(IllegalArgumentException::class.java) {
                RecordingUploadPolicy.verify(recording, tampered)
            }
        }
    }

    @Test
    fun retryableServerFailureCannotProduceAnAcknowledgementAction() {
        val stillUploading = RecordingUploadPolicy.verify(recording, receipt(offset = 0))
        assertTrue(
            RecordingUploadPolicy.nextAction(recording, stillUploading, false) is RecordingUploadAction.UploadChunk,
        )
        val action = RecordingUploadAction.UploadChunk(0, RecordingUploadPolicy.CHUNK_BYTES)
        assertThrows(IllegalArgumentException::class.java) {
            RecordingUploadPolicy.requireAdvanced(action, RecordingUploadPolicy.verify(recording, receipt(offset = 12)))
        }
    }

    private fun receipt(
        status: String = "uploading",
        offset: Long = 0,
        acknowledged: Boolean = false,
    ): JSONObject = JSONObject()
        .put("id", recording.recordingId)
        .put("status", status)
        .put("upload_offset", offset)
        .put("expected_size_bytes", recording.sizeBytes)
        .put("source_sha256", recording.sha256)
        .put("acknowledged", acknowledged)
}
