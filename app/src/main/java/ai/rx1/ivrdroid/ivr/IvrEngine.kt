package ai.rx1.ivrdroid.ivr

class IvrEngine(private val flow: IvrFlow) {
    private var currentNode: IvrNode? = null
    private var retries = 0
    private var awaitingDigit = false

    init {
        val errors = flow.validationErrors()
        require(errors.isEmpty()) { errors.joinToString(separator = " ") }
    }

    fun start(): List<IvrAction> {
        reset()
        return enter(flow.startNodeId)
    }

    fun onPromptFinished(): List<IvrAction> {
        val node = currentNode ?: return emptyList()
        if (node.terminalAfterPrompt) {
            awaitingDigit = false
            return listOf(IvrAction.Disconnect("terminal-node:${node.id}"))
        }
        awaitingDigit = true
        return listOf(IvrAction.AwaitDigit(node.timeoutMs))
    }

    fun onDigit(digit: Char): List<IvrAction> {
        val node = currentNode ?: return emptyList()
        if (!awaitingDigit) return emptyList()

        val target = node.transitions[digit]
        return if (target != null) {
            enter(target)
        } else {
            retryOrDisconnect("invalid-digit:$digit")
        }
    }

    fun onTimeout(): List<IvrAction> {
        if (!awaitingDigit || currentNode == null) return emptyList()
        return retryOrDisconnect("input-timeout")
    }

    fun reset() {
        currentNode = null
        retries = 0
        awaitingDigit = false
    }

    private fun enter(nodeId: String): List<IvrAction> {
        val node = requireNotNull(flow.nodes[nodeId])
        currentNode = node
        retries = 0
        awaitingDigit = false
        return listOf(IvrAction.PlayPrompt(node.promptId))
    }

    private fun retryOrDisconnect(reason: String): List<IvrAction> {
        val node = requireNotNull(currentNode)
        awaitingDigit = false
        retries += 1
        return if (retries > node.maxRetries) {
            listOf(IvrAction.Disconnect(reason))
        } else {
            listOf(IvrAction.PlayPrompt(node.promptId))
        }
    }
}

sealed interface IvrAction {
    data class PlayPrompt(val promptId: String) : IvrAction
    data class AwaitDigit(val timeoutMs: Long) : IvrAction
    data class Disconnect(val reason: String) : IvrAction
}

