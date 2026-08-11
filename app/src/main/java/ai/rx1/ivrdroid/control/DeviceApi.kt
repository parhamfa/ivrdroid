package ai.rx1.ivrdroid.control

import ai.rx1.ivrdroid.BuildConfig
import ai.rx1.ivrdroid.telecom.CallRuntimeState
import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.InputStream
import java.io.File
import java.io.FileOutputStream
import java.net.HttpURLConnection
import java.net.URL
import javax.net.ssl.HttpsURLConnection

class DeviceApi(private val enrollment: Enrollment? = null) {
    fun enroll(code: String, deviceName: String, appVersion: String, helperVersion: String): Enrollment {
        require(code.matches(Regex("[0-9]{8}"))) { "Pairing code must contain eight digits." }
        val request = JSONObject()
            .put("code", code)
            .put("device_name", deviceName)
            .put("app_version", appVersion)
            .put("helper_version", helperVersion)
        val response = requestJson("POST", BuildConfig.CONTROL_PLANE_URL, "/api/device/v1/enroll", request, false)
        val serverUrl = response.getString("server_url").trimEnd('/')
        require(serverUrl == BuildConfig.CONTROL_PLANE_URL) { "Enrollment returned an unexpected server." }
        return Enrollment(
            deviceId = response.getString("device_id"),
            deviceToken = response.getString("device_token"),
            serviceClientId = response.getString("service_client_id"),
            serviceClientSecret = response.getString("service_client_secret"),
            serverUrl = serverUrl,
        )
    }

    fun sync(body: JSONObject): JSONObject = authenticatedJson("POST", "/api/device/v1/sync", body)
    fun manifest(revisionId: Long): JSONObject = authenticatedJson("GET", "/api/device/v1/revisions/$revisionId/manifest", null)

    fun acknowledge(revisionId: Long, state: String, error: String? = null) {
        val body = JSONObject().put("state", state).put("error", error ?: JSONObject.NULL)
        authenticatedJson("POST", "/api/device/v1/revisions/$revisionId/ack", body, allowEmpty = true)
    }

    fun uploadEvents(events: List<PendingCallEvent>): Set<String> {
        if (events.isEmpty()) return emptySet()
        val calls = JSONArray()
        events.forEach { event ->
            calls.put(CallEventPayload.encode(event))
        }
        val response = authenticatedJson("POST", "/api/device/v1/events:batch", JSONObject().put("calls", calls))
        val accepted = response.getJSONArray("accepted_call_ids")
        return buildSet { for (index in 0 until accepted.length()) add(accepted.getString(index)) }
    }

    fun beginRecording(recording: PendingRecording): JSONObject = authenticatedJson(
        "POST",
        RecordingApiPaths.segment(recording) ?: "/api/device/v1/recordings",
        RecordingUploadPolicy.createRequest(recording),
    )

    fun ensureConversation(recording: PendingRecording): JSONObject {
        require(recording.kind == "conversation")
        val response = if (recording.segmentIndex == 0) {
            authenticatedJson(
                "POST",
                RecordingApiPaths.CONVERSATIONS,
                ConversationUploadPolicy.logicalCreateRequest(recording),
            )
        } else {
            authenticatedJson(
                "GET",
                RecordingApiPaths.conversation(recording.recordingId),
                null,
            )
        }
        ConversationUploadPolicy.verifyLogical(recording, response)
        return response
    }

    fun recordingUpload(recording: PendingRecording): JSONObject = authenticatedJson(
        "GET",
        RecordingApiPaths.segment(recording)?.plus("/upload")
            ?: "/api/device/v1/recordings/${recording.recordingId}/upload",
        null,
    )

    fun uploadRecordingChunk(recording: PendingRecording, offset: Long, content: ByteArray): JSONObject {
        require(offset >= 0 && content.isNotEmpty() && content.size <= 1024 * 1024)
        if (CallRuntimeState.isBusy()) error("Call started; synchronization paused.")
        val connection = open(
            enrollment!!.serverUrl,
            RecordingApiPaths.segment(recording)?.plus("/content")
                ?: "/api/device/v1/recordings/${recording.recordingId}/content",
            "PUT",
            true,
        )
        val callInterruption = CallRuntimeState.interruptWhenBusy { connection.disconnect() }
        try {
            if (CallRuntimeState.isBusy()) error("Call started; synchronization paused.")
            connection.setRequestProperty("Content-Type", "application/offset+octet-stream")
            connection.setRequestProperty("Upload-Offset", offset.toString())
            connection.doOutput = true
            connection.setFixedLengthStreamingMode(content.size)
            connection.outputStream.use { it.write(content) }
            requireSuccess(connection)
            if (CallRuntimeState.isBusy()) error("Call started; synchronization paused.")
            return JSONObject(String(readBounded(connection.inputStream, 64 * 1024), Charsets.UTF_8))
        } finally {
            callInterruption.close()
            connection.disconnect()
        }
    }

    fun completeRecording(recording: PendingRecording): JSONObject = authenticatedJson(
            "POST",
            RecordingApiPaths.segment(recording)?.plus("/complete")
                ?: "/api/device/v1/recordings/${recording.recordingId}/complete",
            JSONObject(),
        )

    fun completeConversation(recording: PendingRecording): JSONObject {
        require(recording.kind == "conversation" && recording.stopReason != "segment_boundary")
        val response = authenticatedJson(
            "POST",
            "${RecordingApiPaths.conversation(recording.recordingId)}/complete",
            JSONObject().put("segment_count", requireNotNull(recording.segmentIndex) + 1),
        )
        ConversationUploadPolicy.verifyCompleted(recording, response)
        return response
    }


    fun downloadPrompt(contentHash: String, destination: File, maximumBytes: Long) {
        require(contentHash.matches(Regex("[0-9a-f]{64}")))
        val connection = open(enrollment!!.serverUrl, "/api/device/v1/prompts/$contentHash", "GET", true)
        try {
            requireSuccess(connection)
            val declared = connection.contentLengthLong
            if (declared > maximumBytes) error("Prompt download exceeds the device limit.")
            FileOutputStream(destination).use { output ->
                connection.inputStream.use { input ->
                    val buffer = ByteArray(16 * 1024)
                    var total = 0L
                    while (true) {
                        if (CallRuntimeState.isBusy()) error("Call started; synchronization paused.")
                        val count = input.read(buffer)
                        if (count < 0) break
                        total += count
                        if (total > maximumBytes) error("Prompt download exceeds the device limit.")
                        output.write(buffer, 0, count)
                    }
                    output.fd.sync()
                }
            }
        } finally {
            connection.disconnect()
        }
    }

    private fun authenticatedJson(
        method: String,
        path: String,
        body: JSONObject?,
        allowEmpty: Boolean = false,
    ): JSONObject = requestJson(method, enrollment!!.serverUrl, path, body, true, allowEmpty)

    private fun requestJson(
        method: String,
        baseUrl: String,
        path: String,
        body: JSONObject?,
        authenticate: Boolean,
        allowEmpty: Boolean = false,
    ): JSONObject {
        val connection = open(baseUrl, path, method, authenticate)
        try {
            if (body != null) {
                val bytes = body.toString().toByteArray(Charsets.UTF_8)
                connection.doOutput = true
                connection.setFixedLengthStreamingMode(bytes.size)
                connection.outputStream.use { it.write(bytes) }
            }
            requireSuccess(connection)
            if (connection.responseCode == HttpURLConnection.HTTP_NO_CONTENT && allowEmpty) return JSONObject()
            val bytes = readBounded(connection.inputStream, 4 * 1024 * 1024)
            return JSONObject(String(bytes, Charsets.UTF_8))
        } finally {
            connection.disconnect()
        }
    }

    private fun open(baseUrl: String, path: String, method: String, authenticate: Boolean): HttpsURLConnection {
        val base = URL(baseUrl)
        require(base.protocol == "https" && base.host.isNotBlank() && base.path.isEmpty())
        val connection = URL(base, path).openConnection() as HttpsURLConnection
        connection.requestMethod = method
        connection.connectTimeout = 10_000
        connection.readTimeout = 35_000
        connection.instanceFollowRedirects = false
        connection.setRequestProperty("Accept", "application/json")
        connection.setRequestProperty("Content-Type", "application/json")
        connection.setRequestProperty("User-Agent", "IVRdroid/${BuildConfig.VERSION_NAME}")
        if (authenticate) {
            val credentials = requireNotNull(enrollment)
            connection.setRequestProperty("Authorization", "Bearer ${credentials.deviceToken}")
            connection.setRequestProperty("CF-Access-Client-Id", credentials.serviceClientId)
            connection.setRequestProperty("CF-Access-Client-Secret", credentials.serviceClientSecret)
        }
        return connection
    }

    private fun requireSuccess(connection: HttpsURLConnection) {
        if (connection.responseCode in 200..299) return
        val message = runCatching {
            val stream = connection.errorStream ?: return@runCatching ""
            val bytes = readBounded(stream, 16 * 1024)
            JSONObject(String(bytes, Charsets.UTF_8)).optString("detail")
        }.getOrDefault("")
        error(message.takeIf { it.isNotBlank() } ?: "Server request failed (${connection.responseCode}).")
    }

    private fun readBounded(input: InputStream, maximum: Int): ByteArray {
        val output = ByteArrayOutputStream(minOf(maximum, 64 * 1024))
        input.use {
            val buffer = ByteArray(8192)
            var total = 0
            while (true) {
                val count = it.read(buffer)
                if (count < 0) break
                total += count
                require(total <= maximum) { "Server response exceeded the device limit." }
                output.write(buffer, 0, count)
            }
        }
        return output.toByteArray()
    }
}
