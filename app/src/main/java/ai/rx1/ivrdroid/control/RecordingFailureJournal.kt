package ai.rx1.ivrdroid.control

import android.content.Context
import android.os.SystemClock
import android.system.Os
import android.system.OsConstants
import android.util.Log
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.time.Instant

/** Bounded, private on-device evidence. Exception details never enter public call metadata. */
object RecordingFailureJournal {
    internal const val MAXIMUM_EVENTS = 64
    private const val MAXIMUM_BYTES = 1024 * 1024L

    internal fun entry(
        stage: String, callId: String?, recordingKey: String?, elapsedMs: Long,
        durationMs: Long?, sourceBytes: Long?, freeBytes: Long?, error: Throwable?,
        componentReadiness: String? = null, sessionPhase: String? = null,
        processExitReason: String? = null,
    ): JSONObject = JSONObject()
        .put("occurred_at", Instant.now().toString())
        .put("elapsed_ms", elapsedMs)
        .put("stage", stage)
        .put("call_id", callId ?: JSONObject.NULL)
        .put("recording_key", recordingKey ?: JSONObject.NULL)
        .put("duration_ms", durationMs ?: JSONObject.NULL)
        .put("source_bytes", sourceBytes ?: JSONObject.NULL)
        .put("free_bytes", freeBytes ?: JSONObject.NULL)
        .put("component_readiness", componentReadiness ?: JSONObject.NULL)
        .put("session_phase", sessionPhase ?: JSONObject.NULL)
        .put("process_exit_reason", processExitReason?.take(1024) ?: JSONObject.NULL)
        .put("exception", error?.stackTraceToString()?.take(8192) ?: JSONObject.NULL)
        .put("causes", JSONArray(generateSequence(error) { it.cause?.takeUnless { cause -> cause === it } }
            .take(8).map { JSONObject().put("class", it.javaClass.name).put("message", it.message?.take(1024)) }.toList()))

    @Synchronized
    fun capture(
        context: Context, stage: String, callId: String? = null, recordingKey: String? = null,
        elapsedMs: Long = SystemClock.elapsedRealtime(), durationMs: Long? = null,
        sourceBytes: Long? = null, freeBytes: Long? = null, error: Throwable? = null,
        componentReadiness: String? = null, sessionPhase: String? = null,
        processExitReason: String? = null,
    ) {
        if (error != null) Log.e("IVRdroidRecording", "Recording operation failed at $stage", error)
        runCatching {
            val directory = File(context.filesDir, "recording-diagnostics")
            require(directory.isDirectory || directory.mkdirs())
            require(!Files.isSymbolicLink(directory.toPath()))
            Os.chmod(directory.absolutePath, 0b111000000)
            val destination = File(directory, "failures.json")
            val previous = if (destination.isFile && !Files.isSymbolicLink(destination.toPath()) &&
                destination.length() in 2..MAXIMUM_BYTES
            ) runCatching { JSONArray(destination.readText()) }.getOrDefault(JSONArray()) else JSONArray()
            val retained = JSONArray()
            for (index in maxOf(0, previous.length() - MAXIMUM_EVENTS + 1) until previous.length()) {
                retained.put(previous.getJSONObject(index))
            }
            retained.put(entry(stage, callId, recordingKey, elapsedMs, durationMs, sourceBytes, freeBytes, error,
                componentReadiness, sessionPhase, processExitReason))
            var bytes = retained.toString().toByteArray(Charsets.UTF_8)
            while (bytes.size > MAXIMUM_BYTES && retained.length() > 1) {
                retained.remove(0)
                bytes = retained.toString().toByteArray(Charsets.UTF_8)
            }
            require(bytes.size <= MAXIMUM_BYTES)
            val temporary = File(directory, ".failures.tmp")
            require(!Files.isSymbolicLink(temporary.toPath()))
            FileOutputStream(temporary).use { output ->
                Os.chmod(temporary.absolutePath, 0b110000000)
                output.write(bytes)
                output.fd.sync()
            }
            Files.move(temporary.toPath(), destination.toPath(), StandardCopyOption.ATOMIC_MOVE,
                StandardCopyOption.REPLACE_EXISTING)
            val fd = Os.open(directory.absolutePath, OsConstants.O_RDONLY, 0)
            try { Os.fsync(fd) } finally { Os.close(fd) }
        }.onFailure { Log.e("IVRdroidRecording", "Could not persist recording diagnostics", it) }
    }
}
