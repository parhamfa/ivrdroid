package ai.rx1.ivrdroid.telecom

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.telecom.Call
import android.util.Log
import ai.rx1.ivrdroid.audio.AudioBridge
import ai.rx1.ivrdroid.audio.BridgeResult
import ai.rx1.ivrdroid.audio.DeviceAudioProfile
import ai.rx1.ivrdroid.audio.PrivilegedHelperAudioBridge
import ai.rx1.ivrdroid.ivr.IvrAction
import ai.rx1.ivrdroid.ivr.IvrEngine
import ai.rx1.ivrdroid.ivr.IvrFlow
import java.util.UUID

class CallSessionController(
    context: Context,
    private val bridge: AudioBridge = PrivilegedHelperAudioBridge(),
    private val engine: IvrEngine = IvrEngine(IvrFlow.default()),
) {
    private val mainHandler = Handler(Looper.getMainLooper())
    private val appContext = context.applicationContext
    private var activeCall: Call? = null
    private var sessionId: String? = null
    private var timeout: Runnable? = null

    fun onCallStateChanged(call: Call, state: Int) {
        when (state) {
            Call.STATE_ACTIVE -> beginSession(call)
            Call.STATE_DISCONNECTED, Call.STATE_DISCONNECTING -> {
                if (call === activeCall) endSession()
            }
        }
    }

    fun onCallRemoved(call: Call) {
        if (call === activeCall) endSession()
    }

    fun close() {
        endSession()
    }

    private fun beginSession(call: Call) {
        if (call === activeCall) return
        if (activeCall != null) {
            Log.w(TAG, "Ignoring a second active call while an IVR session exists.")
            return
        }

        val profile = DeviceAudioProfile.forCurrentDevice(appContext)
        if (profile == null) {
            Log.w(TAG, "No exact audio profile matches this device; IVR audio remains disabled.")
            return
        }

        val newSessionId = UUID.randomUUID().toString()
        when (val result = bridge.prepare(newSessionId, profile)) {
            BridgeResult.Ok -> {
                activeCall = call
                sessionId = newSessionId
                dispatch(engine.start())
            }
            is BridgeResult.Unavailable -> {
                Log.w(TAG, result.reason)
                endSession()
            }
            is BridgeResult.Failed -> {
                Log.e(TAG, result.reason)
                bridge.restore(newSessionId)
            }
        }
    }

    private fun dispatch(actions: List<IvrAction>) {
        actions.forEach { action ->
            when (action) {
                is IvrAction.PlayPrompt -> playPrompt(action.promptId)
                is IvrAction.AwaitDigit -> awaitDigit(action.timeoutMs)
                is IvrAction.Disconnect -> {
                    Log.i(TAG, "Ending IVR call: ${action.reason}")
                    activeCall?.disconnect()
                }
            }
        }
    }

    private fun playPrompt(promptId: String) {
        cancelTimeout()
        bridge.stopDtmf()
        when (val result = bridge.playPrompt(promptId) { completion ->
            mainHandler.post {
                when (completion) {
                    BridgeResult.Ok -> {
                        if (activeCall != null) dispatch(engine.onPromptFinished())
                    }
                    is BridgeResult.Unavailable -> {
                        Log.w(TAG, completion.reason)
                        endSession()
                    }
                    is BridgeResult.Failed -> {
                        Log.e(TAG, completion.reason)
                        endSession()
                    }
                }
            }
        }) {
            BridgeResult.Ok -> Unit
            is BridgeResult.Unavailable -> {
                Log.w(TAG, result.reason)
                endSession()
            }
            is BridgeResult.Failed -> {
                Log.e(TAG, result.reason)
                endSession()
            }
        }
    }

    private fun awaitDigit(timeoutMs: Long) {
        when (val result = bridge.startDtmf { digit ->
            mainHandler.post {
                if (activeCall != null) dispatch(engine.onDigit(digit))
            }
        }) {
            BridgeResult.Ok -> {
                timeout = Runnable {
                    if (activeCall != null) dispatch(engine.onTimeout())
                }.also { mainHandler.postDelayed(it, timeoutMs) }
            }
            is BridgeResult.Unavailable -> {
                Log.w(TAG, result.reason)
                endSession()
            }
            is BridgeResult.Failed -> {
                Log.e(TAG, result.reason)
                endSession()
            }
        }
    }

    private fun endSession() {
        cancelTimeout()
        bridge.stopDtmf()
        sessionId?.let { id ->
            when (val result = bridge.restore(id)) {
                BridgeResult.Ok -> Unit
                is BridgeResult.Unavailable -> Log.w(TAG, result.reason)
                is BridgeResult.Failed -> Log.e(TAG, "Audio restore failed: ${result.reason}")
            }
        }
        engine.reset()
        sessionId = null
        activeCall = null
    }

    private fun cancelTimeout() {
        timeout?.let(mainHandler::removeCallbacks)
        timeout = null
    }

    private companion object {
        const val TAG = "IVRdroidSession"
    }
}
