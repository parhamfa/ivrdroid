package ai.rx1.ivrdroid.telecom

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.os.SystemClock
import ai.rx1.ivrdroid.audio.RootAudioTrigger
import ai.rx1.ivrdroid.control.CallEventPayload
import ai.rx1.ivrdroid.control.PendingCallEvent
import ai.rx1.ivrdroid.control.SecureControlStore
import ai.rx1.ivrdroid.control.SessionAuditFiles
import ai.rx1.ivrdroid.telecom.external.*
import org.json.JSONObject
import java.io.File
import java.time.Instant

/** One exact screened caller may wait for local hardware recovery. No network work here. */
object IncomingCallAdmission {
    private fun file(context: Context) = File(SessionAuditFiles.directory(File(context.filesDir, "call-session")), "incoming.enc")
    private fun read(context: Context): JSONObject? = file(context).takeIf { it.exists() }?.let {
        JSONObject(String(SessionAuditFiles.decrypt(it, "incoming:1", 64 * 1024), Charsets.UTF_8))
    }
    private fun save(context: Context, value: JSONObject) = SessionAuditFiles.encrypt(file(context), "incoming:1", value.toString().toByteArray())
    fun isPending(context: Context): Boolean = file(context).exists()

    @Synchronized
    fun register(context: Context, event: PendingCallEvent, callKey: String, elapsed: Long): Boolean {
        if (file(context).exists()) return false
        if (!OwnedCallRegistry.registerScreenedCaller(context, callKey, event.callId, event.caller, elapsed)) return false
        save(context, JSONObject().put("boot", BootIdentity.current()).put("event", CallEventPayload.encode(event))
            .put("audit_policy", event.auditPolicyVersion ?: JSONObject.NULL).put("audit_quota", event.auditQuotaBytes ?: JSONObject.NULL)
            .put("started_elapsed", elapsed).put("prepared", false).put("seen", false))
        CallRuntimeState.setBusy(true) // Cancel/yield media I/O from the first ring.
        PrivilegedInCallService.notifyOwnershipChanged(event.callId)
        return true
    }

    private fun clear(context: Context, session: String) {
        require(file(context).delete())
        SessionAuditFiles.sync(requireNotNull(file(context).parentFile))
        SessionAuditFiles.write(File(context.filesDir, "bridge/incoming-caller"), "UNAVAILABLE\n".toByteArray())
        if (!LocalCallSession.owns(session)) OwnedCallRegistry.removeSession(context, session)
    }

    @Synchronized
    fun tick(context: Context, telecom: AndroidTelecomControl) {
        val value = read(context) ?: return
        val event = CallEventPayload.decode(value.getJSONObject("event")).copy(
            auditPolicyVersion = value.optLong("audit_policy", 0).takeIf { it > 0 },
            auditQuotaBytes = value.optLong("audit_quota", 0).takeIf { it > 0 })
        val now = SystemClock.elapsedRealtime()
        fun finish(reason: String) {
            if (value.getBoolean("prepared")) {
                SessionAuditFiles.write(File(context.filesDir, "bridge/call-results/${event.callId}.cancelled"),
                    "CANCEL1 ${event.callId} ${value.getString("boot")} $now\n".toByteArray(Charsets.US_ASCII))
                RootAudioTrigger.cancelPendingStartMenu(context)
            }
            if (value.getBoolean("prepared")) LocalCallSession.finishRequested(context, reason, emptyList(), event.callId)
            else SecureControlStore.enqueueCall(context, event.copy(result = reason,
                endedAt = Instant.now().toString(), cleanupStatus = "complete"))
            clear(context, event.callId)
        }
        if (value.getString("boot") != BootIdentity.current()) { finish("RECOVERED_AFTER_REBOOT"); return }
        if (context.checkSelfPermission(Manifest.permission.ANSWER_PHONE_CALLS) != PackageManager.PERMISSION_GRANTED ||
            !CallerPolicyEngine.decide(context, event.caller).shouldHandle) { finish("ADMISSION_CANCELLED"); return }
        val claim = telecom.claimIncomingCaller(event.callId, signedSessionAuthorized = false)
        val live = telecom.calls().filter { it.state != TelecomCallState.DISCONNECTED }
        val caller = live.singleOrNull { it.ownerSessionId == event.callId }
        if (caller == null) {
            val native = CallRecoveryBridge.snapshot(context)
            val fresh = native != null && native.parsed && native.boot == value.getString("boot") &&
                native.elapsedMs <= now && now - native.elapsedMs <= 2000 &&
                native.elapsedMs > value.optLong("last_seen_elapsed", value.getLong("started_elapsed"))
            val absent = fresh && native!!.calls.none { it.id == value.optString("native_id") } &&
                (value.getBoolean("seen") || (native.calls.isEmpty() && native.elapsedMs > value.getLong("started_elapsed") + 2000))
            if (absent) {
                if (!value.has("absent_since")) {
                    value.put("absent_since", now).put("absent_sequence", native!!.sequence); save(context, value)
                } else if (now - value.getLong("absent_since") >= 500 && native!!.sequence != value.getLong("absent_sequence") && native.elapsedMs > value.getLong("absent_since")) {
                    finish("MISSED_DURING_RECOVERY"); return
                }
            } else if (value.has("absent_since")) {
                value.remove("absent_since"); value.remove("absent_sequence"); save(context, value)
            }
            if (claim is CallerClaimDecision.Rejected && claim.reason == "CALLER_TOPOLOGY_AMBIGUOUS" &&
                live.count { !LocalCallSession.owns(it.ownerSessionId) } > 1) finish("AMBIGUOUS_INCOMING_CALL")
            return
        }
        value.remove("absent_since"); value.remove("absent_sequence")
        if (!value.getBoolean("seen") || value.optString("native_id") != caller.nativeId) {
            value.put("seen", true).put("native_id", caller.nativeId).put("last_seen_elapsed", now); save(context, value)
        }
        if (caller.state != TelecomCallState.RINGING) {
            // If our answer was already dispatched, the local session journal owns it.
            if (LocalCallSession.answerRequested(event.callId)) clear(context, event.callId)
            else finish("ANSWERED_ELSEWHERE")
            return
        }
        val others = live.filter { it.id != caller.id }
        if (others.isNotEmpty()) {
            if (others.any { it.ownerSessionId == null || !LocalCallSession.owns(it.ownerSessionId) }) finish("AMBIGUOUS_INCOMING_CALL")
            return
        }
        val nativeId = caller.nativeId ?: return
        if (caller.emergency || caller.parentNativeId != null || caller.childrenNativeIds.isNotEmpty()) { finish("AMBIGUOUS_INCOMING_CALL"); return }
        SessionAuditFiles.write(File(context.filesDir, "bridge/incoming-caller"),
            "INCOMING1 ${event.callId} ${value.getString("boot")} $nativeId $now\n".toByteArray(Charsets.US_ASCII))
        LocalCallSession.reconcile(context)
        if (!value.getBoolean("prepared")) {
            if (!LocalCallSession.canHandleNewCaller(context)) return
            if (!RootAudioTrigger.readState(context).callAdmissionCapable) { finish("INCOMPATIBLE_DEVICE"); return }
            require(RootAudioTrigger.prepareCallBridge(context, event.callId))
            LocalCallSession.begin(context, event, caller.id, value.getLong("started_elapsed"))
            value.put("prepared", true); save(context, value)
            SecureControlStore.enqueueCall(context, event)
        }
        val helper = RootAudioTrigger.readState(context)
        if (helper.isIdle) {
            // Safe to retry the same identity after a helper restart; never while it owns audio.
            RootAudioTrigger.requestStartMenu(context, event.callId)
            return
        }
        val native = CallRecoveryBridge.snapshot(context)
        if (IncomingAdmissionPolicy.mayAnswer(event.callId, value.getString("boot"), caller, native, now,
                helper.hasClaimedSession, LocalCallSession.answerRequested(event.callId),
                CallerPolicyEngine.decide(context, event.caller).shouldHandle)) {
            if (LocalCallSession.beforeAnswer(context)) {
                android.util.Log.i("IVRdroidAdmission", "call=${event.callId} boot=${value.getString("boot")} ready_ms=$now answer_request_ms=${SystemClock.elapsedRealtime()}")
                if (telecom.answerRecoveredCaller(caller.id)) clear(context, event.callId)
                // Persisted intent prevents a callback from issuing a second answer.
            }
        }
    }
}
