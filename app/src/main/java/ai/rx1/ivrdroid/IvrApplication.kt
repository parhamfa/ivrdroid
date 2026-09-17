package ai.rx1.ivrdroid

import android.app.Application
import ai.rx1.ivrdroid.audio.RootAudioTrigger
import ai.rx1.ivrdroid.telecom.LocalCallSession

class IvrApplication : Application() {
    override fun onCreate() {
        super.onCreate()
        RootAudioTrigger.initialize(this)
        // Restore ownership before any service, activity or network synchronization starts.
        LocalCallSession.start(this)
        // Android retains process-death evidence across an app restart. Read only
        // this package, bounded and off the Telecom/main thread, without tuning LMKD.
        if (android.os.Build.VERSION.SDK_INT >= 30) Thread({
            runCatching {
                val preferences = getSharedPreferences("process-recovery-diagnostics", MODE_PRIVATE)
                val previous = preferences.getLong("last_exit_timestamp", 0)
                val manager = getSystemService(android.app.ActivityManager::class.java)
                val exits = manager.getHistoricalProcessExitReasons(packageName, 0, 4).filter { it.timestamp > previous }
                for (exit in exits.sortedBy { it.timestamp }) {
                    ai.rx1.ivrdroid.control.RecordingFailureJournal.capture(this, "app_process_exit",
                        componentReadiness = LocalCallSession.readiness,
                        processExitReason = "pid=${exit.pid}; process=${exit.processName}; reason=${exit.reason}; status=${exit.status}; " +
                            "importance=${exit.importance}; pss_kib=${exit.pss}; rss_kib=${exit.rss}; exited_at_ms=${exit.timestamp}")
                }
                exits.maxOfOrNull { it.timestamp }?.let { preferences.edit().putLong("last_exit_timestamp", it).apply() }
            }
        }, "ivr-exit-evidence").start()
    }
}
