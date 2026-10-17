package ai.rx1.ivrdroid.control

import android.content.Context
import android.os.Build
import ai.rx1.ivrdroid.BuildConfig
import ai.rx1.ivrdroid.IvrPreferences
import ai.rx1.ivrdroid.audio.RootAudioTrigger
import ai.rx1.ivrdroid.telecom.CallRuntimeState
import ai.rx1.ivrdroid.telecom.external.ExternalCallCapabilities
import ai.rx1.ivrdroid.telecom.external.ExternalCallRuntimeStatus
import org.json.JSONObject

object SyncCoordinator {
    private const val HELPER_OPERATION_TIMEOUT_MS = 45_000L

    @Synchronized
    fun synchronize(context: Context): SyncResult {
        val application = context.applicationContext
        ai.rx1.ivrdroid.telecom.LocalCallSession.reconcile(application)
        // Local recovery is independent of enrollment, connectivity and server availability.
        runCatching { ContinuousRecordingSpool.recover(application) }
        val enrollment = SecureControlStore.enrollment(application) ?: return SyncResult.NotEnrolled
        val initialHelper = RootAudioTrigger.readState(application)
        val api = DeviceApi(enrollment)
        if (CallRuntimeState.isBusy() || !initialHelper.isIdle) {
            // Recovery is local and already running. A metadata-only heartbeat
            // can report its readiness or a disabled supervisor without moving
            // audio, changing a call's policy snapshot, or waiting on the network.
            runCatching { api.sync(buildSyncRequest(application, initialHelper)) }
            return SyncResult.Busy
        }
        var desiredRevision: Long? = null
        return try {
            // Control synchronization takes priority over the recording data plane.
            SessionAuditSettings.restore(application)
            CallSafetySettings.restore(application)
            runCatching { SessionAuditSpool.publishCapacity(application) }
            runCatching { ContinuousRecordingSpool.reconcile(application) }
            runCatching { SessionAuditSpool.reconcile(application) }
            val response = api.sync(buildSyncRequest(application, initialHelper))
            response.optJSONObject("call_safety_policy")?.let { CallSafetySettings.apply(application, it) }
            response.optJSONObject("audit_policy")?.let { policy ->
                SessionAuditSettings.apply(application, policy)
                // Publish the applied-policy acknowledgement before sending call metadata.
                api.sync(buildSyncRequest(application, RootAudioTrigger.readState(application)))
            }
            desiredRevision = response.optLong("desired_revision_id", 0).takeIf { it > 0 }
            if (desiredRevision != null && desiredRevision != initialHelper.activeRevision) {
                val manifestHash = response.getString("manifest_sha256")
                val signature = response.getString("signature_b64")
                val verified = RevisionVerifier.verify(
                    api.manifest(desiredRevision),
                    desiredRevision,
                    manifestHash,
                    signature,
                )
                RevisionFiles.prepare(application, verified, api)
                require(RootAudioTrigger.requestStageRevision(application, desiredRevision, manifestHash)) {
                    "The helper was not idle for staging."
                }
                require(waitForHelper(application) { it.stagedRevision == desiredRevision && it.isIdle }) {
                    "The helper rejected or timed out while staging revision $desiredRevision."
                }
                api.acknowledge(desiredRevision, "staged")
                require(RootAudioTrigger.requestActivateStaged(application, desiredRevision)) {
                    "The helper was not idle for activation."
                }
                require(waitForHelper(application) { it.activeRevision == desiredRevision && it.isIdle }) {
                    "The helper rejected or timed out while activating revision $desiredRevision."
                }
                SecureControlStore.saveActiveManifest(application, verified.manifest)
                api.acknowledge(desiredRevision, "activated")
            }
            val failures = mutableListOf<String>()
            runCatching { RecordingSpool.reconcile(application) }.onFailure { failures.add("Recording handoff is pending.") }
            runCatching { SessionAuditSpool.reconcile(application) }.onFailure { failures.add("Audit handoff is pending.") }
            uploadPendingEvents(application, api)
            runCatching { ContinuousRecordingSpool.uploadPending(application, api) }.onFailure {
                failures.add("Continuous recording upload is pending: ${it.message?.take(160)}")
                RecordingFailureJournal.capture(application, "continuous_upload", error = it)
            }
            // Existing voicemail/operator uploads always get the first opportunity.
            runCatching { uploadPendingRecordings(application, api) }.onFailure {
                failures.add("Recording upload is pending.")
                RecordingFailureJournal.capture(application, "upload", error = it)
            }
            if (!CallRuntimeState.isBusy()) {
                runCatching { SessionAuditSpool.uploadPending(application, api) }.onFailure { failures.add("Audit upload is pending.") }
            }
            SecureControlStore.updateSyncStatus(application, failures.takeIf { it.isNotEmpty() }?.joinToString(" "))
            SyncResult.Success(RootAudioTrigger.readState(application).activeRevision)
        } catch (error: Exception) {
            val message = (error.message ?: "Synchronization failed.").take(500)
            if (desiredRevision != null) runCatching { api.acknowledge(desiredRevision, "failed", message) }
            SecureControlStore.updateSyncStatus(application, message)
            SyncResult.Failed(message)
        }
    }

    fun enroll(context: Context, pairingCode: String, deviceName: String = Build.MODEL): Enrollment {
        val application = context.applicationContext
        val state = RootAudioTrigger.readState(application)
        val enrollment = DeviceApi().enroll(
            pairingCode,
            deviceName.ifBlank { "SM-T585" },
            BuildConfig.VERSION_NAME,
            state.helperVersion,
        )
        SecureControlStore.saveEnrollment(application, enrollment)
        SecureControlStore.updateSyncStatus(application, null)
        RecoveryScheduler.ensure(application)
        SyncService.start(application, true)
        return enrollment
    }

    fun status(context: Context): SyncStatus {
        val helper = RootAudioTrigger.readState(context)
        return SyncStatus(
            enrolled = SecureControlStore.enrollment(context) != null,
            activeRevision = helper.activeRevision,
            stagedRevision = helper.stagedRevision,
            lastSyncEpochMs = SecureControlStore.lastSyncEpochMs(context),
            lastError = SecureControlStore.lastError(context),
        )
    }

    private fun buildSyncRequest(context: Context, helper: ai.rx1.ivrdroid.audio.HelperBridgeState): JSONObject {
        val external = ExternalCallCapabilities.snapshot(context, helper)
        val advertisedRuntimes = (helper.runtimeVersions.ifEmpty {
            if (helper.recordingCapable) setOf(1, 2, 3) else setOf(1, 2)
        }).filter { it != 4 || external.runtimeV4Capable }.sorted()
        val callState = when {
            CallRuntimeState.isBusy() -> "active"
            helper.isIdle -> "idle"
            helper.current == "WAITING_FOR_CALL" || helper.current == "ARMING_PRIVACY" -> "ringing"
            else -> "unknown"
        }
        val auditPolicy = SessionAuditSettings.applied(context)
        val callSafety = CallSafetySettings.applied(context)
        val continuousAudit = ContinuousRecordingSpool.pendingUsage(context, "session_audit")
        val continuousConversation = ContinuousRecordingSpool.pendingUsage(context, "conversation")
        val status = JSONObject()
            .put("helper_state", helper.current.take(80))
            .put("helper_result", helper.lastResult.take(80))
            .put("call_state", callState)
            .put("local_kill_switch", !IvrPreferences.isLocalIvrEnabled(context))
            .put("storage_free_bytes", context.filesDir.usableSpace.coerceAtLeast(0))
            .put("last_error", SecureControlStore.lastError(context) ?: JSONObject.NULL)
            .put("runtime_versions", org.json.JSONArray(advertisedRuntimes))
            .put("recording_capable", helper.recordingCapable)
            .put("external_call_control_capable", external.externalCallControlCapable)
            .put("conversation_recording_capable", external.conversationRecordingCapable)
            .put("prompt_barge_in_capable", helper.promptBargeInCapable)
            .put("session_audit_capable", helper.sessionAuditCapable)
            .put("audit_policy_version", auditPolicy.version)
            .put("call_safety_capable", CallSafetySettings.capable(context))
            .put("call_safety_policy_version", callSafety?.version ?: JSONObject.NULL)
            .put("maximum_call_duration_seconds", callSafety?.maximumSeconds ?: JSONObject.NULL)
            .put("continuous_recording_capable", CallSafetySettings.capable(context))
            .put("local_recovery_state", ai.rx1.ivrdroid.telecom.LocalCallSession.readiness)
            .put("telecom_recovery_state", ExternalCallRuntimeStatus.recovery.readiness)
            .put("telecom_recovery_duration_ms", ExternalCallRuntimeStatus.recovery.durationMs)
            .put("telecom_recovery_phase", ExternalCallRuntimeStatus.recovery.phase)
            .put("helper_supervisor_state", runCatching {
                String(SessionAuditFiles.read(java.io.File(context.filesDir, "bridge/helper-supervisor"), 160), Charsets.US_ASCII)
                    .trim().split(' ').takeIf { it.size == 5 && it[0] == "SUP1" }?.get(1)
                    ?.takeIf { it.matches(Regex("[A-Z_]{1,40}")) }
            }.getOrNull() ?: "UNKNOWN")
            .put("audit_enabled", auditPolicy.enabled)
            .put("source_commit", BuildConfig.SOURCE_COMMIT)
            .put("helper_source_commit", helper.sourceCommit.take(40))
            .put("audit_spool_bytes", SessionAuditSpool.usageBytes(context) + continuousAudit.first)
            .put("audit_spool_count", (SessionAuditSpool.pendingIds(context) + continuousAudit.second).size)
            .put("audit_last_error", SessionAuditSpool.lastError(context) ?: JSONObject.NULL)
            .put(
                "call_control_protocol_version",
                external.protocolVersion ?: JSONObject.NULL,
            )
            .put("call_control_state", ExternalCallRuntimeStatus.state(context))
            .put("call_control_recovery_pending", ExternalCallRuntimeStatus.recoveryPending(context))
            .put("recording_spool_bytes", RecordingSpool.usageBytes(context) + continuousConversation.first)
            .put("recording_spool_count", RecordingSpool.pending(context).size + continuousConversation.second.size)
            .put("voicemail_spool_bytes", RecordingSpool.voicemailUsageBytes(context))
            .put("voicemail_spool_count", RecordingSpool.voicemailCount(context))
            .put("conversation_spool_bytes", RecordingSpool.conversationUsageBytes(context) + continuousConversation.first)
            .put("conversation_spool_count", RecordingSpool.conversationCount(context) + continuousConversation.second.size)
            .put("recording_filesystem_free_bytes", context.filesDir.usableSpace.coerceAtLeast(0))
        BootWifiRecoveryStore.latest(context)?.let { report ->
            status.put(
                "boot_wifi_recovery",
                JSONObject()
                    .put("outcome", report.result.outcome.wireValue)
                    .put("completed_at", report.completedAtIso8601())
                    .put("elapsed_since_boot_ms", report.result.elapsedSinceBootMs)
                    .put("internet_validated", report.result.internetValidated)
                    .put("reconnect_attempts", report.result.reconnectAttempts)
                    .put("wifi_enable_attempts", report.result.wifiEnableAttempts),
            )
        }
        return JSONObject()
            .put("app_version", BuildConfig.VERSION_NAME)
            .put("helper_version", helper.helperVersion.take(80))
            .put("active_revision_id", helper.activeRevision ?: JSONObject.NULL)
            .put("status", status)
    }

    private fun uploadPendingEvents(context: Context, api: DeviceApi) {
        var failure: Exception? = null
        val events = SecureControlStore.pendingCalls(context).filter { SessionAuditSpool.hasBinding(context, it) }
        events.chunked(100).forEach { batch ->
            try {
            val accepted = api.uploadEvents(batch)
            SessionAuditSpool.acknowledgeCalls(context, batch, accepted)
            SecureControlStore.acknowledgeCalls(context, batch, accepted)
            } catch (error: Exception) { failure = error }
        }
        // Sub-events remain uploadable even when the parent was acknowledged earlier.
        SecureControlStore.pendingCallEvents(context).entries.chunked(100).forEach { entries ->
            val submitted = entries.associate { it.key to it.value }
            SecureControlStore.acknowledgeCallEvents(context, submitted, api.uploadCallEvents(submitted))
        }
        failure?.let { throw it }
    }

    private fun uploadPendingRecordings(context: Context, api: DeviceApi) {
        RecordingSpool.pending(context).forEach { recording ->
            val logicalReady = if (recording.kind == "conversation") {
                ConversationUploadPolicy.verifyLogical(recording, api.ensureConversation(recording))
            } else {
                false
            }
            var receipt = if (logicalReady) api.recordingUpload(recording) else api.beginRecording(recording)
            var verified = RecordingUploadPolicy.verify(recording, receipt)
            var audio: ByteArray? = null
            while (true) {
                when (val action = RecordingUploadPolicy.nextAction(recording, verified, CallRuntimeState.isBusy())) {
                    RecordingUploadAction.PauseForCall -> error("Call started; synchronization paused.")
                    RecordingUploadAction.AcknowledgeAndDelete -> {
                        RecordingSpool.acknowledge(context, recording, receipt)
                        break
                    }
                    RecordingUploadAction.FinalizeConversation -> {
                        ConversationUploadPolicy.verifyCompleted(recording, api.completeConversation(recording))
                        RecordingSpool.acknowledge(context, recording, receipt)
                        break
                    }
                    RecordingUploadAction.Complete -> {
                        receipt = api.completeRecording(recording)
                        verified = RecordingUploadPolicy.verify(recording, receipt)
                    }
                    is RecordingUploadAction.UploadChunk -> {
                        val content = audio ?: RecordingSpool.readAudio(context, recording).also {
                            require(it.size.toLong() == recording.sizeBytes)
                            audio = it
                        }
                        receipt = api.uploadRecordingChunk(
                            recording,
                            action.offset,
                            content.copyOfRange(
                                action.offset.toInt(),
                                action.offset.toInt() + action.byteCount,
                            ),
                        )
                        verified = RecordingUploadPolicy.verify(recording, receipt)
                        RecordingUploadPolicy.requireAdvanced(action, verified)
                    }
                }
            }
        }
    }

    private fun waitForHelper(
        context: Context,
        predicate: (ai.rx1.ivrdroid.audio.HelperBridgeState) -> Boolean,
    ): Boolean {
        val deadline = android.os.SystemClock.elapsedRealtime() + HELPER_OPERATION_TIMEOUT_MS
        val initialResult = RootAudioTrigger.readState(context).lastResult
        var observedOperation = false
        while (android.os.SystemClock.elapsedRealtime() < deadline) {
            if (CallRuntimeState.isBusy()) return false
            val state = RootAudioTrigger.readState(context)
            if (predicate(state)) return true
            if (!state.isIdle || state.lastResult != initialResult) observedOperation = true
            if (state.current == "ERROR" || state.current == "STOPPED" ||
                (observedOperation && state.lastResult == "REVISION_REJECTED")) return false
            Thread.sleep(250)
        }
        return false
    }
}
