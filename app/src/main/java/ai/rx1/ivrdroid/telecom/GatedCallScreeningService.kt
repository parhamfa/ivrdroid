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
import ai.rx1.ivrdroid.IvrPreferences
import ai.rx1.ivrdroid.audio.RootAudioTrigger
import java.util.concurrent.atomic.AtomicLong

class GatedCallScreeningService : CallScreeningService() {
    private val mainHandler = Handler(Looper.getMainLooper())

    override fun onScreenCall(callDetails: Call.Details) {
        if (callDetails.callDirection != Call.Details.DIRECTION_INCOMING) return

        val token = latestIncomingCall.incrementAndGet()

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

        val caller = callDetails.handle?.schemeSpecificPart
        if (!TestCallGate.matches(caller)) {
            Log.i(TAG, "Incoming call did not match the test allowlist; no action taken.")
            return
        }
        if (!IvrPreferences.isAutoAnswerEnabled(this)) {
            Log.i(TAG, "Allowlisted call matched, but gated auto-answer is disabled.")
            return
        }
        if (checkSelfPermission(Manifest.permission.ANSWER_PHONE_CALLS) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            Log.w(TAG, "Allowlisted call matched, but answer permission is missing.")
            return
        }
        if (!RootAudioTrigger.isIdle(this)) {
            Log.w(TAG, "Allowlisted call matched, but the helper is busy; leaving it ringing.")
            return
        }

        if (!RootAudioTrigger.requestStartMenu(this)) {
            Log.e(TAG, "Could not queue the fixed privileged menu handoff.")
            return
        }
        Log.i(TAG, "Allowlisted incoming call matched; waiting for the helper to claim it.")
        awaitHelperClaim(token, 0)
    }

    private fun awaitHelperClaim(token: Long, attempt: Int) {
        if (latestIncomingCall.get() != token) return

        val state = RootAudioTrigger.readState(this)
        if (state.hasClaimedSession) {
            answerCurrentRingingCall()
            return
        }
        if (!state.isIdle && !state.isArmingPrivacy) {
            Log.e(TAG, "Helper entered ${state.current} before claiming the call; not answering.")
            return
        }
        if (attempt < MAXIMUM_CLAIM_POLLS) {
            mainHandler.postDelayed(
                { awaitHelperClaim(token, attempt + 1) },
                CLAIM_POLL_INTERVAL_MS,
            )
            return
        }

        RootAudioTrigger.cancelPendingStartMenu(this)
        mainHandler.postDelayed(
            { finishClaimTimeout(token) },
            CLAIM_CANCELLATION_SETTLE_MS,
        )
    }

    private fun finishClaimTimeout(token: Long) {
        if (latestIncomingCall.get() != token) return
        val state = RootAudioTrigger.readState(this)
        if (state.hasClaimedSession) {
            answerCurrentRingingCall()
        } else {
            Log.e(TAG, "Helper did not claim START_MENU; leaving the call to Android.")
        }
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
            Log.i(TAG, "Answer request sent for allowlisted incoming call.")
        } catch (error: SecurityException) {
            Log.e(TAG, "Android rejected the gated answer request.", error)
        }
    }

    private companion object {
        const val TAG = "IVRdroidScreen"
        const val CLAIM_POLL_INTERVAL_MS = 50L
        const val CLAIM_CANCELLATION_SETTLE_MS = 250L
        const val MAXIMUM_CLAIM_POLLS = 100
        val latestIncomingCall = AtomicLong()
    }
}
