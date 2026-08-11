package ai.rx1.ivrdroid.audio

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assert.assertThrows
import org.junit.Test

class HelperProtocolTest {
    @Test
    fun formatsFrozenV2RecordingCapacityCanonically() {
        assertEquals(
            "IVRDROID_RECORDING_CAPACITY_V2 12 3 456 7 8910\n",
            RecordingCapacityProtocol.format(12, 3, 456, 7, 8910),
        )
    }

    @Test
    fun exposesOnlyTheFixedStartMenuRequest() {
        assertEquals("START_MENU\n", HelperProtocol.START_MENU_REQUEST)
        assertTrue(HelperProtocol.START_MENU_REQUEST.endsWith('\n'))
        assertTrue(HelperProtocol.START_MENU_REQUEST.length < HelperProtocol.MAXIMUM_FILE_BYTES)
    }

    @Test
    fun includesOnlyACanonicalCallUuidInTheV3CompatibleRequest() {
        val callId = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
        assertEquals("START_MENU $callId\n", HelperProtocol.startMenuRequest(callId))
        assertThrows(IllegalArgumentException::class.java) {
            HelperProtocol.startMenuRequest(callId.uppercase())
        }
        assertThrows(IllegalArgumentException::class.java) {
            HelperProtocol.startMenuRequest("not-a-uuid")
        }
    }

    @Test
    fun readyStateAllowsARequestAndWaitingAcknowledgesIt() {
        assertTrue(HelperBridgeState("READY", "NONE").isIdle)
        assertFalse(HelperBridgeState("PLAYING_MAIN", "NONE").isIdle)
        assertFalse(HelperBridgeState("UNAVAILABLE", "NONE").isIdle)
        assertTrue(
            HelperBridgeState("ARMING_PRIVACY", "NONE").isArmingPrivacy,
        )
        assertFalse(
            HelperBridgeState("READY", "NONE").isArmingPrivacy,
        )
        assertTrue(
            HelperBridgeState("WAITING_FOR_CALL", "NONE").hasClaimedSession,
        )
        assertFalse(HelperBridgeState("READY", "NONE").hasClaimedSession)
    }

    @Test
    fun protocolUsesOneExplicitClaimState() {
        assertEquals("ARMING_PRIVACY", HelperProtocol.ARMING_PRIVACY)
        assertEquals("WAITING_FOR_CALL", HelperProtocol.WAITING_FOR_CALL)
    }

    @Test
    fun parsesPromptBargeInCapabilityWithoutExpandingRuntimeVersions() {
        val parsed = HelperCapabilityProtocol.parse(
            "runtime=1,2,3,4,5;recording=1;call_control=1;" +
                "conversation_recording=1;prompt_barge_in=1",
        )
        assertEquals(setOf(1, 2, 3, 4), parsed.runtimeVersions)
        assertTrue(parsed.promptBargeInCapable)
        assertFalse(HelperCapabilityProtocol.parse("runtime=1,2,3,4").promptBargeInCapable)
    }
}
