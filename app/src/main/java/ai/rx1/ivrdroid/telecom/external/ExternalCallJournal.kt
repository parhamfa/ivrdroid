package ai.rx1.ivrdroid.telecom.external

import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.nio.file.Files
import java.nio.file.StandardCopyOption

class ExternalCallJournal(private val file: File) {
    @Synchronized
    fun save(snapshot: ExternalCallSessionSnapshot) {
        val document = JSONObject()
            .put("version", VERSION)
            .put("session_id", snapshot.config.sessionId)
            .put("revision_id", snapshot.config.revisionId)
            .put("block_id", snapshot.config.blockId)
            .put("phone_number", snapshot.config.phoneNumber)
            .put("answer_timeout_ms", snapshot.config.answerTimeoutMs)
            .put("boot_id", snapshot.config.bootId)
            .put("phase", snapshot.phase.name)
            .put("caller_id", snapshot.callerId ?: JSONObject.NULL)
            .put("operator_id", snapshot.operatorId ?: JSONObject.NULL)
            .put("conference_id", snapshot.conferenceId ?: JSONObject.NULL)
            .put("deadline_elapsed_ms", snapshot.deadlineElapsedMs)
            .put("updated_elapsed_ms", snapshot.updatedElapsedMs)
            .put("reason", snapshot.reason)
            .put("last_request_sequence", snapshot.lastRequestSequence)
            .put("last_request_elapsed_ms", snapshot.lastRequestElapsedMs)
            .put("last_request_kind", snapshot.lastRequestKind)
            .put("cleanup_heartbeat_status", snapshot.cleanupHeartbeatStatus?.name ?: JSONObject.NULL)
            .put("cleanup_outcome_reason", snapshot.cleanupOutcomeReason ?: JSONObject.NULL)
            .put(
                "operator_disconnect_requested_elapsed_ms",
                snapshot.operatorDisconnectRequestedElapsedMs ?: JSONObject.NULL,
            )
            .put(
                "operator_safe_since_elapsed_ms",
                snapshot.operatorSafeSinceElapsedMs ?: JSONObject.NULL,
            )
            .put(
                "caller_unhold_requested_elapsed_ms",
                snapshot.callerUnholdRequestedElapsedMs ?: JSONObject.NULL,
            )
            .put(
                "caller_active_since_elapsed_ms",
                snapshot.callerActiveSinceElapsedMs ?: JSONObject.NULL,
            )
            .put("caller_active_observed", snapshot.callerActiveObserved)
            .put("cleanup_unhold_allowed", snapshot.cleanupUnholdAllowed)
        atomicWrite(document.toString().toByteArray(Charsets.UTF_8))
    }

    @Synchronized
    fun load(): ExternalCallSessionSnapshot? {
        if (!file.isFile || Files.isSymbolicLink(file.toPath()) || file.length() !in 2..MAXIMUM_BYTES) return null
        return runCatching {
            val document = JSONObject(file.readText(Charsets.UTF_8))
            val version = document.getInt("version").also { require(it in 1..VERSION) }
            val config = ExternalCallConfig(
                document.getString("session_id"),
                document.getLong("revision_id"),
                document.getString("block_id"),
                document.getString("phone_number"),
                document.getInt("answer_timeout_ms"),
                document.getString("boot_id"),
            )
            ExternalCallSessionSnapshot(
                config,
                ExternalCallPhase.valueOf(document.getString("phase")),
                document.nullableString("caller_id"),
                document.nullableString("operator_id"),
                document.nullableString("conference_id"),
                document.getLong("deadline_elapsed_ms").also { require(it >= 0) },
                document.getLong("updated_elapsed_ms").also { require(it >= 0) },
                document.getString("reason").also {
                    require(it.matches(Regex("(?:-|[A-Z][A-Z0-9_]{0,63})")))
                },
                if (version >= 2) document.getLong("last_request_sequence").also { require(it > 0) } else 1,
                if (version >= 2) {
                    document.getLong("last_request_elapsed_ms").also { require(it >= 0) }
                } else {
                    0
                },
                if (version >= 2) {
                    document.getString("last_request_kind").also {
                        require(it in setOf("DIAL", "RECORDER_READY", "CANCEL"))
                    }
                } else {
                    "DIAL"
                },
                if (version >= 2 && !document.isNull("cleanup_heartbeat_status")) {
                    CallControlStatus.valueOf(document.getString("cleanup_heartbeat_status"))
                } else {
                    null
                },
                if (version >= 3) document.nullableReason("cleanup_outcome_reason") else null,
                if (version >= 3) {
                    document.nullableNonnegativeLong("operator_disconnect_requested_elapsed_ms")
                } else {
                    null
                },
                if (version >= 3) {
                    document.nullableNonnegativeLong("operator_safe_since_elapsed_ms")
                } else {
                    null
                },
                if (version >= 3) {
                    document.nullableNonnegativeLong("caller_unhold_requested_elapsed_ms")
                } else {
                    null
                },
                if (version >= 3) {
                    document.nullableNonnegativeLong("caller_active_since_elapsed_ms")
                } else {
                    null
                },
                if (version >= 3) document.getBoolean("caller_active_observed") else false,
                if (version >= 3) document.getBoolean("cleanup_unhold_allowed") else true,
            )
        }.getOrNull()
    }

    @Synchronized
    fun clear() {
        file.delete()
    }

    private fun atomicWrite(bytes: ByteArray) {
        require(bytes.size <= MAXIMUM_BYTES)
        val directory = requireNotNull(file.parentFile)
        require(directory.isDirectory || directory.mkdirs())
        val temporary = File(directory, ".${file.name}.tmp")
        FileOutputStream(temporary).use { output ->
            output.write(bytes)
            output.fd.sync()
        }
        require(temporary.setReadable(false, false))
        require(temporary.setWritable(false, false))
        require(temporary.setReadable(true, true))
        require(temporary.setWritable(true, true))
        Files.move(
            temporary.toPath(),
            file.toPath(),
            StandardCopyOption.ATOMIC_MOVE,
            StandardCopyOption.REPLACE_EXISTING,
        )
    }

    private fun JSONObject.nullableString(name: String): String? =
        if (isNull(name)) null else getString(name).also { require(it.length in 1..256) }

    private fun JSONObject.nullableReason(name: String): String? =
        if (isNull(name)) null else getString(name).also {
            require(it.matches(Regex("[A-Z][A-Z0-9_]{0,63}")))
        }

    private fun JSONObject.nullableNonnegativeLong(name: String): Long? =
        if (isNull(name)) null else getLong(name).also { require(it >= 0) }

    private companion object {
        const val VERSION = 3
        const val MAXIMUM_BYTES = 8 * 1024L
    }
}
