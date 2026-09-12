package ai.rx1.ivrdroid.control

import ai.rx1.ivrdroid.BuildConfig
import net.i2p.crypto.eddsa.EdDSAEngine
import net.i2p.crypto.eddsa.EdDSAPublicKey
import net.i2p.crypto.eddsa.spec.EdDSANamedCurveTable
import net.i2p.crypto.eddsa.spec.EdDSAPublicKeySpec
import org.json.JSONArray
import org.json.JSONObject
import java.security.MessageDigest
import java.time.Instant
import java.util.Base64
import java.util.UUID

data class SessionAuditPolicy(val version: Long = 0, val enabled: Boolean = false, val quotaBytes: Long = 1L shl 30) {
    fun bridgeText(): String = "version=$version\nenabled=${if (enabled) 1 else 0}\nquota=$quotaBytes\n"
}

/** This policy has its own signature and version sequence, independent of flow publication. */
object SessionAuditProtocol {
    val terminalReasons = setOf("session_complete", "caller_hangup", "preempted", "capture_failure", "storage_full", "interrupted", "writer_failure")
    private val eventTypes = setOf("answered", "prompt", "digit", "timeout", "invalid", "schedule", "return", "voicemail", "external_call", "ended", "gap")

    fun verifyPolicy(envelope: JSONObject, keyB64: String = BuildConfig.CONFIG_SIGNING_PUBLIC_KEY_B64): SessionAuditPolicy {
        val document = envelope.getJSONObject("document")
        require(document.getString("kind") == "session_audit_policy")
        val policy = SessionAuditPolicy(document.getLong("version"), document.getBoolean("enabled"), document.getLong("local_quota_bytes"))
        require(policy.version >= 0 && (!policy.enabled || policy.version > 0))
        require(policy.quotaBytes in (64L shl 20)..(4L shl 30))
        val canonical = CanonicalJson.encode(document).toByteArray(Charsets.UTF_8)
        require(MessageDigest.getInstance("SHA-256").digest(canonical).toHex() == envelope.getString("sha256"))
        val signature = Base64.getDecoder().decode(envelope.getString("signature_b64")).also { require(it.size == 64) }
        val publicKey = Base64.getDecoder().decode(keyB64).also { require(it.size == 32) }
        val parameters = EdDSANamedCurveTable.getByName("Ed25519")
        val verifier = EdDSAEngine(MessageDigest.getInstance(parameters.hashAlgorithm))
        verifier.initVerify(EdDSAPublicKey(EdDSAPublicKeySpec(publicKey, parameters)))
        verifier.update(canonical)
        require(verifier.verify(signature)) { "Audit policy signature is invalid." }
        return policy
    }

    fun canonicalId(value: String): String = value.also { require(UUID.fromString(it).toString() == it) }

    fun validateReport(callId: String, report: JSONObject): JSONObject {
        canonicalId(callId)
        require(report.getString("recording_id") == callId)
        require(report.getLong("policy_version") > 0)
        val duration = report.getLong("duration_ms").also { require(it in 0..86_400_000) }
        val state = report.getString("state").also { require(it in setOf("pending_upload", "unavailable")) }
        require(state != "pending_upload" || duration > 0)
        if (!report.isNull("captured_at")) Instant.parse(report.getString("captured_at"))
        else require(state == "unavailable")
        val reason = report.getString("stop_reason").also { require(it in terminalReasons) }
        require(report.getBoolean("partial") == (reason !in setOf("session_complete", "caller_hangup")))
        val events = report.getJSONArray("events").also { require(it.length() <= 4096) }
        for (index in 0 until events.length()) {
            val event = events.getJSONObject(index)
            require(event.getLong("offset_ms") in 0..(duration + 1000))
            require(event.getString("type") in eventTypes)
            if (!event.isNull("block_id")) canonicalId(event.getString("block_id"))
            val detail = event.optString("detail")
            require(detail.length <= 80 && detail.matches(Regex("[A-Za-z0-9_ .:#/*-]*")) && detail.count(Char::isDigit) < 8)
        }
        // Older helpers appended the coverage gap after a slightly later disconnect.
        // Preserve every observed timestamp, while normalizing local receipts for upload.
        val ordered = (0 until events.length()).map(events::getJSONObject).sortedBy { it.getLong("offset_ms") }
        return report.put("events", JSONArray(ordered))
    }

    fun validateSegment(callId: String, index: Int, metadata: JSONObject): JSONObject {
        canonicalId(callId)
        require(metadata.getString("kind") == "session_audit" && metadata.getString("call_id") == callId && metadata.getString("recording_id") == callId)
        require(metadata.getLong("policy_version") > 0 && index in 0..5759 && metadata.getInt("segment_index") == index)
        Instant.parse(metadata.getString("captured_at"))
        val duration = metadata.getLong("duration_ms").also { require(it in 1..15_000) }
        require(metadata.getLong("size_bytes") == 44 + duration * 192)
        require(metadata.getString("sha256").matches(Regex("[0-9a-f]{64}")))
        val reason = metadata.getString("stop_reason")
        require(reason in terminalReasons || reason == "segment_boundary")
        require(reason != "segment_boundary" || duration == 15_000L)
        require(metadata.getBoolean("partial") == (reason !in setOf("session_complete", "caller_hangup", "segment_boundary")))
        return metadata
    }

    fun segmentRequest(metadata: JSONObject, report: JSONObject, last: Boolean): JSONObject = JSONObject(metadata.toString()).apply {
        if (last) {
            put("stop_reason", report.getString("stop_reason"))
            put("partial", report.getBoolean("partial"))
        }
        put("expected_size_bytes", getLong("size_bytes")); remove("size_bytes")
        put("source_sha256", getString("sha256")); remove("sha256")
    }

    fun verifyUpload(callId: String, index: Int, metadata: JSONObject, response: JSONObject): Long {
        require(response.getString("id") == callId && response.getInt("segment_index") == index)
        require(response.getLong("expected_size_bytes") == metadata.getLong("size_bytes"))
        require(response.getString("source_sha256") == metadata.getString("sha256"))
        val offset = response.getLong("upload_offset").also { require(it in 0..metadata.getLong("size_bytes")) }
        val status = response.getString("status").also { require(it in setOf("uploading", "verified", "assembled")) }
        require(response.getBoolean("acknowledged") == (status in setOf("verified", "assembled")))
        require(!response.getBoolean("acknowledged") || offset == metadata.getLong("size_bytes"))
        return offset
    }
}
