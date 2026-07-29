package ai.rx1.ivrdroid.audio

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class HelperProtocolTest {
    @Test
    fun promptIdsMapOnlyToFixedCommands() {
        assertEquals("START_MENU\n", HelperCommand.StartMenu.wireBody)
        assertEquals(HelperCommand.PlayMain, HelperCommand.forPrompt("main-menu"))
        assertEquals(
            HelperCommand.PlaySupport,
            HelperCommand.forPrompt("support-unavailable"),
        )
        assertNull(HelperCommand.forPrompt("../../arbitrary.wav"))
        assertTrue(HelperCommand.entries.all { it.wireBody.endsWith('\n') })
    }

    @Test
    fun statusesParseToBoundedEvents() {
        assertEquals(
            HelperEvent.PromptDone("main-menu"),
            HelperEvent.parse("PROMPT_DONE_MAIN"),
        )
        assertEquals(HelperEvent.Digit('1'), HelperEvent.parse("DTMF_1"))
        assertEquals(HelperEvent.Digit('#'), HelperEvent.parse("DTMF_#"))
        assertEquals(HelperEvent.DigitTimeout, HelperEvent.parse("DTMF_TIMEOUT"))
        assertEquals(
            HelperEvent.Intermediate("MENU_SERVICE_STARTED"),
            HelperEvent.parse("MENU_SERVICE_STARTED"),
        )
        assertEquals(HelperEvent.Error("ERROR_CAPTURE"), HelperEvent.parse("ERROR_CAPTURE"))
        assertEquals(HelperEvent.Unknown("DTMF_A"), HelperEvent.parse("DTMF_A"))
    }
}
