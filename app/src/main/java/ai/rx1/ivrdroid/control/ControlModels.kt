package ai.rx1.ivrdroid.control

import org.json.JSONArray
import org.json.JSONObject
import java.time.Instant

data class Enrollment(
    val deviceId: String,
    val deviceToken: String,
    val serviceClientId: String,
    val serviceClientSecret: String,
    val serverUrl: String,
)

data class PendingCallEvent(
    val callId: String,
    val startedAt: String,
    val caller: String?,
    val policyDecision: String,
    val revisionId: Long?,
    val menuPath: List<String>,
    val result: String,
    val durationSeconds: Int,
    val events: List<PendingCallSubEvent> = emptyList(),
    val sessionAudit: JSONObject? = null,
    val auditPolicyVersion: Long? = null,
    val auditQuotaBytes: Long? = null,
)

data class PendingCallSubEvent(
    val occurredAt: String,
    val status: String,
    val blockId: String,
    val reason: String,
) {
    init {
        Instant.parse(occurredAt)
        require(status.matches(Regex("[A-Z][A-Z0-9_]{0,63}")))
        require(blockId.matches(Regex("[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}")))
        require(reason.matches(Regex("(?:-|[A-Z][A-Z0-9_]{0,63})")))
    }
}

object CallEventPayload {
    fun preserveEvents(previous: PendingCallEvent, replacement: PendingCallEvent): PendingCallEvent {
        require(previous.callId == replacement.callId)
        return replacement.copy(
            events = replacement.events.ifEmpty { previous.events },
            sessionAudit = replacement.sessionAudit ?: previous.sessionAudit,
            auditPolicyVersion = previous.auditPolicyVersion ?: replacement.auditPolicyVersion,
            auditQuotaBytes = previous.auditQuotaBytes ?: replacement.auditQuotaBytes,
        )
    }

    fun encode(event: PendingCallEvent): JSONObject = JSONObject()
        .put("call_id", event.callId)
        .put("started_at", event.startedAt)
        .put("caller", event.caller ?: JSONObject.NULL)
        .put("policy_decision", event.policyDecision)
        .put("revision_id", event.revisionId ?: JSONObject.NULL)
        .put("menu_path", JSONArray(event.menuPath))
        .put("result", event.result)
        .put("duration_seconds", event.durationSeconds)
        .put("events", encodeEvents(event.events))
        .put("session_audit", event.sessionAudit ?: JSONObject.NULL)

    fun decode(item: JSONObject): PendingCallEvent = PendingCallEvent(
        callId = item.getString("call_id"),
        startedAt = item.getString("started_at"),
        caller = if (item.isNull("caller")) null else item.optString("caller").takeIf { it.isNotEmpty() },
        policyDecision = item.getString("policy_decision"),
        revisionId = item.optLong("revision_id", 0).takeIf { it > 0 },
        menuPath = item.optJSONArray("menu_path")?.let { path -> List(path.length()) { path.getString(it) } } ?: emptyList(),
        result = item.getString("result"),
        durationSeconds = item.optInt("duration_seconds", 0),
        events = decodeEvents(item.optJSONArray("events")),
        sessionAudit = item.optJSONObject("session_audit"),
    )

    fun encodeEvents(events: List<PendingCallSubEvent>): JSONArray = JSONArray().also { array ->
        require(events.size <= 128)
        events.forEach { event ->
            array.put(
                JSONObject()
                    .put("event", "external_call")
                    .put("occurred_at", event.occurredAt)
                    .put("status", event.status)
                    .put("block_id", event.blockId)
                    .put("reason", event.reason),
            )
        }
    }

    fun decodeEvents(array: JSONArray?): List<PendingCallSubEvent> {
        if (array == null) return emptyList()
        require(array.length() <= 128)
        return buildList {
            for (index in 0 until array.length()) {
                val event = array.getJSONObject(index)
                require(event.getString("event") == "external_call")
                add(
                    PendingCallSubEvent(
                        event.getString("occurred_at"),
                        event.getString("status"),
                        event.getString("block_id"),
                        event.getString("reason"),
                    ),
                )
            }
        }
    }
}

data class PendingRecording(
    val recordingId: String,
    val callId: String,
    val revisionId: Long,
    val blockId: String,
    val sequence: Int,
    val capturedAt: String,
    val durationMs: Int,
    val stopReason: String,
    val sizeBytes: Long,
    val sha256: String,
    val kind: String = "voicemail",
    val segmentIndex: Int? = null,
    val partial: Boolean = false,
)

data class SyncStatus(
    val enrolled: Boolean,
    val activeRevision: Long?,
    val stagedRevision: Long?,
    val lastSyncEpochMs: Long,
    val lastError: String?,
)

sealed class SyncResult {
    data class Success(val activeRevision: Long?) : SyncResult()
    data object NotEnrolled : SyncResult()
    data object Busy : SyncResult()
    data class Failed(val message: String) : SyncResult()
}
