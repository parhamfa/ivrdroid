package ai.rx1.ivrdroid.control

import org.json.JSONObject
import java.time.Instant

internal data class ContinuousCapture(
    val kind: String, val id: String, val callId: String, val bootId: String,
    val revision: Long, val blockId: String, val policyVersion: Long,
    val wallMs: Long, val elapsedMs: Long, val pid: Int, val processStart: Long, val frames: Long,
    val version: Int = 1, val captureEndMs: Long = 0, val finalizedMs: Long = 0,
) {
    val sizeBytes: Long get() = frames * 4
    val durationMs: Long get() = frames * 1000 / 48000
    fun manifest(hash: String, reason: String, partial: Boolean): JSONObject {
        require(hash.matches(Regex("[a-f0-9]{64}")))
        require(reason in reasons && (reason in completeReasons || partial))
        require(frames > 0)
        return JSONObject().put("format_version", version).put("recording_id", id).put("call_id", callId)
            .put("kind", kind).put("revision_id", revision.takeIf { kind == "conversation" } ?: JSONObject.NULL)
            .put("block_id", blockId.takeIf { kind == "conversation" } ?: JSONObject.NULL)
            .put("policy_version", policyVersion.takeIf { kind == "session_audit" } ?: JSONObject.NULL)
            .put("captured_at", Instant.ofEpochMilli(wallMs).toString()).put("source_format", "pcm_s16le_48000_stereo")
            .put("frames", frames).put("duration_ms", durationMs).put("expected_size_bytes", sizeBytes)
            .put("source_sha256", hash).put("partial", partial).put("stop_reason", reason).also {
                if (version == 2) it.put("capture_receipt", JSONObject().put("version", 2).put("boot_id", bootId)
                    .put("started_elapsed_ms", elapsedMs).put("ended_elapsed_ms", captureEndMs)
                    .put("finalized_elapsed_ms", finalizedMs.takeIf { time -> time > 0 } ?: JSONObject.NULL))
            }
    }

    companion object {
        val completeReasons = setOf("session_complete", "caller_hangup", "operator_hangup", "max_call_duration", "completed")
        val reasons = completeReasons + setOf("preempted", "capture_failure", "storage_full", "interrupted", "writer_failure", "buffer_overrun", "recording_failure")
        fun parse(text: String): ContinuousCapture {
            require(text.length <= 1024 && text.endsWith('\n'))
            val p = text.trimEnd().split(' ')
            require((p.size == 13 && p[0] == "PCM1") || (p.size == 15 && p[0] == "PCM2"))
            val value = ContinuousCapture(p[1], p[2], p[3], p[4], p[5].toLong(), p[6], p[7].toLong(),
                p[8].toLong(), p[9].toLong(), p[10].toInt(), p[11].toLong(), p[12].toLong(),
                if (p[0] == "PCM2") 2 else 1, p.getOrNull(13)?.toLong() ?: 0, p.getOrNull(14)?.toLong() ?: 0)
            // Legacy checkpoints and versioned capture boundaries are both preserved.
            return value.validate()
        }
        private fun ContinuousCapture.validate(): ContinuousCapture {
            require(version == 1 || ((frames == 0L || captureEndMs >= elapsedMs) && (finalizedMs == 0L || finalizedMs >= captureEndMs)))
            require(kind in setOf("conversation", "session_audit"))
            listOf(id, callId, bootId).forEach(SessionAuditProtocol::canonicalId)
            require(wallMs > 0 && elapsedMs > 0 && pid > 0 && processStart > 0 && frames in 0..(48000L * 86400))
            require(if (kind == "conversation") revision > 0 && policyVersion == 0L && SessionAuditProtocol.canonicalId(blockId) == blockId
                else policyVersion > 0 && blockId == "-" && id == callId)
            return this
        }
    }
}
