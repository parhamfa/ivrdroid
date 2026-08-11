package ai.rx1.ivrdroid.telecom

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.os.Handler
import android.os.Looper
import android.telecom.Call
import android.telecom.CallScreeningService
import android.telecom.TelecomManager
import android.telecom.VideoProfile
import android.util.Log
import ai.rx1.ivrdroid.audio.RootAudioTrigger
import ai.rx1.ivrdroid.control.PendingCallEvent
import ai.rx1.ivrdroid.control.SecureControlStore
import ai.rx1.ivrdroid.telecom.external.OwnedCallRegistry
import ai.rx1.ivrdroid.telecom.external.TelecomCallKey
import java.time.Instant
import java.util.UUID
import java.util.concurrent.atomic.AtomicLong

class GatedCallScreeningService : CallScreeningService() {
    private val mainHandler = Handler(Looper.getMainLooper())

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
        if (CallRuntimeState.isBusy() || !RootAudioTrigger.isIdle(this)) {
            queueEvent(callId, startedAt, decision, "HELPER_BUSY", 0, emptyList())
            Log.w(TAG, "IVR caller matched, but the helper is busy; leaving it ringing.")
            return
        }

        val telecomCallId = TelecomCallKey.from(callDetails)
        val ownershipRegistered = OwnedCallRegistry.registerScreenedCaller(
            this,
            telecomCallId,
            callId,
            callDetails.handle?.schemeSpecificPart,
            startedElapsed,
        )
        if (!RootAudioTrigger.requestStartMenu(this, callId)) {
            if (ownershipRegistered) OwnedCallRegistry.removeSession(this, callId)
            queueEvent(callId, startedAt, decision, "HELPER_REQUEST_FAILED", 0, emptyList())
            Log.e(TAG, "Could not queue the fixed privileged menu handoff.")
            return
        }
        val token = latestIncomingCall.incrementAndGet()
        CallRuntimeState.setBusy(true)
        if (!ownershipRegistered) {
            Log.w(TAG, "Incoming IVR call could not be registered for privileged external-call ownership.")
        } else {
            PrivilegedInCallService.notifyOwnershipChanged(callId)
        }
        queueEvent(callId, startedAt, decision, "IN_PROGRESS", 0, emptyList())
        Log.i(TAG, "Incoming caller matched the active policy; waiting for the helper to claim it.")
        awaitHelperClaim(token, 0, callId, startedAt, startedElapsed, decision)
    }

    private fun awaitHelperClaim(
        token: Long,
        attempt: Int,
        callId: String,
        startedAt: Instant,
        startedElapsed: Long,
        decision: CallerPolicyDecision,
    ) {
        if (latestIncomingCall.get() != token) return

        val state = RootAudioTrigger.readState(this)
        if (state.hasClaimedSession) {
            answerCurrentRingingCall()
            monitorSession(token, callId, startedAt, startedElapsed, decision)
            return
        }
        if (!state.isIdle && !state.isArmingPrivacy) {
            finishEvent(callId, startedAt, startedElapsed, decision, state.lastResult, state.sessionPath)
            Log.e(TAG, "Helper entered ${state.current} before claiming the call; not answering.")
            return
        }
        if (attempt < MAXIMUM_CLAIM_POLLS) {
            mainHandler.postDelayed(
                { awaitHelperClaim(token, attempt + 1, callId, startedAt, startedElapsed, decision) },
                CLAIM_POLL_INTERVAL_MS,
            )
            return
        }

        RootAudioTrigger.cancelPendingStartMenu(this)
        mainHandler.postDelayed(
            { finishClaimTimeout(token, callId, startedAt, startedElapsed, decision) },
            CLAIM_CANCELLATION_SETTLE_MS,
        )
    }

    private fun finishClaimTimeout(
        token: Long,
        callId: String,
        startedAt: Instant,
        startedElapsed: Long,
        decision: CallerPolicyDecision,
    ) {
        val state = RootAudioTrigger.readState(this)
        if (state.hasClaimedSession) {
            answerCurrentRingingCall()
            monitorSession(token, callId, startedAt, startedElapsed, decision)
        } else {
            finishEvent(callId, startedAt, startedElapsed, decision, "HELPER_CLAIM_TIMEOUT", state.sessionPath)
            Log.e(TAG, "Helper did not claim START_MENU; leaving the call to Android.")
        }
    }

    private fun monitorSession(
        token: Long,
        callId: String,
        startedAt: Instant,
        startedElapsed: Long,
        decision: CallerPolicyDecision,
    ) {
        if (latestIncomingCall.get() != token) return
        val state = RootAudioTrigger.readState(this)
        if (SessionMonitorPolicy.isTerminal(state.current)) {
            finishEvent(callId, startedAt, startedElapsed, decision, state.lastResult, state.sessionPath)
            return
        }
        mainHandler.postDelayed(
            { monitorSession(token, callId, startedAt, startedElapsed, decision) },
            SESSION_POLL_INTERVAL_MS,
        )
    }

    private fun finishEvent(
        callId: String,
        startedAt: Instant,
        startedElapsed: Long,
        decision: CallerPolicyDecision,
        result: String,
        menuPath: List<String>,
    ) {
        val duration = ((android.os.SystemClock.elapsedRealtime() - startedElapsed) / 1000L)
            .coerceIn(0, 86_400).toInt()
        queueEvent(callId, startedAt, decision, result, duration, menuPath)
        OwnedCallRegistry.removeSession(this, callId)
        CallRuntimeState.setBusy(false)
    }

    private fun queueEvent(
        callId: String,
        startedAt: Instant,
        decision: CallerPolicyDecision,
        result: String,
        duration: Int,
        menuPath: List<String>,
    ) {
        val revision = RootAudioTrigger.readState(this).activeRevision
        SecureControlStore.enqueueCall(
            this,
            PendingCallEvent(
                callId = callId,
                startedAt = startedAt.toString(),
                caller = decision.canonicalCaller,
                policyDecision = decision.reason,
                revisionId = revision,
                menuPath = menuPath,
                result = result.take(64),
                durationSeconds = duration,
            ),
        )
    }

    @Suppress("DEPRECATION")
    private fun answerCurrentRingingCall() {
        if (checkSelfPermission(Manifest.permission.ANSWER_PHONE_CALLS) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            return
        }
        if (!RootAudioTrigger.readState(this).hasClaimedSession) {
            Log.w(TAG, "Helper no longer owns a waiting session; leaving the call to Android.")
            return
        }

        val telecom = getSystemService(Context.TELECOM_SERVICE) as TelecomManager
        try {
            telecom.acceptRingingCall(VideoProfile.STATE_AUDIO_ONLY)
            Log.i(TAG, "Answer request sent for the policy-matched incoming call.")
        } catch (error: SecurityException) {
            Log.e(TAG, "Android rejected the gated answer request.", error)
        }
    }

    private companion object {
        const val TAG = "IVRdroidScreen"
        const val CLAIM_POLL_INTERVAL_MS = 50L
        const val CLAIM_CANCELLATION_SETTLE_MS = 250L
        const val MAXIMUM_CLAIM_POLLS = 100
        const val SESSION_POLL_INTERVAL_MS = 500L
        val latestIncomingCall = AtomicLong()
    }
}
