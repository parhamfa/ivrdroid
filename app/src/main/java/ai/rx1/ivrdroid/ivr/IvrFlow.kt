package ai.rx1.ivrdroid.ivr

data class IvrFlow(
    val startNodeId: String,
    val nodes: Map<String, IvrNode>,
) {
    fun validationErrors(): List<String> {
        val errors = mutableListOf<String>()
        if (startNodeId !in nodes) errors += "Start node '$startNodeId' does not exist."

        nodes.forEach { (key, node) ->
            if (key != node.id) errors += "Node key '$key' does not match id '${node.id}'."
            if (node.promptId.isBlank()) errors += "Node '$key' has no prompt."
            if (node.timeoutMs <= 0) errors += "Node '$key' has a non-positive timeout."
            if (node.maxRetries < 0) errors += "Node '$key' has a negative retry count."
            if (node.terminalAfterPrompt && node.transitions.isNotEmpty()) {
                errors += "Terminal node '$key' cannot have digit transitions."
            }

            node.transitions.forEach { (digit, target) ->
                if (digit !in VALID_DIGITS) {
                    errors += "Node '$key' has invalid DTMF key '$digit'."
                }
                if (target !in nodes) {
                    errors += "Node '$key' targets missing node '$target'."
                }
            }
        }
        return errors
    }

    companion object {
        private val VALID_DIGITS = ('0'..'9').toSet() + '*' + '#'

        fun default(): IvrFlow = IvrFlow(
            startNodeId = "main",
            nodes = listOf(
                IvrNode(
                    id = "main",
                    promptId = "main-menu",
                    transitions = mapOf(
                        '1' to "sales",
                        '2' to "support",
                        '0' to "operator",
                    ),
                ),
                IvrNode("sales", "sales-unavailable", terminalAfterPrompt = true),
                IvrNode("support", "support-unavailable", terminalAfterPrompt = true),
                IvrNode("operator", "operator-unavailable", terminalAfterPrompt = true),
            ).associateBy(IvrNode::id),
        )
    }
}

data class IvrNode(
    val id: String,
    val promptId: String,
    val transitions: Map<Char, String> = emptyMap(),
    val timeoutMs: Long = 8_000,
    val maxRetries: Int = 2,
    val terminalAfterPrompt: Boolean = false,
)

