package ai.rx1.ivrdroid.telecom.external

class CallOwnershipTracker {
    private val sessions = mutableMapOf<String, String>()

    fun remember(callId: String, sessionId: String) {
        sessions[callId] = sessionId
    }

    fun owner(callId: String): String? = sessions[callId]

    fun remove(callId: String) {
        sessions.remove(callId)
    }

    fun clear() = sessions.clear()
}
