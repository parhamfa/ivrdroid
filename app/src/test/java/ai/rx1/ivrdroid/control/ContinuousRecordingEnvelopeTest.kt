package ai.rx1.ivrdroid.control

import java.io.ByteArrayInputStream
import java.io.File
import java.io.RandomAccessFile
import javax.crypto.spec.SecretKeySpec
import org.junit.Assert.*
import org.junit.Test

class ContinuousRecordingEnvelopeTest {
    private val key = SecretKeySpec(ByteArray(32) { it.toByte() }, "AES")
    private val identity = "conversation:2b2d5320-9b3c-4907-9d94-f80eb4f10914"
    @Test fun increasingAudioSizesUseBoundedReadsAndEncryptionBuffers() {
        for (size in listOf(8L, 64L, 512L).map { it * 1024 * 1024 }) {
            val file = File.createTempFile("continuous-memory-", ".enc")
            val source = File.createTempFile("continuous-memory-", ".pcm")
            RandomAccessFile(source, "rw").use { it.setLength(size) }
            var peak = 0L
            val sample = { peak = maxOf(peak, Runtime.getRuntime().totalMemory() - Runtime.getRuntime().freeMemory()); Unit }
            try {
                ContinuousRecordingEnvelope.encryptResuming(key, identity, size, source, file, sample)
                ContinuousRecordingEnvelope.Reader(key, identity, file).use { assertEquals(size, it.verify(sample).size) }
                println("Continuous envelope: source_bytes=$size peak_heap_bytes=$peak")
            } finally { file.delete(); source.delete() }
        }
    }
    private fun recording(bytes: ByteArray): File = File.createTempFile("continuous-", ".enc").also { file ->
        file.deleteOnExit()
        file.outputStream().use { ContinuousRecordingEnvelope.encrypt(key, identity, bytes.size.toLong(), ByteArrayInputStream(bytes), it) }
    }

    @Test fun arbitraryResumeAndBoundaries() {
        for (size in listOf(0, 1, 65535, 65536, 65537, 2 * 1024 * 1024 + 17)) {
            val bytes = ByteArray(size) { (it * 31).toByte() }
            val file = recording(bytes)
            ContinuousRecordingEnvelope.Reader(key, identity, file).use { reader ->
                assertEquals(size.toLong(), reader.verify().size)
                assertEquals(ContinuousRecordingEnvelope.storedSize(size.toLong()), file.length())
                if (size > 65537) assertArrayEquals(bytes.copyOfRange(65531, 65537), reader.read(65531, 6))
                assertArrayEquals(bytes.takeLast(minOf(17, size)).toByteArray(), reader.read((size - minOf(17, size)).toLong(), minOf(17, size)))
            }
            file.delete()
        }
    }

    @Test fun truncationIdentityAndContentTamperingAreRejected() {
        val file = recording(ByteArray(200000) { it.toByte() })
        assertThrows(Exception::class.java) { ContinuousRecordingEnvelope.Reader(key, "conversation:other", file).close() }
        RandomAccessFile(file, "rw").use { it.seek(40); val byte = it.readByte(); it.seek(40); it.writeByte(byte.toInt() xor 1) }
        assertThrows(Exception::class.java) { ContinuousRecordingEnvelope.Reader(key, identity, file).use { it.verify() } }
        RandomAccessFile(file, "rw").use { it.setLength(file.length() - 1) }
        assertThrows(Exception::class.java) { ContinuousRecordingEnvelope.Reader(key, identity, file).close() }
        file.delete()
    }

    @Test fun swappingAuthenticatedRecordsIsRejected() {
        val file = recording(ByteArray(3 * 65536) { (it / 65536).toByte() })
        RandomAccessFile(file, "rw").use {
            val first = ByteArray(65568); val second = ByteArray(65568)
            it.seek(20); it.readFully(first); it.readFully(second)
            it.seek(20); it.write(second); it.write(first)
        }
        assertThrows(Exception::class.java) { ContinuousRecordingEnvelope.Reader(key, identity, file).use { it.verify() } }
        file.delete()
    }

    @Test fun interruptedEncryptionCannotBeMistakenForACompleteFile() {
        val file = File.createTempFile("interrupted-", ".enc")
        var count = 0
        assertThrows(IllegalStateException::class.java) {
            file.outputStream().use {
                ContinuousRecordingEnvelope.encrypt(key, identity, 200000, ByteArrayInputStream(ByteArray(200000)), it) {
                    if (++count == 2) error("New incoming call")
                }
            }
        }
        assertThrows(Exception::class.java) { ContinuousRecordingEnvelope.Reader(key, identity, file).close() }
        file.delete()
    }

    @Test fun interruptedEncryptionResumesAuthenticatedPrefixAndRepairsTornTail() {
        val source = File.createTempFile("resume-pcm-", ".pcm")
        val target = File.createTempFile("resume-encrypted-", ".enc")
        val bytes = ByteArray(300000) { (it * 17).toByte() }
        source.writeBytes(bytes)
        try {
            var checks = 0
            assertThrows(IllegalStateException::class.java) {
                ContinuousRecordingEnvelope.encryptResuming(key, identity, source.length(), source, target) {
                    if (++checks == 3) error("Incoming call")
                }
            }
            val firstRecord = target.inputStream().use { it.readNBytes(20 + 65568) }
            RandomAccessFile(target, "rw").use { it.setLength(it.length() - 8) }
            ContinuousRecordingEnvelope.encryptResuming(key, identity, source.length(), source, target)
            assertArrayEquals(firstRecord, target.inputStream().use { it.readNBytes(firstRecord.size) })
            ContinuousRecordingEnvelope.Reader(key, identity, target).use {
                assertEquals(source.length(), it.verify().size)
                assertArrayEquals(bytes, it.read(0, bytes.size))
            }
            // A same-size changed source cannot silently replace committed audio.
            RandomAccessFile(source, "rw").use { it.writeByte(123) }
            assertThrows(IllegalArgumentException::class.java) {
                ContinuousRecordingEnvelope.encryptResuming(key, identity, source.length(), source, target)
            }
        } finally { source.delete(); target.delete() }
    }
}
