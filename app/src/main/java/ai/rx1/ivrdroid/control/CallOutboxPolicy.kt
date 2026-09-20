package ai.rx1.ivrdroid.control

/** An HTTP response owns only the immutable snapshot sent in that request. */
object CallOutboxPolicy {
    fun afterAcknowledgement(current: List<PendingCallEvent>, submitted: List<PendingCallEvent>, accepted: Set<String>): List<PendingCallEvent> {
        val sent = submitted.associateBy { it.callId }
        return current.filterNot { event ->
            event.result != "IN_PROGRESS" && event.callId in accepted &&
                sent[event.callId]?.let { it.generation == event.generation &&
                    CanonicalJson.encode(CallEventPayload.encode(it)) == CanonicalJson.encode(CallEventPayload.encode(event)) } == true
        }
    }
}
