package ai.rx1.ivrdroid.control

import org.json.JSONObject

data class VerifiedRecordingReceipt(
    val id: String,
    val status: String,
    val uploadOffset: Long,
    val expectedSizeBytes: Long,
    val sourceSha256: String,
    val acknowledged: Boolean,
)

sealed interface RecordingUploadAction {
    data object PauseForCall : RecordingUploadAction
    data object AcknowledgeAndDelete : RecordingUploadAction
    data object Complete : RecordingUploadAction
    data object FinalizeConversation : RecordingUploadAction
    data class UploadChunk(val offset: Long, val byteCount: Int) : RecordingUploadAction
}

object RecordingUploadPolicy {
    const val CHUNK_BYTES = 1024 * 1024

    fun createRequest(recording: PendingRecording): JSONObject = if (recording.kind == "conversation") {
        JSONObject()
            .put("kind", recording.kind)
            .put("recording_id", recording.recordingId)
            .put("call_id", recording.callId)
            .put("revision_id", recording.revisionId)
            .put("block_id", recording.blockId)
            .put("sequence", recording.sequence)
            .put("segment_index", requireNotNull(recording.segmentIndex))
            .put("captured_at", recording.capturedAt)
            .put("duration_ms", recording.durationMs)
            .put("stop_reason", recording.stopReason)
            .put("expected_size_bytes", recording.sizeBytes)
            .put("source_sha256", recording.sha256)
            .put("partial", recording.partial)
    } else {
        JSONObject()
            .put("recording_id", recording.recordingId)
            .put("call_id", recording.callId)
            .put("revision_id", recording.revisionId)
            .put("block_id", recording.blockId)
            .put("sequence", recording.sequence)
            .put("captured_at", recording.capturedAt)
            .put("duration_ms", recording.durationMs)
            .put("stop_reason", recording.stopReason)
            .put("expected_size_bytes", recording.sizeBytes)
            .put("source_sha256", recording.sha256)
    }

    fun verify(recording: PendingRecording, receipt: JSONObject): VerifiedRecordingReceipt {
        val verified = VerifiedRecordingReceipt(
            id = receipt.getString("id"),
            status = receipt.getString("status"),
            uploadOffset = receipt.getLong("upload_offset"),
            expectedSizeBytes = receipt.getLong("expected_size_bytes"),
            sourceSha256 = receipt.getString("source_sha256"),
            acknowledged = receipt.getBoolean("acknowledged"),
        )
        require(verified.id == recording.recordingId) { "Server returned another recording identifier." }
        require(verified.expectedSizeBytes == recording.sizeBytes) { "Server returned another recording size." }
        require(verified.sourceSha256 == recording.sha256) { "Server returned another recording digest." }
        if (recording.kind == "conversation") {
            require(receipt.getInt("segment_index") == recording.segmentIndex) {
                "Server returned another conversation segment identifier."
            }
        }
        require(verified.uploadOffset in 0..recording.sizeBytes) { "Server returned an unsafe recording offset." }
        val acceptedStatuses = if (recording.kind == "conversation") {
            setOf("uploading", "verified", "assembled", "failed")
        } else {
            setOf("uploading", "ready", "failed", "deleted")
        }
        require(verified.status in acceptedStatuses) {
            "Server returned an unknown recording state."
        }
        val readyStatuses = if (recording.kind == "conversation") {
            setOf("verified", "assembled")
        } else {
            setOf("ready")
        }
        require(!verified.acknowledged || verified.status in readyStatuses) {
            "Server acknowledged an incomplete recording."
        }
        require(verified.status !in readyStatuses || verified.acknowledged) {
            "Server returned ready without an acknowledgement."
        }
        return verified
    }

    fun nextAction(
        recording: PendingRecording,
        receipt: VerifiedRecordingReceipt,
        callInProgress: Boolean,
    ): RecordingUploadAction {
        if (callInProgress) return RecordingUploadAction.PauseForCall
        if (receipt.acknowledged) {
            if (recording.kind == "conversation" && recording.stopReason != "segment_boundary") {
                return RecordingUploadAction.FinalizeConversation
            }
            return RecordingUploadAction.AcknowledgeAndDelete
        }
        require(receipt.status == "uploading") { "Recording upload cannot continue from ${receipt.status}." }
        if (receipt.uploadOffset == recording.sizeBytes) return RecordingUploadAction.Complete
        return RecordingUploadAction.UploadChunk(
            receipt.uploadOffset,
            minOf(CHUNK_BYTES.toLong(), recording.sizeBytes - receipt.uploadOffset).toInt(),
        )
    }

    fun requireAdvanced(
        previous: RecordingUploadAction.UploadChunk,
        receipt: VerifiedRecordingReceipt,
    ) {
        require(receipt.uploadOffset == previous.offset + previous.byteCount) {
            "Server returned an unsafe recording offset."
        }
        require(!receipt.acknowledged) { "Chunk upload unexpectedly acknowledged completion." }
    }

    fun isAcknowledgedTerminalStatus(recording: PendingRecording, status: String): Boolean =
        status in if (recording.kind == "conversation") setOf("verified", "assembled") else setOf("ready")
}

object ConversationUploadPolicy {
    fun logicalCreateRequest(recording: PendingRecording): JSONObject {
        require(recording.kind == "conversation" && recording.segmentIndex == 0)
        return JSONObject()
            .put("recording_id", recording.recordingId)
            .put("call_id", recording.callId)
            .put("revision_id", recording.revisionId)
            .put("block_id", recording.blockId)
            .put("captured_at", recording.capturedAt)
    }

    fun verifyLogical(recording: PendingRecording, response: JSONObject): Boolean {
        require(recording.kind == "conversation")
        require(response.getString("id") == recording.recordingId)
        val status = response.getString("status")
        require(status in setOf("uploading", "ready"))
        val acknowledged = response.getBoolean("acknowledged")
        require(acknowledged == (status == "ready"))
        val segmentCount = response.getInt("segment_count").also { require(it >= 0) }
        response.getInt("duration_ms").also { require(it >= 0) }
        if (status == "uploading") {
            require(segmentCount in 0..(requireNotNull(recording.segmentIndex) + 1))
        } else {
            require(segmentCount >= requireNotNull(recording.segmentIndex) + 1)
        }
        return status == "ready"
    }

    fun verifyCompleted(recording: PendingRecording, response: JSONObject) {
        require(recording.kind == "conversation" && recording.stopReason != "segment_boundary")
        require(verifyLogical(recording, response))
        require(response.getInt("segment_count") == requireNotNull(recording.segmentIndex) + 1)
    }
}
