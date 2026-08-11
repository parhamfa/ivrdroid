package ai.rx1.ivrdroid.telecom.external

import android.content.Context
import android.system.Os
import ai.rx1.ivrdroid.control.PendingCallSubEvent
import ai.rx1.ivrdroid.control.SecureControlStore
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.time.Instant

object ExternalCallAuditStore {
    private const val MAXIMUM_EVENTS = 128
    private const val MAXIMUM_BYTES = 128 * 1024L

    @Synchronized
    fun capture(
        context: Context,
        snapshot: ExternalCallSessionSnapshot,
        status: CallControlStatus,
        reason: String,
    ): Boolean = runCatching {
            val destination = File(context.filesDir, "external-call/events.json")
            val existing = if (destination.isFile && !Files.isSymbolicLink(destination.toPath()) &&
                destination.length() in 2..MAXIMUM_BYTES
            ) {
                runCatching { JSONArray(destination.readText(Charsets.UTF_8)) }.getOrDefault(JSONArray())
            } else {
                JSONArray()
            }
            val retained = JSONArray()
            val first = maxOf(0, existing.length() - (MAXIMUM_EVENTS - 1))
            for (index in first until existing.length()) retained.put(existing.getJSONObject(index))
            // Deliberately exclude phone numbers, Telecom call IDs, disconnect strings, and audio paths.
            val occurredAt = Instant.now().toString()
            retained.put(
                JSONObject()
                    .put("occurred_at", occurredAt)
                    .put("elapsed_ms", snapshot.updatedElapsedMs)
                    .put("session_id", snapshot.config.sessionId)
                    .put("revision_id", snapshot.config.revisionId)
                    .put("block_id", snapshot.config.blockId)
                    .put("status", status.name)
                    .put("reason", reason),
            )
            val bytes = retained.toString().toByteArray(Charsets.UTF_8)
            require(bytes.size <= MAXIMUM_BYTES)
            val directory = requireNotNull(destination.parentFile)
            require(directory.isDirectory || directory.mkdirs())
            Os.chmod(directory.absolutePath, 0b111000000)
            val temporary = File(directory, ".${destination.name}.tmp")
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
            SecureControlStore.appendCallEvent(
                context,
                snapshot.config.sessionId,
                PendingCallSubEvent(
                    occurredAt,
                    status.name,
                    snapshot.config.blockId,
                    reason,
                ),
            )
            if (status == CallControlStatus.CONFERENCED) {
                retainConferenceEvidence(context, snapshot, occurredAt)
            }
        }.isSuccess

    @Synchronized
    fun verifiedConferenceAt(
        context: Context,
        sessionId: String,
        revisionId: Long,
        blockId: String,
    ): Instant? = runCatching {
        val evidence = conferenceEvidence(context)
        val key = evidenceKey(sessionId, revisionId, blockId)
        evidence.optString(key).takeIf { it.isNotEmpty() }?.let(Instant::parse)?.let {
            return@runCatching it
        }
        val source = File(context.filesDir, "external-call/events.json")
        if (!source.isFile || Files.isSymbolicLink(source.toPath()) || source.length() !in 2..MAXIMUM_BYTES) {
            return@runCatching null
        }
        val events = JSONArray(source.readText(Charsets.UTF_8))
        (0 until events.length()).mapNotNull { index ->
            val event = events.getJSONObject(index)
            if (event.getString("session_id") == sessionId &&
                event.getLong("revision_id") == revisionId &&
                event.getString("block_id") == blockId &&
                event.getString("status") == CallControlStatus.CONFERENCED.name
            ) {
                Instant.parse(event.getString("occurred_at"))
            } else {
                null
            }
        }.minOrNull()
    }.getOrNull()

    @Synchronized
    fun releaseConferenceEvidence(context: Context, sessionId: String, revisionId: Long, blockId: String) {
        runCatching {
            val evidence = conferenceEvidence(context)
            evidence.remove(evidenceKey(sessionId, revisionId, blockId))
            writePrivateJson(File(context.filesDir, "external-call/conference-evidence.json"), evidence.toString())
        }
    }

    private fun retainConferenceEvidence(
        context: Context,
        snapshot: ExternalCallSessionSnapshot,
        occurredAt: String,
    ) {
        val evidence = conferenceEvidence(context)
        require(evidence.length() < 128 || evidence.has(
            evidenceKey(snapshot.config.sessionId, snapshot.config.revisionId, snapshot.config.blockId),
        ))
        evidence.put(
            evidenceKey(snapshot.config.sessionId, snapshot.config.revisionId, snapshot.config.blockId),
            occurredAt,
        )
        writePrivateJson(File(context.filesDir, "external-call/conference-evidence.json"), evidence.toString())
    }

    private fun conferenceEvidence(context: Context): JSONObject {
        val file = File(context.filesDir, "external-call/conference-evidence.json")
        if (!file.isFile || Files.isSymbolicLink(file.toPath()) || file.length() !in 2..MAXIMUM_BYTES) {
            return JSONObject()
        }
        return runCatching { JSONObject(file.readText(Charsets.UTF_8)) }.getOrDefault(JSONObject())
    }

    private fun evidenceKey(sessionId: String, revisionId: Long, blockId: String): String =
        "$sessionId|$revisionId|$blockId"

    private fun writePrivateJson(destination: File, value: String) {
        val bytes = value.toByteArray(Charsets.UTF_8)
        require(bytes.size <= MAXIMUM_BYTES)
        val directory = requireNotNull(destination.parentFile)
        require(directory.isDirectory || directory.mkdirs())
        Os.chmod(directory.absolutePath, 0b111000000)
        val temporary = File(directory, ".${destination.name}.tmp")
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
    }
}
