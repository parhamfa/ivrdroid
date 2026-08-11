package ai.rx1.ivrdroid.control

import java.nio.charset.StandardCharsets
import java.security.MessageDigest
import javax.crypto.Cipher
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

internal object RecordingEnvelope {
    private val magic = "IVRSPL1\u0000".toByteArray(StandardCharsets.US_ASCII)
    private const val NONCE_BYTES = 12
    private const val TAG_BYTES = 16
    const val OVERHEAD_BYTES = 8L + NONCE_BYTES + TAG_BYTES

    fun encrypt(
        key: SecretKey,
        aad: String,
        plaintext: ByteArray,
        nonce: ByteArray? = null,
    ): ByteArray {
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        if (nonce == null) {
            cipher.init(Cipher.ENCRYPT_MODE, key)
        } else {
            require(nonce.size == NONCE_BYTES)
            cipher.init(Cipher.ENCRYPT_MODE, key, GCMParameterSpec(TAG_BYTES * 8, nonce))
        }
        cipher.updateAAD(aad.toByteArray(StandardCharsets.US_ASCII))
        val actualNonce = cipher.iv.also { require(it.size == NONCE_BYTES) }
        return magic + actualNonce + cipher.doFinal(plaintext)
    }

    fun decrypt(key: SecretKey, aad: String, envelope: ByteArray): ByteArray {
        require(envelope.size > OVERHEAD_BYTES)
        val prefix = envelope.copyOfRange(0, magic.size)
        require(MessageDigest.isEqual(prefix, magic))
        val nonce = envelope.copyOfRange(magic.size, magic.size + NONCE_BYTES)
        val ciphertext = envelope.copyOfRange(magic.size + NONCE_BYTES, envelope.size)
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.DECRYPT_MODE, key, GCMParameterSpec(TAG_BYTES * 8, nonce))
        cipher.updateAAD(aad.toByteArray(StandardCharsets.US_ASCII))
        return cipher.doFinal(ciphertext)
    }
}
