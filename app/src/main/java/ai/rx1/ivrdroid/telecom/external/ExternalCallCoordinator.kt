package ai.rx1.ivrdroid.telecom.external

import android.content.Context
import android.os.SystemClock
import ai.rx1.ivrdroid.audio.RootAudioTrigger
import ai.rx1.ivrdroid.control.ConversationHandoffIdentity
import ai.rx1.ivrdroid.control.SecureControlStore

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

    @Synchronized
    fun tick(nowElapsedMs: Long = SystemClock.elapsedRealtime()) {
        if (!initialized) recover(nowElapsedMs)
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
        val snapshot = engine?.snapshot()
        if (snapshot != null && !snapshot.terminal &&
            lastStatus != null && nowElapsedMs - lastPublishedElapsedMs >= HEARTBEAT_MS
        ) {
            publishStatus(lastStatus!!, lastReason, snapshot, nowElapsedMs, audit = false)
        }
        if (!bridgeHealthy && snapshot?.terminal == false) {
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
    }

    private fun recover(nowElapsedMs: Long) {
        initialized = true
        val recovered = journal.load() ?: return
        val bootId = BootIdentity.current()
        if (bootId == null || recovered.config.bootId != bootId) {
            // A Telecom call identifier is valid only within its originating boot. Never mutate
            // current calls using a prior-boot journal.
            journal.clear()
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
            return
        }
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
        engine?.recover(bootId, nowElapsedMs)
    }

    private fun handleDial(request: CallControlRequest.Dial, nowElapsedMs: Long) {
        val existing = engine?.snapshot()
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
        if (!bridgeHealthy && engine?.snapshot()?.terminal == false) {
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
            bridgeHealthy = false
            return
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
            if (conferenceEvidenceCommitted) {
                ExternalCallAuditStore.releaseConferenceEvidence(
                    appContext,
                    snapshot.config.sessionId,
                    snapshot.config.revisionId,
                    snapshot.config.blockId,
                )
            }
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

object ExternalCallRuntimeStatus {
    fun state(context: Context): String = CallControlBridge.readStatus(context)?.status?.name ?: "UNAVAILABLE"

    fun recoveryPending(context: Context): Boolean {
        val snapshot = CallControlBridge.journal(context).load() ?: return false
        return snapshot.config.bootId == BootIdentity.current() && !snapshot.terminal
    }
}
