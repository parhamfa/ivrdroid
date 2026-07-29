package ai.rx1.ivrdroid.audio

/**
 * Safe placeholder for the future session-oriented playback and DTMF bridge.
 *
 * The gated fixed-prompt test uses [RootAudioTrigger] instead. This class remains unavailable
 * until the helper supports call-scoped prompts and DTMF capture. It intentionally never invokes
 * `su`, executes a shell, or mutates mixer state from the Android process.
 */
class PrivilegedHelperAudioBridge : AudioBridge {
    override val availability: BridgeAvailability =
        BridgeAvailability.Unavailable(UNAVAILABLE_REASON)

    override fun prepare(
        sessionId: String,
        profile: DeviceAudioProfile,
    ): BridgeResult = BridgeResult.Unavailable(UNAVAILABLE_REASON)

    override fun playPrompt(
        promptId: String,
        onFinished: (BridgeResult) -> Unit,
    ): BridgeResult = BridgeResult.Unavailable(UNAVAILABLE_REASON)

    override fun startDtmf(onDigit: (Char) -> Unit): BridgeResult =
        BridgeResult.Unavailable(UNAVAILABLE_REASON)

    override fun stopDtmf(): BridgeResult = BridgeResult.Ok

    override fun restore(sessionId: String): BridgeResult = BridgeResult.Ok

    private companion object {
        const val UNAVAILABLE_REASON =
            "Session-oriented prompt and DTMF control is not implemented."
    }
}
