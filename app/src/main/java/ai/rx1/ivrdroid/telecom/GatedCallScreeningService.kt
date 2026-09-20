package ai.rx1.ivrdroid.telecom

import java.util.concurrent.atomic.AtomicLong

import android.Manifest
import android.content.pm.PackageManager
import android.telecom.Call
import android.telecom.CallScreeningService
import android.util.Log
import ai.rx1.ivrdroid.audio.RootAudioTrigger
import ai.rx1.ivrdroid.control.PendingCallEvent
import ai.rx1.ivrdroid.control.SessionAuditSettings
import ai.rx1.ivrdroid.control.SecureControlStore
import ai.rx1.ivrdroid.telecom.external.TelecomCallKey
import java.time.Instant
import java.util.UUID

class GatedCallScreeningService : CallScreeningService() {
    override fun onScreenCall(callDetails: Call.Details) {
        if (callDetails.callDirection != Call.Details.DIRECTION_INCOMING) return

        // Respond immediately and always allow the call. Non-matching callers continue to the
        // existing Android dialer without being blocked, rejected, hidden, or silenced.
        respondToCall(
            callDetails,
            CallResponse.Builder()
                .setDisallowCall(false)
                .setRejectCall(false)
                .setSilenceCall(false)
                .setSkipCallLog(false)
                .setSkipNotification(false)
                .build(),
        )

        val decision = CallerPolicyEngine.decide(this, callDetails.handle?.schemeSpecificPart)
        val callId = UUID.randomUUID().toString()
        val startedAt = Instant.now()
        val startedElapsed = android.os.SystemClock.elapsedRealtime()
        if (!decision.shouldHandle) {
            queueEvent(callId, startedAt, decision, "STOCK_DIALER", 0, emptyList())
            Log.i(TAG, "Incoming call remained with the stock dialer under the active caller policy.")
            return
        }
        if (checkSelfPermission(Manifest.permission.ANSWER_PHONE_CALLS) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            queueEvent(callId, startedAt, decision, "ANSWER_PERMISSION_MISSING", 0, emptyList())
            Log.w(TAG, "IVR caller matched, but answer permission is missing.")
            return
        }
        val event = buildEvent(callId, startedAt, decision, "IN_PROGRESS", 0, emptyList())
        if (!IncomingCallAdmission.register(this, event, TelecomCallKey.from(callDetails), startedElapsed)) {
            queueEvent(callId, startedAt, decision, "AMBIGUOUS_INCOMING_CALL", 0, emptyList())
        }
    }

    private fun queueEvent(callId: String, startedAt: Instant, decision: CallerPolicyDecision,
        result: String, duration: Int, menuPath: List<String>) {
        SecureControlStore.enqueueCall(this, buildEvent(callId, startedAt, decision, result, duration, menuPath))
    }

    private fun buildEvent(
        callId: String,
        startedAt: Instant,
        decision: CallerPolicyDecision,
        result: String,
        duration: Int,
        menuPath: List<String>,
    ): PendingCallEvent {
        val revision = RootAudioTrigger.readState(this).activeRevision
        val audit = SessionAuditSettings.applied(this)
        val event = PendingCallEvent(
                callId = callId,
                startedAt = startedAt.toString(),
                caller = decision.canonicalCaller,
                policyDecision = decision.reason,
                revisionId = revision,
                menuPath = menuPath,
                result = result.take(64),
                durationSeconds = duration,
                auditPolicyVersion = audit.version.takeIf { audit.enabled && result != "STOCK_DIALER" },
                auditQuotaBytes = audit.quotaBytes.takeIf { audit.enabled && result != "STOCK_DIALER" },
            )
        return event
    }

    private companion object {
        const val TAG = "IVRdroidScreen"
        val latestIncomingCall = AtomicLong()
    }
}
