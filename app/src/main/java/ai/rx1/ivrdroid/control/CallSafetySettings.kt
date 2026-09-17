package ai.rx1.ivrdroid.control

import android.content.Context
import ai.rx1.ivrdroid.BuildConfig
import ai.rx1.ivrdroid.audio.RootAudioTrigger
import ai.rx1.ivrdroid.telecom.CallRuntimeState
import net.i2p.crypto.eddsa.EdDSAEngine
import net.i2p.crypto.eddsa.EdDSAPublicKey
import net.i2p.crypto.eddsa.spec.EdDSANamedCurveTable
import net.i2p.crypto.eddsa.spec.EdDSAPublicKeySpec
import org.json.JSONObject
import java.io.File
import java.security.MessageDigest
import java.util.Base64

data class CallSafetyPolicy(val version: Long = 0, val maximumSeconds: Int = 3600) {
    init { require(version >= 0 && maximumSeconds in 60..86400 && maximumSeconds % 60 == 0) }
    fun bridgeText() = "version=$version\nmaximum_seconds=$maximumSeconds\n"
}

object CallSafetySettings {
    fun verify(envelope: JSONObject, keyB64: String = BuildConfig.CONFIG_SIGNING_PUBLIC_KEY_B64): CallSafetyPolicy {
        val document = envelope.getJSONObject("document")
        for (field in listOf("schema_version", "version", "maximum_call_duration_seconds")) {
            require(document.get(field) is Int || document.get(field) is Long) { "Call safety policy numbers must be integers." }
        }
        require(document.getString("kind") == "call_safety_policy" && document.getLong("schema_version") == 1L)
        val seconds = document.getLong("maximum_call_duration_seconds").also { require(it in 60..86400) }
        val policy = CallSafetyPolicy(document.getLong("version"), seconds.toInt())
        val canonical = CanonicalJson.encode(document).toByteArray(Charsets.UTF_8)
        require(MessageDigest.getInstance("SHA-256").digest(canonical).toHex() == envelope.getString("sha256"))
        val signature = Base64.getDecoder().decode(envelope.getString("signature_b64")).also { require(it.size == 64) }
        val key = Base64.getDecoder().decode(keyB64).also { require(it.size == 32) }
        val curve = EdDSANamedCurveTable.getByName("Ed25519")
        val verifier = EdDSAEngine(MessageDigest.getInstance(curve.hashAlgorithm))
        verifier.initVerify(EdDSAPublicKey(EdDSAPublicKeySpec(key, curve))); verifier.update(canonical)
        require(verifier.verify(signature)) { "Call safety policy signature is invalid." }
        return policy
    }
    private fun file(context: Context) = File(context.filesDir, "call-safety-policy.json")
    private fun appliedFile(context: Context) = File(context.filesDir, "call-safety-applied.json")
    fun verified(context: Context): CallSafetyPolicy? = runCatching {
        verify(JSONObject(String(SessionAuditFiles.read(file(context), 4096), Charsets.UTF_8)))
    }.getOrNull()
    fun capable(context: Context): Boolean = runCatching {
        String(SessionAuditFiles.read(File(context.filesDir, "bridge/call-safety-protocol"), 16), Charsets.US_ASCII) == "1\n"
    }.getOrDefault(false)
    fun applied(context: Context): CallSafetyPolicy? {
        if (!capable(context)) return null
        val state = runCatching { String(SessionAuditFiles.read(File(context.filesDir, "bridge/call-safety-state"), 160), Charsets.US_ASCII) }.getOrNull()
        val desired = verified(context)
        if (desired != null && state == desired.bridgeText()) {
            val envelope = SessionAuditFiles.read(file(context), 4096)
            val saved = runCatching { SessionAuditFiles.read(appliedFile(context), 4096) }.getOrNull()
            if (saved == null || !saved.contentEquals(envelope)) SessionAuditFiles.write(appliedFile(context), envelope)
            return desired
        }
        return runCatching { verify(JSONObject(String(SessionAuditFiles.read(appliedFile(context), 4096), Charsets.UTF_8))) }
            .getOrNull()?.takeIf { state == it.bridgeText() }
    }
    fun apply(context: Context, envelope: JSONObject) {
        val incoming = verify(envelope)
        verified(context)?.let { previous ->
            require(incoming.version >= previous.version && (incoming.version != previous.version || incoming == previous)) { "Call safety policy rollback or version reuse was rejected." }
        }
        applied(context) // Preserve the signed policy that the native guardian still uses.
        SessionAuditFiles.write(file(context), envelope.toString().toByteArray(Charsets.UTF_8))
        restore(context)
    }
    fun restore(context: Context) {
        if (CallRuntimeState.isBusy() || !RootAudioTrigger.isIdle(context)) return
        val policy = verified(context) ?: CallSafetyPolicy()
        SessionAuditFiles.write(File(context.filesDir, "bridge/call-safety-policy"), policy.bridgeText().toByteArray(Charsets.US_ASCII))
    }
}
