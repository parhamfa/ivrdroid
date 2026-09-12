package ai.rx1.ivrdroid.control

import android.content.Context
import android.Manifest
import android.content.pm.PackageManager
import android.annotation.SuppressLint
import ai.rx1.ivrdroid.telecom.CallRuntimeState
import ai.rx1.ivrdroid.audio.RootAudioTrigger
import android.telecom.TelecomManager
import org.json.JSONObject
import java.io.File
import java.security.MessageDigest
import java.time.Instant

/** Dedicated files and key keep audit failures outside voicemail and operator handoff contracts. */
object SessionAuditSpool {
    private const val MAX_SEGMENT = 3L * 1024 * 1024
    private const val MAX_REPORT = 1024L * 1024
    private const val RESERVE = 576L * 1024 * 1024
    private val segmentName = Regex("[0-9]{5}\\.json")
    private val safeId = Regex("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")

    private fun root(context: Context) = SessionAuditFiles.directory(File(context.filesDir, "session-audit-spool"))
    private fun inbox(context: Context) = SessionAuditFiles.directory(File(context.filesDir, "bridge/session-audit"))
    private fun directory(context: Context, id: String): File {
        SessionAuditProtocol.canonicalId(id)
        return SessionAuditFiles.directory(File(root(context), id))
    }
    private fun directories(context: Context) = root(context).listFiles { file -> file.isDirectory && safeId.matches(file.name) }.orEmpty().sortedBy(File::getName)
    private fun readJson(directory: File, name: String, maximum: Long = MAX_REPORT): JSONObject =
        JSONObject(String(SessionAuditFiles.decrypt(File(directory, name), "audit:${directory.name}:$name", maximum), Charsets.UTF_8))
    private fun writeJson(directory: File, name: String, value: JSONObject) =
        SessionAuditFiles.encrypt(File(directory, name), "audit:${directory.name}:$name", value.toString().toByteArray(Charsets.UTF_8))

    @Synchronized
    private fun bindCall(context: Context, event: PendingCallEvent) {
        if (event.result == "STOCK_DIALER" || event.auditPolicyVersion == null) return
        val existing = File(root(context), "${event.callId}/call.meta")
        val directory = directory(context, event.callId)
        val previous = if (existing.exists()) readJson(directory, "call.meta") else null
        val document = CallEventPayload.encode(event)
            .put("audit_policy_version", previous?.getLong("audit_policy_version") ?: event.auditPolicyVersion)
            .put("audit_quota_bytes", previous?.getLong("audit_quota_bytes") ?: requireNotNull(event.auditQuotaBytes))
        writeJson(directory, "call.meta", document)
    }

    @Synchronized
    fun usageBytes(context: Context): Long = directories(context).sumOf { folder -> folder.listFiles().orEmpty().filter(File::isFile).sumOf(File::length) }
    @Synchronized
    fun count(context: Context): Int = directories(context).size
    fun lastError(context: Context): String? = context.getSharedPreferences("session-audit-status", Context.MODE_PRIVATE).getString("error", null)
    private fun error(context: Context, value: String?) {
        context.getSharedPreferences("session-audit-status", Context.MODE_PRIVATE).edit().putString("error", value).apply()
    }

    @SuppressLint("MissingPermission") // Runtime check below plus a fail-closed revocation handler.
    private fun telecomIdle(context: Context): Boolean {
        if (context.checkSelfPermission(Manifest.permission.READ_PHONE_STATE) != PackageManager.PERMISSION_GRANTED) return false
        return try {
            !(context.getSystemService(Context.TELECOM_SERVICE) as TelecomManager).isInCall
        } catch (_: SecurityException) { false }
    }

    @Synchronized
    fun publishCapacity(context: Context) = SessionAuditFiles.write(
        File(context.filesDir, "bridge/audit-capacity"), "${usageBytes(context)}\n".toByteArray(Charsets.US_ASCII),
    )

    /** Runs during calls. Only completed, fsynced segments are read; network uploads wait for idle. */
    @Synchronized
    fun reconcile(context: Context) {
        var failed = false
        SecureControlStore.pendingCalls(context).filter { it.auditPolicyVersion != null }.forEach { event ->
            val existing = File(root(context), "${event.callId}/call.meta")
            if (!existing.exists() || readJson(requireNotNull(existing.parentFile), "call.meta").getString("result") != event.result) bindCall(context, event)
        }
        // Finish interrupted app sessions only after both helper and Telecom report idle.
        val idle = !CallRuntimeState.isBusy() && RootAudioTrigger.isIdle(context) && telecomIdle(context)
        root(context).listFiles { file -> file.name.startsWith(".") && file.name.endsWith(".acked") }.orEmpty().forEach { it.deleteRecursively() }
        inbox(context).listFiles { file -> file.isDirectory && safeId.matches(file.name) }.orEmpty().forEach { incoming ->
            try {
                val folder = directory(context, incoming.name)
                if (!File(folder, "call.meta").exists()) return@forEach
                val call = readJson(folder, "call.meta")
                val policyVersion = call.getLong("audit_policy_version")
                incoming.listFiles { file -> segmentName.matches(file.name) }.orEmpty().sortedBy(File::getName).forEach { receipt ->
                    val stem = receipt.name.removeSuffix(".json")
                    val source = File(incoming, "$stem.wav")
                    if (!source.exists()) return@forEach
                    val metadata = SessionAuditProtocol.validateSegment(incoming.name, stem.toInt(),
                        JSONObject(String(SessionAuditFiles.read(receipt, 16 * 1024), Charsets.UTF_8)))
                    require(metadata.getLong("policy_version") == policyVersion)
                    val audioFile = File(folder, "$stem.audio")
                    val metaFile = File(folder, "$stem.meta")
                    val bytes = SessionAuditFiles.read(source, MAX_SEGMENT)
                    require(bytes.size.toLong() == metadata.getLong("size_bytes"))
                    require(MessageDigest.getInstance("SHA-256").digest(bytes).toHex() == metadata.getString("sha256"))
                    val additional = (if (audioFile.exists()) 0L else bytes.size + RecordingEnvelope.OVERHEAD_BYTES) +
                        (if (metaFile.exists()) 0L else receipt.length() + RecordingEnvelope.OVERHEAD_BYTES)
                    require(usageBytes(context) + additional <= call.getLong("audit_quota_bytes")) { "Audit storage limit reached." }
                    require(folder.usableSpace - additional >= RESERVE) { "Audit filesystem reserve reached." }
                    if (!audioFile.exists()) SessionAuditFiles.encrypt(audioFile, "audit:${incoming.name}:$stem.audio", bytes)
                    val verified = SessionAuditFiles.decrypt(audioFile, "audit:${incoming.name}:$stem.audio", MAX_SEGMENT)
                    require(MessageDigest.isEqual(bytes, verified))
                    if (!metaFile.exists()) writeJson(folder, "$stem.meta", metadata)
                    require(CanonicalJson.encode(readJson(folder, "$stem.meta")) == CanonicalJson.encode(metadata))
                    require(source.delete())
                    require(receipt.delete())
                    SessionAuditFiles.sync(incoming)
                }
                val reportFile = File(incoming, "report.json")
                if (reportFile.exists()) {
                    val report = SessionAuditProtocol.validateReport(incoming.name,
                        JSONObject(String(SessionAuditFiles.read(reportFile, MAX_REPORT), Charsets.UTF_8)))
                    require(report.getLong("policy_version") == policyVersion)
                    if (report.getString("state") == "pending_upload") {
                        val count = ((report.getLong("duration_ms") + 14_999) / 15_000).toInt()
                        for (index in 0 until count) {
                            require(File(folder, "%05d.meta".format(index)).exists())
                            require(File(folder, "%05d.audio".format(index)).exists())
                        }
                    }
                    writeJson(folder, "report.meta", report)
                    // report.json is the writer's final publication. Earlier partial tails are
                    // outside its durable coverage and may be removed only after this commit.
                    incoming.listFiles().orEmpty().forEach { file -> require(file.isFile && !java.nio.file.Files.isSymbolicLink(file.toPath())); require(file.delete()) }
                    SessionAuditFiles.sync(incoming)
                    incoming.delete()
                }
            } catch (_: Exception) {
                failed = true
                error(context, "Audit handoff pending; capture marked partial if storage is unavailable.")
                runCatching { SessionAuditFiles.write(File(incoming, "abort"), "writer_failure\n".toByteArray(Charsets.US_ASCII)) }
            }
        }
        directories(context).forEach { folder ->
            if (!File(folder, "call.meta").exists() || File(folder, "metadata.ack").exists()) return@forEach
            var call = readJson(folder, "call.meta")
            if (!File(folder, "report.meta").exists()) {
                // Absence has a state, never fabricated capture timestamps or audio.
                if (!idle || File(inbox(context), "${folder.name}/context").exists()) return@forEach
                writeJson(folder, "report.meta", JSONObject().put("recording_id", folder.name)
                    .put("policy_version", call.getLong("audit_policy_version")).put("state", "unavailable")
                    .put("captured_at", JSONObject.NULL).put("duration_ms", 0).put("partial", true)
                    .put("stop_reason", "capture_failure").put("events", org.json.JSONArray()))
            }
            val report = readJson(folder, "report.meta")
            if (call.getString("result") == "IN_PROGRESS") {
                if (!idle) return@forEach
                val knownEnd = if (report.isNull("captured_at")) Instant.parse(call.getString("started_at"))
                    else Instant.parse(report.getString("captured_at")).plusMillis(report.getLong("duration_ms"))
                val observedSeconds = java.time.Duration.between(Instant.parse(call.getString("started_at")), knownEnd).seconds.coerceIn(0, 86400)
                call = JSONObject(call.toString()).put("result", "RECOVERED_AND_ENDED").put("duration_seconds", observedSeconds)
                writeJson(folder, "call.meta", call)
            }
            val event = CallEventPayload.decode(call).copy(sessionAudit = report)
            SecureControlStore.enqueueCall(context, event)
        }
        if (!failed) error(context, null)
        publishCapacity(context)
    }

    @Synchronized
    fun acknowledgeCalls(context: Context, events: List<PendingCallEvent>, accepted: Set<String>) {
        events.filter { it.callId in accepted && it.sessionAudit != null }.forEach { event ->
            val folder = File(root(context), event.callId)
            if (!folder.isDirectory || !File(folder, "report.meta").exists()) return@forEach
            SessionAuditFiles.write(File(folder, "metadata.ack"), byteArrayOf(1))
            if (event.sessionAudit!!.getString("state") == "unavailable") remove(folder)
        }
    }

    private fun remove(folder: File) {
        // Rename first makes acknowledgement durable even if deletion is interrupted.
        val acknowledged = File(folder.parentFile, ".${folder.name}.acked")
        require(folder.renameTo(acknowledged)); SessionAuditFiles.sync(requireNotNull(folder.parentFile))
        require(acknowledged.deleteRecursively())
        SessionAuditFiles.sync(requireNotNull(folder.parentFile))
    }

    fun hasBinding(context: Context, event: PendingCallEvent): Boolean = event.auditPolicyVersion == null ||
        File(root(context), "${event.callId}/call.meta").isFile

    fun uploadPending(context: Context, api: DeviceApi) {
        // Do not hold the spool monitor across network IO: an incoming call's handoff stays local.
        val ready = synchronized(this) { directories(context).filter { File(it, "metadata.ack").exists() && File(it, "report.meta").exists() } }
        ready.forEach { folder ->
            require(!CallRuntimeState.isBusy()) { "Call started; audit upload paused." }
            val id = folder.name
            val report = readJson(folder, "report.meta")
            val count = ((report.getLong("duration_ms") + 14_999) / 15_000).toInt()
            val logical = JSONObject().put("recording_id", id).put("call_id", id)
                .put("policy_version", report.getLong("policy_version")).put("captured_at", report.getString("captured_at"))
            var response = api.auditJson("POST", "", logical)
            verifyLogical(id, report, count, response)
            if (!response.getBoolean("acknowledged")) {
                for (index in 0 until count) {
                    require(!CallRuntimeState.isBusy()) { "Call started; audit upload paused." }
                    val stem = "%05d".format(index)
                    val metadata = SessionAuditProtocol.validateSegment(id, index, readJson(folder, "$stem.meta"))
                    require(metadata.getLong("policy_version") == report.getLong("policy_version"))
                    require(Instant.parse(metadata.getString("captured_at")) == Instant.parse(report.getString("captured_at")).plusMillis(index * 15_000L))
                    val path = "/$id/segments/$index"
                    var receipt = api.auditJson("POST", path, SessionAuditProtocol.segmentRequest(metadata, report, index == count - 1))
                    var offset = SessionAuditProtocol.verifyUpload(id, index, metadata, receipt)
                    if (!receipt.getBoolean("acknowledged")) {
                        val bytes = SessionAuditFiles.decrypt(File(folder, "$stem.audio"), "audit:$id:$stem.audio", MAX_SEGMENT)
                        require(bytes.size.toLong() == metadata.getLong("size_bytes") && MessageDigest.getInstance("SHA-256").digest(bytes).toHex() == metadata.getString("sha256"))
                        while (offset < bytes.size) {
                            val end = minOf(bytes.size.toLong(), offset + 1024 * 1024).toInt()
                            receipt = api.uploadAuditChunk(path, offset, bytes.copyOfRange(offset.toInt(), end))
                            offset = SessionAuditProtocol.verifyUpload(id, index, metadata, receipt)
                            require(offset == end.toLong())
                        }
                        receipt = api.auditJson("POST", "$path/complete", JSONObject())
                        SessionAuditProtocol.verifyUpload(id, index, metadata, receipt)
                        require(receipt.getBoolean("acknowledged"))
                    }
                }
                response = api.auditJson("POST", "/$id/complete", JSONObject().put("segment_count", count))
            }
            verifyLogical(id, report, count, response)
            require(response.getBoolean("acknowledged"))
            synchronized(this) { remove(folder); publishCapacity(context) }
        }
    }

    private fun verifyLogical(id: String, report: JSONObject, count: Int, response: JSONObject) {
        require(response.getString("id") == id)
        val status = response.getString("status").also { require(it in setOf("uploading", "ready")) }
        require(response.getBoolean("acknowledged") == (status == "ready"))
        if (status == "ready") {
            require(response.getInt("segment_count") == count)
            require(kotlin.math.abs(response.getLong("duration_ms") - report.getLong("duration_ms")) <= 30)
        }
    }
}
