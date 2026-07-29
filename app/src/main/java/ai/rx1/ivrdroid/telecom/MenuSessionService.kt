package ai.rx1.ivrdroid.telecom

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.SystemClock
import android.telecom.TelecomManager
import android.util.Log
import ai.rx1.ivrdroid.MainActivity
import ai.rx1.ivrdroid.R
import ai.rx1.ivrdroid.audio.HelperCommand
import ai.rx1.ivrdroid.audio.HelperEvent
import ai.rx1.ivrdroid.audio.RootAudioTrigger
import ai.rx1.ivrdroid.ivr.IvrAction
import ai.rx1.ivrdroid.ivr.IvrEngine
import ai.rx1.ivrdroid.ivr.IvrFlow

class MenuSessionService : Service() {
    private val mainHandler = Handler(Looper.getMainLooper())
    private val engine = IvrEngine(IvrFlow.default())
    private var running = false
    private var pendingOperation: PendingOperation? = null
    private var lastLoggedStatus: String? = null

    private val statusPoll = object : Runnable {
        override fun run() {
            if (!running) return
            pollHelper()
            if (running) mainHandler.postDelayed(this, STATUS_POLL_INTERVAL_MS)
        }
    }

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
        startForeground(
            NOTIFICATION_ID,
            buildNotification(getString(R.string.ivr_session_starting)),
        )
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_START_MENU && !running) {
            running = true
            engine.reset()
            dispatch(engine.start())
            mainHandler.post(statusPoll)
        }
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        running = false
        mainHandler.removeCallbacks(statusPoll)
        pendingOperation = null
        engine.reset()
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun dispatch(actions: List<IvrAction>) {
        actions.forEach { action ->
            if (!running) return
            when (action) {
                is IvrAction.PlayPrompt -> playPrompt(action.promptId)
                is IvrAction.AwaitDigit -> listenForDigit(action.timeoutMs)
                is IvrAction.Disconnect -> {
                    finishSession(
                        endCall = true,
                        reason = action.reason,
                    )
                }
            }
        }
    }

    private fun playPrompt(promptId: String) {
        val command = HelperCommand.forPrompt(promptId)
        if (command == null) {
            finishSession(endCall = true, reason = "unknown-prompt:$promptId")
            return
        }
        queueCommand(
            command,
            PendingOperation.Prompt(
                promptId = promptId,
                baselineStatus = RootAudioTrigger.readStatus(this),
                startedAtMs = SystemClock.elapsedRealtime(),
            ),
            getString(R.string.ivr_session_playing, promptId),
        )
    }

    private fun listenForDigit(timeoutMs: Long) {
        queueCommand(
            HelperCommand.ListenDtmf,
            PendingOperation.Digit(
                baselineStatus = RootAudioTrigger.readStatus(this),
                startedAtMs = SystemClock.elapsedRealtime(),
                engineTimeoutMs = timeoutMs,
            ),
            getString(R.string.ivr_session_listening),
        )
    }

    private fun queueCommand(
        command: HelperCommand,
        operation: PendingOperation,
        notificationText: String,
    ) {
        if (pendingOperation != null) {
            finishSession(endCall = true, reason = "overlapping-helper-command")
            return
        }
        pendingOperation = operation
        if (!RootAudioTrigger.requestCommand(this, command)) {
            finishSession(endCall = true, reason = "helper-command-write-failed")
            return
        }
        updateNotification(notificationText)
    }

    private fun pollHelper() {
        val operation = pendingOperation ?: return
        val now = SystemClock.elapsedRealtime()
        if (now - operation.startedAtMs > operation.maximumDurationMs) {
            finishSession(endCall = true, reason = "helper-command-timeout")
            return
        }

        val status = RootAudioTrigger.readStatus(this)
        if (status == operation.baselineStatus) return
        if (status != lastLoggedStatus) {
            Log.i(TAG, "Helper status: $status")
            lastLoggedStatus = status
        }

        when (val event = HelperEvent.parse(status)) {
            is HelperEvent.Intermediate -> Unit
            is HelperEvent.Error -> {
                finishSession(endCall = true, reason = "helper:${event.status}")
            }
            is HelperEvent.Unknown -> {
                finishSession(endCall = true, reason = "unknown-helper-status:${event.status}")
            }
            HelperEvent.CallEnded -> finishSession(endCall = false, reason = "call-ended")
            is HelperEvent.PromptDone -> {
                if (operation !is PendingOperation.Prompt ||
                    operation.promptId != event.promptId
                ) {
                    finishSession(endCall = true, reason = "unexpected-prompt-completion")
                    return
                }
                pendingOperation = null
                dispatch(engine.onPromptFinished())
            }
            is HelperEvent.Digit -> {
                if (operation !is PendingOperation.Digit) {
                    finishSession(endCall = true, reason = "unexpected-digit")
                    return
                }
                pendingOperation = null
                dispatch(engine.onDigit(event.digit))
            }
            HelperEvent.DigitTimeout -> {
                if (operation !is PendingOperation.Digit) {
                    finishSession(endCall = true, reason = "unexpected-digit-timeout")
                    return
                }
                pendingOperation = null
                dispatch(engine.onTimeout())
            }
        }
    }

    private fun finishSession(endCall: Boolean, reason: String) {
        if (!running) return
        running = false
        mainHandler.removeCallbacks(statusPoll)
        pendingOperation = null
        engine.reset()
        Log.i(TAG, "Finishing menu session: $reason")

        if (endCall) {
            @Suppress("DEPRECATION")
            val ended = if (
                checkSelfPermission(Manifest.permission.ANSWER_PHONE_CALLS) ==
                PackageManager.PERMISSION_GRANTED
            ) {
                runCatching {
                    (getSystemService(Context.TELECOM_SERVICE) as TelecomManager).endCall()
                }.onFailure { error ->
                    Log.e(TAG, "Telecom rejected the end-call request.", error)
                }.getOrDefault(false)
            } else {
                false
            }
            Log.i(TAG, "Telecom end-call request result: $ended")
        }
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    private fun createNotificationChannel() {
        val manager = getSystemService(NotificationManager::class.java)
        manager.createNotificationChannel(
            NotificationChannel(
                NOTIFICATION_CHANNEL_ID,
                getString(R.string.ivr_notification_channel),
                NotificationManager.IMPORTANCE_LOW,
            ),
        )
    }

    private fun buildNotification(text: String): Notification {
        val activityIntent = Intent(this, MainActivity::class.java)
        val pendingIntent = PendingIntent.getActivity(
            this,
            0,
            activityIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
        return Notification.Builder(this, NOTIFICATION_CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_launcher)
            .setContentTitle(getString(R.string.app_name))
            .setContentText(text)
            .setContentIntent(pendingIntent)
            .setOngoing(true)
            .build()
    }

    private fun updateNotification(text: String) {
        getSystemService(NotificationManager::class.java)
            .notify(NOTIFICATION_ID, buildNotification(text))
    }

    private sealed interface PendingOperation {
        val baselineStatus: String
        val startedAtMs: Long
        val maximumDurationMs: Long

        data class Prompt(
            val promptId: String,
            override val baselineStatus: String,
            override val startedAtMs: Long,
        ) : PendingOperation {
            override val maximumDurationMs: Long = PROMPT_OPERATION_TIMEOUT_MS
        }

        data class Digit(
            override val baselineStatus: String,
            override val startedAtMs: Long,
            val engineTimeoutMs: Long,
        ) : PendingOperation {
            override val maximumDurationMs: Long =
                engineTimeoutMs + DTMF_OPERATION_GRACE_MS
        }
    }

    companion object {
        private const val TAG = "IVRdroidMenu"
        private const val ACTION_START_MENU = "ai.rx1.ivrdroid.action.START_MENU"
        private const val NOTIFICATION_CHANNEL_ID = "ivr_session"
        private const val NOTIFICATION_ID = 1701
        private const val STATUS_POLL_INTERVAL_MS = 75L
        private const val PROMPT_OPERATION_TIMEOUT_MS = 20_000L
        private const val DTMF_OPERATION_GRACE_MS = 4_000L

    }
}
