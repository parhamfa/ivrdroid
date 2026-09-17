package ai.rx1.ivrdroid.telecom.external

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class ExternalCallAuditStoreTest {
    private val config = ExternalCallConfig("11111111-1111-4111-8111-111111111111", 21,
        "22222222-2222-4222-8222-222222222222", "03136644636", 30_000,
        "33333333-3333-4333-8333-333333333333")
    private val snapshot = ExternalCallSessionSnapshot(config, ExternalCallPhase.SYSTEM_FAILURE,
        null, null, null, 0, 210815501, reason = "HELPER_CANCELLED")

    @Test
    fun recoveryRecognizesOldTerminalEventAndKeepsItsOriginalTime() {
        val original = JSONObject().put("session_id", config.sessionId).put("revision_id", 21)
            .put("block_id", config.blockId).put("elapsed_ms", snapshot.updatedElapsedMs)
            .put("status", "SYSTEM_FAILURE").put("reason", "HELPER_CANCELLED")
            .put("occurred_at", "2026-09-15T06:20:55.469Z")
        val events = JSONArray().put(original)
        assertEquals("2026-09-15T06:20:55.469Z",
            ExternalCallAuditStore.existingTransition(events, snapshot, CallControlStatus.SYSTEM_FAILURE,
                "HELPER_CANCELLED")?.getString("occurred_at"))
        assertNull(ExternalCallAuditStore.existingTransition(events,
            snapshot.copy(updatedElapsedMs = snapshot.updatedElapsedMs + 1000),
            CallControlStatus.SYSTEM_FAILURE, "HELPER_CANCELLED"))
        original.put("boot_id", "44444444-4444-4444-8444-444444444444")
        assertNull(ExternalCallAuditStore.existingTransition(events, snapshot,
            CallControlStatus.SYSTEM_FAILURE, "HELPER_CANCELLED"))
    }
}
