package ai.rx1.ivrdroid.audio

import android.content.Context
import android.system.Os
import android.util.Log
import java.io.File
import java.io.FileOutputStream
import java.nio.file.Files
import java.nio.file.StandardCopyOption

object RootAudioTrigger {
    private const val TAG = "IVRdroidBridge"
    private const val DIRECTORY_NAME = "bridge"
    private const val COMMAND_NAME = "command.request"
    private const val TEMP_COMMAND_NAME = ".command.request.tmp"
    private const val STATUS_NAME = "status"
    private const val LAST_RESULT_NAME = "last_result"
    private const val ACTIVE_REVISION_NAME = "active_revision"
    private const val STAGED_REVISION_NAME = "staged_revision"
    private const val SESSION_PATH_NAME = "session_path"
    private const val HELPER_VERSION_NAME = "helper_version"
    private const val CAPABILITIES_NAME = "capabilities"
    private const val RECORDING_CAPACITY_NAME = "recording_capacity"
    private const val CALL_CONTROL_REQUEST_NAME = "call_control.request"
    private const val CALL_CONTROL_STATUS_NAME = "call_control.status"
    private const val CALL_CONTROL_RECORDING_ACK_NAME = "call_control.recording_ack"

    @Synchronized
    fun initialize(context: Context): Boolean {
        return try {
            val directory = bridgeDirectory(context)
            if (!directory.isDirectory && !directory.mkdirs()) {
                Log.e(TAG, "Could not create the private helper bridge directory.")
                return false
            }
            Os.chmod(directory.absolutePath, 0b111000000)
            val recordings = File(directory, "recordings")
            if (!recordings.isDirectory && !recordings.mkdirs()) return false
            Os.chmod(recordings.absolutePath, 0b111000000)

            ensurePrivateFile(
                File(directory, STATUS_NAME),
                HelperProtocol.INITIAL_STATUS,
            ) &&
                ensurePrivateFile(
                    File(directory, LAST_RESULT_NAME),
                    HelperProtocol.INITIAL_LAST_RESULT,
                ) &&
                ensurePrivateFile(File(directory, ACTIVE_REVISION_NAME), "0") &&
                ensurePrivateFile(File(directory, STAGED_REVISION_NAME), "0") &&
                ensurePrivateFile(File(directory, SESSION_PATH_NAME), "none") &&
                ensurePrivateFile(File(directory, HELPER_VERSION_NAME), "NOT_INSTALLED") &&
                ensurePrivateFile(File(directory, "helper_source_commit"), "unknown") &&
                ensurePrivateFile(File(directory, CAPABILITIES_NAME), "runtime=1,2;recording=0") &&
                ensureRecordingCapacityFile(
                    File(directory, RECORDING_CAPACITY_NAME),
                    context.filesDir.usableSpace.coerceAtLeast(0),
                ) &&
                ensurePrivateFile(File(directory, CALL_CONTROL_REQUEST_NAME), "UNAVAILABLE") &&
                ensurePrivateFile(File(directory, CALL_CONTROL_STATUS_NAME), "UNAVAILABLE") &&
                ensurePrivateFile(File(directory, CALL_CONTROL_RECORDING_ACK_NAME), "UNAVAILABLE") &&
                // Native publication deliberately requires an existing app-owned file.
                // Provision every V2 output before local recovery waits for its contents.
                // UNAVAILABLE cannot be mistaken for native readiness or a policy ack.
                listOf(
                    "native-calls", "call-safety-protocol", "call-safety-state",
                    "call-lifetime", "call-outcome", "call_control.attached",
                    "conversation_capture_failure.json", "conversation_finalizer_failure.json",
                ).all { ensurePrivateFile(File(directory, it), "UNAVAILABLE") }
        } catch (error: Exception) {
            Log.e(TAG, "Could not initialize the private helper bridge.", error)
            false
        }
    }

    @Synchronized
    fun requestStartMenu(context: Context, callId: String): Boolean {
        if (!readState(context).isIdle) return false
        val request = runCatching { HelperProtocol.startMenuRequest(callId) }.getOrNull() ?: return false
        return queueFixedRequest(context, request)
    }

    @Synchronized
    fun requestStageRevision(context: Context, revisionId: Long, manifestSha256: String): Boolean {
        if (revisionId <= 0 || !manifestSha256.matches(Regex("[0-9a-f]{64}")) || !readState(context).isIdle) return false
        return queueFixedRequest(context, HelperProtocol.stageRevisionRequest(revisionId, manifestSha256))
    }

    @Synchronized
    fun requestActivateStaged(context: Context, revisionId: Long): Boolean {
        if (revisionId <= 0 || !readState(context).isIdle) return false
        return queueFixedRequest(context, HelperProtocol.activateStagedRequest(revisionId))
    }

    @Synchronized
    fun cancelPendingStartMenu(context: Context) {
        val directory = bridgeDirectory(context)
        File(directory, TEMP_COMMAND_NAME).delete()
        File(directory, COMMAND_NAME).delete()
    }

    private fun queueFixedRequest(
        context: Context,
        body: String,
    ): Boolean {
        val directory = bridgeDirectory(context)
        val trigger = File(directory, COMMAND_NAME)
        val temporary = File(directory, TEMP_COMMAND_NAME)
        if (temporary.exists() && !temporary.delete()) return false

        return try {
            FileOutputStream(temporary).use { output ->
                output.write(body.toByteArray(Charsets.US_ASCII))
                output.fd.sync()
            }
            Os.chmod(temporary.absolutePath, 0b110000000)
            if (trigger.exists() && !trigger.delete()) {
                temporary.delete()
                false
            } else if (!temporary.renameTo(trigger)) {
                temporary.delete()
                false
            } else {
                true
            }
        } catch (error: Exception) {
            temporary.delete()
            Log.e(TAG, "Could not queue the fixed helper request.", error)
            false
        }
    }

    fun readState(context: Context): HelperBridgeState {
        if (!initialize(context)) {
            return HelperBridgeState("UNAVAILABLE", "UNAVAILABLE")
        }
        val directory = bridgeDirectory(context)
        val capabilities = HelperCapabilityProtocol.parse(
            readBoundedFile(File(directory, CAPABILITIES_NAME)),
        )
        return HelperBridgeState(
            current = readBoundedFile(File(directory, STATUS_NAME)),
            lastResult = readBoundedFile(File(directory, LAST_RESULT_NAME)),
            activeRevision = readRevision(File(directory, ACTIVE_REVISION_NAME)),
            stagedRevision = readRevision(File(directory, STAGED_REVISION_NAME)),
            sessionPath = readBoundedFile(
                File(directory, SESSION_PATH_NAME),
                HelperProtocol.MAXIMUM_SESSION_PATH_BYTES,
            ).split('>').take(64).filter { it.matches(LEGACY_TRACE) || it.matches(V2_TRACE) || it == "builtin" },
            helperVersion = readBoundedFile(File(directory, HELPER_VERSION_NAME)),
            sourceCommit = readBoundedFile(File(directory, "helper_source_commit")),
            recordingCapable = capabilities.recordingCapable,
            runtimeVersions = capabilities.runtimeVersions,
            callControlCapable = capabilities.callControlCapable,
            conversationRecordingCapable = capabilities.conversationRecordingCapable,
            promptBargeInCapable = capabilities.promptBargeInCapable,
            sessionAuditCapable = capabilities.sessionAuditCapable,
        )
    }

    fun isIdle(context: Context): Boolean = readState(context).isIdle

    @Synchronized
    fun publishRecordingCapacity(
        context: Context,
        voicemailBytes: Long,
        voicemailCount: Int,
        conversationBytes: Long,
        conversationCount: Int,
        filesystemFreeBytes: Long,
    ): Boolean {
        if (listOf(voicemailBytes, conversationBytes, filesystemFreeBytes).any { it < 0 } ||
            voicemailCount < 0 || conversationCount < 0 || !initialize(context)
        ) return false
        return queueBridgeValue(
            File(bridgeDirectory(context), RECORDING_CAPACITY_NAME),
            RecordingCapacityProtocol.format(
                voicemailBytes,
                voicemailCount,
                conversationBytes,
                conversationCount,
                filesystemFreeBytes,
            ),
        )
    }

    private fun queueBridgeValue(file: File, body: String): Boolean = try {
        val temporary = File(file.parentFile, ".${file.name}.tmp")
        FileOutputStream(temporary).use { output ->
            output.write(body.toByteArray(Charsets.US_ASCII))
            output.fd.sync()
        }
        Os.chmod(temporary.absolutePath, 0b110000000)
        Files.move(
            temporary.toPath(),
            file.toPath(),
            StandardCopyOption.ATOMIC_MOVE,
            StandardCopyOption.REPLACE_EXISTING,
        )
        true
    } catch (error: Exception) {
        Log.e(TAG, "Could not publish recording capacity.", error)
        false
    }

    private fun ensurePrivateFile(file: File, initialValue: String): Boolean {
        if (!file.exists()) {
            FileOutputStream(file).use { output ->
                output.write("$initialValue\n".toByteArray(Charsets.US_ASCII))
                output.fd.sync()
            }
            Os.chmod(file.absolutePath, 0b110000000)
        }
        return file.isFile
    }

    private fun ensureRecordingCapacityFile(file: File, filesystemFreeBytes: Long): Boolean {
        val initial = RecordingCapacityProtocol.format(0, 0, 0, 0, filesystemFreeBytes).trimEnd()
        if (!ensurePrivateFile(file, initial)) return false
        val existing = readBoundedFile(file, 256)
        return if (existing.startsWith("${RecordingCapacityProtocol.PREFIX} ")) {
            true
        } else {
            queueBridgeValue(file, "$initial\n")
        }
    }

    private fun readRevision(file: File): Long? =
        readBoundedFile(file).toLongOrNull()?.takeIf { it > 0 }

    private fun readBoundedFile(
        file: File,
        maximumBytes: Long = HelperProtocol.MAXIMUM_FILE_BYTES,
    ): String {
        if (!file.isFile || file.length() !in 1..maximumBytes) {
            return "UNAVAILABLE"
        }
        return runCatching {
            file.readText(Charsets.US_ASCII)
                .trim()
                .take(maximumBytes.toInt())
                .ifEmpty { "UNAVAILABLE" }
        }.getOrDefault("UNAVAILABLE")
    }

    private fun bridgeDirectory(context: Context): File =
        File(context.filesDir, DIRECTORY_NAME)

    private val LEGACY_TRACE = Regex("[a-z][a-z0-9_-]{0,31}")
    private val V2_TRACE = Regex(
        "[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}\\|" +
            "(?:Play prompt|End call|Collect one digit\\|(?:digit:[0-9*#]|timeout|invalid)|" +
            "Check schedule\\|(?:open|closed|holiday)|Return to menu\\|(?:return:[1-3]/[1-3]|return-limit)|" +
            "Record message\\|(?:finish-key|maximum|hangup|unavailable)|" +
            "External call\\|(?:external_completed|external_not_connected|external_system_failure))",
    )
}
