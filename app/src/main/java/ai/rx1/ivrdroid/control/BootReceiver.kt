package ai.rx1.ivrdroid.control

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.os.Build
import ai.rx1.ivrdroid.audio.RootAudioTrigger

class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != Intent.ACTION_BOOT_COMPLETED &&
            intent.action != Intent.ACTION_MY_PACKAGE_REPLACED) return
        // Bootstrap/upgrade the owner-only helper bridge even before enrollment so the root helper
        // never has to create app-owned call-control endpoints.
        RootAudioTrigger.initialize(context)
        if (SecureControlStore.enrollment(context) != null) {
            RecoveryScheduler.ensure(context)
            RecoveryScheduler.enqueueNow(context)
            // Android 12 permits a data-sync foreground service from BOOT_COMPLETED. Android 15
            // no longer does, so newer platforms keep the WorkManager recovery path only.
            if (Build.VERSION.SDK_INT < 35) {
                runCatching {
                    SyncService.start(
                        context,
                        immediate = false,
                        bootWifiRecovery = intent.action == Intent.ACTION_BOOT_COMPLETED,
                    )
                }
            }
        }
    }
}
