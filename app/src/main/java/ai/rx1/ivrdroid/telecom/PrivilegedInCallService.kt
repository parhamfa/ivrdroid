package ai.rx1.ivrdroid.telecom

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.telecom.Call
import android.telecom.InCallService
import android.telecom.TelecomManager
import android.util.Log
import ai.rx1.ivrdroid.control.SessionAuditHandoffWorker
import ai.rx1.ivrdroid.audio.RootAudioTrigger
import ai.rx1.ivrdroid.telecom.external.AndroidTelecomControl
import ai.rx1.ivrdroid.telecom.external.ExternalCallCoordinator
import ai.rx1.ivrdroid.telecom.external.ConversationHandoffWorker
import java.lang.ref.WeakReference

/**
 * Privileged, deliberately non-UI call-control companion. Android's stock dialer remains the
 * user-facing in-call service; this service observes and controls only UUID-owned IVR legs.
 */
class PrivilegedInCallService : InCallService() {
    private val handler = Handler(Looper.getMainLooper())
    private lateinit var telecom: AndroidTelecomControl
    private lateinit var coordinator: ExternalCallCoordinator
    private lateinit var auditWorker: SessionAuditHandoffWorker
    private lateinit var handoffWorker: ConversationHandoffWorker
    private var running = false
    private val poll = object : Runnable {
        override fun run() {
            if (!running) return
            runCatching { tickCallControl() }
                .onFailure { Log.e(TAG, "External-call controller tick failed closed.", it) }
            handoffWorker.updateSession(coordinator.conversationHandoffSession())
            handler.postDelayed(this, POLL_INTERVAL_MS)
        }
    }

    private fun tickCallControl() {
        LocalCallSession.observeCalls(applicationContext, telecom.calls())
        IncomingCallAdmission.tick(applicationContext, telecom)
        LocalCallSession.recoverAnswer(applicationContext, telecom.calls())?.let { telecom.answerRecoveredCaller(it) }
        val expiredSession = LocalCallSession.expiredSession()
        if (expiredSession == null) {
            coordinator.tick()
            return
        }
        // Callback-triggered ticks obey the same cap as timer ticks. Otherwise
        // disconnecting the first leg could race into an IVR fallback prompt.
        val owned = telecom.calls().filter { it.ownerSessionId == expiredSession && !it.emergency }
        if (owned.isNotEmpty()) {
            LocalCallSession.finishRequested(applicationContext, "MAX_CALL_DURATION",
                RootAudioTrigger.readState(applicationContext).sessionPath, expiredSession)
            owned.forEach { telecom.disconnect(it.id) }
        }
    }

    override fun onCreate() {
        super.onCreate()
        RootAudioTrigger.initialize(this)
        telecom = AndroidTelecomControl(
            this,
            getSystemService(Context.TELECOM_SERVICE) as TelecomManager,
            handler,
        ) { handler.post { if (running) tickCallControl() } }
        coordinator = ExternalCallCoordinator(this, telecom)
        handoffWorker = ConversationHandoffWorker(this) { identity, elapsedMs ->
            // The coordinator serializes worker and Telecom ticks. Calling it here also covers a
            // final receipt published after Telecom has already destroyed this service binding.
            coordinator.recordingHandoffFailed(identity, elapsedMs)
        }
        handoffWorker.start()
        auditWorker = SessionAuditHandoffWorker(this)
        auditWorker.start()
        instance = WeakReference(this)
        running = true
        handler.post(poll)
    }

    override fun onCallAdded(call: Call) {
        super.onCallAdded(call)
        telecom.add(call)
    }

    override fun onCallRemoved(call: Call) {
        telecom.remove(call)
        super.onCallRemoved(call)
    }

    override fun onDestroy() {
        running = false
        handoffWorker.stopAfterFinalizerDrain()
        auditWorker.stopAfterFinalizerDrain()
        handler.removeCallbacksAndMessages(null)
        telecom.close()
        if (instance?.get() === this) instance = null
        super.onDestroy()
    }

    companion object {
        private const val TAG = "IVRdroidInCall"
        private const val POLL_INTERVAL_MS = 250L
        @Volatile private var instance: WeakReference<PrivilegedInCallService>? = null

        fun notifyOwnershipChanged(sessionId: String) {
            instance?.get()?.let { service ->
                service.handler.post {
                    if (service.running) {
                        service.telecom.claimIncomingCaller(
                            sessionId,
                            signedSessionAuthorized = false,
                        )
                        service.tickCallControl()
                    }
                }
            }
        }
    }
}
