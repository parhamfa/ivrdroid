package ai.rx1.ivrdroid.control

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Test
import javax.crypto.spec.SecretKeySpec
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream

class RecordingEnvelopeTest {
    private val key = SecretKeySpec(ByteArray(32) { it.toByte() }, "AES")
    private val audio = "caller speech that must remain private".toByteArray()

    @Test
    fun streamingEncryptionIsCompatibleAcrossBufferBoundaries() {
        val nonce = ByteArray(12) { (it + 1).toByte() }
        for (length in listOf(1, 65535, 65536, 65537, 3 * 65536 + 11)) {
            val samples = ByteArray(length) { (it % 251).toByte() }
            val output = ByteArrayOutputStream()
            RecordingEnvelope.encryptStream(key, "recording-id", ByteArrayInputStream(samples), output, nonce)
            assertArrayEquals(RecordingEnvelope.encrypt(key, "recording-id", samples, nonce), output.toByteArray())
            assertArrayEquals(samples, RecordingEnvelope.decrypt(key, "recording-id", output.toByteArray()))
        }
    }

    @Test
    fun encryptedSpoolEnvelopeRecoversAfterRestartWithoutExposingPlaintext() {
        val encrypted = RecordingEnvelope.encrypt(
            key,
            "11111111-1111-4111-8111-111111111111",
            audio,
            ByteArray(12) { (it + 1).toByte() },
        )
        assertFalse(encrypted.toList().windowed(audio.size).any { it.toByteArray().contentEquals(audio) })
        assertArrayEquals(
            audio,
            RecordingEnvelope.decrypt(
                key,
                "11111111-1111-4111-8111-111111111111",
                encrypted.copyOf(),
            ),
        )
    }

    @Test
    fun tamperingWrongCorrelationOrWrongKeyCannotRecoverSpoolAudio() {
        val encrypted = RecordingEnvelope.encrypt(key, "recording-id", audio)
        encrypted[encrypted.lastIndex] = (encrypted.last().toInt() xor 1).toByte()
        assertThrows(Exception::class.java) {
            RecordingEnvelope.decrypt(key, "recording-id", encrypted)
        }

        val intact = RecordingEnvelope.encrypt(key, "recording-id", audio)
        assertThrows(Exception::class.java) {
            RecordingEnvelope.decrypt(key, "another-recording", intact)
        }
        assertThrows(Exception::class.java) {
            RecordingEnvelope.decrypt(SecretKeySpec(ByteArray(32) { 7 }, "AES"), "recording-id", intact)
        }
    }
}
