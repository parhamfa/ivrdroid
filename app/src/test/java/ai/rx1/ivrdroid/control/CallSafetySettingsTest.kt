package ai.rx1.ivrdroid.control

import net.i2p.crypto.eddsa.EdDSAEngine
import net.i2p.crypto.eddsa.EdDSAPrivateKey
import net.i2p.crypto.eddsa.spec.EdDSANamedCurveTable
import net.i2p.crypto.eddsa.spec.EdDSAPrivateKeySpec
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import java.security.MessageDigest
import java.util.Base64

class CallSafetySettingsTest {
    private val curve = EdDSANamedCurveTable.getByName("Ed25519")
    private val spec = EdDSAPrivateKeySpec(ByteArray(32) { it.toByte() }, curve)
    private val publicKey = Base64.getEncoder().encodeToString(spec.a.toByteArray())
    private fun document(seconds: Any = 3600) = JSONObject().put("kind", "call_safety_policy")
        .put("schema_version", 1).put("version", 0).put("maximum_call_duration_seconds", seconds)
    private fun signed(document: JSONObject): JSONObject {
        val bytes = CanonicalJson.encode(document).toByteArray(Charsets.UTF_8)
        val signer = EdDSAEngine(MessageDigest.getInstance(curve.hashAlgorithm)).apply {
            initSign(EdDSAPrivateKey(spec)); update(bytes)
        }
        return JSONObject().put("document", document)
            .put("sha256", MessageDigest.getInstance("SHA-256").digest(bytes).toHex())
            .put("signature_b64", Base64.getEncoder().encodeToString(signer.sign()))
    }

    @Test fun defaultAndBoundsRequireSignedIntegerMinutes() {
        assertEquals(CallSafetyPolicy(0, 3600), CallSafetySettings.verify(signed(document()), publicKey))
        for (seconds in listOf(60, 86400)) {
            assertEquals(seconds, CallSafetySettings.verify(signed(document(seconds)), publicKey).maximumSeconds)
        }
        for (seconds in listOf(0, 59, 61, 86401, 4294970896L, "3600", 3600.5)) {
            assertThrows(Exception::class.java) { CallSafetySettings.verify(signed(document(seconds)), publicKey) }
        }
    }

    @Test fun tamperingWrongSignerWrongKindAndNegativeVersionAreRejected() {
        val changed = signed(document())
        changed.getJSONObject("document").put("maximum_call_duration_seconds", 60)
        assertThrows(IllegalArgumentException::class.java) { CallSafetySettings.verify(changed, publicKey) }
        assertThrows(Exception::class.java) { CallSafetySettings.verify(signed(document()), Base64.getEncoder().encodeToString(ByteArray(32))) }
        for (document in listOf(document().put("version", -1), document().put("kind", "session_audit_policy"), document().put("schema_version", 2))) {
            assertThrows(IllegalArgumentException::class.java) { CallSafetySettings.verify(signed(document), publicKey) }
        }
    }
}
