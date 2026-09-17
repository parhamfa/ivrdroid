package ai.rx1.ivrdroid.control

import android.os.Process
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.system.Os
import android.system.OsConstants
import java.io.File
import java.io.FileOutputStream
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.security.KeyStore
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey

internal object SessionAuditFiles {
    fun directory(file: File): File = file.apply {
        require(exists() || mkdirs())
        require(!Files.isSymbolicLink(toPath()))
        val state = Os.lstat(absolutePath)
        require(OsConstants.S_ISDIR(state.st_mode) && state.st_uid == Process.myUid())
        Os.chmod(absolutePath, 0b111000000)
    }

    fun read(file: File, maximum: Long): ByteArray {
        val state = Os.lstat(file.absolutePath)
        require(OsConstants.S_ISREG(state.st_mode) && state.st_uid == Process.myUid())
        require(state.st_mode and 0b111111 == 0 && state.st_size in 0..maximum)
        return file.readBytes().also { require(it.size.toLong() <= maximum) }
    }

    fun sync(directory: File) {
        val fd = Os.open(directory.absolutePath, OsConstants.O_RDONLY or OsConstants.O_CLOEXEC, 0)
        try { Os.fsync(fd) } finally { Os.close(fd) }
    }

    fun write(file: File, bytes: ByteArray) {
        val temporary = File(file.parentFile, ".${file.name}.tmp")
        if (temporary.exists()) require(temporary.delete())
        require(temporary.createNewFile())
        Os.chmod(temporary.absolutePath, 0b110000000)
        FileOutputStream(temporary).use { it.write(bytes); it.fd.sync() }
        Files.move(temporary.toPath(), file.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING)
        sync(requireNotNull(file.parentFile))
    }

    fun encrypt(file: File, aad: String, plaintext: ByteArray) = write(file, RecordingEnvelope.encrypt(key(), aad, plaintext))
    fun decrypt(file: File, aad: String, maximum: Long): ByteArray = RecordingEnvelope.decrypt(key(), aad, read(file, maximum + RecordingEnvelope.OVERHEAD_BYTES))

    @Synchronized
    internal fun key(): SecretKey {
        val alias = "ivrdroid-session-audit-spool-v1"
        val store = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (store.getKey(alias, null) as? SecretKey)?.let { return it }
        return KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore").run {
            init(KeyGenParameterSpec.Builder(alias, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM).setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setRandomizedEncryptionRequired(true).build())
            generateKey()
        }
    }
}
