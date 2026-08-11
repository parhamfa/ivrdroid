package ai.rx1.ivrdroid.control

import ai.rx1.ivrdroid.BuildConfig
import net.i2p.crypto.eddsa.EdDSAEngine
import net.i2p.crypto.eddsa.EdDSAPublicKey
import net.i2p.crypto.eddsa.spec.EdDSANamedCurveTable
import net.i2p.crypto.eddsa.spec.EdDSAPublicKeySpec
import org.json.JSONObject
import java.security.MessageDigest
import java.util.Base64

object RevisionVerifier {
    data class VerifiedRevision(
        val manifest: JSONObject,
        val revisionId: Long,
        val manifestSha256: String,
    )

    fun verify(
        response: JSONObject,
        expectedRevisionId: Long,
        expectedManifestSha256: String,
        expectedSignatureB64: String,
        signingPublicKeyB64: String = BuildConfig.CONFIG_SIGNING_PUBLIC_KEY_B64,
    ): VerifiedRevision {
        require(expectedRevisionId > 0) { "Revision identifier is invalid." }
        require(expectedManifestSha256.matches(Regex("[0-9a-f]{64}"))) { "Manifest hash is invalid." }
        val manifest = response.getJSONObject("manifest")
        require(response.getString("manifest_sha256") == expectedManifestSha256) { "Manifest hash changed during download." }
        require(response.getString("signature_b64") == expectedSignatureB64) { "Manifest signature changed during download." }
        require(manifest.getInt("schema_version") in setOf(1, 2, 3, 4)) { "Unsupported configuration schema." }
        require(manifest.getLong("revision_id") == expectedRevisionId) { "Revision identifier mismatch." }

        val canonical = CanonicalJson.encode(manifest).toByteArray(Charsets.UTF_8)
        val digest = MessageDigest.getInstance("SHA-256").digest(canonical).toHex()
        require(digest == expectedManifestSha256) { "Manifest digest verification failed." }
        val signature = Base64.getDecoder().decode(expectedSignatureB64)
        require(signature.size == 64) { "Manifest signature length is invalid." }
        val publicKey = Base64.getDecoder().decode(signingPublicKeyB64)
        require(publicKey.size == 32) { "Compiled verification key is invalid." }
        val parameters = EdDSANamedCurveTable.getByName("Ed25519")
        val verifier = EdDSAEngine(MessageDigest.getInstance(parameters.hashAlgorithm))
        verifier.initVerify(EdDSAPublicKey(EdDSAPublicKeySpec(publicKey, parameters)))
        verifier.update(canonical)
        require(verifier.verify(signature)) { "Manifest signature verification failed." }
        return VerifiedRevision(manifest, expectedRevisionId, expectedManifestSha256)
    }
}

internal fun ByteArray.toHex(): String = joinToString("") { "%02x".format(it) }
