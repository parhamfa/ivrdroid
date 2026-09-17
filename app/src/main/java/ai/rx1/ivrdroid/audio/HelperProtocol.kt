package ai.rx1.ivrdroid.audio

data class HelperBridgeState(
    val current: String,
    val lastResult: String,
    val activeRevision: Long? = null,
    val stagedRevision: Long? = null,
    val sessionPath: List<String> = emptyList(),
    val helperVersion: String = "NOT_INSTALLED",
    val sourceCommit: String = "unknown",
    val recordingCapable: Boolean = false,
    val runtimeVersions: Set<Int> = emptySet(),
    val callControlCapable: Boolean = false,
    val conversationRecordingCapable: Boolean = false,
    val promptBargeInCapable: Boolean = false,
    val sessionAuditCapable: Boolean = false,
) {
    val isIdle: Boolean
        get() = current == HelperProtocol.READY

    val isArmingPrivacy: Boolean
        get() = current == HelperProtocol.ARMING_PRIVACY

    val hasClaimedSession: Boolean
        get() = current == HelperProtocol.WAITING_FOR_CALL
}

data class HelperCapabilities(
    val recordingCapable: Boolean,
    val runtimeVersions: Set<Int>,
    val callControlCapable: Boolean,
    val conversationRecordingCapable: Boolean,
    val promptBargeInCapable: Boolean,
    val sessionAuditCapable: Boolean = false,
)

object HelperCapabilityProtocol {
    fun parse(wire: String): HelperCapabilities {
        val values = wire.split(';')
        return HelperCapabilities(
            recordingCapable = values.contains("recording=1"),
            runtimeVersions = values.singleOrNull { it.startsWith("runtime=") }
                ?.removePrefix("runtime=")
                ?.split(',')
                ?.mapNotNull(String::toIntOrNull)
                ?.filter { it in 1..4 }
                ?.toSet()
                .orEmpty(),
            callControlCapable = values.contains("call_control=2"),
            conversationRecordingCapable = values.contains("conversation_recording=1"),
            promptBargeInCapable = values.contains("prompt_barge_in=1"),
            sessionAuditCapable = values.contains("session_audit=1"),
        )
    }
}

object HelperProtocol {
    const val START_MENU_REQUEST = "START_MENU\n"
    fun startMenuRequest(callId: String): String {
        val parsed = java.util.UUID.fromString(callId)
        require(parsed.toString() == callId) { "Call identifier must be a canonical UUID." }
        return "START_MENU $callId\n"
    }
    fun stageRevisionRequest(revisionId: Long, manifestSha256: String): String =
        "STAGE_REVISION $revisionId $manifestSha256\n"
    fun activateStagedRequest(revisionId: Long): String = "ACTIVATE_STAGED $revisionId\n"
    const val READY = "READY"
    const val ARMING_PRIVACY = "ARMING_PRIVACY"
    const val WAITING_FOR_CALL = "WAITING_FOR_CALL"
    const val INITIAL_STATUS = "NOT_INSTALLED"
    const val INITIAL_LAST_RESULT = "NONE"
    const val MAXIMUM_FILE_BYTES = 128L
    const val MAXIMUM_SESSION_PATH_BYTES = 8192L
}

object RecordingCapacityProtocol {
    const val PREFIX = "IVRDROID_RECORDING_CAPACITY_V2"

    fun format(
        voicemailBytes: Long,
        voicemailCount: Int,
        conversationBytes: Long,
        conversationCount: Int,
        filesystemFreeBytes: Long,
    ): String {
        require(listOf(voicemailBytes, conversationBytes, filesystemFreeBytes).all { it >= 0 })
        require(voicemailCount >= 0 && conversationCount >= 0)
        return "$PREFIX $voicemailBytes $voicemailCount $conversationBytes " +
            "$conversationCount $filesystemFreeBytes\n"
    }
}
