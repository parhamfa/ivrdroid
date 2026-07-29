package ai.rx1.ivrdroid.audio

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class HelperProtocolTest {
    @Test
    fun exposesOnlyTheFixedStartMenuRequest() {
        assertEquals("START_MENU\n", HelperProtocol.START_MENU_REQUEST)
        assertTrue(HelperProtocol.START_MENU_REQUEST.endsWith('\n'))
        assertTrue(HelperProtocol.START_MENU_REQUEST.length < HelperProtocol.MAXIMUM_FILE_BYTES)
    }

    @Test
    fun readyStateAllowsARequestAndWaitingAcknowledgesIt() {
        assertTrue(HelperBridgeState("READY", "NONE").isIdle)
        assertFalse(HelperBridgeState("PLAYING_MAIN", "NONE").isIdle)
        assertFalse(HelperBridgeState("UNAVAILABLE", "NONE").isIdle)
        assertTrue(
            HelperBridgeState("WAITING_FOR_CALL", "NONE").hasClaimedSession,
        )
        assertFalse(HelperBridgeState("READY", "NONE").hasClaimedSession)
    }

    @Test
    fun protocolUsesOneExplicitClaimState() {
        assertEquals("WAITING_FOR_CALL", HelperProtocol.WAITING_FOR_CALL)
    }
}
