package ai.rx1.ivrdroid.ivr

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class IvrEngineTest {
    @Test
    fun validDigitMovesToTargetAndEndsAfterItsPrompt() {
        val engine = IvrEngine(IvrFlow.default())

        assertEquals(listOf(IvrAction.PlayPrompt("main-menu")), engine.start())
        assertEquals(listOf(IvrAction.AwaitDigit(8_000)), engine.onPromptFinished())
        assertEquals(listOf(IvrAction.PlayPrompt("support-unavailable")), engine.onDigit('2'))
        assertEquals(
            listOf(IvrAction.Disconnect("terminal-node:support")),
            engine.onPromptFinished(),
        )
    }

    @Test
    fun invalidDigitRetriesThenDisconnects() {
        val engine = IvrEngine(IvrFlow.default())
        engine.start()
        engine.onPromptFinished()

        assertEquals(listOf(IvrAction.PlayPrompt("main-menu")), engine.onDigit('9'))
        engine.onPromptFinished()
        assertEquals(listOf(IvrAction.PlayPrompt("main-menu")), engine.onDigit('9'))
        engine.onPromptFinished()
        assertEquals(
            listOf(IvrAction.Disconnect("invalid-digit:9")),
            engine.onDigit('9'),
        )
    }

    @Test
    fun flowValidationRejectsMissingTransitionTarget() {
        val broken = IvrFlow(
            startNodeId = "main",
            nodes = mapOf(
                "main" to IvrNode(
                    id = "main",
                    promptId = "menu",
                    transitions = mapOf('1' to "missing"),
                ),
            ),
        )

        assertTrue(broken.validationErrors().any { "missing node" in it })
    }

    @Test
    fun timeoutUsesTheSameBoundedRetryPolicy() {
        val engine = IvrEngine(IvrFlow.default())
        engine.start()
        engine.onPromptFinished()

        assertEquals(listOf(IvrAction.PlayPrompt("main-menu")), engine.onTimeout())
        engine.onPromptFinished()
        assertEquals(listOf(IvrAction.PlayPrompt("main-menu")), engine.onTimeout())
        engine.onPromptFinished()
        assertEquals(
            listOf(IvrAction.Disconnect("input-timeout")),
            engine.onTimeout(),
        )
    }
}
