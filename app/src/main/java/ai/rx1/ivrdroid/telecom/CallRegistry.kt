package ai.rx1.ivrdroid.telecom

import android.telecom.Call

object CallRegistry {
    fun interface Listener {
        fun onCallsChanged(calls: List<Call>)
    }

    private val calls = linkedSetOf<Call>()
    private val listeners = linkedSetOf<Listener>()

    fun add(call: Call) {
        calls += call
        notifyListeners()
    }

    fun update(call: Call) {
        if (call in calls) notifyListeners()
    }

    fun remove(call: Call) {
        calls -= call
        notifyListeners()
    }

    fun addListener(listener: Listener) {
        listeners += listener
        listener.onCallsChanged(snapshot())
    }

    fun removeListener(listener: Listener) {
        listeners -= listener
    }

    fun snapshot(): List<Call> = calls.toList()

    fun primaryCall(): Call? = calls.firstOrNull { it.currentState == Call.STATE_RINGING }
        ?: calls.firstOrNull { it.currentState == Call.STATE_ACTIVE }
        ?: calls.firstOrNull()

    private fun notifyListeners() {
        val snapshot = snapshot()
        listeners.toList().forEach { it.onCallsChanged(snapshot) }
    }
}
