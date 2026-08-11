package ai.rx1.ivrdroid.telecom.external

enum class TelecomCallDirection { INCOMING, OUTGOING, UNKNOWN }

enum class TelecomCallState {
    NEW,
    RINGING,
    DIALING,
    ACTIVE,
    HOLDING,
    DISCONNECTING,
    DISCONNECTED,
    UNKNOWN,
}

enum class TelecomDisconnectKind { BUSY, REJECTED, ERROR, LOCAL, REMOTE, UNKNOWN }

data class TelecomCallSnapshot(
    val id: String,
    val direction: TelecomCallDirection,
    val state: TelecomCallState,
    val ownerSessionId: String?,
    val phoneNumber: String?,
    val emergency: Boolean,
    val canHold: Boolean,
    val disconnectKind: TelecomDisconnectKind = TelecomDisconnectKind.UNKNOWN,
)

interface TelecomControl {
    fun calls(): List<TelecomCallSnapshot>
    fun claimIncomingCaller(
        sessionId: String,
        signedSessionAuthorized: Boolean = true,
    ): CallerClaimDecision {
        val present = calls().filter {
            it.state !in setOf(TelecomCallState.DISCONNECTING, TelecomCallState.DISCONNECTED)
        }
        if (present.size != 1) {
            return CallerClaimDecision.Rejected(
                if (present.isEmpty()) "CALLER_NOT_VISIBLE" else "CALLER_TOPOLOGY_AMBIGUOUS",
            )
        }
        val caller = present.single()
        return when {
            caller.ownerSessionId != sessionId -> CallerClaimDecision.Rejected("CALLER_NOT_OWNED")
            caller.direction != TelecomCallDirection.INCOMING ->
                CallerClaimDecision.Rejected("CALLER_DIRECTION_INVALID")
            caller.emergency -> CallerClaimDecision.Rejected("CALLER_EMERGENCY")
            else -> CallerClaimDecision.Claimed(caller.id)
        }
    }
    fun isEmergencyNumber(phoneNumber: String): Boolean
    fun hold(callId: String): Boolean
    fun unhold(callId: String): Boolean
    fun disconnect(callId: String): Boolean
    fun placeCall(phoneNumber: String, sessionId: String, callerId: String): Boolean
    fun canConference(callerId: String, operatorId: String): Boolean
    fun conference(callerId: String, operatorId: String): Boolean
    fun conferenceId(callerId: String, operatorId: String): String?
}

data class ExternalCallConfig(
    val sessionId: String,
    val revisionId: Long,
    val blockId: String,
    val phoneNumber: String,
    val answerTimeoutMs: Int,
    val bootId: String,
) {
    init {
        CallControlProtocol.parseRequest(
            "${CallControlProtocol.PREFIX} DIAL $sessionId $revisionId $blockId 1 $bootId 0 " +
                "$phoneNumber $answerTimeoutMs\n",
        )
    }
}

enum class ExternalCallPhase {
    HOLDING_CALLER,
    WAITING_DIALING,
    DIALING,
    WAITING_RECORDER,
    WAITING_CONFERENCEABLE,
    MERGING,
    CONFERENCED,
    RETURNING_CALLER,
    CLEANING_CALLER_HANGUP,
    CLEANUP_NOT_CONNECTED,
    CLEANUP_SYSTEM_FAILURE,
    CLEANUP_ABORTED,
    COMPLETED,
    NOT_CONNECTED,
    SYSTEM_FAILURE,
}

data class ExternalCallSessionSnapshot(
    val config: ExternalCallConfig,
    val phase: ExternalCallPhase,
    val callerId: String?,
    val operatorId: String?,
    val conferenceId: String?,
    val deadlineElapsedMs: Long,
    val updatedElapsedMs: Long,
    val reason: String = "-",
    val lastRequestSequence: Long = 1,
    val lastRequestElapsedMs: Long = 0,
    val lastRequestKind: String = "DIAL",
    val cleanupHeartbeatStatus: CallControlStatus? = null,
    val cleanupOutcomeReason: String? = null,
    val operatorDisconnectRequestedElapsedMs: Long? = null,
    val operatorSafeSinceElapsedMs: Long? = null,
    val callerUnholdRequestedElapsedMs: Long? = null,
    val callerActiveSinceElapsedMs: Long? = null,
    val callerActiveObserved: Boolean = false,
    val cleanupUnholdAllowed: Boolean = true,
) {
    val terminal: Boolean
        get() = phase in setOf(
            ExternalCallPhase.COMPLETED,
            ExternalCallPhase.NOT_CONNECTED,
            ExternalCallPhase.SYSTEM_FAILURE,
        )
}

interface ExternalCallObserver {
    fun onStatus(status: CallControlStatus, reason: String, snapshot: ExternalCallSessionSnapshot)
    fun onSnapshot(snapshot: ExternalCallSessionSnapshot)
}

class ExternalCallEngine(
    private val telecom: TelecomControl,
    private val observer: ExternalCallObserver,
    recovered: ExternalCallSessionSnapshot? = null,
) {
    private var current: ExternalCallSessionSnapshot? = recovered

    fun snapshot(): ExternalCallSessionSnapshot? = current

    fun begin(
        config: ExternalCallConfig,
        callerId: String,
        nowElapsedMs: Long,
        requestSequence: Long = 1,
        requestElapsedMs: Long = 0,
    ) {
        require(current == null || current?.terminal == true) { "Another external-call session is active." }
        require(requestSequence > 0 && requestElapsedMs in 0..nowElapsedMs)
        val caller = telecom.calls().singleOrNull { it.id == callerId }
        val validationFailure = when {
            caller == null -> "CALLER_NOT_VISIBLE"
            caller.ownerSessionId != config.sessionId -> "CALLER_OWNER_MISMATCH"
            caller.direction != TelecomCallDirection.INCOMING -> "CALLER_DIRECTION_INVALID"
            caller.state != TelecomCallState.ACTIVE -> "CALLER_STATE_INVALID"
            caller.emergency -> "CALLER_EMERGENCY"
            telecom.isEmergencyNumber(config.phoneNumber) -> "DESTINATION_EMERGENCY"
            else -> null
        }
        if (validationFailure != null) {
            current = ExternalCallSessionSnapshot(
                config,
                ExternalCallPhase.SYSTEM_FAILURE,
                null,
                null,
                null,
                0,
                nowElapsedMs,
                reason = validationFailure,
                lastRequestSequence = requestSequence,
                lastRequestElapsedMs = requestElapsedMs,
            )
            persist()
            publish(CallControlStatus.SYSTEM_FAILURE, validationFailure)
            return
        }
        val verifiedCaller = requireNotNull(caller)
        if (!verifiedCaller.canHold) {
            current = ExternalCallSessionSnapshot(
                config,
                ExternalCallPhase.HOLDING_CALLER,
                verifiedCaller.id,
                null,
                null,
                0,
                nowElapsedMs,
                reason = "-",
                lastRequestSequence = requestSequence,
                lastRequestElapsedMs = requestElapsedMs,
            )
            persist()
            publish(CallControlStatus.ACK, "-")
            failSystem("HOLD_UNAVAILABLE", nowElapsedMs)
            return
        }

        current = ExternalCallSessionSnapshot(
            config,
            ExternalCallPhase.HOLDING_CALLER,
            verifiedCaller.id,
            null,
            null,
            checkedDeadline(nowElapsedMs, SETUP_TIMEOUT_MS),
            nowElapsedMs,
            lastRequestSequence = requestSequence,
            lastRequestElapsedMs = requestElapsedMs,
        )
        persist()
        publish(CallControlStatus.ACK, "-")
        if (!telecom.hold(verifiedCaller.id)) failSystem("HOLD_FAILED", nowElapsedMs)
    }

    fun recorderReady(nowElapsedMs: Long) {
        val session = current ?: return
        if (session.terminal) return
        if (session.phase != ExternalCallPhase.WAITING_RECORDER) {
            failSystem("RECORDER_PROTOCOL_ORDER", nowElapsedMs)
            return
        }
        val caller = ownedCaller() ?: return failSystem("CALLER_LOST", nowElapsedMs)
        val operator = ownedOperator() ?: return failNotConnected("OPERATOR_DISCONNECTED", nowElapsedMs)
        if (caller.state != TelecomCallState.HOLDING || operator.state != TelecomCallState.ACTIVE) {
            failSystem("LEGS_NOT_READY", nowElapsedMs)
            return
        }
        update(
            session.copy(
                phase = ExternalCallPhase.WAITING_CONFERENCEABLE,
                deadlineElapsedMs = checkedDeadline(nowElapsedMs, MERGE_TIMEOUT_MS),
                updatedElapsedMs = nowElapsedMs,
            ),
        )
        reconcile(nowElapsedMs)
    }

    fun cancel(reason: String, nowElapsedMs: Long) {
        require(reason.matches(Regex("[A-Z][A-Z0-9_]{0,63}")))
        val session = current ?: return
        if (session.terminal) return
        if (reason == "ANSWER_TIMEOUT") {
            when {
                session.phase == ExternalCallPhase.DIALING &&
                    nowElapsedMs >= session.deadlineElapsedMs ->
                    failNotConnected("ANSWER_TIMEOUT", nowElapsedMs)
                session.phase == ExternalCallPhase.CLEANUP_NOT_CONNECTED &&
                    session.cleanupOutcomeReason == "ANSWER_TIMEOUT" ->
                    reconcileCleanup(telecom.calls(), nowElapsedMs)
                else -> failSystem("CANCEL_OUTCOME_MISMATCH", nowElapsedMs)
            }
            return
        }
        failSystem(reason, nowElapsedMs)
    }

    fun recordRequest(kind: String, sequence: Long, elapsedMs: Long, nowElapsedMs: Long): Boolean {
        val session = current ?: return false
        if (session.terminal || kind !in setOf("RECORDER_READY", "CANCEL") ||
            sequence <= session.lastRequestSequence || elapsedMs < session.lastRequestElapsedMs ||
            elapsedMs > nowElapsedMs
        ) return false
        update(
            session.copy(
                lastRequestSequence = sequence,
                lastRequestElapsedMs = elapsedMs,
                lastRequestKind = kind,
                updatedElapsedMs = nowElapsedMs,
            ),
        )
        return true
    }

    fun recordingHandoffFailed(nowElapsedMs: Long) {
        val session = current ?: return
        if (session.phase == ExternalCallPhase.COMPLETED) {
            current = session.copy(
                phase = ExternalCallPhase.SYSTEM_FAILURE,
                updatedElapsedMs = nowElapsedMs,
                reason = "RECORDING_FAILURE",
            )
            persist()
            publish(CallControlStatus.SYSTEM_FAILURE, "RECORDING_FAILURE")
        } else if (!session.terminal) {
            failSystem("RECORDING_FAILURE", nowElapsedMs)
        }
    }

    fun protocolFailure(reason: String, nowElapsedMs: Long) {
        require(reason.matches(Regex("[A-Z][A-Z0-9_]{0,63}")))
        if (current?.terminal != false) return
        failSystem(reason, nowElapsedMs)
    }

    fun recover(currentBootId: String, nowElapsedMs: Long) {
        val session = current ?: return
        if (session.config.bootId != currentBootId) {
            current = session.copy(
                phase = ExternalCallPhase.SYSTEM_FAILURE,
                callerId = null,
                operatorId = null,
                conferenceId = null,
                deadlineElapsedMs = 0,
                updatedElapsedMs = nowElapsedMs,
                reason = "STALE_BOOT",
            )
            persist()
            publish(CallControlStatus.SYSTEM_FAILURE, "STALE_BOOT")
            return
        }
        if (session.terminal) {
            publish(statusFor(session.phase), session.reason)
            return
        }
        val caller = ownedCaller()
        if (caller == null || caller.emergency) {
            failSystem("RECOVERY_UNOWNED", nowElapsedMs, unholdCaller = false)
            return
        }
        if (session.phase == ExternalCallPhase.HOLDING_CALLER && caller.state == TelecomCallState.ACTIVE) {
            if (!telecom.hold(caller.id)) {
                failSystem("RECOVERY_HOLD_FAILED", nowElapsedMs)
                return
            }
        }
        reconcile(nowElapsedMs)
        current?.takeIf { !it.terminal }?.let { publish(statusFor(it.phase), "-") }
    }

    fun reconcile(nowElapsedMs: Long) {
        val session = current ?: return
        if (session.terminal) return
        val calls = telecom.calls()
        if (session.phase in CLEANUP_PHASES) {
            reconcileCleanup(calls, nowElapsedMs)
            return
        }
        val caller = session.callerId?.let { id -> calls.singleOrNull { it.id == id } }
        if (caller == null || caller.state == TelecomCallState.DISCONNECTED) {
            startCleanup(ExternalCallPhase.CLEANING_CALLER_HANGUP, "CALLER_HANGUP", nowElapsedMs)
            return
        }
        if (caller.ownerSessionId != session.config.sessionId || caller.emergency) {
            failSystem("CALLER_OWNERSHIP_LOST", nowElapsedMs, unholdCaller = false)
            return
        }

        val knownConferenceId = session.conferenceId ?: session.operatorId?.let { operatorId ->
            telecom.conferenceId(caller.id, operatorId)
        }
        val tagged = calls.filter {
            it.id != caller.id && it.id != knownConferenceId &&
                it.ownerSessionId == session.config.sessionId &&
                it.direction == TelecomCallDirection.OUTGOING
        }
        if (tagged.size > 1) {
            // Ambiguous ownership is not permission to disconnect either call.
            failSystem("AMBIGUOUS_CALL_TOPOLOGY", nowElapsedMs)
            return
        }
        val discovered = tagged.singleOrNull()
        if (discovered != null &&
            (discovered.direction != TelecomCallDirection.OUTGOING || discovered.emergency ||
                discovered.phoneNumber != session.config.phoneNumber)
        ) {
            failSystem("OPERATOR_IDENTITY_MISMATCH", nowElapsedMs)
            return
        }
        if (session.operatorId == null && discovered != null) {
            update(session.copy(operatorId = discovered.id, updatedElapsedMs = nowElapsedMs))
        } else if (session.operatorId != null && discovered != null && session.operatorId != discovered.id) {
            failSystem("OPERATOR_IDENTITY_CHANGED", nowElapsedMs)
            return
        }

        val active = requireNotNull(current)
        val operator = active.operatorId?.let { id -> calls.singleOrNull { it.id == id } }
        val allowedIds = setOfNotNull(caller.id, active.operatorId, active.conferenceId ?: knownConferenceId)
        val unexpected = calls.any {
            it.state !in setOf(TelecomCallState.DISCONNECTED, TelecomCallState.DISCONNECTING) &&
                it.id !in allowedIds
        }
        if (unexpected) {
            failSystem("UNEXPECTED_CALL_TOPOLOGY", nowElapsedMs)
            return
        }
        when (active.phase) {
            ExternalCallPhase.HOLDING_CALLER -> {
                if (caller.state == TelecomCallState.HOLDING) {
                    update(
                        active.copy(
                            phase = ExternalCallPhase.WAITING_DIALING,
                            updatedElapsedMs = nowElapsedMs,
                        ),
                    )
                    // Persist the transition timestamp before exposing it. A coordinator
                    // heartbeat may have advanced the public timestamp since the prior engine
                    // mutation; publishing the old snapshot here would violate the signed
                    // helper protocol's monotonic-time rule and cancel our own outgoing call.
                    publish(CallControlStatus.CALLER_HELD, "-")
                    if (!telecom.placeCall(active.config.phoneNumber, active.config.sessionId, caller.id)) {
                        failSystem("DIAL_FAILED", nowElapsedMs)
                    }
                } else if (nowElapsedMs >= active.deadlineElapsedMs) {
                    failSystem("SETUP_TIMEOUT", nowElapsedMs)
                }
            }
            ExternalCallPhase.WAITING_DIALING -> {
                when {
                    operator?.state in setOf(TelecomCallState.DIALING, TelecomCallState.ACTIVE) -> {
                        update(
                            active.copy(
                                phase = ExternalCallPhase.DIALING,
                                deadlineElapsedMs = checkedDeadline(
                                    nowElapsedMs,
                                    active.config.answerTimeoutMs.toLong(),
                                ),
                                updatedElapsedMs = nowElapsedMs,
                            ),
                        )
                        publish(CallControlStatus.DIALING, "-")
                        if (operator?.state == TelecomCallState.ACTIVE) operatorAnswered(nowElapsedMs)
                    }
                    operator?.state == TelecomCallState.DISCONNECTED ->
                        failNotConnected(disconnectReason(operator.disconnectKind), nowElapsedMs)
                    nowElapsedMs >= active.deadlineElapsedMs -> failSystem("SETUP_TIMEOUT", nowElapsedMs)
                }
            }
            ExternalCallPhase.DIALING -> {
                when {
                    operator?.state == TelecomCallState.ACTIVE -> operatorAnswered(nowElapsedMs)
                    operator?.state == TelecomCallState.DISCONNECTED -> {
                        failNotConnected(disconnectReason(operator.disconnectKind), nowElapsedMs)
                    }
                    nowElapsedMs >= active.deadlineElapsedMs -> failNotConnected("ANSWER_TIMEOUT", nowElapsedMs)
                }
            }
            ExternalCallPhase.WAITING_RECORDER -> {
                when {
                    operator == null || operator.state == TelecomCallState.DISCONNECTED ->
                        failNotConnected("OPERATOR_DISCONNECTED", nowElapsedMs)
                    nowElapsedMs >= active.deadlineElapsedMs -> failSystem("RECORDER_TIMEOUT", nowElapsedMs)
                }
            }
            ExternalCallPhase.WAITING_CONFERENCEABLE -> {
                when {
                    operator == null || operator.state == TelecomCallState.DISCONNECTED ->
                        failNotConnected("OPERATOR_DISCONNECTED", nowElapsedMs)
                    caller.state != TelecomCallState.HOLDING || operator.state != TelecomCallState.ACTIVE ->
                        if (nowElapsedMs >= active.deadlineElapsedMs) {
                            failSystem("LEGS_NOT_READY", nowElapsedMs)
                        }
                    telecom.canConference(caller.id, operator.id) -> {
                        update(
                            active.copy(
                                phase = ExternalCallPhase.MERGING,
                                updatedElapsedMs = nowElapsedMs,
                            ),
                        )
                        publish(CallControlStatus.MERGING, "-")
                        if (!telecom.conference(caller.id, operator.id)) {
                            failSystem("MERGE_FAILED", nowElapsedMs)
                        }
                    }
                    nowElapsedMs >= active.deadlineElapsedMs ->
                        failSystem("MERGE_UNAVAILABLE", nowElapsedMs)
                }
            }
            ExternalCallPhase.MERGING -> {
                when {
                    operator == null || operator.state == TelecomCallState.DISCONNECTED ->
                        failNotConnected("OPERATOR_DISCONNECTED", nowElapsedMs)
                    telecom.conferenceId(caller.id, operator.id) != null -> {
                        val conferenceId = telecom.conferenceId(caller.id, operator.id)
                        update(
                            active.copy(
                                phase = ExternalCallPhase.CONFERENCED,
                                conferenceId = conferenceId,
                                deadlineElapsedMs = 0,
                                updatedElapsedMs = nowElapsedMs,
                            ),
                        )
                        publish(CallControlStatus.CONFERENCED, "-")
                    }
                    nowElapsedMs >= active.deadlineElapsedMs -> failSystem("MERGE_TIMEOUT", nowElapsedMs)
                }
            }
            ExternalCallPhase.CONFERENCED -> {
                if (operator == null || operator.state == TelecomCallState.DISCONNECTED) {
                    startCleanup(ExternalCallPhase.RETURNING_CALLER, "OPERATOR_HANGUP", nowElapsedMs)
                }
            }
            ExternalCallPhase.RETURNING_CALLER -> Unit
            ExternalCallPhase.CLEANING_CALLER_HANGUP,
            ExternalCallPhase.CLEANUP_NOT_CONNECTED,
            ExternalCallPhase.CLEANUP_SYSTEM_FAILURE,
            ExternalCallPhase.CLEANUP_ABORTED,
            -> Unit
            ExternalCallPhase.COMPLETED,
            ExternalCallPhase.NOT_CONNECTED,
            ExternalCallPhase.SYSTEM_FAILURE,
            -> Unit
        }
    }

    private fun ownedCaller(): TelecomCallSnapshot? {
        val session = current ?: return null
        return telecom.calls().singleOrNull {
            it.id == session.callerId && it.ownerSessionId == session.config.sessionId && !it.emergency
        }
    }

    private fun ownedOperator(): TelecomCallSnapshot? {
        val session = current ?: return null
        return telecom.calls().singleOrNull {
            it.id == session.operatorId && it.ownerSessionId == session.config.sessionId &&
                it.direction == TelecomCallDirection.OUTGOING && !it.emergency &&
                it.phoneNumber == session.config.phoneNumber
        }
    }

    private fun operatorAnswered(nowElapsedMs: Long) {
        val session = current ?: return
        update(
            session.copy(
                phase = ExternalCallPhase.WAITING_RECORDER,
                deadlineElapsedMs = checkedDeadline(nowElapsedMs, RECORDER_TIMEOUT_MS),
                updatedElapsedMs = nowElapsedMs,
            ),
        )
        publish(CallControlStatus.OPERATOR_ANSWERED, "-")
    }

    private fun startCleanup(
        phase: ExternalCallPhase,
        reason: String,
        nowElapsedMs: Long,
        unholdCaller: Boolean = true,
    ) {
        val session = current ?: return
        if (session.terminal) return
        require(phase in CLEANUP_PHASES)
        if (session.phase == ExternalCallPhase.CLEANUP_ABORTED) {
            reconcileCleanup(telecom.calls(), nowElapsedMs)
            return
        }
        val alreadyCleaning = session.phase in CLEANUP_PHASES
        update(
            session.copy(
                phase = phase,
                deadlineElapsedMs = if (alreadyCleaning) {
                    session.deadlineElapsedMs
                } else {
                    checkedDeadline(nowElapsedMs, CLEANUP_TIMEOUT_MS)
                },
                updatedElapsedMs = nowElapsedMs,
                reason = reason,
                cleanupHeartbeatStatus = session.cleanupHeartbeatStatus ?: statusFor(session.phase),
                cleanupOutcomeReason = reason,
                cleanupUnholdAllowed = session.cleanupUnholdAllowed && unholdCaller,
            ),
        )
        reconcileCleanup(telecom.calls(), nowElapsedMs)
    }

    private fun reconcileCleanup(calls: List<TelecomCallSnapshot>, nowElapsedMs: Long) {
        var session = current ?: return
        if (session.operatorId == null) {
            val lateOwnedOperators = calls.filter {
                it.ownerSessionId == session.config.sessionId &&
                    it.direction == TelecomCallDirection.OUTGOING && !it.emergency &&
                    it.phoneNumber == session.config.phoneNumber
            }
            if (lateOwnedOperators.size == 1) {
                // Telecom can surface the outgoing Call after a fail-closed transition has
                // already begun. Adopt only the uniquely owned, signed destination so cleanup
                // can still disconnect that exact leg; never fall back to a global hangup.
                update(
                    session.copy(
                        operatorId = lateOwnedOperators.single().id,
                        operatorSafeSinceElapsedMs = null,
                        callerActiveSinceElapsedMs = null,
                        updatedElapsedMs = nowElapsedMs,
                    ),
                )
                session = requireNotNull(current)
            }
        }
        val operatorMatches = session.operatorId?.let { id -> calls.filter { it.id == id } }.orEmpty()
        // A DISCONNECTED Call can remain registered with Samsung Telecom briefly. Treating it
        // as gone races the carrier teardown: an unhold issued in that window is queued and can
        // later become a stale Swap after the caller is already ACTIVE. Require exact removal.
        val operatorSafe = session.operatorId == null || operatorMatches.isEmpty()
        if (!operatorSafe) {
            val operator = operatorMatches.singleOrNull()
            if (operator != null && operator.state !in setOf(
                    TelecomCallState.DISCONNECTING,
                    TelecomCallState.DISCONNECTED,
                ) &&
                session.operatorDisconnectRequestedElapsedMs == null &&
                operator.ownerSessionId == session.config.sessionId &&
                operator.direction == TelecomCallDirection.OUTGOING && !operator.emergency &&
                operator.phoneNumber == session.config.phoneNumber
            ) {
                // The journal marker is deliberately persisted before Telecom is touched. A
                // callback or process recovery therefore cannot replay an uncertain command.
                update(
                    session.copy(
                        operatorDisconnectRequestedElapsedMs = nowElapsedMs,
                        operatorSafeSinceElapsedMs = null,
                        callerActiveSinceElapsedMs = null,
                        updatedElapsedMs = nowElapsedMs,
                    ),
                )
                telecom.disconnect(operator.id)
                return
            }
            clearCallerActiveWindow(session, nowElapsedMs)
            abortCleanupIfExpired(nowElapsedMs)
            return
        }
        val conferenceMatches = session.conferenceId?.let { id -> calls.filter { it.id == id } }.orEmpty()
        val conferenceSafe = session.conferenceId == null || conferenceMatches.isEmpty()
        val expectedIds = setOfNotNull(session.callerId, session.operatorId, session.conferenceId)
        val unexpectedLiveCall = calls.any {
            it.id !in expectedIds && it.state != TelecomCallState.DISCONNECTED
        }
        if (!conferenceSafe || unexpectedLiveCall) {
            clearCallerActiveWindow(session, nowElapsedMs)
            abortCleanupIfExpired(nowElapsedMs)
            return
        }
        if (session.operatorSafeSinceElapsedMs == null) {
            // Start the carrier auto-unhold grace only after every owned non-caller Call object
            // has actually disappeared, not merely after the operator child has disconnected.
            update(
                session.copy(
                    operatorSafeSinceElapsedMs = nowElapsedMs,
                    updatedElapsedMs = nowElapsedMs,
                ),
            )
            session = requireNotNull(current)
        }

        session = requireNotNull(current)
        val callerMatches = session.callerId?.let { id -> calls.filter { it.id == id } }.orEmpty()
        val caller = callerMatches.singleOrNull()
        val callerEnded = session.callerId == null || callerMatches.isEmpty() ||
            (callerMatches.size == 1 && caller?.state == TelecomCallState.DISCONNECTED)
        if (callerEnded) {
            if (session.phase == ExternalCallPhase.CLEANUP_ABORTED) {
                terminal(
                    ExternalCallPhase.SYSTEM_FAILURE,
                    CallControlStatus.SYSTEM_FAILURE,
                    "CLEANUP_TIMEOUT",
                    nowElapsedMs,
                )
            } else {
                complete("CALLER_HANGUP", nowElapsedMs)
            }
            return
        }
        val callerOwned = callerMatches.size == 1 && caller != null &&
            caller.ownerSessionId == session.config.sessionId &&
            caller.direction == TelecomCallDirection.INCOMING && !caller.emergency
        if (!callerOwned) {
            clearCallerActiveWindow(session, nowElapsedMs)
            abortCleanupIfExpired(nowElapsedMs)
            return
        }

        when (caller.state) {
            TelecomCallState.ACTIVE -> {
                val activeSince = session.callerActiveSinceElapsedMs
                if (activeSince == null) {
                    update(
                        session.copy(
                            callerActiveSinceElapsedMs = nowElapsedMs,
                            callerActiveObserved = true,
                            updatedElapsedMs = nowElapsedMs,
                        ),
                    )
                } else if (nowElapsedMs - activeSince >= CALLER_ACTIVE_STABLE_MS) {
                    finishSafeCleanup(session, nowElapsedMs)
                    return
                }
            }
            TelecomCallState.HOLDING -> {
                clearCallerActiveWindow(session, nowElapsedMs)
                session = requireNotNull(current)
                if (session.cleanupUnholdAllowed && !session.callerActiveObserved &&
                    session.callerUnholdRequestedElapsedMs == null &&
                    nowElapsedMs - requireNotNull(session.operatorSafeSinceElapsedMs) >=
                    CARRIER_AUTO_UNHOLD_GRACE_MS
                ) {
                    // Never unhold while an operator/conference leg remains, and journal the
                    // issuance before invoking Telecom. The marker permanently suppresses a
                    // queued stale unhold after the caller first becomes ACTIVE.
                    update(
                        session.copy(
                            callerUnholdRequestedElapsedMs = nowElapsedMs,
                            updatedElapsedMs = nowElapsedMs,
                        ),
                    )
                    telecom.unhold(caller.id)
                    return
                }
            }
            else -> clearCallerActiveWindow(session, nowElapsedMs)
        }
        abortCleanupIfExpired(nowElapsedMs)
    }

    private fun clearCallerActiveWindow(session: ExternalCallSessionSnapshot, nowElapsedMs: Long) {
        if (session.callerActiveSinceElapsedMs != null) {
            update(
                session.copy(
                    callerActiveSinceElapsedMs = null,
                    updatedElapsedMs = nowElapsedMs,
                ),
            )
        }
    }

    private fun finishSafeCleanup(session: ExternalCallSessionSnapshot, nowElapsedMs: Long) {
        when (session.phase) {
            ExternalCallPhase.RETURNING_CALLER -> complete("OPERATOR_HANGUP", nowElapsedMs)
            ExternalCallPhase.CLEANUP_NOT_CONNECTED ->
                terminal(
                    ExternalCallPhase.NOT_CONNECTED,
                    CallControlStatus.NOT_CONNECTED,
                    session.reason,
                    nowElapsedMs,
                )
            ExternalCallPhase.CLEANUP_SYSTEM_FAILURE ->
                terminal(
                    ExternalCallPhase.SYSTEM_FAILURE,
                    CallControlStatus.SYSTEM_FAILURE,
                    session.reason,
                    nowElapsedMs,
                )
            ExternalCallPhase.CLEANUP_ABORTED ->
                terminal(
                    ExternalCallPhase.SYSTEM_FAILURE,
                    CallControlStatus.SYSTEM_FAILURE,
                    "CLEANUP_TIMEOUT",
                    nowElapsedMs,
                )
            ExternalCallPhase.CLEANING_CALLER_HANGUP -> abortCleanup(nowElapsedMs)
            else -> error("Not a cleanup phase: ${session.phase}")
        }
    }

    private fun abortCleanupIfExpired(nowElapsedMs: Long) {
        val session = current ?: return
        if (session.phase != ExternalCallPhase.CLEANUP_ABORTED &&
            nowElapsedMs >= session.deadlineElapsedMs
        ) {
            abortCleanup(nowElapsedMs)
        }
    }

    private fun abortCleanup(nowElapsedMs: Long) {
        val session = current ?: return
        if (session.phase == ExternalCallPhase.CLEANUP_ABORTED) return
        update(
            session.copy(
                phase = ExternalCallPhase.CLEANUP_ABORTED,
                deadlineElapsedMs = 0,
                updatedElapsedMs = nowElapsedMs,
                reason = "CLEANUP_TIMEOUT",
                cleanupOutcomeReason = "CLEANUP_TIMEOUT",
            ),
        )
        publish(CallControlStatus.SYSTEM_FAILURE, "CLEANUP_TIMEOUT")
    }

    private fun failNotConnected(reason: String, nowElapsedMs: Long) {
        startCleanup(ExternalCallPhase.CLEANUP_NOT_CONNECTED, reason, nowElapsedMs)
    }

    private fun failSystem(reason: String, nowElapsedMs: Long, unholdCaller: Boolean = true) {
        if (!unholdCaller) {
            startCleanup(ExternalCallPhase.CLEANUP_SYSTEM_FAILURE, reason, nowElapsedMs, unholdCaller = false)
        } else {
            startCleanup(ExternalCallPhase.CLEANUP_SYSTEM_FAILURE, reason, nowElapsedMs)
        }
    }

    private fun complete(reason: String, nowElapsedMs: Long) {
        terminal(ExternalCallPhase.COMPLETED, CallControlStatus.COMPLETED, reason, nowElapsedMs)
    }

    private fun terminal(
        phase: ExternalCallPhase,
        status: CallControlStatus,
        reason: String,
        nowElapsedMs: Long,
    ) {
        val session = current ?: return
        if (session.terminal) return
        current = session.copy(
            phase = phase,
            callerId = null,
            operatorId = null,
            conferenceId = null,
            deadlineElapsedMs = 0,
            updatedElapsedMs = nowElapsedMs,
            reason = reason,
        )
        persist()
        publish(status, reason)
    }

    private fun update(snapshot: ExternalCallSessionSnapshot) {
        current = snapshot
        persist()
    }

    private fun persist() {
        current?.let(observer::onSnapshot)
    }

    private fun publish(status: CallControlStatus, reason: String) {
        current?.let { observer.onStatus(status, reason, it) }
    }

    private fun statusFor(phase: ExternalCallPhase): CallControlStatus = when (phase) {
        ExternalCallPhase.HOLDING_CALLER -> CallControlStatus.ACK
        ExternalCallPhase.WAITING_DIALING -> CallControlStatus.CALLER_HELD
        ExternalCallPhase.DIALING -> CallControlStatus.DIALING
        ExternalCallPhase.WAITING_RECORDER -> CallControlStatus.OPERATOR_ANSWERED
        ExternalCallPhase.WAITING_CONFERENCEABLE -> CallControlStatus.OPERATOR_ANSWERED
        ExternalCallPhase.MERGING -> CallControlStatus.MERGING
        ExternalCallPhase.CONFERENCED -> CallControlStatus.CONFERENCED
        ExternalCallPhase.RETURNING_CALLER -> CallControlStatus.CONFERENCED
        ExternalCallPhase.CLEANING_CALLER_HANGUP,
        ExternalCallPhase.CLEANUP_NOT_CONNECTED,
        ExternalCallPhase.CLEANUP_SYSTEM_FAILURE,
        -> requireNotNull(current?.cleanupHeartbeatStatus) { "Cleanup lost its monotonic public phase." }
        ExternalCallPhase.CLEANUP_ABORTED -> CallControlStatus.SYSTEM_FAILURE
        ExternalCallPhase.COMPLETED -> CallControlStatus.COMPLETED
        ExternalCallPhase.NOT_CONNECTED -> CallControlStatus.NOT_CONNECTED
        ExternalCallPhase.SYSTEM_FAILURE -> CallControlStatus.SYSTEM_FAILURE
    }

    private fun disconnectReason(kind: TelecomDisconnectKind): String = when (kind) {
        TelecomDisconnectKind.BUSY -> "BUSY"
        TelecomDisconnectKind.REJECTED -> "REJECTED"
        TelecomDisconnectKind.ERROR -> "NETWORK_ERROR"
        TelecomDisconnectKind.LOCAL -> "LOCAL_DISCONNECT"
        TelecomDisconnectKind.REMOTE -> "OPERATOR_DISCONNECTED"
        TelecomDisconnectKind.UNKNOWN -> "NOT_CONNECTED"
    }

    private fun checkedDeadline(now: Long, duration: Long): Long = Math.addExact(now, duration)

    private companion object {
        const val SETUP_TIMEOUT_MS = 10_000L
        const val RECORDER_TIMEOUT_MS = 10_000L
        const val MERGE_TIMEOUT_MS = 10_000L
        const val CLEANUP_TIMEOUT_MS = 12_000L
        const val CALLER_ACTIVE_STABLE_MS = 1_000L
        const val CARRIER_AUTO_UNHOLD_GRACE_MS = 2_000L
        val CLEANUP_PHASES = setOf(
            ExternalCallPhase.RETURNING_CALLER,
            ExternalCallPhase.CLEANING_CALLER_HANGUP,
            ExternalCallPhase.CLEANUP_NOT_CONNECTED,
            ExternalCallPhase.CLEANUP_SYSTEM_FAILURE,
            ExternalCallPhase.CLEANUP_ABORTED,
        )
    }
}
