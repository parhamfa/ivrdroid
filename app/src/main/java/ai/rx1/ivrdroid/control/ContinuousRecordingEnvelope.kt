package ai.rx1.ivrdroid.control

import java.io.Closeable
import java.io.DataOutputStream
import java.io.DataOutput
import java.io.File
import java.io.InputStream
import java.io.OutputStream
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.security.MessageDigest
import javax.crypto.Cipher
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/** A single seekable file. Crypto records are bounded, and never rotate the audio file.
 * Every record authenticates recording identity, total size, position and length. A
 * mandatory authenticated digest footer makes truncation distinguishable from completion.
 * IVRSPL1 remains available through RecordingEnvelope for legacy spooled recordings.
 */
internal object ContinuousRecordingEnvelope {
    private val magic = "IVRSPL2\u0000".toByteArray(Charsets.US_ASCII)
    const val BLOCK_BYTES = 64 * 1024
    const val MAX_READ_BYTES = 1024 * 1024
    private const val HEADER_BYTES = 20L
    private const val RECORD_OVERHEAD = 32L // length, nonce, authentication tag
    private const val FOOTER_BYTES = 64L // record overhead and SHA-256
    private const val MAX_BYTES = 192_000L * 86_400 // 24 hours of 48 kHz stereo PCM16

    data class Summary(val size: Long, val sha256: String)

    fun storedSize(size: Long): Long {
        require(size in 0..MAX_BYTES)
        return HEADER_BYTES + size + ((size + BLOCK_BYTES - 1) / BLOCK_BYTES) * RECORD_OVERHEAD + FOOTER_BYTES
    }

    private fun aad(identity: String, size: Long, offset: Long, length: Int, footer: Boolean): ByteArray {
        require(identity.length in 1..160 && identity.all { it.code in 33..126 })
        return "IVRSPL2\n$identity\n$BLOCK_BYTES\n$size\n$offset\n$length\n${if (footer) "digest" else "audio"}".toByteArray(Charsets.US_ASCII)
    }

    private fun writeRecord(key: SecretKey, identity: String, size: Long, offset: Long,
                            bytes: ByteArray, length: Int, output: DataOutput, footer: Boolean = false) {
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, key)
        require(cipher.iv.size == 12)
        cipher.updateAAD(aad(identity, size, offset, length, footer))
        output.writeInt(if (footer) 0 else length)
        output.write(cipher.iv)
        output.write(cipher.doFinal(bytes, 0, length))
    }

    fun encrypt(key: SecretKey, identity: String, size: Long, input: InputStream, output: OutputStream,
                checkpoint: () -> Unit = {}): Summary {
        storedSize(size)
        val target = DataOutputStream(output)
        target.write(magic); target.writeInt(BLOCK_BYTES); target.writeLong(size)
        val digest = MessageDigest.getInstance("SHA-256")
        val buffer = ByteArray(BLOCK_BYTES)
        var offset = 0L
        while (offset < size) {
            checkpoint()
            val length = minOf(BLOCK_BYTES.toLong(), size - offset).toInt()
            var read = 0
            while (read < length) {
                val count = input.read(buffer, read, length - read)
                require(count > 0) { "Plaintext ended before its committed length." }
                read += count
            }
            digest.update(buffer, 0, length)
            writeRecord(key, identity, size, offset, buffer, length, target)
            offset += length
        }
        require(input.read() == -1) { "Plaintext changed during encryption." }
        val hash = digest.digest()
        writeRecord(key, identity, size, size, hash, hash.size, target, footer = true)
        target.flush()
        return Summary(size, hex(hash))
    }

    /** Retain authenticated records across calls/restarts. A torn final record is
     * regenerated from the retained PCM, using a fresh nonce. No digest state or
     * plaintext is trusted from a partially written encrypted file. */
    fun encryptResuming(key: SecretKey, identity: String, size: Long, source: File, target: File,
                        checkpoint: () -> Unit = {}): Summary {
        storedSize(size)
        require(source.length() == size)
        val header = ByteBuffer.allocate(HEADER_BYTES.toInt()).put(magic).putInt(BLOCK_BYTES).putLong(size).array()
        RandomAccessFile(source, "r").use { input ->
            RandomAccessFile(target, "rw").use { output ->
                if (output.length() >= HEADER_BYTES) {
                    val existing = ByteArray(header.size); output.readFully(existing)
                    if (!MessageDigest.isEqual(header, existing)) output.setLength(0)
                } else output.setLength(0)
                if (output.length() == 0L) { output.seek(0); output.write(header) }
                output.seek(HEADER_BYTES)
                val digest = MessageDigest.getInstance("SHA-256")
                val buffer = ByteArray(BLOCK_BYTES)
                var offset = 0L
                var syncedAt = System.nanoTime()
                try {
                    while (offset < size) {
                        checkpoint()
                        val length = minOf(BLOCK_BYTES.toLong(), size - offset).toInt()
                        input.readFully(buffer, 0, length)
                        val position = output.filePointer
                        val existing = if (output.length() - position >= length + RECORD_OVERHEAD) runCatching {
                            require(output.readInt() == length)
                            val nonce = ByteArray(12); output.readFully(nonce)
                            val ciphertext = ByteArray(length + 16); output.readFully(ciphertext)
                            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
                            cipher.init(Cipher.DECRYPT_MODE, key, GCMParameterSpec(128, nonce))
                            cipher.updateAAD(aad(identity, size, offset, length, false))
                            cipher.doFinal(ciphertext)
                        }.getOrNull() else null
                        if (existing == null) {
                            output.setLength(position); output.seek(position)
                            writeRecord(key, identity, size, offset, buffer, length, output)
                        } else {
                            require(existing.indices.all { existing[it] == buffer[it] }) { "Committed plaintext changed during encryption recovery." }
                        }
                        digest.update(buffer, 0, length); offset += length
                        if (System.nanoTime() - syncedAt >= 1_000_000_000L) {
                            output.fd.sync(); syncedAt = System.nanoTime()
                        }
                    }
                    require(input.read() == -1) { "Plaintext changed during encryption." }
                    val hash = digest.digest()
                    writeRecord(key, identity, size, size, hash, hash.size, output, footer = true)
                    output.setLength(output.filePointer)
                    return Summary(size, hex(hash))
                } finally { output.fd.sync() }
            }
        }
    }

    class Reader(private val key: SecretKey, private val identity: String, file: File) : Closeable {
        private val input = RandomAccessFile(file, "r")
        val size: Long
        val sha256: String
        init {
            try {
                val header = ByteArray(HEADER_BYTES.toInt()); input.readFully(header)
                require(MessageDigest.isEqual(header.copyOfRange(0, 8), magic)) { "Unknown recording format." }
                val fields = ByteBuffer.wrap(header, 8, 12)
                require(fields.int == BLOCK_BYTES)
                size = fields.long
                require(input.length() == storedSize(size)) { "Recording is truncated or has trailing data." }
                input.seek(input.length() - FOOTER_BYTES)
                sha256 = hex(readRecord(size, 32, footer = true))
            } catch (error: Throwable) { input.close(); throw error }
        }

        private fun readRecord(offset: Long, length: Int, footer: Boolean = false): ByteArray {
            require(input.readInt() == if (footer) 0 else length) { "Recording record length changed." }
            val nonce = ByteArray(12); input.readFully(nonce)
            val ciphertext = ByteArray(length + 16); input.readFully(ciphertext)
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(Cipher.DECRYPT_MODE, key, GCMParameterSpec(128, nonce))
            cipher.updateAAD(aad(identity, size, offset, length, footer))
            return cipher.doFinal(ciphertext)
        }

        /** Resume at any byte offset, allocating at most one transport buffer and one crypto record. */
        fun read(offset: Long, length: Int): ByteArray {
            require(offset in 0..size && length in 0..MAX_READ_BYTES && length.toLong() <= size - offset)
            val output = ByteArray(length)
            var written = 0
            while (written < length) {
                val position = offset + written
                val block = position / BLOCK_BYTES
                val base = block * BLOCK_BYTES
                val count = minOf(BLOCK_BYTES.toLong(), size - base).toInt()
                input.seek(HEADER_BYTES + block * (BLOCK_BYTES + RECORD_OVERHEAD))
                val plain = readRecord(base, count)
                val start = (position - base).toInt()
                val copy = minOf(count - start, length - written)
                plain.copyInto(output, written, start, start + copy)
                written += copy
            }
            return output
        }

        fun verify(checkpoint: () -> Unit = {}): Summary {
            val digest = MessageDigest.getInstance("SHA-256")
            var offset = 0L
            while (offset < size) {
                checkpoint()
                val bytes = read(offset, minOf(BLOCK_BYTES.toLong(), size - offset).toInt())
                digest.update(bytes); offset += bytes.size
            }
            require(hex(digest.digest()) == sha256) { "Recording checksum changed." }
            return Summary(size, sha256)
        }

        override fun close() = input.close()
    }

    private fun hex(bytes: ByteArray): String = bytes.joinToString("") { "%02x".format(it.toInt() and 255) }
}
