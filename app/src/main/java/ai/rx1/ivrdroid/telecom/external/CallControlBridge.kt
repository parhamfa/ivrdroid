package ai.rx1.ivrdroid.telecom.external

import android.content.Context
import android.os.Process
import android.system.Os
import android.system.OsConstants
import java.io.File
import java.io.FileOutputStream
import java.nio.file.Files
import java.nio.file.StandardCopyOption

object CallControlBridge {
    private const val REQUEST_NAME = "call_control.request"
    private const val STATUS_NAME = "call_control.status"
    private const val RECORDING_ACK_NAME = "call_control.recording_ack"

    fun readRequest(context: Context): CallControlRequest? {
        val file = File(bridge(context), REQUEST_NAME)
        if (!file.isFile || Files.isSymbolicLink(file.toPath()) ||
            file.length() !in 1..CallControlProtocol.MAXIMUM_RECORD_BYTES.toLong()
        ) return null
        return runCatching {
            val state = Os.lstat(file.absolutePath)
            require(OsConstants.S_ISREG(state.st_mode))
            require(state.st_uid == Process.myUid() || state.st_uid == 0)
            require(state.st_mode and (OsConstants.S_IRWXG or OsConstants.S_IRWXO) == 0)
            CallControlProtocol.parseRequest(file.readText(Charsets.US_ASCII))
        }.getOrNull()
    }

    fun readStatus(context: Context): CallControlStatusRecord? {
        val file = File(bridge(context), STATUS_NAME)
        if (!file.isFile || Files.isSymbolicLink(file.toPath()) ||
            file.length() !in 1..CallControlProtocol.MAXIMUM_RECORD_BYTES.toLong()
        ) return null
        return runCatching { CallControlProtocol.parseStatus(file.readText(Charsets.US_ASCII)) }.getOrNull()
    }

    fun readConversationHandoff(context: Context): ConversationHandoffRecord? {
        val file = File(bridge(context), RECORDING_ACK_NAME)
        if (!file.isFile || Files.isSymbolicLink(file.toPath()) ||
            file.length() !in 1..ConversationHandoffProtocol.MAXIMUM_RECORD_BYTES.toLong()
        ) return null
        return runCatching {
            ConversationHandoffProtocol.parse(file.readText(Charsets.US_ASCII))
        }.getOrNull()
    }

    @Synchronized
    fun publish(context: Context, record: CallControlStatusRecord): Boolean = runCatching {
        val directory = bridge(context)
        require(directory.isDirectory || directory.mkdirs())
        Os.chmod(directory.absolutePath, 0b111000000)
        val destination = File(directory, STATUS_NAME)
        val temporary = File(directory, ".$STATUS_NAME.tmp")
        val bytes = CallControlProtocol.formatStatus(record).toByteArray(Charsets.US_ASCII)
        FileOutputStream(temporary).use { output ->
            output.write(bytes)
            output.fd.sync()
        }
        Os.chmod(temporary.absolutePath, 0b110000000)
        Files.move(
            temporary.toPath(),
            destination.toPath(),
            StandardCopyOption.ATOMIC_MOVE,
            StandardCopyOption.REPLACE_EXISTING,
        )
        true
    }.getOrDefault(false)

    @Synchronized
    fun publishConversationHandoff(context: Context, record: ConversationHandoffRecord): Boolean = runCatching {
        val directory = bridge(context)
        require(directory.isDirectory || directory.mkdirs())
        Os.chmod(directory.absolutePath, 0b111000000)
        readConversationHandoff(context)?.takeIf {
            it.sessionId == record.sessionId && it.revisionId == record.revisionId &&
                it.blockId == record.blockId && it.recordingId == record.recordingId &&
                it.bootId == record.bootId
        }?.let { previous ->
            require(record.segmentIndex >= previous.segmentIndex && record.elapsedMs >= previous.elapsedMs)
            if (record.segmentIndex == previous.segmentIndex) {
                require(record.result == previous.result && record.reason == previous.reason)
            }
        }
        val destination = File(directory, RECORDING_ACK_NAME)
        val temporary = File(directory, ".$RECORDING_ACK_NAME.tmp")
        val bytes = ConversationHandoffProtocol.format(record).toByteArray(Charsets.US_ASCII)
        FileOutputStream(temporary).use { output ->
            output.write(bytes)
            output.fd.sync()
        }
        Os.chmod(temporary.absolutePath, 0b110000000)
        Files.move(
            temporary.toPath(),
            destination.toPath(),
            StandardCopyOption.ATOMIC_MOVE,
            StandardCopyOption.REPLACE_EXISTING,
        )
        true
    }.getOrDefault(false)

    fun journal(context: Context): ExternalCallJournal =
        ExternalCallJournal(File(context.filesDir, "external-call/recovery.json"))

    private fun bridge(context: Context): File = File(context.filesDir, "bridge")
}
