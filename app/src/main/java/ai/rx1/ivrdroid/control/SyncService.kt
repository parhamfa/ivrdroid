package ai.rx1.ivrdroid.control

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.net.ConnectivityManager
import android.net.Network
import android.os.IBinder
import ai.rx1.ivrdroid.MainActivity
import ai.rx1.ivrdroid.R
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

class SyncService : Service() {
    private val executor = Executors.newSingleThreadExecutor()
    private val running = AtomicBoolean(false)
    private val handler by lazy { android.os.Handler(mainLooper) }
    private val syncRunnable = Runnable { runSync() }
    private var outageDelayMs = SUCCESS_INTERVAL_MS
    private var bootWifiRecovery: BootWifiRecovery? = null
    private lateinit var connectivity: ConnectivityManager
    private val networkCallback = object : ConnectivityManager.NetworkCallback() {
        override fun onAvailable(network: Network) {
            outageDelayMs = SUCCESS_INTERVAL_MS
            trigger(0)
        }
    }

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
        startForeground(NOTIFICATION_ID, notification("Monitoring configuration"))
        connectivity = getSystemService(ConnectivityManager::class.java)
        connectivity.registerDefaultNetworkCallback(networkCallback)
        RecoveryScheduler.ensure(this)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.getBooleanExtra(EXTRA_BOOT_WIFI_RECOVERY, false) == true && bootWifiRecovery == null) {
            bootWifiRecovery = BootWifiRecovery(this) { bootWifiRecovery = null }.also { it.start() }
        }
        trigger(if (intent?.getBooleanExtra(EXTRA_IMMEDIATE, false) == true) 0 else SUCCESS_INTERVAL_MS)
        return START_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onDestroy() {
        handler.removeCallbacks(syncRunnable)
        bootWifiRecovery?.stop()
        bootWifiRecovery = null
        runCatching { connectivity.unregisterNetworkCallback(networkCallback) }
        executor.shutdownNow()
        super.onDestroy()
    }

    override fun onTimeout(startId: Int, fgsType: Int) {
        stopSelf(startId)
    }

    private fun trigger(delayMs: Long) {
        handler.removeCallbacks(syncRunnable)
        handler.postDelayed(syncRunnable, delayMs)
    }

    private fun runSync() {
        if (!running.compareAndSet(false, true)) return
        executor.execute {
            val result = SyncCoordinator.synchronize(this)
            running.set(false)
            val next = when (result) {
                is SyncResult.Success -> {
                    outageDelayMs = SUCCESS_INTERVAL_MS
                    updateNotification("Revision ${result.activeRevision ?: "built-in"} · synchronized")
                    SUCCESS_INTERVAL_MS
                }
                SyncResult.NotEnrolled -> {
                    stopSelf()
                    return@execute
                }
                SyncResult.Busy -> BUSY_INTERVAL_MS
                is SyncResult.Failed -> {
                    updateNotification("Offline · cached IVR remains active")
                    outageDelayMs = (outageDelayMs * 2).coerceIn(MINIMUM_OUTAGE_MS, MAXIMUM_OUTAGE_MS)
                    outageDelayMs
                }
            }
            trigger(next)
        }
    }

    private fun createNotificationChannel() {
        val manager = getSystemService(NotificationManager::class.java)
        manager.createNotificationChannel(
            NotificationChannel(CHANNEL_ID, "IVRdroid synchronization", NotificationManager.IMPORTANCE_LOW),
        )
    }

    private fun notification(text: String): Notification {
        val intent = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        return Notification.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_launcher)
            .setContentTitle("IVRdroid")
            .setContentText(text)
            .setOngoing(true)
            .setContentIntent(intent)
            .build()
    }

    private fun updateNotification(text: String) {
        getSystemService(NotificationManager::class.java).notify(NOTIFICATION_ID, notification(text))
    }

    companion object {
        private const val CHANNEL_ID = "ivrdroid-sync"
        private const val NOTIFICATION_ID = 1401
        private const val EXTRA_IMMEDIATE = "immediate"
        private const val EXTRA_BOOT_WIFI_RECOVERY = "boot_wifi_recovery"
        private const val SUCCESS_INTERVAL_MS = 60_000L
        private const val BUSY_INTERVAL_MS = 15_000L
        private const val MINIMUM_OUTAGE_MS = 60_000L
        private const val MAXIMUM_OUTAGE_MS = 15 * 60_000L

        fun start(context: Context, immediate: Boolean, bootWifiRecovery: Boolean = false) {
            val intent = Intent(context, SyncService::class.java)
                .putExtra(EXTRA_IMMEDIATE, immediate)
                .putExtra(EXTRA_BOOT_WIFI_RECOVERY, bootWifiRecovery)
            context.startForegroundService(intent)
        }
    }
}
