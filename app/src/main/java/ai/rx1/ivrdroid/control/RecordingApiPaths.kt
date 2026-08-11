package ai.rx1.ivrdroid.control

object RecordingApiPaths {
    const val CONVERSATIONS = "/api/device/v1/conversation-recordings"

    fun conversation(recordingId: String): String {
        require(recordingId.matches(Regex("[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}")))
        return "$CONVERSATIONS/$recordingId"
    }

    fun segment(recording: PendingRecording): String? = if (recording.kind == "conversation") {
        "${conversation(recording.recordingId)}/segments/${requireNotNull(recording.segmentIndex)}"
    } else {
        null
    }
}
