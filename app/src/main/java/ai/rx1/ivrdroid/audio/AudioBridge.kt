package ai.rx1.ivrdroid.audio

interface AudioBridge {
    val availability: BridgeAvailability

    fun prepare(sessionId: String, profile: DeviceAudioProfile): BridgeResult

    fun playPrompt(
        promptId: String,
        onFinished: (BridgeResult) -> Unit,
    ): BridgeResult

    fun startDtmf(onDigit: (Char) -> Unit): BridgeResult

    fun stopDtmf(): BridgeResult

    fun restore(sessionId: String): BridgeResult
}

sealed interface BridgeAvailability {
    data object Ready : BridgeAvailability
    data class Unavailable(val reason: String) : BridgeAvailability
}

sealed interface BridgeResult {
    data object Ok : BridgeResult
    data class Unavailable(val reason: String) : BridgeResult
    data class Failed(val reason: String, val recoverable: Boolean) : BridgeResult
}

