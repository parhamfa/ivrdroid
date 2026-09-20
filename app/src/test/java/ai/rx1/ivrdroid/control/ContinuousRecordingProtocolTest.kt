package ai.rx1.ivrdroid.control

import org.junit.Assert.*
import org.junit.Test

class ContinuousRecordingProtocolTest {
    private val id = "11111111-1111-4111-8111-111111111111"
    private val legacy = "PCM1 session_audit $id $id $id 0 - 1 1700000000000 1000 42 99 4800\n"

    @Test fun legacyAndVersionedReceiptsKeepCaptureSeparateFromFinalization() {
        val old = ContinuousCapture.parse(legacy)
        assertEquals(100L, old.durationMs)
        assertFalse(old.manifest("a".repeat(64), "caller_hangup", false).has("capture_receipt"))
        val current = ContinuousCapture.parse(legacy.replace("PCM1", "PCM2").trimEnd() + " 1100 9000\n")
        assertEquals(100L, current.durationMs)
        assertEquals(1100L, current.manifest("a".repeat(64), "caller_hangup", false)
            .getJSONObject("capture_receipt").getLong("ended_elapsed_ms"))
        assertThrows(IllegalArgumentException::class.java) {
            ContinuousCapture.parse(legacy.replace("PCM1", "PCM2").trimEnd() + " 1100 1050\n")
        }
        assertTrue(ContinuousCapture.parse(legacy.replace("PCM1", "PCM2").trimEnd() + " 1100 0\n")
            .manifest("a".repeat(64), "interrupted", true).getJSONObject("capture_receipt").isNull("finalized_elapsed_ms"))
    }
}
