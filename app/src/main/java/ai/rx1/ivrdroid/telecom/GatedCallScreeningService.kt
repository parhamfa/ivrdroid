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
import ai.rx1.ivrdroid.audio.HelperCommand
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

        Log.i(TAG, "Allowlisted incoming call matched; scheduling audio-only answer.")
        mainHandler.postDelayed(
            {
                if (latestIncomingCall.get() == token) answerCurrentRingingCall()
            },
            ANSWER_DELAY_MS,
        )
    }

    @Suppress("DEPRECATION")
    private fun answerCurrentRingingCall() {
        if (checkSelfPermission(Manifest.permission.ANSWER_PHONE_CALLS) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            return
        }

        val telecom = getSystemService(Context.TELECOM_SERVICE) as TelecomManager
        try {
            telecom.acceptRingingCall(VideoProfile.STATE_AUDIO_ONLY)
            Log.i(TAG, "Answer request sent for allowlisted incoming call.")
            if (RootAudioTrigger.requestCommand(this, HelperCommand.StartMenu)) {
                Log.i(TAG, "Queued the fixed privileged menu handoff.")
            } else {
                Log.e(TAG, "Could not queue the fixed privileged menu handoff.")
            }
        } catch (error: SecurityException) {
            Log.e(TAG, "Android rejected the gated answer request.", error)
        }
    }

    private companion object {
        const val TAG = "IVRdroidScreen"
        const val ANSWER_DELAY_MS = 250L
        val latestIncomingCall = AtomicLong()
    }
}
