package ai.rx1.ivrdroid.telecom.external

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.file.Files

class ExternalCallEngineTest {
    private val sessionId = "11111111-1111-4111-8111-111111111111"
    private val blockId = "22222222-2222-4222-8222-222222222222"
    private val bootId = "33333333-3333-4333-8333-333333333333"
    private val config = ExternalCallConfig(sessionId, 91, blockId, "03136644636", 30_000, bootId)

    @Test
    fun confirmedConversationRecoversAcrossAllControllerOutagesWithoutRepeatingActions() {
        for (outage in listOf(1000L, 3000L, 10000L, 40000L, 130000L)) {
            val fixture = Fixture()
            fixture.beginAndDial()
            fixture.telecom.addOperator(TelecomCallState.ACTIVE)
            fixture.engine.reconcile(2000)
            fixture.engine.recorderReady(2100)
            fixture.telecom.conference = "conference"
            fixture.engine.reconcile(2200)
            val snapshot = requireNotNull(fixture.engine.snapshot())
            fixture.telecom.actions.clear()
            val recovered = ExternalCallEngine(fixture.telecom, fixture.observer, snapshot)
            recovered.recover(bootId, 2200 + outage, independentlyReconciled = true)
            assertEquals(ExternalCallPhase.CONFERENCED, recovered.snapshot()?.phase)
            assertTrue(fixture.telecom.actions.isEmpty())
        }
    }

    @Test
    fun successfulAnswerRecordingGateMergeAndOperatorHangup() {
        val fixture = Fixture()
        fixture.beginAndDial()
        fixture.telecom.addOperator(TelecomCallState.ACTIVE)
        fixture.engine.reconcile(2_000)
        assertEquals(CallControlStatus.OPERATOR_ANSWERED, fixture.observer.statuses.last().first)

        fixture.engine.recorderReady(2_100)
        assertTrue(fixture.telecom.actions.contains("conference:caller:operator"))
        fixture.telecom.conference = "conference"
        fixture.engine.reconcile(2_200)
        assertEquals(CallControlStatus.CONFERENCED, fixture.observer.statuses.last().first)

        fixture.telecom.update("operator") { copy(state = TelecomCallState.DISCONNECTED) }
        fixture.telecom.update("caller") { copy(state = TelecomCallState.ACTIVE) }
        fixture.engine.reconcile(3_000)
        assertEquals(ExternalCallPhase.RETURNING_CALLER, fixture.engine.snapshot()?.phase)
        fixture.telecom.calls.removeAll { it.id == "operator" }
        fixture.engine.reconcile(3_100)
        fixture.engine.reconcile(4_100)
        assertEquals(ExternalCallPhase.COMPLETED, fixture.engine.snapshot()?.phase)
        assertEquals(CallControlStatus.COMPLETED to "OPERATOR_HANGUP", fixture.observer.statuses.last())
    }

    @Test
    fun callerHeldStatusUsesTheTransitionTimestamp() {
        val fixture = Fixture()
        fixture.engine.begin(config, "caller", 1_000)
        fixture.telecom.update("caller") { copy(state = TelecomCallState.HOLDING) }

        fixture.engine.reconcile(1_100)

        assertEquals(CallControlStatus.CALLER_HELD, fixture.observer.statuses.last().first)
        assertEquals(1_100L, fixture.observer.statusSnapshots.last().updatedElapsedMs)
    }

    @Test
    fun recorderWaitsForTelecomConferenceabilityBeforeRequestingMerge() {
        val fixture = Fixture()
        fixture.beginAndDial()
        fixture.telecom.addOperator(TelecomCallState.ACTIVE)
        fixture.engine.reconcile(2_000)
        fixture.telecom.conferenceAvailable = false

        fixture.engine.recorderReady(2_100)

        assertEquals(ExternalCallPhase.WAITING_CONFERENCEABLE, fixture.engine.snapshot()?.phase)
        assertFalse(fixture.telecom.actions.contains("conference:caller:operator"))
        assertEquals(CallControlStatus.OPERATOR_ANSWERED, fixture.observer.statuses.last().first)

        fixture.telecom.conferenceAvailable = true
        fixture.engine.reconcile(2_200)
        assertEquals(ExternalCallPhase.MERGING, fixture.engine.snapshot()?.phase)
        assertTrue(fixture.telecom.actions.contains("conference:caller:operator"))
    }

    @Test
    fun lateRecordingFailurePreservesCompletedCallOutcome() {
        val fixture = Fixture()
        fixture.beginAndDial()
        fixture.telecom.addOperator(TelecomCallState.ACTIVE)
        fixture.engine.reconcile(2_000)
        fixture.engine.recorderReady(2_100)
        fixture.telecom.conference = "conference"
        fixture.engine.reconcile(2_200)
        fixture.telecom.update("operator") { copy(state = TelecomCallState.DISCONNECTED) }
        fixture.telecom.update("caller") { copy(state = TelecomCallState.ACTIVE) }
        fixture.engine.reconcile(3_000)
        fixture.telecom.calls.removeAll { it.id == "operator" }
        fixture.engine.reconcile(3_100)
        fixture.engine.reconcile(4_100)
        assertEquals(ExternalCallPhase.COMPLETED, fixture.engine.snapshot()?.phase)

        fixture.engine.recordingHandoffFailed(42_100)

        assertEquals(ExternalCallPhase.COMPLETED, fixture.engine.snapshot()?.phase)
        assertEquals(CallControlStatus.COMPLETED to "OPERATOR_HANGUP", fixture.observer.statuses.last())
        assertEquals(0, fixture.telecom.actionCount("disconnect:operator"))
    }

    @Test
    fun recordingFailureDuringConversationDoesNotDisconnectOrPlayFailureBranch() {
        val fixture = Fixture()
        fixture.beginAndDial()
        fixture.telecom.addOperator(TelecomCallState.ACTIVE)
        fixture.engine.reconcile(2_000)
        fixture.engine.recorderReady(2_100)
        fixture.telecom.conference = "conference"
        fixture.engine.reconcile(2_200)
        val established = fixture.engine.snapshot()

        fixture.engine.recordingHandoffFailed(192_400)
        fixture.engine.reconcile(192_500)
        fixture.engine.recordingHandoffFailed(220_400)
        fixture.engine.reconcile(400_000)

        assertEquals(established, fixture.engine.snapshot())
        assertEquals(CallControlStatus.CONFERENCED to "-", fixture.observer.statuses.last())
        assertEquals(0, fixture.telecom.actionCount("disconnect:operator"))
        assertEquals(0, fixture.telecom.actionCount("disconnect:caller"))
        assertEquals(0, fixture.telecom.actionCount("unhold:caller"))
    }

    @Test
    fun configuredAnswerTimeoutCleansOnlyOwnedOperatorAndUnholdsCaller() {
        val fixture = Fixture()
        fixture.beginAndDial()
        fixture.telecom.addOperator(TelecomCallState.DIALING)
        fixture.engine.reconcile(2_000)
        fixture.engine.reconcile(32_001)

        fixture.telecom.update("operator") { copy(state = TelecomCallState.DISCONNECTED) }
        fixture.engine.reconcile(32_100)
        assertEquals(0, fixture.telecom.actionCount("unhold:caller"))
        fixture.telecom.calls.removeAll { it.id == "operator" }
        fixture.engine.reconcile(32_200)
        fixture.engine.reconcile(34_100)
        fixture.engine.reconcile(34_200)
        fixture.telecom.update("caller") { copy(state = TelecomCallState.ACTIVE) }
        fixture.engine.reconcile(34_300)
        fixture.engine.reconcile(35_300)
        assertEquals(CallControlStatus.NOT_CONNECTED to "ANSWER_TIMEOUT", fixture.observer.statuses.last())
        assertTrue("disconnect:operator" in fixture.telecom.actions)
        assertTrue("unhold:caller" in fixture.telecom.actions)
    }

    @Test
    fun busyAndPreMergeDisconnectUseNotConnectedBranch() {
        val busy = Fixture()
        busy.beginAndDial()
        busy.telecom.addOperator(TelecomCallState.DISCONNECTED, TelecomDisconnectKind.BUSY)
        busy.engine.reconcile(2_000)
        busy.telecom.update("caller") { copy(state = TelecomCallState.ACTIVE) }
        busy.telecom.calls.removeAll { it.id == "operator" }
        busy.engine.reconcile(2_100)
        busy.engine.reconcile(3_100)
        assertEquals(CallControlStatus.NOT_CONNECTED to "BUSY", busy.observer.statuses.last())

        val dropped = Fixture()
        dropped.beginAndDial()
        dropped.telecom.addOperator(TelecomCallState.ACTIVE)
        dropped.engine.reconcile(2_000)
        dropped.telecom.update("operator") { copy(state = TelecomCallState.DISCONNECTED) }
        dropped.engine.reconcile(2_100)
        dropped.telecom.update("caller") { copy(state = TelecomCallState.ACTIVE) }
        dropped.telecom.calls.removeAll { it.id == "operator" }
        dropped.engine.reconcile(2_200)
        dropped.engine.reconcile(3_200)
        assertEquals(CallControlStatus.NOT_CONNECTED to "OPERATOR_DISCONNECTED", dropped.observer.statuses.last())
    }

    @Test
    fun mergeFailureIsFailClosedAndNeverClaimsSuccess() {
        val fixture = Fixture()
        fixture.beginAndDial()
        fixture.telecom.addOperator(TelecomCallState.ACTIVE)
        fixture.engine.reconcile(2_000)
        fixture.telecom.conferenceAccepted = false
        fixture.engine.recorderReady(2_100)
        fixture.telecom.update("operator") { copy(state = TelecomCallState.DISCONNECTED) }
        fixture.telecom.update("caller") { copy(state = TelecomCallState.ACTIVE) }
        fixture.engine.reconcile(2_200)
        fixture.telecom.calls.removeAll { it.id == "operator" }
        fixture.engine.reconcile(2_300)
        fixture.engine.reconcile(3_300)

        assertEquals(CallControlStatus.SYSTEM_FAILURE to "MERGE_FAILED", fixture.observer.statuses.last())
        assertTrue("disconnect:operator" in fixture.telecom.actions)
        assertFalse("unhold:caller" in fixture.telecom.actions)
        assertFalse(fixture.observer.statuses.any { it.first == CallControlStatus.CONFERENCED })
    }

    @Test
    fun callerHangupWaitsForExactOperatorCleanupBeforeCompleted() {
        val fixture = Fixture()
        fixture.beginAndDial()
        fixture.telecom.addOperator(TelecomCallState.ACTIVE)
        fixture.engine.reconcile(2_000)
        fixture.telecom.update("caller") { copy(state = TelecomCallState.DISCONNECTED) }
        fixture.engine.reconcile(2_100)

        assertEquals(ExternalCallPhase.CLEANING_CALLER_HANGUP, fixture.engine.snapshot()?.phase)
        assertTrue("disconnect:operator" in fixture.telecom.actions)
        assertFalse(fixture.observer.statuses.any { it == CallControlStatus.COMPLETED to "CALLER_HANGUP" })

        fixture.telecom.calls.removeAll { it.id == "operator" }
        fixture.engine.reconcile(2_200)
        assertEquals(CallControlStatus.COMPLETED to "CALLER_HANGUP", fixture.observer.statuses.last())
    }

    @Test
    fun emergencyOrUnownedCallsAreNeverMutated() {
        val unowned = Fixture(callerOwner = null)
        unowned.engine.begin(config, "caller", 1_000)
        assertEquals(CallControlStatus.SYSTEM_FAILURE to "CALLER_OWNER_MISMATCH", unowned.observer.statuses.last())
        assertTrue(unowned.telecom.actions.isEmpty())

        val emergency = Fixture(emergencyDestination = true)
        emergency.engine.begin(config, "caller", 1_000)
        assertEquals(CallControlStatus.SYSTEM_FAILURE to "DESTINATION_EMERGENCY", emergency.observer.statuses.last())
        assertTrue(emergency.telecom.actions.isEmpty())
    }

    @Test
    fun sameBootRecoveryTimesOutAndStaleBootRecoveryTouchesNothing() {
        val sameBoot = Fixture()
        sameBoot.telecom.update("caller") { copy(state = TelecomCallState.HOLDING) }
        sameBoot.telecom.addOperator(TelecomCallState.DIALING)
        val recovered = ExternalCallSessionSnapshot(
            config,
            ExternalCallPhase.DIALING,
            "caller",
            "operator",
            null,
            10_000,
            2_000,
        )
        val sameBootEngine = ExternalCallEngine(sameBoot.telecom, sameBoot.observer, recovered)
        sameBootEngine.recover(bootId, 11_000)
        sameBoot.telecom.update("operator") { copy(state = TelecomCallState.DISCONNECTED) }
        sameBoot.telecom.update("caller") { copy(state = TelecomCallState.ACTIVE) }
        sameBootEngine.reconcile(11_100)
        sameBoot.telecom.calls.removeAll { it.id == "operator" }
        sameBootEngine.reconcile(11_200)
        sameBootEngine.reconcile(12_200)
        assertEquals(CallControlStatus.NOT_CONNECTED to "ANSWER_TIMEOUT", sameBoot.observer.statuses.last())
        assertTrue("disconnect:operator" in sameBoot.telecom.actions)

        val stale = Fixture()
        val staleEngine = ExternalCallEngine(stale.telecom, stale.observer, recovered)
        staleEngine.recover("44444444-4444-4444-8444-444444444444", 11_000)
        assertEquals(CallControlStatus.SYSTEM_FAILURE to "STALE_BOOT", stale.observer.statuses.last())
        assertTrue(stale.telecom.actions.isEmpty())
    }

    @Test
    fun setupWatchdogEndsAtObservedDialingAndOnlyThenStartsAnswerTimer() {
        val fixture = Fixture()
        fixture.beginAndDial()
        fixture.telecom.addOperator(TelecomCallState.NEW)
        fixture.engine.reconcile(9_000)
        assertEquals(ExternalCallPhase.WAITING_DIALING, fixture.engine.snapshot()?.phase)

        fixture.telecom.update("operator") { copy(state = TelecomCallState.DIALING) }
        fixture.engine.reconcile(9_500)
        assertEquals(ExternalCallPhase.DIALING, fixture.engine.snapshot()?.phase)
        assertEquals(39_500L, fixture.engine.snapshot()?.deadlineElapsedMs)
        fixture.engine.reconcile(35_000)
        assertEquals(ExternalCallPhase.DIALING, fixture.engine.snapshot()?.phase)
    }

    @Test
    fun cleanupTimeoutNeverResumesABranchAndRetainsExactIdsForRecovery() {
        val fixture = Fixture()
        fixture.beginAndDial()
        fixture.telecom.addOperator(TelecomCallState.DIALING)
        fixture.engine.reconcile(2_000)
        fixture.engine.reconcile(32_001)
        assertEquals(ExternalCallPhase.CLEANUP_NOT_CONNECTED, fixture.engine.snapshot()?.phase)

        fixture.engine.reconcile(44_002)
        val snapshot = fixture.engine.snapshot()
        assertEquals(ExternalCallPhase.CLEANUP_ABORTED, snapshot?.phase)
        assertEquals("caller", snapshot?.callerId)
        assertEquals("operator", snapshot?.operatorId)
        assertEquals(CallControlStatus.SYSTEM_FAILURE to "CLEANUP_TIMEOUT", fixture.observer.statuses.last())
        assertFalse(snapshot?.terminal ?: true)

        fixture.telecom.calls.removeAll { it.id == "operator" }
        fixture.telecom.update("caller") { copy(state = TelecomCallState.DISCONNECTED) }
        fixture.engine.reconcile(44_100)
        assertEquals(ExternalCallPhase.SYSTEM_FAILURE, fixture.engine.snapshot()?.phase)
        assertEquals(CallControlStatus.SYSTEM_FAILURE to "CLEANUP_TIMEOUT", fixture.observer.statuses.last())
        assertFalse(fixture.observer.statuses.any { it == CallControlStatus.COMPLETED to "CALLER_HANGUP" })
    }

    @Test
    fun cleanupRecoveryPreservesTheLastMonotonicPublicPhase() {
        val fixture = Fixture()
        fixture.beginAndDial()
        fixture.telecom.addOperator(TelecomCallState.ACTIVE)
        fixture.engine.reconcile(2_000)
        assertEquals(CallControlStatus.OPERATOR_ANSWERED, fixture.observer.statuses.last().first)

        fixture.telecom.update("operator") { copy(state = TelecomCallState.DISCONNECTED) }
        fixture.engine.reconcile(2_100)
        assertEquals(ExternalCallPhase.CLEANUP_NOT_CONNECTED, fixture.engine.snapshot()?.phase)
        val recovered = ExternalCallEngine(
            fixture.telecom,
            fixture.observer,
            requireNotNull(fixture.engine.snapshot()),
        )
        recovered.recover(bootId, 2_200)

        assertEquals(CallControlStatus.OPERATOR_ANSWERED, fixture.observer.statuses.last().first)
        assertFalse(fixture.observer.statuses.any { it.first == CallControlStatus.DIALING && it.second != "-" })
    }

    @Test
    fun answerTimeoutCancelIsIdempotentAndCleanupActionsAreSingleFlight() {
        val fixture = Fixture()
        fixture.beginAndDial()
        fixture.telecom.addOperator(TelecomCallState.DIALING)
        fixture.engine.reconcile(2_000)
        assertEquals(32_000L, fixture.engine.snapshot()?.deadlineElapsedMs)

        fixture.engine.cancel("ANSWER_TIMEOUT", 32_000)
        val cleanupDeadline = fixture.engine.snapshot()?.deadlineElapsedMs
        assertEquals(ExternalCallPhase.CLEANUP_NOT_CONNECTED, fixture.engine.snapshot()?.phase)
        assertEquals("ANSWER_TIMEOUT", fixture.engine.snapshot()?.cleanupOutcomeReason)
        assertEquals(1, fixture.telecom.actionCount("disconnect:operator"))
        assertEquals(0, fixture.telecom.actionCount("unhold:caller"))

        // Callback and 250ms poll storms while Telecom still reports DIALING cannot queue more
        // disconnects or an early unhold.
        listOf(32_001L, 32_250L, 32_500L).forEach(fixture.engine::reconcile)
        fixture.engine.cancel("ANSWER_TIMEOUT", 32_750)
        assertEquals(cleanupDeadline, fixture.engine.snapshot()?.deadlineElapsedMs)
        assertEquals(1, fixture.telecom.actionCount("disconnect:operator"))
        assertEquals(0, fixture.telecom.actionCount("unhold:caller"))

        fixture.telecom.update("operator") { copy(state = TelecomCallState.DISCONNECTING) }
        fixture.engine.reconcile(33_000)
        assertEquals(0, fixture.telecom.actionCount("unhold:caller"))

        fixture.telecom.update("operator") { copy(state = TelecomCallState.DISCONNECTED) }
        fixture.engine.reconcile(33_100)
        assertEquals(0, fixture.telecom.actionCount("unhold:caller"))
        fixture.telecom.calls.removeAll { it.id == "operator" }
        fixture.engine.reconcile(33_200)
        fixture.engine.reconcile(35_099)
        assertEquals(0, fixture.telecom.actionCount("unhold:caller"))
        fixture.engine.reconcile(35_200)
        assertEquals(1, fixture.telecom.actionCount("unhold:caller"))
        listOf(35_101L, 35_250L, 35_500L).forEach(fixture.engine::reconcile)
        assertEquals(1, fixture.telecom.actionCount("unhold:caller"))

        // A queued stale callback after ACTIVE must never enqueue another unhold/Swap.
        fixture.telecom.update("caller") { copy(state = TelecomCallState.ACTIVE) }
        fixture.engine.reconcile(36_000)
        fixture.telecom.update("caller") { copy(state = TelecomCallState.HOLDING) }
        listOf(36_250L, 36_500L, 36_750L).forEach(fixture.engine::reconcile)
        assertEquals(1, fixture.telecom.actionCount("unhold:caller"))
        assertEquals(ExternalCallPhase.CLEANUP_NOT_CONNECTED, fixture.engine.snapshot()?.phase)

        fixture.telecom.update("caller") { copy(state = TelecomCallState.ACTIVE) }
        fixture.engine.reconcile(37_000)
        fixture.engine.reconcile(37_999)
        assertEquals(ExternalCallPhase.CLEANUP_NOT_CONNECTED, fixture.engine.snapshot()?.phase)
        fixture.engine.reconcile(38_000)
        assertEquals(CallControlStatus.NOT_CONNECTED to "ANSWER_TIMEOUT", fixture.observer.statuses.last())

        fixture.engine.cancel("ANSWER_TIMEOUT", 38_100)
        assertEquals(ExternalCallPhase.NOT_CONNECTED, fixture.engine.snapshot()?.phase)
        assertEquals("ANSWER_TIMEOUT", fixture.engine.snapshot()?.reason)
    }

    @Test
    fun carrierAutoUnholdWithinGraceSuppressesExplicitUnhold() {
        val fixture = Fixture()
        fixture.beginAndDial()
        fixture.telecom.addOperator(TelecomCallState.DIALING)
        fixture.engine.reconcile(2_000)
        fixture.engine.cancel("ANSWER_TIMEOUT", 32_000)

        fixture.telecom.update("operator") { copy(state = TelecomCallState.DISCONNECTED) }
        fixture.engine.reconcile(32_100)
        fixture.telecom.calls.removeAll { it.id == "operator" }
        fixture.engine.reconcile(32_200)

        // The carrier resumes the original caller before the two-second fallback grace expires.
        fixture.telecom.update("caller") { copy(state = TelecomCallState.ACTIVE) }
        fixture.engine.reconcile(33_900)
        fixture.engine.reconcile(34_900)

        assertEquals(0, fixture.telecom.actionCount("unhold:caller"))
        assertEquals(CallControlStatus.NOT_CONNECTED to "ANSWER_TIMEOUT", fixture.observer.statuses.last())
    }

    @Test
    fun answerTimeoutCancelInAnsweredPhaseFailsClosedAsOutcomeMismatch() {
        val fixture = Fixture()
        fixture.beginAndDial()
        fixture.telecom.addOperator(TelecomCallState.ACTIVE)
        fixture.engine.reconcile(2_000)
        assertEquals(ExternalCallPhase.WAITING_RECORDER, fixture.engine.snapshot()?.phase)

        fixture.engine.cancel("ANSWER_TIMEOUT", 2_100)
        assertEquals(ExternalCallPhase.CLEANUP_SYSTEM_FAILURE, fixture.engine.snapshot()?.phase)
        assertEquals("CANCEL_OUTCOME_MISMATCH", fixture.engine.snapshot()?.reason)
        assertEquals(1, fixture.telecom.actionCount("disconnect:operator"))

        fixture.telecom.update("operator") { copy(state = TelecomCallState.DISCONNECTED) }
        fixture.telecom.update("caller") { copy(state = TelecomCallState.ACTIVE) }
        fixture.engine.reconcile(2_200)
        fixture.telecom.calls.removeAll { it.id == "operator" }
        fixture.engine.reconcile(2_300)
        fixture.engine.reconcile(3_300)
        assertEquals(
            CallControlStatus.SYSTEM_FAILURE to "CANCEL_OUTCOME_MISMATCH",
            fixture.observer.statuses.last(),
        )
        assertFalse(fixture.observer.statuses.any {
            it == CallControlStatus.NOT_CONNECTED to "ANSWER_TIMEOUT"
        })
    }

    @Test
    fun helperCancelledReasonRemainsSystemFailure() {
        val fixture = Fixture()
        fixture.beginAndDial()
        fixture.telecom.addOperator(TelecomCallState.DIALING)
        fixture.engine.reconcile(2_000)

        fixture.engine.cancel("HELPER_CANCELLED", 2_100)
        assertEquals(ExternalCallPhase.CLEANUP_SYSTEM_FAILURE, fixture.engine.snapshot()?.phase)
        assertEquals("HELPER_CANCELLED", fixture.engine.snapshot()?.reason)
    }

    @Test
    fun cleanupAdoptsAndDisconnectsAnOwnedOperatorThatAppearsLate() {
        val fixture = Fixture()
        fixture.beginAndDial()
        fixture.engine.cancel("HELPER_CANCELLED", 1_150)
        assertEquals(null, fixture.engine.snapshot()?.operatorId)

        fixture.telecom.addOperator(TelecomCallState.DIALING)
        fixture.engine.reconcile(1_200)

        assertEquals("operator", fixture.engine.snapshot()?.operatorId)
        assertEquals(1, fixture.telecom.actionCount("disconnect:operator"))
        assertFalse(fixture.engine.snapshot()?.terminal ?: true)
    }

    @Test
    fun recoveryNeverReissuesJournaledTelecomActions() {
        val disconnectFixture = Fixture()
        disconnectFixture.telecom.update("caller") { copy(state = TelecomCallState.HOLDING) }
        disconnectFixture.telecom.addOperator(TelecomCallState.DIALING)
        val disconnectIssued = ExternalCallSessionSnapshot(
            config = config,
            phase = ExternalCallPhase.CLEANUP_NOT_CONNECTED,
            callerId = "caller",
            operatorId = "operator",
            conferenceId = null,
            deadlineElapsedMs = 20_000,
            updatedElapsedMs = 3_000,
            reason = "ANSWER_TIMEOUT",
            cleanupHeartbeatStatus = CallControlStatus.DIALING,
            cleanupOutcomeReason = "ANSWER_TIMEOUT",
            operatorDisconnectRequestedElapsedMs = 2_500,
        )
        ExternalCallEngine(
            disconnectFixture.telecom,
            disconnectFixture.observer,
            disconnectIssued,
        ).recover(bootId, 3_100)
        assertEquals(0, disconnectFixture.telecom.actionCount("disconnect:operator"))
        assertEquals(0, disconnectFixture.telecom.actionCount("unhold:caller"))

        val unholdFixture = Fixture()
        unholdFixture.telecom.update("caller") { copy(state = TelecomCallState.HOLDING) }
        val unholdIssued = disconnectIssued.copy(
            operatorSafeSinceElapsedMs = 2_000,
            callerUnholdRequestedElapsedMs = 4_000,
            updatedElapsedMs = 4_000,
        )
        ExternalCallEngine(
            unholdFixture.telecom,
            unholdFixture.observer,
            unholdIssued,
        ).recover(bootId, 4_100)
        assertEquals(0, unholdFixture.telecom.actionCount("disconnect:operator"))
        assertEquals(0, unholdFixture.telecom.actionCount("unhold:caller"))
    }

    @Test
    fun requestSequenceAndElapsedTimeAreStrictlyMonotonicAndJournaled() {
        val fixture = Fixture()
        fixture.engine.begin(config, "caller", 1_000, requestSequence = 1, requestElapsedMs = 900)
        assertTrue(fixture.engine.recordRequest("RECORDER_READY", 2, 950, 1_100))
        assertFalse(fixture.engine.recordRequest("RECORDER_READY", 2, 950, 1_100))
        assertFalse(fixture.engine.recordRequest("CANCEL", 3, 949, 1_100))
        assertFalse(fixture.engine.recordRequest("CANCEL", 3, 1_101, 1_100))
        assertEquals(2L, fixture.engine.snapshot()?.lastRequestSequence)
        assertEquals(950L, fixture.engine.snapshot()?.lastRequestElapsedMs)
    }

    @Test
    fun unrelatedThirdCallFailsClosedButIsNeverMutated() {
        val fixture = Fixture()
        fixture.beginAndDial()
        fixture.telecom.addOperator(TelecomCallState.DIALING)
        fixture.engine.reconcile(2_000)
        fixture.telecom.calls.add(
            TelecomCallSnapshot(
                "third-party",
                TelecomCallDirection.INCOMING,
                TelecomCallState.ACTIVE,
                null,
                "+989120000000",
                false,
                true,
            ),
        )
        fixture.engine.reconcile(2_100)

        assertEquals(ExternalCallPhase.CLEANUP_SYSTEM_FAILURE, fixture.engine.snapshot()?.phase)
        assertFalse("disconnect:third-party" in fixture.telecom.actions)
        assertTrue("disconnect:operator" in fixture.telecom.actions)
    }

    @Test
    fun conferenceParentWithPropagatedSessionExtraIsNotAnOperatorLeg() {
        val fixture = Fixture()
        fixture.beginAndDial()
        fixture.telecom.addOperator(TelecomCallState.ACTIVE)
        fixture.engine.reconcile(2_000)
        fixture.engine.recorderReady(2_100)
        fixture.telecom.conference = "conference"
        fixture.telecom.calls.add(
            TelecomCallSnapshot(
                "conference",
                TelecomCallDirection.UNKNOWN,
                TelecomCallState.ACTIVE,
                sessionId,
                null,
                false,
                false,
            ),
        )
        fixture.engine.reconcile(2_200)

        assertEquals(ExternalCallPhase.CONFERENCED, fixture.engine.snapshot()?.phase)
        assertFalse(fixture.observer.statuses.any { it.second == "AMBIGUOUS_CALL_TOPOLOGY" })
    }

    @Test
    fun recoveryJournalRoundTripsAndRejectsCorruption() {
        val directory = Files.createTempDirectory("ivrdroid-external-call-test").toFile()
        val journal = ExternalCallJournal(directory.resolve("recovery.json"))
        val snapshot = ExternalCallSessionSnapshot(
            config,
            ExternalCallPhase.CLEANUP_NOT_CONNECTED,
            "caller",
            "operator",
            null,
            12_000,
            4_200,
            reason = "ANSWER_TIMEOUT",
            cleanupHeartbeatStatus = CallControlStatus.DIALING,
            cleanupOutcomeReason = "ANSWER_TIMEOUT",
            operatorDisconnectRequestedElapsedMs = 2_000,
            operatorSafeSinceElapsedMs = 2_100,
            callerUnholdRequestedElapsedMs = 4_100,
            callerActiveSinceElapsedMs = 4_200,
            callerActiveObserved = true,
        )
        journal.save(snapshot)
        assertEquals(snapshot, journal.load())
        directory.resolve("recovery.json").writeText("not-json")
        assertEquals(null, journal.load())
    }

    private inner class Fixture(
        callerOwner: String? = sessionId,
        emergencyDestination: Boolean = false,
    ) {
        val telecom = FakeTelecom(emergencyDestination).apply {
            calls.add(
                TelecomCallSnapshot(
                    "caller",
                    TelecomCallDirection.INCOMING,
                    TelecomCallState.ACTIVE,
                    callerOwner,
                    "+982100000000",
                    false,
                    true,
                ),
            )
        }
        val observer = FakeObserver()
        val engine = ExternalCallEngine(telecom, observer)

        fun beginAndDial() {
            engine.begin(config, "caller", 1_000)
            telecom.update("caller") { copy(state = TelecomCallState.HOLDING) }
            engine.reconcile(1_100)
            assertTrue("place:${config.phoneNumber}:$sessionId:caller" in telecom.actions)
        }
    }

    private class FakeObserver : ExternalCallObserver {
        val statuses = mutableListOf<Pair<CallControlStatus, String>>()
        val statusSnapshots = mutableListOf<ExternalCallSessionSnapshot>()
        val snapshots = mutableListOf<ExternalCallSessionSnapshot>()
        override fun onStatus(status: CallControlStatus, reason: String, snapshot: ExternalCallSessionSnapshot) {
            statuses += status to reason
            statusSnapshots += snapshot
        }
        override fun onSnapshot(snapshot: ExternalCallSessionSnapshot) {
            snapshots += snapshot
        }
    }

    private class FakeTelecom(private val emergencyDestination: Boolean) : TelecomControl {
        val calls = mutableListOf<TelecomCallSnapshot>()
        val actions = mutableListOf<String>()
        var conferenceAvailable = true
        var conferenceAccepted = true
        var conference: String? = null

        override fun calls(): List<TelecomCallSnapshot> = calls.toList()
        override fun isEmergencyNumber(phoneNumber: String): Boolean = emergencyDestination
        override fun hold(callId: String): Boolean = actions.add("hold:$callId")
        override fun unhold(callId: String): Boolean = actions.add("unhold:$callId")
        override fun disconnect(callId: String): Boolean = actions.add("disconnect:$callId")
        override fun placeCall(phoneNumber: String, sessionId: String, callerId: String): Boolean =
            actions.add("place:$phoneNumber:$sessionId:$callerId")
        override fun canConference(callerId: String, operatorId: String): Boolean = conferenceAvailable
        override fun conference(callerId: String, operatorId: String): Boolean {
            actions.add("conference:$callerId:$operatorId")
            return conferenceAccepted
        }
        override fun conferenceId(callerId: String, operatorId: String): String? = conference

        fun addOperator(
            state: TelecomCallState,
            disconnect: TelecomDisconnectKind = TelecomDisconnectKind.UNKNOWN,
        ) {
            calls.add(
                TelecomCallSnapshot(
                    "operator",
                    TelecomCallDirection.OUTGOING,
                    state,
                    "11111111-1111-4111-8111-111111111111",
                    "03136644636",
                    false,
                    true,
                    disconnect,
                ),
            )
        }

        fun update(id: String, transform: TelecomCallSnapshot.() -> TelecomCallSnapshot) {
            val index = calls.indexOfFirst { it.id == id }
            calls[index] = calls[index].transform()
        }

        fun actionCount(action: String): Int = actions.count { it == action }
    }
}
