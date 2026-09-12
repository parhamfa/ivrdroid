package ai.rx1.ivrdroid.control

import android.content.Context
import android.os.PowerManager
import android.os.SystemClock
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

/** Audit work never invokes the operator coordinator or its required-recording failure callback. */
class SessionAuditHandoffWorker(context: Context) {
    private val application = context.applicationContext
    private val executor = Executors.newSingleThreadScheduledExecutor { task -> Thread(task, "ivrdroid-session-audit") }
    private val wakeLock = (application.getSystemService(Context.POWER_SERVICE) as PowerManager)
        .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "IVRdroid:session-audit").apply { setReferenceCounted(false) }
    @Volatile private var stopAt = Long.MAX_VALUE

    fun start() {
        executor.scheduleWithFixedDelay({
            try {
                if (SessionAuditSettings.applied(application).enabled || SessionAuditSpool.count(application) > 0) {
                    wakeLock.acquire(30_000)
                    SessionAuditSpool.reconcile(application)
                }
            } catch (_: Exception) {
                // The spool retains durable files for a later retry. Call handling is independent.
            } finally {
                if (SystemClock.elapsedRealtime() >= stopAt) {
                    executor.shutdown()
                    if (wakeLock.isHeld) wakeLock.release()
                }
            }
        }, 0, 500, TimeUnit.MILLISECONDS)
    }

    fun stopAfterFinalizerDrain() {
        stopAt = SystemClock.elapsedRealtime() + 10_000
    }
}
