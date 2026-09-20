package ai.rx1.ivrdroid.telecom

import android.content.Context
import android.os.SystemClock
import ai.rx1.ivrdroid.audio.RootAudioTrigger
import ai.rx1.ivrdroid.control.CallEventPayload
import ai.rx1.ivrdroid.control.CallSafetySettings
import ai.rx1.ivrdroid.control.PendingCallEvent
import ai.rx1.ivrdroid.control.RecordingFailureJournal
import ai.rx1.ivrdroid.control.SecureControlStore
import ai.rx1.ivrdroid.control.SessionAuditFiles
import ai.rx1.ivrdroid.telecom.external.BootIdentity
import ai.rx1.ivrdroid.telecom.external.CallRecoveryBridge
import ai.rx1.ivrdroid.telecom.external.OwnedCallRegistry
import ai.rx1.ivrdroid.telecom.external.TelecomCallSnapshot
import ai.rx1.ivrdroid.telecom.external.TelecomCallState
import org.json.JSONObject
import java.io.File
import java.time.Instant
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

/** The lifetime of the original caller, including answering, menus and transfers.
 * This journal exists whether or not recording is enabled. Recovery never replays
 * answer/dial actions: Telecom and the native guardian decide what actually exists.
 */
object LocalCallSession {
    private var loaded = false
    private var journal: JSONObject? = null
    private var schedulerStarted = false
    private var absentSince: Long? = null
    private var firstEmptySequence = 0L
    private val processStartedElapsed = SystemClock.elapsedRealtime()
    private val processGeneration = java.util.UUID.randomUUID().toString()
    private val answerRecoveryBarrier = ai.rx1.ivrdroid.telecom.external.TelecomRecoveryBarrier()
    private var restoredSession = false
    private var recordedControllerRecovery = ""
    @Volatile var readiness = "RECOVERING_LOCAL_SESSION"
        private set
    private val executor = Executors.newSingleThreadScheduledExecutor()
    private fun directory(context: Context) = SessionAuditFiles.directory(File(context.filesDir, "call-session"))
    private fun path(context: Context) = File(directory(context), "active.enc")
    private fun save(context: Context) {
        val value = journal ?: return
        SessionAuditFiles.encrypt(path(context), "call-session:1", value.toString().toByteArray(Charsets.UTF_8))
    }

    @Synchronized
    fun owns(session: String?): Boolean = session != null && journal?.getJSONObject("event")?.getString("call_id") == session

    @Synchronized
    fun answerRequested(session: String): Boolean = owns(session) && journal?.optBoolean("answer_requested") == true

    private fun finalizing(context: Context) = SessionAuditFiles.directory(File(directory(context), "finalizing"))
    private fun resultWire(context: Context, id: String, extension: String): List<String>? = runCatching {
        String(SessionAuditFiles.read(File(context.filesDir, "bridge/call-results/$id.$extension"), 512), Charsets.US_ASCII).trim().split(' ')
    }.getOrNull()

    @Synchronized
    fun canHandleNewCaller(context: Context): Boolean {
        restore(context)
        // A newly screened ringing call can legitimately arrive before idle polling.
        // The helper still requires an independently verified single original caller.
        val native = CallRecoveryBridge.snapshot(context) ?: return false
        val now = SystemClock.elapsedRealtime()
        return loaded && journal == null && RootAudioTrigger.isIdle(context) && native.parsed &&
            native.boot == BootIdentity.current() && native.elapsedMs <= now && now - native.elapsedMs <= 2000 &&
            !native.emergency && native.calls.size <= 1
    }

    @Synchronized
    fun expiredSession(): String? = journal?.takeIf {
        it.getString("boot") == BootIdentity.current() && it.getLong("deadline_elapsed") > 0 &&
            SystemClock.elapsedRealtime() >= it.getLong("deadline_elapsed")
    }?.getJSONObject("event")?.getString("call_id")

    @Synchronized
    fun restore(context: Context) {
        if (loaded) return
        val file = path(context)
        journal = if (!file.exists()) null else runCatching {
            JSONObject(String(SessionAuditFiles.decrypt(file, "call-session:1", 64 * 1024), Charsets.UTF_8)).also {
                require(it.getInt("schema") == 1)
                CallEventPayload.decode(it.getJSONObject("event"))
            }
        }.getOrElse {
            readiness = "SESSION_JOURNAL_UNREADABLE"
            CallRuntimeState.setBusy(true)
            RecordingFailureJournal.capture(context, "session_restore", error = it)
            return
        }
        loaded = true
        restoredSession = journal != null
        journal?.let { value ->
            val event = CallEventPayload.decode(value.optJSONObject("terminal") ?: value.getJSONObject("event")).copy(
                auditPolicyVersion = value.optLong("audit_policy_version", 0).takeIf { it > 0 },
                auditQuotaBytes = value.optLong("audit_quota_bytes", 0).takeIf { it > 0 })
            if (SecureControlStore.pendingCalls(context).none { it.callId == event.callId }) SecureControlStore.enqueueCall(context, event)
        }
        CallRuntimeState.setBusy(true) // Cleared only by independent local reconciliation.
        readiness = if (journal == null) "WAITING_NATIVE_RECONCILIATION" else "RECOVERING_ACTIVE_SESSION"
    }

    @Synchronized
    fun start(context: Context) {
        restore(context)
        if (schedulerStarted) return
        schedulerStarted = true
        val application = context.applicationContext
        executor.scheduleWithFixedDelay({ runCatching { reconcile(application) }.onFailure {
            readiness = "SESSION_RECOVERY_FAILED"
            RecordingFailureJournal.capture(application, "session_reconcile", error = it)
        }
            runCatching { finalizePending(application) }.onFailure {
                RecordingFailureJournal.capture(application, "session_finalization", error = it)
            }
        }, 0, 500, TimeUnit.MILLISECONDS)
    }

    @Synchronized
    fun begin(context: Context, event: PendingCallEvent, callerKey: String, startedElapsed: Long) {
        restore(context)
        require(loaded && journal == null) { "Previous caller ownership is still recovering." }
        val policy = CallSafetySettings.applied(context)
        journal = JSONObject().put("schema", 1).put("boot", requireNotNull(BootIdentity.current()))
            .put("event", CallEventPayload.encode(event))
            .put("audit_policy_version", event.auditPolicyVersion ?: JSONObject.NULL)
            .put("audit_quota_bytes", event.auditQuotaBytes ?: JSONObject.NULL)
            .put("started_elapsed", startedElapsed).put("caller_key", callerKey).put("caller_native", JSONObject.NULL)
            .put("policy_version", policy?.version ?: 0).put("maximum_seconds", policy?.maximumSeconds ?: 3600)
            .put("phase", "ANSWER_PENDING").put("answer_requested", false).put("answered_elapsed", 0)
            .put("deadline_elapsed", 0).put("last_observed_elapsed", startedElapsed)
        save(context)
        restoredSession = false
        absentSince = null
        CallRuntimeState.setBusy(true); readiness = "ACTIVE_SESSION"
    }

    @Synchronized
    fun beforeAnswer(context: Context): Boolean {
        val value = journal ?: return false
        if (value.getBoolean("answer_requested")) return false
        value.put("answer_requested", true).put("answer_generation", processGeneration).put("phase", "ANSWER_REQUESTED")
        save(context)
        return true
    }

    /** Reconcile an interrupted answer using the exact owned ringing Call. Never
     * repeat an answer in one process or answer a different newly arriving caller. */
    @Synchronized
    fun recoverAnswer(context: Context, calls: List<TelecomCallSnapshot>): String? {
        val value = journal ?: return null
        if (!restoredSession || value.optBoolean("answer_requested") || value.optString("answer_generation") == processGeneration ||
            value.getLong("answered_elapsed") != 0L || value.getString("boot") != BootIdentity.current()) return null
        val session = value.getJSONObject("event").getString("call_id")
        val native = CallRecoveryBridge.snapshot(context)
        if (!RootAudioTrigger.readState(context).hasClaimedSession ||
            answerRecoveryBarrier.observe(native, calls, BootIdentity.current(), session, SystemClock.elapsedRealtime()) !=
            ai.rx1.ivrdroid.telecom.external.TelecomReadiness.READY) return null
        val caller = calls.singleOrNull()?.takeIf {
            it.ownerSessionId == session && !it.emergency && it.state == TelecomCallState.RINGING &&
                it.direction == ai.rx1.ivrdroid.telecom.external.TelecomCallDirection.INCOMING
        } ?: return null
        value.put("answer_requested", true).put("answer_generation", processGeneration)
            .put("phase", "ANSWER_REQUESTED").put("caller_native", caller.nativeId)
        save(context)
        return caller.id
    }

    @Synchronized
    fun observeCalls(context: Context, calls: List<TelecomCallSnapshot>) {
        val value = journal ?: return
        val caller = calls.singleOrNull { it.id == value.getString("caller_key") }
        if (caller == null || caller.state == TelecomCallState.DISCONNECTED) {
            // Callback restoration can temporarily expose no Call objects. Only
            // an explicit disconnect (or independent native reconciliation) is evidence.
            if (caller != null && value.getLong("answered_elapsed") > 0) {
                if (!value.has("ended_elapsed")) value.put("ended_elapsed", SystemClock.elapsedRealtime())
                if (caller?.disconnectKind == ai.rx1.ivrdroid.telecom.external.TelecomDisconnectKind.REMOTE)
                    value.put("disconnect_result", "REMOTE_HANGUP")
                save(context)
            }
            return
        }
        var changed = false
        if (value.isNull("caller_native") && caller.nativeId != null) { value.put("caller_native", caller.nativeId); changed = true }
        if (value.getLong("answered_elapsed") == 0L && caller.state in setOf(TelecomCallState.ACTIVE, TelecomCallState.HOLDING)) {
            val now = SystemClock.elapsedRealtime()
            android.util.Log.i("IVRdroidAdmission", "call=${value.getJSONObject("event").getString("call_id")} active_observed_ms=$now")
            value.put("answered_elapsed", now).put("deadline_elapsed", now + value.getInt("maximum_seconds") * 1000L)
            changed = true
        }
        if (changed) save(context)
    }

    @Synchronized
    fun finishRequested(context: Context, result: String, menuPath: List<String>, sessionId: String? = null) {
        val value = journal ?: return
        if (sessionId != null && value.getJSONObject("event").getString("call_id") != sessionId) return
        if (value.optString("requested_result") != "MAX_CALL_DURATION") value.put("requested_result", result)
        value.put("menu_path", org.json.JSONArray(menuPath))
        save(context)
        // An idle helper alone cannot establish that the caller has disconnected.
        reconcile(context)
    }

    @Synchronized
    fun reconcile(context: Context) {
        restore(context)
        if (!loaded) return
        val controller = ai.rx1.ivrdroid.telecom.external.ExternalCallRuntimeStatus.recovery
        val recoveryIdentity = "${controller.startedElapsedMs}:${controller.readiness}"
        if (controller.readiness != "NOT_BOUND" && recoveryIdentity != recordedControllerRecovery) {
            recordedControllerRecovery = recoveryIdentity
            android.util.Log.i("IVRdroidAdmission", "controller_recovery duration_ms=${controller.durationMs} readiness=${controller.readiness} phase=${controller.phase}")
        }
        val now = SystemClock.elapsedRealtime()
        val boot = BootIdentity.current() ?: return
        val native = CallRecoveryBridge.snapshot(context)
        if (native == null || !native.parsed || native.boot != boot || native.elapsedMs > now || now - native.elapsedMs > 2000) {
            readiness = "WAITING_NATIVE_RECONCILIATION"; absentSince = null; return
        }
        val value = journal
        // Never use an idle snapshot produced before this process or this caller
        // existed to complete a newly created or restored session.
        val evidenceAfter = maxOf(processStartedElapsed,
            value?.takeIf { it.getString("boot") == boot }?.getLong("started_elapsed") ?: 0L)
        if (native.elapsedMs < evidenceAfter) {
            absentSince = null; readiness = "WAITING_NATIVE_RECONCILIATION"; return
        }
        if (value == null) {
            if (native.calls.isNotEmpty() || !RootAudioTrigger.isIdle(context)) {
                absentSince = null; readiness = "CALL_PRESENT"; CallRuntimeState.setBusy(true); return
            }
            if (absentSince == null) { absentSince = now; firstEmptySequence = native.sequence }
            val settled = now - requireNotNull(absentSince) >= 500 && native.sequence != firstEmptySequence && native.elapsedMs > requireNotNull(absentSince)
            readiness = if (settled) "READY" else "WAITING_NATIVE_RECONCILIATION"
            CallRuntimeState.setBusy(!settled || IncomingCallAdmission.isPending(context))
            return
        }
        val event = CallEventPayload.decode(value.getJSONObject("event")).copy(
            auditPolicyVersion = value.optLong("audit_policy_version", 0).takeIf { it > 0 },
            auditQuotaBytes = value.optLong("audit_quota_bytes", 0).takeIf { it > 0 })
        val sameBoot = value.getString("boot") == boot
        if (sameBoot) {
            val lifetime = runCatching { String(SessionAuditFiles.read(File(context.filesDir, "bridge/call-lifetime"), 512), Charsets.US_ASCII).trim().split(' ') }.getOrNull()
            if (lifetime != null && lifetime.size == 7 && lifetime[0] == "CALL1" && lifetime[1] == event.callId && lifetime[2] == boot) {
                val answered = lifetime[5].toLong(); val deadline = lifetime[6].toLong(); val seconds = lifetime[4].toInt()
                require(answered in 1..now && seconds in 60..86400 && deadline == answered + seconds * 1000L)
                if (value.getLong("deadline_elapsed") != deadline) {
                    value.put("answered_elapsed", answered).put("deadline_elapsed", deadline)
                        .put("policy_version", lifetime[3].toLong()).put("maximum_seconds", seconds)
                    save(context)
                }
            }
            if (value.isNull("caller_native") && native.session == event.callId && native.calls.size == 1 && !value.has("ended_elapsed")) {
                value.put("caller_native", native.calls.single().id); save(context)
            }
        }
        val callerNative = value.optString("caller_native").takeUnless { it == "null" || it.isEmpty() }
        val callerPresent = sameBoot && native.calls.any { it.id == callerNative }
        val helper = RootAudioTrigger.readState(context)
        val sessionRunning = sameBoot && !value.has("ended_elapsed") && native.session == event.callId && !helper.isIdle && native.calls.isNotEmpty()
        if (callerPresent || sessionRunning || (sameBoot && !value.has("ended_elapsed") && callerNative == null && native.calls.isNotEmpty())) {
            absentSince = null; CallRuntimeState.setBusy(true); readiness = "ACTIVE_SESSION"
            if (value.getString("phase") != helper.current) {
                value.put("phase", helper.current).put("last_observed_elapsed", native.elapsedMs); save(context)
            }
            return
        }
        if (absentSince == null) { absentSince = now; firstEmptySequence = native.sequence; return }
        if (now - requireNotNull(absentSince) < 500 || native.sequence == firstEmptySequence || native.elapsedMs <= requireNotNull(absentSince)) return
        if (!value.has("ended_elapsed")) {
            value.put("ended_elapsed", if (sameBoot) native.elapsedMs else value.getLong("last_observed_elapsed"))
            if (!sameBoot) value.put("ended_at_unknown", true)
        }
        val release = resultWire(context, event.callId, "released")
        val released = release?.let { it.size == 4 && it[0] == "RELEASE1" && it[1] == event.callId &&
            it[2] == value.getString("boot") && (it[3].toLongOrNull() ?: Long.MAX_VALUE) <= now } == true
        // Unanswered requests and legacy restored journals can finish on verified
        // helper idle; normal calls require the per-session resource-release proof.
        val noAnswer = value.getLong("answered_elapsed") == 0L && helper.isIdle && native.calls.isEmpty()
        val recoveredIdle = helper.isIdle && !callerPresent && (native.calls.isEmpty() || IncomingCallAdmission.isPending(context))
        if (sameBoot && !released && !noAnswer && !recoveredIdle) { readiness = "WAITING_RESOURCE_RELEASE"; return }
        value.put("released_elapsed", now).put("cleanup_status", if (released || noAnswer) "complete" else "recovered")
        save(context)
        Files.move(path(context).toPath(), File(finalizing(context), "${event.callId}.enc").toPath(), StandardCopyOption.ATOMIC_MOVE)
        SessionAuditFiles.sync(directory(context)); SessionAuditFiles.sync(finalizing(context))
        OwnedCallRegistry.removeSession(context, event.callId)
        journal = null; absentSince = null
        CallRuntimeState.setBusy(native.calls.isNotEmpty() || !helper.isIdle || IncomingCallAdmission.isPending(context))
        readiness = if (CallRuntimeState.isBusy()) "CALL_PRESENT" else "READY"
    }

    /** No active-session lock: ended sessions cannot hold the next caller's slot. */
    private fun finalizePending(context: Context) {
        if (IncomingCallAdmission.isPending(context)) return
        for (file in finalizing(context).listFiles().orEmpty()) {
            if (!file.name.matches(Regex("[0-9a-f-]{36}\\.enc"))) continue
            runCatching { finalizeFile(context, file) }.onFailure {
                RecordingFailureJournal.capture(context, "session_finalization", callId = file.name.removeSuffix(".enc"), error = it)
            }
        }
    }

    private fun finalizeFile(context: Context, file: File) {
        val value = JSONObject(String(SessionAuditFiles.decrypt(file, "call-session:1", 64 * 1024), Charsets.UTF_8))
        val event = CallEventPayload.decode(value.getJSONObject("event"))
        val outcome = resultWire(context, event.callId, "outcome")?.takeIf {
            it.size == 5 && it[0] == "END1" && it[1] == event.callId && it[2] == value.getString("boot")
        }
        val sameBoot = value.getString("boot") == BootIdentity.current()
        val requested = value.optString("requested_result").takeIf { it.isNotEmpty() }
        if (outcome == null && requested == null && sameBoot &&
            SystemClock.elapsedRealtime() - value.getLong("released_elapsed") < 10_000) return
        val result = if (requested == "MAX_CALL_DURATION") requested else value.optString("disconnect_result").takeIf { it.isNotEmpty() } ?: outcome?.get(4) ?: requested ?:
            if (!sameBoot) "RECOVERED_AFTER_REBOOT" else "END_DETAILS_UNAVAILABLE"
        if (result == "END_DETAILS_UNAVAILABLE" && value.optBoolean("pending_reported")) return
        val ended = value.getLong("ended_elapsed").coerceAtLeast(value.getLong("started_elapsed"))
        val elapsed = ended - value.getLong("started_elapsed")
        val ownPath = runCatching { String(SessionAuditFiles.read(File(context.filesDir,
            "bridge/call-results/${event.callId}.path"), 8192), Charsets.US_ASCII).trim() }.getOrNull()
        val menu = ownPath?.takeUnless { it == "UNAVAILABLE" || it == "none" }?.split('>')?.take(64)
            ?: value.optJSONArray("menu_path")?.let { path -> List(path.length()) { path.getString(it) } } ?: event.menuPath
        SecureControlStore.enqueueCall(context, event.copy(result = result, durationSeconds = (elapsed / 1000).coerceAtMost(86400).toInt(),
            endedAt = if (value.optBoolean("ended_at_unknown")) null else Instant.parse(event.startedAt).plusMillis(elapsed).toString(),
            cleanupStatus = if (result == "END_DETAILS_UNAVAILABLE") "pending" else value.getString("cleanup_status"), menuPath = menu,
            auditPolicyVersion = value.optLong("audit_policy_version", 0).takeIf { it > 0 },
            auditQuotaBytes = value.optLong("audit_quota_bytes", 0).takeIf { it > 0 }))
        // Keep incomplete evidence repairable when a delayed per-session result arrives.
        if (result != "END_DETAILS_UNAVAILABLE") { require(file.delete()); SessionAuditFiles.sync(finalizing(context)) }
        else SessionAuditFiles.encrypt(file, "call-session:1", value.put("pending_reported", true).toString().toByteArray())
    }
}
