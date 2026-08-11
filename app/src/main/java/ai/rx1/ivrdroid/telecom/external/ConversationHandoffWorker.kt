package ai.rx1.ivrdroid.telecom.external

import android.content.Context
import android.os.PowerManager
import android.os.SystemClock
import android.util.Log
import ai.rx1.ivrdroid.control.ConversationHandoffIdentity
import ai.rx1.ivrdroid.control.ConversationHandoffResult
import ai.rx1.ivrdroid.control.RecordingSpool
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledExecutorService
import java.util.concurrent.TimeUnit

data class ConversationHandoffSession(
    val identity: ConversationHandoffIdentity,
    val bootId: String,
)

/** Pure sequencing core: one pair is durably ingested before its acknowledgement is exposed. */
class ConversationHandoffController(
    private val ingest: (ConversationHandoffIdentity) -> ConversationHandoffResult?,
    private val publish: (ConversationHandoffRecord) -> Boolean,
    private val onFailure: (ConversationHandoffIdentity) -> Unit,
) {
    private var pending: ConversationHandoffRecord? = null
    private var nextIngestElapsedMs = 0L

    fun tick(session: ConversationHandoffSession, nowElapsedMs: Long) {
        require(nowElapsedMs >= 0)
        pending?.let { record ->
            if (publish(record)) {
                pending = null
                nextIngestElapsedMs = Math.addExact(nowElapsedMs, ACK_VISIBILITY_MS)
            }
            return
        }
        if (nowElapsedMs < nextIngestElapsedMs) return
        val outcome = ingest(session.identity) ?: return
        val record = ConversationHandoffRecord(
            outcome.identity.callId,
            outcome.identity.revisionId,
            outcome.identity.blockId,
            outcome.recordingId,
            outcome.segmentIndex,
            session.bootId,
            nowElapsedMs,
            if (outcome.succeeded) ConversationHandoffResultKind.OK else ConversationHandoffResultKind.FAILED,
            outcome.reason,
        )
        pending = record
        if (!outcome.succeeded) onFailure(outcome.identity)
        if (publish(record)) {
            pending = null
            nextIngestElapsedMs = Math.addExact(nowElapsedMs, ACK_VISIBILITY_MS)
        }
    }

    internal fun pendingForTest(): ConversationHandoffRecord? = pending

    private companion object {
        const val ACK_VISIBILITY_MS = 250L
    }
}

/**
 * Local-only worker used while Telecom owns the call. It never invokes DeviceApi or network sync.
 * A short post-call drain covers the helper's finalizer when Telecom destroys InCallService first.
 */
class ConversationHandoffWorker(
    context: Context,
    private val onFailure: (ConversationHandoffIdentity, Long) -> Unit,
) {
    private val application = context.applicationContext
    private val executor: ScheduledExecutorService = Executors.newSingleThreadScheduledExecutor { task ->
        Thread(task, "ivrdroid-conversation-handoff").apply { isDaemon = false }
    }
    private val controller = ConversationHandoffController(
        ingest = { identity -> RecordingSpool.handoffNextConversation(application, identity) },
        publish = { record -> CallControlBridge.publishConversationHandoff(application, record) },
        onFailure = { identity -> onFailure(identity, SystemClock.elapsedRealtime()) },
    )
    @Volatile private var session: ConversationHandoffSession? = null
    @Volatile private var stopAtElapsedMs = Long.MAX_VALUE
    private val wakeLock = (application.getSystemService(Context.POWER_SERVICE) as PowerManager)
        .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, WAKE_LOCK_TAG)
        .apply { setReferenceCounted(false) }

    fun start() {
        executor.scheduleWithFixedDelay(::runTick, 0, POLL_INTERVAL_MS, TimeUnit.MILLISECONDS)
    }

    fun updateSession(value: ConversationHandoffSession?) {
        session = value
    }

    fun stopAfterFinalizerDrain() {
        if (stopAtElapsedMs != Long.MAX_VALUE) return
        stopAtElapsedMs = SystemClock.elapsedRealtime() + POST_CALL_DRAIN_MS
        runCatching { wakeLock.acquire(POST_CALL_DRAIN_MS + 1_000L) }
    }

    private fun runTick() {
        val now = SystemClock.elapsedRealtime()
        try {
            session?.let { controller.tick(it, now) }
        } catch (error: Throwable) {
            Log.e(TAG, "Conversation handoff worker tick failed safely.", error)
        } finally {
            if (now >= stopAtElapsedMs) {
                executor.shutdown()
                if (wakeLock.isHeld) runCatching { wakeLock.release() }
            }
        }
    }

    private companion object {
        const val TAG = "IVRdroidHandoff"
        const val WAKE_LOCK_TAG = "IVRdroid:conversation-handoff"
        const val POLL_INTERVAL_MS = 100L
        const val POST_CALL_DRAIN_MS = 10_000L
    }
}
