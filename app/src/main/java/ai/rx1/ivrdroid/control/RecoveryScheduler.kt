package ai.rx1.ivrdroid.control

import android.content.Context
import androidx.work.Constraints
import androidx.work.ExistingPeriodicWorkPolicy
import androidx.work.ExistingWorkPolicy
import androidx.work.NetworkType
import androidx.work.OneTimeWorkRequestBuilder
import androidx.work.PeriodicWorkRequestBuilder
import androidx.work.WorkManager
import androidx.work.Worker
import androidx.work.WorkerParameters
import java.util.concurrent.TimeUnit

object RecoveryScheduler {
    private const val PERIODIC_NAME = "ivrdroid-sync-recovery"
    private const val IMMEDIATE_NAME = "ivrdroid-sync-now"

    fun ensure(context: Context) {
        val constraints = Constraints.Builder().setRequiredNetworkType(NetworkType.CONNECTED).build()
        val request = PeriodicWorkRequestBuilder<RecoveryWorker>(15, TimeUnit.MINUTES)
            .setConstraints(constraints)
            .build()
        WorkManager.getInstance(context).enqueueUniquePeriodicWork(
            PERIODIC_NAME,
            ExistingPeriodicWorkPolicy.UPDATE,
            request,
        )
    }

    fun enqueueNow(context: Context) {
        val request = OneTimeWorkRequestBuilder<RecoveryWorker>()
            .setConstraints(Constraints.Builder().setRequiredNetworkType(NetworkType.CONNECTED).build())
            .build()
        WorkManager.getInstance(context).enqueueUniqueWork(
            IMMEDIATE_NAME,
            ExistingWorkPolicy.REPLACE,
            request,
        )
    }

    fun cancel(context: Context) {
        WorkManager.getInstance(context).cancelUniqueWork(IMMEDIATE_NAME)
        WorkManager.getInstance(context).cancelUniqueWork(PERIODIC_NAME)
    }
}

class RecoveryWorker(context: Context, parameters: WorkerParameters) : Worker(context, parameters) {
    override fun doWork(): Result = when (SyncCoordinator.synchronize(applicationContext)) {
        is SyncResult.Success, SyncResult.NotEnrolled -> Result.success()
        SyncResult.Busy -> Result.retry()
        is SyncResult.Failed -> Result.retry()
    }
}
