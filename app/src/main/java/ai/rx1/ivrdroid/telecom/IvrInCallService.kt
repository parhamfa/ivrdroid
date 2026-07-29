package ai.rx1.ivrdroid.telecom

import android.content.Intent
import android.os.Handler
import android.os.Looper
import android.telecom.Call
import android.telecom.InCallService
import android.telecom.VideoProfile
import ai.rx1.ivrdroid.IvrPreferences

class IvrInCallService : InCallService() {
    private val mainHandler = Handler(Looper.getMainLooper())
    private val callbacks = mutableMapOf<Call, Call.Callback>()
    private lateinit var sessionController: CallSessionController

    override fun onCreate() {
        super.onCreate()
        sessionController = CallSessionController(applicationContext)
    }

    override fun onCallAdded(call: Call) {
        super.onCallAdded(call)

        val callback = object : Call.Callback() {
            override fun onStateChanged(changedCall: Call, state: Int) {
                CallRegistry.update(changedCall)
                if (isAllowlisted(changedCall)) {
                    sessionController.onCallStateChanged(changedCall, state)
                }
                if (state == Call.STATE_RINGING) maybeAutoAnswer(changedCall)
            }

            override fun onDetailsChanged(changedCall: Call, details: Call.Details) {
                CallRegistry.update(changedCall)
            }
        }
        callbacks[call] = callback
        call.registerCallback(callback, mainHandler)
        CallRegistry.add(call)
        if (isAllowlisted(call)) {
            sessionController.onCallStateChanged(call, call.currentState)
        }

        startActivity(
            Intent(this, CallActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP),
        )
        maybeAutoAnswer(call)
    }

    override fun onCallRemoved(call: Call) {
        callbacks.remove(call)?.let(call::unregisterCallback)
        sessionController.onCallRemoved(call)
        CallRegistry.remove(call)
        super.onCallRemoved(call)
    }

    override fun onDestroy() {
        callbacks.forEach { (call, callback) -> call.unregisterCallback(callback) }
        callbacks.clear()
        sessionController.close()
        super.onDestroy()
    }

    private fun maybeAutoAnswer(call: Call) {
        if (!IvrPreferences.isAutoAnswerEnabled(this)) return
        if (!isAllowlisted(call)) return
        if (call.currentState != Call.STATE_RINGING) return

        mainHandler.postDelayed({
            if (
                call.currentState == Call.STATE_RINGING &&
                IvrPreferences.isAutoAnswerEnabled(this)
            ) {
                call.answer(VideoProfile.STATE_AUDIO_ONLY)
            }
        }, AUTO_ANSWER_DELAY_MS)
    }

    private fun isAllowlisted(call: Call): Boolean =
        TestCallGate.matches(call.details.handle?.schemeSpecificPart)

    private companion object {
        const val AUTO_ANSWER_DELAY_MS = 750L
    }
}
