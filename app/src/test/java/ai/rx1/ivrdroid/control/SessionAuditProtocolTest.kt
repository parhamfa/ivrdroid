package ai.rx1.ivrdroid.control

import net.i2p.crypto.eddsa.EdDSAEngine
import net.i2p.crypto.eddsa.EdDSAPrivateKey
import net.i2p.crypto.eddsa.spec.EdDSANamedCurveTable
import net.i2p.crypto.eddsa.spec.EdDSAPrivateKeySpec
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import java.security.MessageDigest
import java.util.Base64
import javax.crypto.spec.SecretKeySpec

class SessionAuditProtocolTest {
    private val id = "23d4b16c-f15b-4de4-9444-d08265e194ab"
    private val time = "2026-09-12T12:00:00.000Z"
    private fun report() = JSONObject().put("recording_id", id).put("policy_version", 1)
        .put("state", "pending_upload").put("captured_at", time).put("duration_ms", 15000)
        .put("partial", false).put("stop_reason", "caller_hangup").put("events", JSONArray()
            .put(JSONObject().put("offset_ms", 0).put("type", "answered").put("block_id", JSONObject.NULL).put("detail", "")))
    private fun segment() = JSONObject().put("kind", "session_audit").put("recording_id", id).put("call_id", id)
        .put("policy_version", 1).put("segment_index", 0).put("captured_at", time).put("duration_ms", 15000)
        .put("stop_reason", "segment_boundary").put("partial", false).put("size_bytes", 44 + 15000 * 192)
        .put("sha256", "a".repeat(64))
    private fun rejects(action: () -> Unit) { assertThrows(IllegalArgumentException::class.java) { action() } }

    @Test fun signedPolicyIsIndependentAndTamperingFails() {
        val parameters = EdDSANamedCurveTable.getByName("Ed25519")
        val spec = EdDSAPrivateKeySpec(ByteArray(32) { it.toByte() }, parameters)
        val key = EdDSAPrivateKey(spec)
        val document = JSONObject().put("kind", "session_audit_policy").put("version", 1).put("enabled", true).put("local_quota_bytes", 1L shl 30)
        val bytes = CanonicalJson.encode(document).toByteArray(Charsets.UTF_8)
        val signer = EdDSAEngine(MessageDigest.getInstance(parameters.hashAlgorithm)).apply { initSign(key); update(bytes) }
        val envelope = JSONObject().put("document", document).put("sha256", MessageDigest.getInstance("SHA-256").digest(bytes).toHex())
            .put("signature_b64", Base64.getEncoder().encodeToString(signer.sign()))
        val publicKey = Base64.getEncoder().encodeToString(spec.a.toByteArray())
        assertEquals("version=1\nenabled=1\nquota=1073741824\n", SessionAuditProtocol.verifyPolicy(envelope, publicKey).bridgeText())
        document.put("enabled", false)
        rejects { SessionAuditProtocol.verifyPolicy(envelope, publicKey) }
    }

    @Test fun exactBoundaryGetsItsTerminalReasonOnlyAfterTheReport() {
        val source = segment()
        SessionAuditProtocol.validateSegment(id, 0, source)
        val request = SessionAuditProtocol.segmentRequest(source, report(), true)
        assertEquals("caller_hangup", request.getString("stop_reason"))
        assertEquals("segment_boundary", source.getString("stop_reason"))
        assertEquals(44 + 15000 * 192, request.getInt("expected_size_bytes"))
        assertFalse(request.has("size_bytes"))
    }

    @Test fun coverageIdentityAndTimelineAreStrict() {
        SessionAuditProtocol.validateReport(id, report())
        rejects { SessionAuditProtocol.validateSegment(id, 1, segment()) }
        rejects { SessionAuditProtocol.validateSegment(id, 0, segment().put("duration_ms", 16000)) }
        rejects { SessionAuditProtocol.validateReport(id, report().put("partial", true)) }
        rejects { SessionAuditProtocol.validateReport(id, report().put("events", JSONArray().put(JSONObject()
            .put("type", "digit").put("offset_ms", 16001).put("detail", "1")))) }
        rejects { SessionAuditProtocol.validateReport(id, report().put("events", JSONArray().put(JSONObject()
            .put("type", "external_call").put("offset_ms", 10).put("detail", "+989123456789")))) }
    }

    @Test fun receiptMustAcknowledgeTheExactSegmentHashSizeAndOffset() {
        val metadata = segment()
        val receipt = JSONObject().put("id", id).put("segment_index", 0).put("source_sha256", "a".repeat(64))
            .put("expected_size_bytes", metadata.getLong("size_bytes")).put("upload_offset", 1024)
            .put("status", "uploading").put("acknowledged", false)
        assertEquals(1024L, SessionAuditProtocol.verifyUpload(id, 0, metadata, receipt))
        rejects { SessionAuditProtocol.verifyUpload(id, 0, metadata, JSONObject(receipt.toString()).put("acknowledged", true)) }
        rejects { SessionAuditProtocol.verifyUpload(id, 0, metadata, JSONObject(receipt.toString()).put("source_sha256", "b".repeat(64))) }
    }

    @Test fun encryptionCannotBeReassignedAcrossCallsOrRecordingKinds() {
        val key = SecretKeySpec(ByteArray(32) { it.toByte() }, "AES")
        val aad = "audit:$id:00000.audio"
        val bytes = RecordingEnvelope.encrypt(key, aad, ByteArray(128) { 42 }, ByteArray(12))
        assertArrayEquals(ByteArray(128) { 42 }, RecordingEnvelope.decrypt(key, aad, bytes))
        assertThrows(Exception::class.java) { RecordingEnvelope.decrypt(key, "$id.00000", bytes) }
        bytes[bytes.lastIndex] = (bytes.last().toInt() xor 1).toByte()
        assertThrows(Exception::class.java) { RecordingEnvelope.decrypt(key, aad, bytes) }
    }
}
