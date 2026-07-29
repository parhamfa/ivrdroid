package ai.rx1.ivrdroid.audio

enum class HelperCommand(val wireBody: String) {
    StartMenu("START_MENU\n"),
    PlayMain("PLAY_MAIN\n"),
    PlaySales("PLAY_SALES\n"),
    PlaySupport("PLAY_SUPPORT\n"),
    PlayOperator("PLAY_OPERATOR\n"),
    ListenDtmf("LISTEN_DTMF\n"),
    ;

    companion object {
        fun forPrompt(promptId: String): HelperCommand? = when (promptId) {
            "main-menu" -> PlayMain
            "sales-unavailable" -> PlaySales
            "support-unavailable" -> PlaySupport
            "operator-unavailable" -> PlayOperator
            else -> null
        }
    }
}

sealed interface HelperEvent {
    data class PromptDone(val promptId: String) : HelperEvent
    data class Digit(val digit: Char) : HelperEvent
    data object DigitTimeout : HelperEvent
    data object CallEnded : HelperEvent
    data class Error(val status: String) : HelperEvent
    data class Intermediate(val status: String) : HelperEvent
    data class Unknown(val status: String) : HelperEvent

    companion object {
        private val validDigits = ('0'..'9').toSet() + '*' + '#'

        fun parse(status: String): HelperEvent = when (status) {
            "PROMPT_DONE_MAIN" -> PromptDone("main-menu")
            "PROMPT_DONE_SALES" -> PromptDone("sales-unavailable")
            "PROMPT_DONE_SUPPORT" -> PromptDone("support-unavailable")
            "PROMPT_DONE_OPERATOR" -> PromptDone("operator-unavailable")
            "DTMF_TIMEOUT" -> DigitTimeout
            "CALL_ENDED" -> CallEnded
            "READY",
            "RESTORED",
            "WAITING_FOR_CALL",
            "STARTING_MENU",
            "MENU_SERVICE_STARTED",
            "LISTENING_DTMF",
            "NOT_INSTALLED",
            "UNAVAILABLE",
            -> Intermediate(status)
            else -> when {
                status.length == 6 &&
                    status.startsWith("DTMF_") &&
                    status.last() in validDigits -> Digit(status.last())
                status.startsWith("PLAYING_") -> Intermediate(status)
                status.startsWith("ERROR_") ||
                    status == "STOPPED" ||
                    status == "STOPPED_RESTORED" -> Error(status)
                else -> Unknown(status)
            }
        }
    }
}
