package ai.rx1.ivrdroid.audio

import android.content.Context
import android.system.Os
import android.util.Log
import java.io.File
import java.io.FileOutputStream

object RootAudioTrigger {
    private const val TAG = "IVRdroidBridge"
    private const val DIRECTORY_NAME = "bridge"
    private const val TRIGGER_NAME = "play_once.request"
    private const val TEMP_TRIGGER_NAME = ".play_once.request.tmp"
    private const val COMMAND_NAME = "command.request"
    private const val TEMP_COMMAND_NAME = ".command.request.tmp"
    private const val STATUS_NAME = "status"
    private const val REQUEST_BODY = "PLAY_ONCE\n"
    private const val INITIAL_STATUS = "NOT_INSTALLED\n"
    private const val MAXIMUM_STATUS_BYTES = 128L

    @Synchronized
    fun initialize(context: Context): Boolean {
        return try {
            val directory = bridgeDirectory(context)
            if (!directory.isDirectory && !directory.mkdirs()) {
                Log.e(TAG, "Could not create the private helper bridge directory.")
                return false
            }
            Os.chmod(directory.absolutePath, 0b111000000)

            val status = File(directory, STATUS_NAME)
            if (!status.exists()) {
                FileOutputStream(status).use { output ->
                    output.write(INITIAL_STATUS.toByteArray(Charsets.US_ASCII))
                    output.fd.sync()
                }
                Os.chmod(status.absolutePath, 0b110000000)
            }
            status.isFile
        } catch (error: Exception) {
            Log.e(TAG, "Could not initialize the private helper bridge.", error)
            false
        }
    }

    @Synchronized
    fun requestOnePrompt(context: Context): Boolean {
        if (!initialize(context)) return false

        return queueFixedRequest(
            context,
            TRIGGER_NAME,
            TEMP_TRIGGER_NAME,
            REQUEST_BODY,
        )
    }

    @Synchronized
    fun requestCommand(context: Context, command: HelperCommand): Boolean {
        if (!initialize(context)) return false

        return queueFixedRequest(
            context,
            COMMAND_NAME,
            TEMP_COMMAND_NAME,
            command.wireBody,
        )
    }

    private fun queueFixedRequest(
        context: Context,
        requestName: String,
        temporaryName: String,
        body: String,
    ): Boolean {
        val directory = bridgeDirectory(context)
        val trigger = File(directory, requestName)
        val temporary = File(directory, temporaryName)
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

    fun readStatus(context: Context): String {
        if (!initialize(context)) return "UNAVAILABLE"
        val status = File(bridgeDirectory(context), STATUS_NAME)
        if (!status.isFile || status.length() !in 1..MAXIMUM_STATUS_BYTES) {
            return "UNAVAILABLE"
        }
        return runCatching {
            status.readText(Charsets.US_ASCII)
                .trim()
                .take(MAXIMUM_STATUS_BYTES.toInt())
                .ifEmpty { "UNAVAILABLE" }
        }.getOrDefault("UNAVAILABLE")
    }

    private fun bridgeDirectory(context: Context): File =
        File(context.filesDir, DIRECTORY_NAME)
}
