package ai.rx1.ivrdroid.audio

import android.content.Context
import android.system.Os
import android.util.Log
import java.io.File
import java.io.FileOutputStream

object RootAudioTrigger {
    private const val TAG = "IVRdroidBridge"
    private const val DIRECTORY_NAME = "bridge"
    private const val COMMAND_NAME = "command.request"
    private const val TEMP_COMMAND_NAME = ".command.request.tmp"
    private const val STATUS_NAME = "status"
    private const val LAST_RESULT_NAME = "last_result"

    @Synchronized
    fun initialize(context: Context): Boolean {
        return try {
            val directory = bridgeDirectory(context)
            if (!directory.isDirectory && !directory.mkdirs()) {
                Log.e(TAG, "Could not create the private helper bridge directory.")
                return false
            }
            Os.chmod(directory.absolutePath, 0b111000000)

            ensurePrivateFile(
                File(directory, STATUS_NAME),
                HelperProtocol.INITIAL_STATUS,
            ) &&
                ensurePrivateFile(
                    File(directory, LAST_RESULT_NAME),
                    HelperProtocol.INITIAL_LAST_RESULT,
                )
        } catch (error: Exception) {
            Log.e(TAG, "Could not initialize the private helper bridge.", error)
            false
        }
    }

    @Synchronized
    fun requestStartMenu(context: Context): Boolean {
        if (!readState(context).isIdle) return false
        return queueFixedRequest(context, HelperProtocol.START_MENU_REQUEST)
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
        return HelperBridgeState(
            current = readBoundedFile(File(directory, STATUS_NAME)),
            lastResult = readBoundedFile(File(directory, LAST_RESULT_NAME)),
        )
    }

    fun isIdle(context: Context): Boolean = readState(context).isIdle

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

    private fun readBoundedFile(file: File): String {
        if (!file.isFile || file.length() !in 1..HelperProtocol.MAXIMUM_FILE_BYTES) {
            return "UNAVAILABLE"
        }
        return runCatching {
            file.readText(Charsets.US_ASCII)
                .trim()
                .take(HelperProtocol.MAXIMUM_FILE_BYTES.toInt())
                .ifEmpty { "UNAVAILABLE" }
        }.getOrDefault("UNAVAILABLE")
    }

    private fun bridgeDirectory(context: Context): File =
        File(context.filesDir, DIRECTORY_NAME)
}
