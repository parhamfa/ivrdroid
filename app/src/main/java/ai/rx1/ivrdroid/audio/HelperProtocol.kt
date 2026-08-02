package ai.rx1.ivrdroid.audio

data class HelperBridgeState(
    val current: String,
    val lastResult: String,
) {
    val isIdle: Boolean
        get() = current == HelperProtocol.READY

    val isArmingPrivacy: Boolean
        get() = current == HelperProtocol.ARMING_PRIVACY

    val hasClaimedSession: Boolean
        get() = current == HelperProtocol.WAITING_FOR_CALL
}

object HelperProtocol {
    const val START_MENU_REQUEST = "START_MENU\n"
    const val READY = "READY"
    const val ARMING_PRIVACY = "ARMING_PRIVACY"
    const val WAITING_FOR_CALL = "WAITING_FOR_CALL"
    const val INITIAL_STATUS = "NOT_INSTALLED"
    const val INITIAL_LAST_RESULT = "NONE"
    const val MAXIMUM_FILE_BYTES = 64L
}
