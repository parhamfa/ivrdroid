package ai.rx1.ivrdroid.telecom.external

import android.content.Context
import ai.rx1.ivrdroid.control.SessionAuditFiles
import java.io.File
import java.util.UUID

object CallRecoveryBridge {
    private val generation = UUID.randomUUID().toString()
    private fun read(context: Context, name: String, maximum: Long = 4096): String? = runCatching {
        String(SessionAuditFiles.read(File(context.filesDir, "bridge/$name"), maximum), Charsets.US_ASCII)
    }.getOrNull()
    fun snapshot(context: Context): NativeCallSnapshot? = runCatching {
        NativeCallSnapshot.parse(requireNotNull(read(context, "native-calls")))
    }.getOrNull()
    fun publishOwnership(context: Context, session: ExternalCallSessionSnapshot, calls: List<TelecomCallSnapshot>): Boolean = runCatching {
        val previous = read(context, "call_control.ownership", 512)?.trim()?.split(' ')
            ?.takeIf { it.size == 8 && it[0] == "OWN2" && it[1] == session.config.sessionId && it[2] == session.config.bootId && it[4] == session.config.blockId }
        fun native(stable: String?, index: Int) = calls.singleOrNull { it.id == stable }?.nativeId ?: previous?.get(index) ?: "-"
        val caller = native(session.callerId, 5)
        if (caller == "-") return false
        val wire = "OWN2 ${session.config.sessionId} ${session.config.bootId} $generation ${session.config.blockId} $caller ${native(session.operatorId, 6)} ${native(session.conferenceId, 7)}\n"
        if (read(context, "call_control.ownership", 512) != wire) SessionAuditFiles.write(
            File(context.filesDir, "bridge/call_control.ownership"), wire.toByteArray(Charsets.US_ASCII))
        true
    }.getOrDefault(false)
    fun attached(context: Context, session: ExternalCallSessionSnapshot): Boolean =
        read(context, "call_control.attached", 512) == "ATTACHED2 ${session.config.sessionId} ${session.config.bootId} $generation ${session.config.blockId}\n"
}
