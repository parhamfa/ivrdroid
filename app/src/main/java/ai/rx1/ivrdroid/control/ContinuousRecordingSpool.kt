package ai.rx1.ivrdroid.control

import android.content.Context
import android.os.Process
import android.os.SystemClock
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import ai.rx1.ivrdroid.audio.RootAudioTrigger
import ai.rx1.ivrdroid.telecom.CallRuntimeState
import ai.rx1.ivrdroid.telecom.external.BootIdentity
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.RandomAccessFile
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.time.Instant

/** One file per recorder, one serial processing lane, no network dependency for recovery. */
object ContinuousRecordingSpool {
    private const val RESERVE = 576L * 1024 * 1024
    private val safeId = Regex("[0-9a-f-]{36}")
    private class CallStarted : Exception()
    private fun root(context: Context) = SessionAuditFiles.directory(File(context.filesDir, "continuous-recording-spool"))
    private fun folders(context: Context) = root(context).listFiles { f -> f.isDirectory && safeId.matches(f.name) && !Files.isSymbolicLink(f.toPath()) }.orEmpty()
    private fun incoming(context: Context) = listOf("continuous-recordings", "session-audit").flatMap { name ->
        File(context.filesDir, "bridge/$name").listFiles { f -> f.isDirectory && safeId.matches(f.name) && !Files.isSymbolicLink(f.toPath()) }.orEmpty().toList()
    }.filter { File(it, "continuous.context").isFile }
    private fun ensureIdle(context: Context) {
        if (CallRuntimeState.isBusy() || !RootAudioTrigger.isIdle(context)) throw CallStarted()
    }
    private fun safeAudio(file: File, minimum: Long) {
        val state = Os.lstat(file.absolutePath)
        require(OsConstants.S_ISREG(state.st_mode) && state.st_uid == Process.myUid() && state.st_mode and 63 == 0)
        require(state.st_size >= minimum && state.st_size <= 192000L * 86400 + 1024 * 1024)
    }
    private fun context(folder: File) = ContinuousCapture.parse(String(SessionAuditFiles.read(File(folder, "continuous.context"), 1024), Charsets.US_ASCII))
    private fun metadata(folder: File, id: String = folder.name) = JSONObject(String(SessionAuditFiles.decrypt(File(folder, "manifest.enc"), "continuous:$id:metadata", 16 * 1024), Charsets.UTF_8))
    private fun writerGone(capture: ContinuousCapture): Boolean {
        val boot = BootIdentity.current() ?: return false
        if (boot != capture.bootId) return true
        try { Os.kill(capture.pid, 0) } catch (error: ErrnoException) {
            return error.errno == OsConstants.ESRCH
        }
        // A PID may have been reused. An inaccessible process is unknown, never presumed dead.
        return runCatching {
            val fields = File("/proc/${capture.pid}/stat").readText().substringAfterLast(')').trim().split(Regex("\\s+"))
            fields[19].toLong() != capture.processStart
        }.getOrDefault(false)
    }

    /** Only durable checkpoints, boot identity and process identity can establish an orphan. */
    @Synchronized
    fun recover(context: Context) {
        root(context).listFiles { f -> f.name.matches(Regex("\\.[0-9a-f-]{36}\\.acked")) }.orEmpty().forEach { folder ->
            runCatching { finishAcknowledged(context, folder, folder.name.removePrefix(".").removeSuffix(".acked")) }
                .onFailure { RecordingFailureJournal.capture(context, "continuous_ack_cleanup", recordingKey = folder.name, error = it) }
        }
        for (folder in incoming(context)) {
            val started = SystemClock.elapsedRealtime()
            runCatching {
                val capture = context(folder)
                require(capture.id == folder.name)
                val sealed = File(folder, "continuous.sealed")
                if (!sealed.exists()) {
                    if (!writerGone(capture)) return@runCatching
                    val source = File(folder, "audio.pcm")
                    safeAudio(source, capture.sizeBytes)
                    RandomAccessFile(source, "rw").use { it.setLength(capture.sizeBytes); it.fd.sync() }
                    SessionAuditFiles.write(sealed, "interrupted 1\n".toByteArray(Charsets.US_ASCII))
                }
                val coverage = seal(folder)
                if (capture.kind == "conversation" && coverage.second) {
                    // This includes a zero-frame failure: call history must retain
                    // the failure even when there is no audio file to upload.
                    SecureControlStore.appendCallEvent(context, capture.callId, PendingCallSubEvent(
                        Instant.ofEpochMilli(capture.wallMs + capture.durationMs).toString(),
                        "RECORDING_FAILURE", capture.blockId, coverage.first.uppercase(java.util.Locale.ROOT)))
                }
                if (capture.kind == "session_audit" && !File(folder, "report.json").exists()) {
                    if (!writerGone(capture)) return@runCatching
                    val seal = seal(folder)
                    val events = runCatching { JSONArray(String(SessionAuditFiles.read(File(folder, "events"), 1024 * 1024), Charsets.UTF_8)) }.getOrDefault(JSONArray())
                    if (events.length() >= 4096) events.remove(events.length() - 1)
                    if (seal.second) events.put(JSONObject().put("offset_ms", capture.durationMs).put("type", "gap").put("block_id", JSONObject.NULL).put("detail", seal.first))
                    val report = JSONObject().put("recording_id", capture.id).put("policy_version", capture.policyVersion)
                        .put("state", if (capture.frames > 0) "pending_upload" else "unavailable")
                        .put("captured_at", if (capture.frames > 0) Instant.ofEpochMilli(capture.wallMs).toString() else JSONObject.NULL)
                        .put("duration_ms", capture.durationMs).put("partial", seal.second).put("stop_reason", seal.first).put("events", events)
                    SessionAuditFiles.write(File(folder, "report.json"), SessionAuditProtocol.validateReport(capture.id, report).toString().toByteArray())
                }
            }.onFailure { RecordingFailureJournal.capture(context, "continuous_recovery", recordingKey = folder.name,
                durationMs = SystemClock.elapsedRealtime() - started, error = it) }
        }
        publishCapacity(context)
    }

    private fun seal(folder: File): Pair<String, Boolean> {
        val fields = String(SessionAuditFiles.read(File(folder, "continuous.sealed"), 80), Charsets.US_ASCII).trim().split(' ')
        require(fields.size == 2 && fields[0] in ContinuousCapture.reasons && fields[1] in setOf("0", "1"))
        val partial = fields[1] == "1"
        require(partial || fields[0] in ContinuousCapture.completeReasons)
        return fields[0] to partial
    }

    @Synchronized
    fun reconcile(context: Context) {
        ensureIdle(context)
        recover(context)
        for (folder in incoming(context)) {
            ensureIdle(context)
            if (!File(folder, "continuous.sealed").exists()) continue
            val started = SystemClock.elapsedRealtime()
            try {
                val capture = context(folder)
                if (capture.frames == 0L) continue // Keep the unavailable receipt; no invented audio.
                val source = File(folder, "audio.pcm")
                val target = SessionAuditFiles.directory(File(root(context), capture.id))
                val audio = File(target, "audio.enc")
                if (audio.exists() && File(target, "manifest.enc").exists() && !source.exists()) {
                    require(metadata(target).getString("recording_id") == capture.id)
                    continue
                }
                val identity = "${capture.kind}:${capture.id}"
                val stop = seal(folder)
                var lastIdleCheck = 0L
                val checkpoint = {
                    if (CallRuntimeState.isBusy()) throw CallStarted()
                    val now = SystemClock.elapsedRealtime()
                    if (now - lastIdleCheck >= 1000) {
                        ensureIdle(context); lastIdleCheck = now
                        require(target.usableSpace >= RESERVE) { "Recording processing reached the filesystem reserve." }
                    }
                    Unit
                }
                // A crash may leave a valid encrypted file before its metadata commit. Verify
                // it in bounded records and recreate metadata from the preserved native receipt.
                var summary = if (audio.exists()) runCatching {
                    ContinuousRecordingEnvelope.Reader(SessionAuditFiles.key(), identity, audio).use { it.verify(checkpoint) }
                }.getOrElse { error ->
                    if (error is CallStarted) throw error
                    if (!source.exists()) throw error
                    require(audio.delete()); null
                } else null
                if (summary == null) {
                    safeAudio(source, capture.sizeBytes)
                    require(source.length() == capture.sizeBytes) { "PCM changed after its final checkpoint." }
                    val storedSize = ContinuousRecordingEnvelope.storedSize(capture.sizeBytes)
                    val temporary = File(target, ".audio.enc.tmp")
                    require(!Files.isSymbolicLink(temporary.toPath()))
                    require(target.usableSpace >= RESERVE + (storedSize - temporary.length()).coerceAtLeast(0)) {
                        "Insufficient workspace to encrypt the preserved recording."
                    }
                    if (!temporary.exists()) require(temporary.createNewFile())
                    Os.chmod(temporary.absolutePath, 0b110000000)
                    ContinuousRecordingEnvelope.encryptResuming(SessionAuditFiles.key(), identity, capture.sizeBytes, source, temporary, checkpoint)
                    val verified = ContinuousRecordingEnvelope.Reader(SessionAuditFiles.key(), identity, temporary).use { it.verify(checkpoint) }
                    Files.move(temporary.toPath(), audio.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING)
                    SessionAuditFiles.sync(target)
                    summary = verified
                }
                require(summary.size == capture.sizeBytes)
                val manifest = capture.manifest(summary.sha256, stop.first, stop.second)
                if (File(target, "manifest.enc").exists()) require(CanonicalJson.encode(metadata(target)) == CanonicalJson.encode(manifest))
                else SessionAuditFiles.encrypt(File(target, "manifest.enc"), "continuous:${capture.id}:metadata", manifest.toString().toByteArray())
                SessionAuditFiles.sync(target)
                if (source.exists()) { require(source.delete()); SessionAuditFiles.sync(folder) }
            } catch (_: CallStarted) { return }
            catch (error: Exception) {
                RecordingFailureJournal.capture(context, "continuous_encrypt", recordingKey = folder.name,
                    durationMs = SystemClock.elapsedRealtime() - started, freeBytes = folder.usableSpace, error = error)
            }
        }
        publishCapacity(context)
    }

    fun uploadPending(context: Context, api: DeviceApi) {
        for (folder in folders(context).sortedBy { it.name }) {
            ensureIdle(context)
            if (!File(folder, "manifest.enc").exists()) continue
            val manifest = metadata(folder)
            val id = folder.name
            require(manifest.getString("recording_id") == id)
            if (manifest.getString("kind") == "session_audit" && !SessionAuditSpool.continuousMetadataAcknowledged(context, id)) continue
            var response = api.continuousJson("POST", "", manifest)
            val size = manifest.getLong("expected_size_bytes")
            fun verify(): Long {
                require(response.getString("id") == id && response.getLong("expected_size_bytes") == size &&
                    response.getString("source_sha256") == manifest.getString("source_sha256") && response.getBoolean("partial") == manifest.getBoolean("partial"))
                val offset = response.getLong("upload_offset").also { require(it in 0..size) }
                require(!response.getBoolean("acknowledged") || (offset == size && response.getString("status") in setOf("ready", "deleted")))
                return offset
            }
            var offset = verify()
            if (response.getString("processing_state") == "invalid") {
                response = api.continuousJson("POST", "/$id/reset", JSONObject())
                offset = verify()
            }
            if (!response.getBoolean("acknowledged") && response.getString("processing_state") == "uploading") {
                ContinuousRecordingEnvelope.Reader(SessionAuditFiles.key(), "${manifest.getString("kind")}:$id", File(folder, "audio.enc")).use { reader ->
                    require(reader.size == size && reader.sha256 == manifest.getString("source_sha256"))
                    while (offset < size) {
                        ensureIdle(context)
                        val count = minOf(1024 * 1024L, size - offset).toInt()
                        response = api.uploadContinuousChunk(id, offset, reader.read(offset, count))
                        val next = verify(); require(next == offset + count); offset = next
                    }
                }
                response = api.continuousJson("POST", "/$id/complete", JSONObject()); verify()
            }
            if (response.getBoolean("acknowledged")) {
                val acknowledged = File(root(context), ".$id.acked")
                require(folder.renameTo(acknowledged)); SessionAuditFiles.sync(root(context))
                finishAcknowledged(context, acknowledged, id)
                publishCapacity(context)
            }
        }
    }

    private fun finishAcknowledged(context: Context, acknowledged: File, id: String) {
        require(safeId.matches(id) && !Files.isSymbolicLink(acknowledged.toPath()))
        // The durable rename is the server acceptance receipt. Keep it until all
        // native receipts and audit bindings are gone, so every crash is retryable.
        for (name in listOf("continuous-recordings", "session-audit")) {
            val inbox = File(context.filesDir, "bridge/$name/$id")
            if (!inbox.exists()) continue
            require(!Files.isSymbolicLink(inbox.toPath()) && !File(inbox, "audio.pcm").exists())
            inbox.listFiles().orEmpty().forEach { file ->
                require(file.isFile && !Files.isSymbolicLink(file.toPath())); require(file.delete())
            }
            SessionAuditFiles.sync(inbox); require(inbox.delete()); SessionAuditFiles.sync(requireNotNull(inbox.parentFile))
        }
        // This operation is idempotent and harmless for a conversation recording.
        SessionAuditSpool.acknowledgeContinuous(context, id)
        require(acknowledged.deleteRecursively()); SessionAuditFiles.sync(root(context))
    }

    fun usageBytes(context: Context, kind: String): Long = folders(context).sumOf { folder ->
        val bytes = folder.listFiles().orEmpty().sumOf(File::length)
        // Unreadable metadata still consumes capacity. Conservatively charge both
        // quotas until recovery establishes its kind; never treat corruption as free space.
        runCatching { if (metadata(folder).getString("kind") == kind) bytes else 0L }.getOrDefault(bytes)
    }

    /** Include native plaintext and processing files, counting each logical recording once. */
    fun pendingUsage(context: Context, kind: String): Pair<Long, Set<String>> {
        val native = incoming(context).filter { runCatching { context(it).kind == kind }.getOrDefault(true) }
        val encrypted = folders(context).filter { runCatching { metadata(it).getString("kind") == kind }.getOrDefault(true) }
        return (native + encrypted).sumOf { folder -> folder.listFiles().orEmpty().sumOf(File::length) } to
            (native + encrypted).map { it.name }.toSet()
    }
    @Synchronized
    fun publishCapacity(context: Context) {
        val conversation = RecordingSpool.conversationUsageBytes(context) + usageBytes(context, "conversation")
        val audit = SessionAuditSpool.usageBytes(context) + usageBytes(context, "session_audit")
        val largest = incoming(context).maxOfOrNull { File(it, "audio.pcm").length() } ?: 0
        SessionAuditFiles.write(File(context.filesDir, "bridge/continuous-capacity"), "PCM1 $conversation $audit $largest\n".toByteArray(Charsets.US_ASCII))
    }
}
