package ai.rx1.ivrdroid.control

import android.content.Context
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.io.RandomAccessFile
import java.security.MessageDigest

object RevisionFiles {
    private const val MAXIMUM_CONFIG_BYTES = 8 * 1024 * 1024
    private const val MAXIMUM_PROMPT_BYTES = 64L * 1024L * 1024L

    fun prepare(
        context: Context,
        revision: RevisionVerifier.VerifiedRevision,
        api: DeviceApi,
    ) {
        val compiled = RevisionCompiler.compile(revision.manifest, revision.manifestSha256)
        val parent = File(context.filesDir, "revisions").apply { mkdirs() }
        val finalDirectory = File(parent, revision.revisionId.toString())
        if (finalDirectory.exists()) {
            require(validateExisting(finalDirectory, revision, compiled)) {
                "Immutable app revision directory already exists with different content."
            }
            return
        }
        val temporary = File(parent, "${revision.revisionId}.tmp")
        require(temporary.canonicalFile.parentFile == parent.canonicalFile)
        temporary.deleteRecursively()
        val prompts = File(temporary, "prompts")
        require(prompts.mkdirs()) { "Could not create the app staging directory." }
        try {
            val config = File(temporary, "config.txt")
            FileOutputStream(config).use { output ->
                val bytes = compiled.document.toByteArray(Charsets.US_ASCII)
                require(bytes.size <= MAXIMUM_CONFIG_BYTES)
                output.write(bytes)
                output.fd.sync()
            }
            compiled.promptAssets.forEach { prompt ->
                val destination = File(prompts, "${prompt.sha256}.wav")
                api.downloadPrompt(prompt.sha256, destination, MAXIMUM_PROMPT_BYTES)
                require(destination.length() == prompt.sizeBytes) { "Prompt size verification failed." }
                require(sha256(destination) == prompt.sha256) { "Prompt digest verification failed." }
                require(WavValidator.isCanonical(destination)) { "Prompt WAV validation failed." }
            }
            require(validateExisting(temporary, revision, compiled)) { "Staged app revision failed validation." }
            require(temporary.renameTo(finalDirectory)) { "Could not atomically finalize the app revision." }
        } catch (error: Exception) {
            temporary.deleteRecursively()
            throw error
        }
    }

    private fun validateExisting(
        directory: File,
        revision: RevisionVerifier.VerifiedRevision,
        compiled: RevisionCompiler.Compiled,
    ): Boolean {
        if (!directory.isDirectory) return false
        val config = File(directory, "config.txt")
        if (!config.isFile || config.length() !in 1..MAXIMUM_CONFIG_BYTES.toLong()) return false
        val document = config.readText(Charsets.US_ASCII)
        val verifiedCompilation = if (document == compiled.document) {
            compiled
        } else {
            RevisionCompiler.verifyCachedCompilation(
                revision.manifest,
                revision.manifestSha256,
                document,
            ) ?: return false
        }
        if (!document.contains("\nMANIFEST ${revision.manifestSha256}\n")) return false
        return verifiedCompilation.promptAssets.all { prompt ->
            val file = File(directory, "prompts/${prompt.sha256}.wav")
            file.isFile && file.length() == prompt.sizeBytes &&
                sha256(file) == prompt.sha256 && WavValidator.isCanonical(file)
        }
    }

    private fun sha256(file: File): String {
        val digest = MessageDigest.getInstance("SHA-256")
        FileInputStream(file).use { input ->
            val buffer = ByteArray(16 * 1024)
            while (true) {
                val count = input.read(buffer)
                if (count < 0) break
                digest.update(buffer, 0, count)
            }
        }
        return digest.digest().toHex()
    }
}

object WavValidator {
    fun isCanonical(file: File): Boolean = runCatching {
        RandomAccessFile(file, "r").use { input ->
            require(input.length() in 45..64L * 1024L * 1024L)
            require(input.readAscii(4) == "RIFF")
            val riffSize = input.readLittleUInt()
            require(input.readAscii(4) == "WAVE")
            require(riffSize + 8 <= input.length())
            var formatFound = false
            var dataFound = false
            repeat(32) {
                if (input.filePointer + 8 > input.length() || dataFound) return@repeat
                val type = input.readAscii(4)
                val size = input.readLittleUInt()
                require(size >= 0 && input.filePointer + size <= input.length())
                if (type == "fmt ") {
                    require(size >= 16)
                    require(input.readLittleUShort() == 1)
                    require(input.readLittleUShort() == 2)
                    require(input.readLittleUInt() == 48_000L)
                    require(input.readLittleUInt() == 192_000L)
                    require(input.readLittleUShort() == 4)
                    require(input.readLittleUShort() == 16)
                    input.seek(input.filePointer + size - 16)
                    formatFound = true
                } else if (type == "data") {
                    require(formatFound && size > 0 && size % 4 == 0L)
                    dataFound = true
                } else {
                    input.seek(input.filePointer + size)
                }
                if (size % 2 == 1L && input.filePointer < input.length()) input.seek(input.filePointer + 1)
            }
            formatFound && dataFound
        }
    }.getOrDefault(false)

    private fun RandomAccessFile.readAscii(count: Int): String {
        val bytes = ByteArray(count)
        readFully(bytes)
        return String(bytes, Charsets.US_ASCII)
    }

    private fun RandomAccessFile.readLittleUShort(): Int {
        val first = readUnsignedByte()
        val second = readUnsignedByte()
        return first or (second shl 8)
    }

    private fun RandomAccessFile.readLittleUInt(): Long {
        var result = 0L
        repeat(4) { result = result or (readUnsignedByte().toLong() shl (it * 8)) }
        return result
    }
}
