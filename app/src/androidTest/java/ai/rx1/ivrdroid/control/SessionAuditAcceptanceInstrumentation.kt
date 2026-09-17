package ai.rx1.ivrdroid.control

import android.app.Activity
import android.app.Instrumentation
import android.content.Context
import android.content.ContextWrapper
import android.content.SharedPreferences
import android.os.Bundle
import ai.rx1.ivrdroid.audio.RootAudioTrigger
import ai.rx1.ivrdroid.telecom.CallRuntimeState
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest
import java.util.UUID
import java.io.RandomAccessFile

/** Actual Android Keystore and disk handoff, isolated from enrollment, media and call history. */
class SessionAuditAcceptanceInstrumentation : Instrumentation() {
    override fun onCreate(arguments: Bundle?) { super.onCreate(arguments); start() }

    override fun onStart() {
        val names = mutableSetOf<String>()
        val prefix = "audit-test-${UUID.randomUUID()}-"
        val files = File(targetContext.filesDir, prefix).apply { mkdirs() }
        val isolated = object : ContextWrapper(targetContext) {
            override fun getApplicationContext(): Context = this
            override fun getFilesDir(): File = files
            override fun getSharedPreferences(name: String, mode: Int): SharedPreferences {
                names.add(prefix + name)
                return baseContext.getSharedPreferences(prefix + name, mode)
            }
        }
        val output = Bundle()
        var result = Activity.RESULT_CANCELED
        try {
            check(!(targetContext.getSystemService(Context.TELECOM_SERVICE) as android.telecom.TelecomManager).isInCall)
            check(RootAudioTrigger.initialize(isolated))
            SessionAuditFiles.write(File(files, "bridge/status"), "READY".toByteArray())
            val id = UUID.randomUUID().toString()
            val event = PendingCallEvent(id, "2026-09-12T12:00:00Z", null, "IVR_HANDLED", null,
                listOf("builtin"), "IN_PROGRESS", 0, auditPolicyVersion = 42, auditQuotaBytes = 1L shl 30)
            SecureControlStore.enqueueCall(isolated, event)
            CallRuntimeState.setBusy(true)
            SessionAuditSpool.reconcile(isolated)
            val incoming = SessionAuditFiles.directory(File(files, "bridge/session-audit/$id"))
            val audio = wave(15_000)
            val metadata = JSONObject().put("kind", "session_audit").put("recording_id", id).put("call_id", id)
                .put("policy_version", 42).put("segment_index", 0).put("captured_at", "2026-09-12T12:00:01.000Z")
                .put("duration_ms", 15_000).put("stop_reason", "segment_boundary").put("partial", false)
                .put("size_bytes", audio.size).put("sha256", MessageDigest.getInstance("SHA-256").digest(audio).toHex())
            SessionAuditFiles.write(File(incoming, "00000.wav"), audio)
            SessionAuditFiles.write(File(incoming, "00000.json"), metadata.toString().toByteArray())
            SessionAuditSpool.reconcile(isolated)
            check(CallRuntimeState.isBusy())
            check(!File(incoming, "00000.wav").exists())
            val encrypted = File(files, "session-audit-spool/$id/00000.audio")
            check(encrypted.isFile && !encrypted.readBytes().contentEquals(audio))
            check(SessionAuditFiles.decrypt(encrypted, "audit:$id:00000.audio", 3L shl 20).contentEquals(audio))
            output.putString("during_call", "PASS: completed segment encrypted by Android Keystore while call remains busy")

            val report = JSONObject().put("recording_id", id).put("policy_version", 42).put("state", "pending_upload")
                .put("captured_at", "2026-09-12T12:00:01.000Z").put("duration_ms", 15_000).put("partial", true)
                .put("stop_reason", "interrupted").put("events", JSONArray().put(JSONObject().put("offset_ms", 0).put("type", "answered")))
            SessionAuditFiles.write(File(incoming, "report.json"), report.toString().toByteArray())
            CallRuntimeState.setBusy(false)
            SessionAuditSpool.reconcile(isolated)
            val recovered = SecureControlStore.pendingCalls(isolated).single()
            check(recovered.result == "RECOVERED_AND_ENDED" && recovered.durationSeconds == 16)
            check(recovered.sessionAudit?.getBoolean("partial") == true)
            check(!CallEventPayload.encode(recovered).has("audit_policy_version"))
            check(!incoming.exists())
            SessionAuditSpool.acknowledgeCalls(isolated, listOf(recovered), setOf(id))
            check(File(files, "session-audit-spool/$id/metadata.ack").exists())
            output.putString("recovery", "PASS: interrupted call metadata recovered with partial audio and durable metadata acknowledgement")

            val limited = UUID.randomUUID().toString()
            SecureControlStore.enqueueCall(isolated, event.copy(callId = limited, auditQuotaBytes = 1))
            CallRuntimeState.setBusy(true)
            SessionAuditSpool.reconcile(isolated)
            val rejected = SessionAuditFiles.directory(File(files, "bridge/session-audit/$limited"))
            SessionAuditFiles.write(File(rejected, "00000.wav"), audio)
            SessionAuditFiles.write(File(rejected, "00000.json"), JSONObject(metadata.toString()).put("recording_id", limited).put("call_id", limited).toString().toByteArray())
            SessionAuditSpool.reconcile(isolated)
            check(CallRuntimeState.isBusy() && File(rejected, "abort").exists())
            check(!File(files, "session-audit-spool/$limited/00000.audio").exists())
            output.putString("failure_isolation", "PASS: audit quota failure requests partial audit only; call state preserved")
            val continuousId = UUID.randomUUID().toString()
            val continuousInbox = SessionAuditFiles.directory(File(files, "bridge/continuous-recordings/$continuousId"))
            val source = File(continuousInbox, "audio.pcm")
            val sourceBytes = 16L * 1024 * 1024
            val buffer = ByteArray(65536) { it.toByte() }
            source.outputStream().use { stream -> repeat((sourceBytes / buffer.size).toInt()) { stream.write(buffer) } }
            android.system.Os.chmod(source.absolutePath, 0b110000000)
            // Different boot identity establishes an interrupted writer; the original
            // recording/call identities and capture time must survive recovery.
            val capture = "PCM1 conversation $continuousId $id ${UUID.randomUUID()} 21 ${UUID.randomUUID()} 0 1789200000000 1000 2147483000 1 ${sourceBytes / 4}\n"
            SessionAuditFiles.write(File(continuousInbox, "continuous.context"), capture.toByteArray())
            CallRuntimeState.setBusy(true)
            ContinuousRecordingSpool.recover(isolated)
            check(File(continuousInbox, "continuous.sealed").readText() == "interrupted 1\n")
            check(runCatching { ContinuousRecordingSpool.reconcile(isolated) }.isFailure && source.exists())
            CallRuntimeState.setBusy(false)
            val processingStarted = android.os.SystemClock.elapsedRealtime()
            ContinuousRecordingSpool.reconcile(isolated)
            val continuousEncrypted = File(files, "continuous-recording-spool/$continuousId/audio.enc")
            check(continuousEncrypted.isFile && !source.exists())
            ContinuousRecordingEnvelope.Reader(SessionAuditFiles.key(), "conversation:$continuousId", continuousEncrypted).use {
                check(it.verify().size == sourceBytes)
                check(it.read(65532, 16).contentEquals(ByteArray(16) { offset -> ((65532 + offset) % 256).toByte() }))
            }
            output.putString("continuous_recovery", "PASS: reboot prefix, busy pause, Android Keystore encryption, durable plaintext removal, bounded seeking; 16 MiB processing_ms=${android.os.SystemClock.elapsedRealtime() - processingStarted}")
            RandomAccessFile(continuousEncrypted, "rw").use { it.setLength(it.length() - 1) }
            check(runCatching { ContinuousRecordingEnvelope.Reader(SessionAuditFiles.key(), "conversation:$continuousId", continuousEncrypted).close() }.isFailure)
            output.putString("continuous_truncation", "PASS: truncated authenticated container is rejected")
            output.putString("stream", "Session audit Android disk and encryption acceptance passed.\n")
            result = Activity.RESULT_OK
        } catch (error: Throwable) {
            output.putString("stream", "Session audit acceptance failed: ${error.javaClass.simpleName}: ${error.message}\n")
        } finally {
            CallRuntimeState.setBusy(false)
            files.deleteRecursively()
            names.forEach { targetContext.deleteSharedPreferences(it) }
        }
        finish(result, output)
    }

    private fun wave(durationMs: Int): ByteArray {
        val frames = durationMs * 48
        val bytes = ByteBuffer.allocate(44 + frames * 4).order(ByteOrder.LITTLE_ENDIAN)
        bytes.put("RIFF".toByteArray()).putInt(36 + frames * 4).put("WAVEfmt ".toByteArray()).putInt(16)
            .putShort(1).putShort(2).putInt(48000).putInt(192000).putShort(4).putShort(16)
            .put("data".toByteArray()).putInt(frames * 4)
        repeat(frames * 2) { bytes.putShort(1000) }
        return bytes.array()
    }
}
