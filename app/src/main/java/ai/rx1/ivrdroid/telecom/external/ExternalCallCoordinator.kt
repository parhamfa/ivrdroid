package ai.rx1.ivrdroid.telecom.external

import android.content.Context
import android.os.SystemClock
import android.util.Log
import ai.rx1.ivrdroid.audio.RootAudioTrigger
import ai.rx1.ivrdroid.control.ConversationHandoffIdentity
import ai.rx1.ivrdroid.control.SecureControlStore
import ai.rx1.ivrdroid.telecom.LocalCallSession

class ExternalCallCoordinator(
    context: Context,
    private val telecom: TelecomControl,
) : ExternalCallObserver {
    private val appContext = context.applicationContext
    private val journal = CallControlBridge.journal(appContext)
    private var engine: ExternalCallEngine? = null
    private var initialized = false
    private var lastRequest: CallControlRequest? = null
    private var lastStatus: CallControlStatus? = null
    private var lastReason = "-"
    private var sequence = 0L
    private var lastPublishedElapsedMs = -1L
    private var bridgeHealthy = true
    private var pendingConferenceEvidence: ExternalCallSessionSnapshot? = null
    private var nextEvidenceRetryElapsedMs = 0L
    private val recoveryBarrier = TelecomRecoveryBarrier()
    private val recoveryStartedElapsedMs = SystemClock.elapsedRealtime()
    var readiness: TelecomReadiness = TelecomReadiness.WAITING_NATIVE
        private set

    @Synchronized
    fun tick(nowElapsedMs: Long = SystemClock.elapsedRealtime()) {
        if (!initialized) {
            recover(nowElapsedMs)
            if (!initialized) return
        }
        val request = CallControlBridge.readRequest(appContext)
        if (request != null && request != lastRequest) {
            lastRequest = request
            when (request) {
                is CallControlRequest.Dial -> handleDial(request, nowElapsedMs)
                is CallControlRequest.RecorderReady -> handleRecorderReady(request, nowElapsedMs)
                is CallControlRequest.Cancel -> handleCancel(request, nowElapsedMs)
            }
        }
        engine?.reconcile(nowElapsedMs)
        pendingConferenceEvidence?.takeIf { nowElapsedMs >= nextEvidenceRetryElapsedMs }?.let {
            nextEvidenceRetryElapsedMs = nowElapsedMs + 5_000
            if (ExternalCallAuditStore.capture(appContext, it, CallControlStatus.CONFERENCED, "-")) {
                pendingConferenceEvidence = null
            }
        }
        val snapshot = engine?.snapshot()
        if (snapshot != null && !snapshot.terminal &&
            lastStatus != null && nowElapsedMs - lastPublishedElapsedMs >= HEARTBEAT_MS
        ) {
            publishStatus(lastStatus!!, lastReason, snapshot, nowElapsedMs, audit = false)
        }
        if (!bridgeHealthy && snapshot?.terminal == false && !snapshot.preservesEstablishedConversation()) {
            engine?.cancel("HELPER_CANCELLED", nowElapsedMs)
        }
    }

    override fun onStatus(
        status: CallControlStatus,
        reason: String,
        snapshot: ExternalCallSessionSnapshot,
    ) {
        publishStatus(status, reason, snapshot, snapshot.updatedElapsedMs, audit = true)
    }

    override fun onSnapshot(snapshot: ExternalCallSessionSnapshot) {
        runCatching { journal.save(snapshot) }.onFailure { bridgeHealthy = false }
        if (!snapshot.terminal) CallRecoveryBridge.publishOwnership(appContext, snapshot, telecom.calls())
    }

    override fun onRecordingFailure(snapshot: ExternalCallSessionSnapshot, nowElapsedMs: Long) {
        // The recording worker already saved the exception outside the call-control lock.
        // Never perform storage work or change Telecom state from this notification.
        Log.w("IVRdroidCall", "Recording failure at $nowElapsedMs; preserving ${snapshot.phase}")
    }

    private fun recover(nowElapsedMs: Long) {
        val recovered = journal.load()
        val bootId = BootIdentity.current()
        val native = CallRecoveryBridge.snapshot(appContext)
        val calls = telecom.calls()
        // An old controller journal must not demand reattachment to a helper
        // that has already released it and admitted the next exact caller.
        val retired = recovered != null && canRetireReleasedController(recovered, calls,
            LocalCallSession.owns(recovered.config.sessionId), RootAudioTrigger.isIdle(appContext),
            native?.session?.let(LocalCallSession::owns) == true)
        readiness = recoveryBarrier.observe(native, calls, bootId,
            recovered?.takeUnless { it.terminal || retired }?.config?.sessionId, nowElapsedMs)
        ExternalCallRuntimeStatus.recovery = ControllerRecoveryStatus(readiness.name,
            nowElapsedMs - recoveryStartedElapsedMs, recovered?.phase?.name ?: "IDLE", recoveryStartedElapsedMs)
        if (readiness != TelecomReadiness.READY) return
        if (recovered == null) { initialized = true; return }
        if (retired) {
            journal.retire(recovered)
            lastRequest = CallControlBridge.readRequest(appContext)?.takeIf { it.sessionId == recovered.config.sessionId }
            initialized = true
            return
        }
        if (bootId == null || recovered.config.bootId != bootId) {
            // A Telecom call identifier is valid only within its originating boot. Never mutate
            // current calls using a prior-boot journal.
            journal.clear()
            initialized = true
            return
        }
        if (!matchesActiveSignedInstruction(recovered.config)) {
            val failed = recovered.copy(
                phase = ExternalCallPhase.SYSTEM_FAILURE,
                callerId = null,
                operatorId = null,
                conferenceId = null,
                deadlineElapsedMs = 0,
                updatedElapsedMs = nowElapsedMs,
                reason = "RECOVERY_UNSIGNED",
            )
            journal.save(failed)
            publishStatus(CallControlStatus.SYSTEM_FAILURE, failed.reason, failed, nowElapsedMs, audit = true)
            initialized = true
            return
        }
        if (!recovered.terminal && (!CallRecoveryBridge.publishOwnership(appContext, recovered, telecom.calls()) ||
            !CallRecoveryBridge.attached(appContext, recovered))) {
            readiness = TelecomReadiness.WAITING_HANDSHAKE
            ExternalCallRuntimeStatus.recovery = ExternalCallRuntimeStatus.recovery.copy(readiness = readiness.name)
            return
        }
        initialized = true
        engine = ExternalCallEngine(telecom, this, recovered)
        CallControlBridge.readStatus(appContext)?.takeIf {
            it.sessionId == recovered.config.sessionId && it.revisionId == recovered.config.revisionId &&
                it.blockId == recovered.config.blockId && it.bootId == recovered.config.bootId &&
                it.elapsedMs <= nowElapsedMs
        }?.let {
            sequence = it.sequence
            lastStatus = it.status
            lastReason = it.reason
            lastPublishedElapsedMs = it.elapsedMs
        }
        engine?.recover(bootId, nowElapsedMs, independentlyReconciled = true)
    }

    private fun handleDial(request: CallControlRequest.Dial, nowElapsedMs: Long) {
        // Durable local admission is the authority. Never replay a dial request
        // from a retired call against a new incoming caller after process death.
        if (!LocalCallSession.owns(request.sessionId)) return
        var existing = engine?.snapshot()
        val native = CallRecoveryBridge.snapshot(appContext)
        if (existing != null && existing.config.sessionId != request.sessionId && native != null &&
            native.parsed && !native.emergency && native.boot == request.bootId && native.session == request.sessionId &&
            native.elapsedMs <= nowElapsedMs && nowElapsedMs - native.elapsedMs <= 2000 &&
            canRetireReleasedController(existing, telecom.calls(), LocalCallSession.owns(existing.config.sessionId), false, true)) {
            journal.retire(existing)
            engine = null; existing = null
            lastStatus = null; lastReason = "-"; pendingConferenceEvidence = null
        }
        if (existing != null && sameIdentity(existing.config, request)) {
            if (request.sequence != existing.lastRequestSequence ||
                request.elapsedMs != existing.lastRequestElapsedMs || existing.lastRequestKind != "DIAL"
            ) {
                engine?.protocolFailure("REQUEST_REPLAY", nowElapsedMs)
                return
            }
            if (existing.terminal && lastStatus == null) {
                val status = terminalStatus(existing.phase)
                publishStatus(status, existing.reason, existing, nowElapsedMs, audit = false)
            }
            return
        }
        if (existing?.terminal == false) {
            engine?.protocolFailure("SESSION_COLLISION", nowElapsedMs)
            return
        }
        if (request.sequence != 1L || request.elapsedMs > nowElapsedMs) {
            publishStandaloneFailure(request, "REQUEST_REPLAY", nowElapsedMs)
            return
        }
        val bootId = BootIdentity.current()
        if (bootId == null || request.bootId != bootId) {
            publishStandaloneFailure(request, "STALE_BOOT", nowElapsedMs)
            return
        }
        val helper = RootAudioTrigger.readState(appContext)
        if (!ExternalCallCapabilities.snapshot(appContext, helper).runtimeV4Capable) {
            publishStandaloneFailure(request, "CAPABILITY_UNAVAILABLE", nowElapsedMs)
            return
        }
        val manifest = SecureControlStore.activeManifest(appContext)
        val signed = runCatching {
            ExternalCallInstructionResolver.resolve(requireNotNull(manifest), request)
        }.getOrNull()
        if (signed == null) {
            publishStandaloneFailure(request, "UNSIGNED_REQUEST", nowElapsedMs)
            return
        }
        val callerClaim = telecom.claimIncomingCaller(
            request.sessionId,
            signedSessionAuthorized = true,
        )
        val callerId = when (callerClaim) {
            is CallerClaimDecision.Claimed -> callerClaim.callId
            is CallerClaimDecision.Rejected -> {
                publishStandaloneFailure(request, callerClaim.reason, nowElapsedMs)
                return
            }
        }
        val config = ExternalCallConfig(
            request.sessionId,
            signed.revisionId,
            signed.blockId,
            signed.phoneNumber,
            signed.answerTimeoutMs,
            request.bootId,
        )
        bridgeHealthy = true
        sequence = CallControlBridge.readStatus(appContext)
            ?.takeIf { it.sessionId == request.sessionId && it.bootId == request.bootId }
            ?.sequence
            ?: 0L
        lastStatus = null
        lastReason = "-"
        engine = ExternalCallEngine(telecom, this)
        engine?.begin(config, callerId, nowElapsedMs, request.sequence, request.elapsedMs)
        if (!bridgeHealthy && engine?.snapshot()?.let { !it.terminal && !it.preservesEstablishedConversation() } == true) {
            engine?.cancel("HELPER_CANCELLED", nowElapsedMs)
        }
    }

    private fun handleRecorderReady(request: CallControlRequest.RecorderReady, nowElapsedMs: Long) {
        val snapshot = engine?.snapshot()
        if (snapshot == null || !sameIdentity(snapshot.config, request) ||
            request.bootId != BootIdentity.current()
        ) {
            if (snapshot?.terminal == false) engine?.protocolFailure("SESSION_COLLISION", nowElapsedMs)
            else publishStandaloneFailure(request, "STALE_SESSION", nowElapsedMs)
            return
        }
        if (isIdempotentReplay(snapshot, request, "RECORDER_READY")) return
        if (request.elapsedMs > nowElapsedMs ||
            engine?.recordRequest("RECORDER_READY", request.sequence, request.elapsedMs, nowElapsedMs) != true
        ) {
            engine?.protocolFailure("REQUEST_REPLAY", nowElapsedMs)
            return
        }
        engine?.recorderReady(nowElapsedMs)
    }

    private fun handleCancel(request: CallControlRequest.Cancel, nowElapsedMs: Long) {
        val snapshot = engine?.snapshot()
        if (snapshot == null || !sameIdentity(snapshot.config, request) ||
            request.bootId != BootIdentity.current()
        ) {
            if (snapshot?.terminal == false) engine?.protocolFailure("SESSION_COLLISION", nowElapsedMs)
            else publishStandaloneFailure(request, "STALE_SESSION", nowElapsedMs)
            return
        }
        if (isIdempotentReplay(snapshot, request, "CANCEL")) return
        if (request.elapsedMs > nowElapsedMs ||
            engine?.recordRequest("CANCEL", request.sequence, request.elapsedMs, nowElapsedMs) != true
        ) {
            engine?.protocolFailure("REQUEST_REPLAY", nowElapsedMs)
            return
        }
        engine?.cancel(request.reason, nowElapsedMs)
    }

    private fun matchesActiveSignedInstruction(config: ExternalCallConfig): Boolean = runCatching {
        val manifest = requireNotNull(SecureControlStore.activeManifest(appContext))
        val resolved = ExternalCallInstructionResolver.resolve(manifest, config.revisionId, config.blockId)
        resolved.phoneNumber == config.phoneNumber && resolved.answerTimeoutMs == config.answerTimeoutMs
    }.getOrDefault(false)

    private fun publishStandaloneFailure(
        request: CallControlRequest,
        reason: String,
        nowElapsedMs: Long,
    ) {
        val config = ExternalCallConfig(
            request.sessionId,
            request.revisionId,
            request.blockId,
            (request as? CallControlRequest.Dial)?.phoneNumber ?: "+00000000",
            (request as? CallControlRequest.Dial)?.answerTimeoutMs ?: 5_000,
            request.bootId,
        )
        val failed = ExternalCallSessionSnapshot(
            config,
            ExternalCallPhase.SYSTEM_FAILURE,
            null,
            null,
            null,
            0,
            nowElapsedMs,
            reason = reason,
            lastRequestSequence = request.sequence,
            lastRequestElapsedMs = request.elapsedMs,
            lastRequestKind = requestKind(request),
        )
        journal.save(failed)
        sequence = 0L
        publishStatus(CallControlStatus.SYSTEM_FAILURE, reason, failed, nowElapsedMs, audit = true)
    }

    private fun publishStatus(
        status: CallControlStatus,
        reason: String,
        snapshot: ExternalCallSessionSnapshot,
        nowElapsedMs: Long,
        audit: Boolean,
    ) {
        val conferenceEvidenceCommitted = audit && status == CallControlStatus.CONFERENCED
        if (conferenceEvidenceCommitted &&
            !ExternalCallAuditStore.capture(appContext, snapshot, status, reason)
        ) {
            // A durable audit receipt is required to publish audio, but is not
            // permission to keep talking. Retry it without disconnecting people.
            pendingConferenceEvidence = snapshot
            nextEvidenceRetryElapsedMs = nowElapsedMs + 5_000
            Log.e("IVRdroidCall", "Conference audit storage unavailable; preserving the established call")
        }
        val existing = CallControlBridge.readStatus(appContext)
        if (existing != null && existing.sessionId == snapshot.config.sessionId &&
            existing.bootId == snapshot.config.bootId
        ) {
            sequence = maxOf(sequence, existing.sequence)
        }
        val record = CallControlStatusRecord(
            status,
            snapshot.config.sessionId,
            snapshot.config.revisionId,
            snapshot.config.blockId,
            Math.addExact(sequence, 1),
            snapshot.config.bootId,
            nowElapsedMs,
            reason,
        )
        if (CallControlBridge.publish(appContext, record)) {
            sequence = record.sequence
            lastPublishedElapsedMs = nowElapsedMs
        } else {
            bridgeHealthy = false
        }
        lastStatus = status
        lastReason = reason
        if (audit && !conferenceEvidenceCommitted) {
            ExternalCallAuditStore.capture(appContext, snapshot, status, reason)
        }
    }

    private fun sameIdentity(config: ExternalCallConfig, request: CallControlRequest): Boolean =
        config.sessionId == request.sessionId && config.revisionId == request.revisionId &&
            config.blockId == request.blockId && config.bootId == request.bootId

    private fun isIdempotentReplay(
        snapshot: ExternalCallSessionSnapshot,
        request: CallControlRequest,
        kind: String,
    ): Boolean = request.sequence == snapshot.lastRequestSequence &&
        request.elapsedMs == snapshot.lastRequestElapsedMs && snapshot.lastRequestKind == kind

    private fun requestKind(request: CallControlRequest): String = when (request) {
        is CallControlRequest.Dial -> "DIAL"
        is CallControlRequest.RecorderReady -> "RECORDER_READY"
        is CallControlRequest.Cancel -> "CANCEL"
    }

    @Synchronized
    fun conversationHandoffSession(): ConversationHandoffSession? = engine?.snapshot()?.let {
        if (it.config.bootId != BootIdentity.current()) null else ConversationHandoffSession(
            ConversationHandoffIdentity(
                it.config.sessionId,
                it.config.revisionId,
                it.config.blockId,
            ),
            it.config.bootId,
            callActive = !it.terminal || telecom.calls().any { call ->
                call.state != TelecomCallState.DISCONNECTED
            },
        )
    }

    @Synchronized
    fun recordingHandoffFailed(identity: ConversationHandoffIdentity, nowElapsedMs: Long) {
        val snapshot = engine?.snapshot() ?: return
        if (snapshot.config.sessionId == identity.callId && snapshot.config.revisionId == identity.revisionId &&
            snapshot.config.blockId == identity.blockId && snapshot.config.bootId == BootIdentity.current()
        ) {
            engine?.recordingHandoffFailed(nowElapsedMs)
        }
    }

    private fun terminalStatus(phase: ExternalCallPhase): CallControlStatus = when (phase) {
        ExternalCallPhase.COMPLETED -> CallControlStatus.COMPLETED
        ExternalCallPhase.NOT_CONNECTED -> CallControlStatus.NOT_CONNECTED
        ExternalCallPhase.SYSTEM_FAILURE -> CallControlStatus.SYSTEM_FAILURE
        else -> error("External-call phase is not terminal.")
    }

    private companion object {
        const val HEARTBEAT_MS = 1_000L
    }
}

internal fun ExternalCallSessionSnapshot.preservesEstablishedConversation(): Boolean =
    phase == ExternalCallPhase.CONFERENCED || phase == ExternalCallPhase.RETURNING_CALLER

object ExternalCallRuntimeStatus {
    @Volatile var recovery = ControllerRecoveryStatus("NOT_BOUND", 0, "UNKNOWN", 0)
    fun state(context: Context): String = CallControlBridge.readStatus(context)?.status?.name ?: "UNAVAILABLE"

    fun recoveryPending(context: Context): Boolean {
        val snapshot = CallControlBridge.journal(context).load() ?: return false
        return snapshot.config.bootId == BootIdentity.current() && !snapshot.terminal
    }
}

data class ControllerRecoveryStatus(val readiness: String, val durationMs: Long, val phase: String, val startedElapsedMs: Long)
