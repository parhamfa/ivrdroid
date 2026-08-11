package ai.rx1.ivrdroid.telecom.external

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.Build
import android.os.SystemClock
import android.telecom.Call
import android.telecom.DisconnectCause
import android.telecom.TelecomManager
import android.telecom.VideoProfile
import android.telephony.TelephonyManager
import android.util.Log
import ai.rx1.ivrdroid.control.SecureControlStore
import java.security.MessageDigest
import java.util.IdentityHashMap

class AndroidTelecomControl(
    private val context: Context,
    private val telecomManager: TelecomManager,
    private val handler: Handler,
    private val changed: () -> Unit,
) : TelecomControl {
    private val live = linkedMapOf<String, Call>()
    private val callbacks = linkedMapOf<String, Call.Callback>()
    private val idsByCall = IdentityHashMap<Call, String>()
    private val ownership = CallOwnershipTracker()

    fun add(call: Call) {
        val id = idsByCall[call] ?: TelecomCallKey.from(call.details).also {
            idsByCall[call] = it
        }
        val oldCall = live.put(id, call)
        if (oldCall === call) return
        val callback = object : Call.Callback() {
            override fun onStateChanged(call: Call, state: Int) = changed()
            override fun onDetailsChanged(call: Call, details: Call.Details) {
                persistOutgoingOwnership(call, id)
                reconcilePendingIncomingOwnership()
                changed()
            }
            override fun onParentChanged(call: Call, parent: Call?) = changed()
            override fun onChildrenChanged(call: Call, children: MutableList<Call>) = changed()
            override fun onConferenceableCallsChanged(call: Call, conferenceableCalls: MutableList<Call>) = changed()
            override fun onCallDestroyed(call: Call) = changed()
        }
        callbacks.put(id, callback)?.let { previous -> oldCall?.unregisterCallback(previous) }
        call.registerCallback(callback, handler)
        persistOutgoingOwnership(call, id)
        reconcilePendingIncomingOwnership()
        changed()
    }

    fun remove(call: Call) {
        val id = idsByCall.remove(call) ?: TelecomCallKey.from(call.details)
        callbacks.remove(id)?.let(call::unregisterCallback)
        live.remove(id)
        ownership.remove(id)
        OwnedCallRegistry.removeCall(context, id)
        changed()
    }

    fun close() {
        callbacks.forEach { (id, callback) -> live[id]?.unregisterCallback(callback) }
        callbacks.clear()
        live.clear()
        idsByCall.clear()
        ownership.clear()
    }

    override fun calls(): List<TelecomCallSnapshot> = live.map { (id, call) -> snapshot(id, call) }

    override fun claimIncomingCaller(
        sessionId: String,
        signedSessionAuthorized: Boolean,
    ): CallerClaimDecision {
        val registration = OwnedCallRegistry.callerRegistration(context, sessionId)
            ?: return CallerClaimDecision.Rejected("CALLER_NOT_OWNED")
        val candidates = live.map { (id, call) -> callerCandidate(id, call, registration.bootId) }
        val decision = IncomingCallerClaimPolicy.decide(
            registration,
            candidates,
            SystemClock.elapsedRealtime(),
            signedSessionAuthorized,
        )
        if (decision !is CallerClaimDecision.Claimed) {
            val reason = (decision as CallerClaimDecision.Rejected).reason
            Log.w(TAG, "Incoming IVR ownership claim rejected: $reason")
            return decision
        }
        if (!OwnedCallRegistry.confirmCaller(
                context,
                sessionId,
                registration.currentCallId,
                decision.callId,
            )
        ) {
            Log.e(TAG, "Incoming IVR ownership claim could not be persisted.")
            return CallerClaimDecision.Rejected("CALLER_OWNERSHIP_PERSIST_FAILED")
        }
        ownership.remember(decision.callId, sessionId)
        Log.i(TAG, "Incoming IVR call correlated with its private InCallService identity.")
        return decision
    }

    fun reconcilePendingIncomingOwnership(nowElapsedMs: Long = SystemClock.elapsedRealtime()) {
        val registration = OwnedCallRegistry.singleRebindableCaller(context, nowElapsedMs) ?: return
        claimIncomingCaller(registration.sessionId, signedSessionAuthorized = false)
    }

    private fun snapshot(id: String, call: Call): TelecomCallSnapshot {
        val details = call.details
        return TelecomCallSnapshot(
            id = id,
            direction = when (details.callDirection) {
                Call.Details.DIRECTION_INCOMING -> TelecomCallDirection.INCOMING
                Call.Details.DIRECTION_OUTGOING -> TelecomCallDirection.OUTGOING
                else -> TelecomCallDirection.UNKNOWN
            },
            state = when (if (Build.VERSION.SDK_INT >= 31) details.state else legacyState(call)) {
                Call.STATE_NEW, Call.STATE_CONNECTING -> TelecomCallState.NEW
                Call.STATE_RINGING, Call.STATE_SIMULATED_RINGING -> TelecomCallState.RINGING
                Call.STATE_DIALING, Call.STATE_SELECT_PHONE_ACCOUNT -> TelecomCallState.DIALING
                Call.STATE_ACTIVE -> TelecomCallState.ACTIVE
                Call.STATE_HOLDING -> TelecomCallState.HOLDING
                Call.STATE_DISCONNECTING -> TelecomCallState.DISCONNECTING
                Call.STATE_DISCONNECTED -> TelecomCallState.DISCONNECTED
                else -> TelecomCallState.UNKNOWN
            },
            ownerSessionId = ownership.owner(id) ?: OwnedCallRegistry.sessionFor(context, id),
            phoneNumber = details.handle?.takeIf { it.scheme == "tel" }?.schemeSpecificPart,
            emergency = isEmergencyCall(details),
            canHold = details.can(Call.Details.CAPABILITY_HOLD) ||
                details.can(Call.Details.CAPABILITY_SUPPORT_HOLD),
            disconnectKind = when (details.disconnectCause?.code) {
                DisconnectCause.BUSY -> TelecomDisconnectKind.BUSY
                DisconnectCause.REJECTED -> TelecomDisconnectKind.REJECTED
                DisconnectCause.ERROR, DisconnectCause.RESTRICTED -> TelecomDisconnectKind.ERROR
                DisconnectCause.LOCAL, DisconnectCause.CANCELED -> TelecomDisconnectKind.LOCAL
                DisconnectCause.REMOTE -> TelecomDisconnectKind.REMOTE
                else -> TelecomDisconnectKind.UNKNOWN
            },
        )
    }

    private fun callerCandidate(
        id: String,
        call: Call,
        bootId: String,
    ): LiveIncomingCallerCandidate {
        val snapshot = snapshot(id, call)
        return LiveIncomingCallerCandidate(
            id,
            CallerHandleEvidence.digest(call.details.handle?.schemeSpecificPart, bootId),
            snapshot.direction,
            snapshot.state,
            snapshot.emergency,
        )
    }

    override fun isEmergencyNumber(phoneNumber: String): Boolean = runCatching {
        (context.getSystemService(Context.TELEPHONY_SERVICE) as TelephonyManager)
            .isEmergencyNumber(phoneNumber)
    }.getOrDefault(true)

    private fun isEmergencyCall(details: Call.Details): Boolean {
        if (details.hasProperty(Call.Details.PROPERTY_NETWORK_IDENTIFIED_EMERGENCY_CALL)) return true
        val number = details.handle?.takeIf { it.scheme == "tel" }?.schemeSpecificPart ?: return true
        return isEmergencyNumber(number)
    }

    override fun hold(callId: String): Boolean = action(callId) { it.hold() }

    override fun unhold(callId: String): Boolean = action(callId) { it.unhold() }

    override fun disconnect(callId: String): Boolean = action(callId) { it.disconnect() }

    override fun placeCall(phoneNumber: String, sessionId: String, callerId: String): Boolean {
        if (context.checkSelfPermission(Manifest.permission.CALL_PHONE) != PackageManager.PERMISSION_GRANTED) return false
        val caller = live[callerId] ?: return false
        return runCatching {
            val outgoing = Bundle().apply { putString(EXTRA_CONTROL_SESSION_ID, sessionId) }
            val extras = Bundle().apply {
                putBundle(TelecomManager.EXTRA_OUTGOING_CALL_EXTRAS, outgoing)
                putInt(TelecomManager.EXTRA_START_CALL_WITH_VIDEO_STATE, VideoProfile.STATE_AUDIO_ONLY)
                caller.details.accountHandle?.let {
                    putParcelable(TelecomManager.EXTRA_PHONE_ACCOUNT_HANDLE, it)
                }
            }
            telecomManager.placeCall(Uri.fromParts("tel", phoneNumber, null), extras)
            true
        }.getOrDefault(false)
    }

    override fun conference(callerId: String, operatorId: String): Boolean {
        val caller = live[callerId] ?: return false
        val operator = live[operatorId] ?: return false
        return runCatching {
            when {
                caller.conferenceableCalls.contains(operator) -> caller.conference(operator)
                operator.conferenceableCalls.contains(caller) -> operator.conference(caller)
                else -> return false
            }
            true
        }.getOrDefault(false)
    }

    override fun canConference(callerId: String, operatorId: String): Boolean {
        val caller = live[callerId] ?: return false
        val operator = live[operatorId] ?: return false
        return caller.conferenceableCalls.contains(operator) ||
            operator.conferenceableCalls.contains(caller)
    }

    override fun conferenceId(callerId: String, operatorId: String): String? {
        val caller = live[callerId] ?: return null
        val operator = live[operatorId] ?: return null
        val callerParent = caller.parent?.let(::idFor)
        val operatorParent = operator.parent?.let(::idFor)
        if (callerParent != null && callerParent == operatorParent) return callerParent
        return live.entries.singleOrNull { (_, candidate) ->
            val children = candidate.children.map(::idFor).toSet()
            callerId in children && operatorId in children
        }?.key
    }

    private fun action(callId: String, action: (Call) -> Unit): Boolean {
        val call = live[callId] ?: return false
        return runCatching { action(call) }.isSuccess
    }

    private fun idFor(call: Call): String = idsByCall[call] ?: TelecomCallKey.from(call.details)

    @Suppress("DEPRECATION")
    private fun legacyState(call: Call): Int = call.state

    private fun outgoingSession(details: Call.Details): String? = sequenceOf(
        details.intentExtras,
        details.extras,
        details.intentExtras?.getBundle(TelecomManager.EXTRA_OUTGOING_CALL_EXTRAS),
        details.extras?.getBundle(TelecomManager.EXTRA_OUTGOING_CALL_EXTRAS),
    ).filterNotNull().mapNotNull { it.getString(EXTRA_CONTROL_SESSION_ID) }.firstOrNull()

    private fun persistOutgoingOwnership(call: Call, id: String) {
        val details = call.details
        if (details.callDirection != Call.Details.DIRECTION_OUTGOING) return
        val sessionId = outgoingSession(details) ?: return
        val phoneNumber = details.handle?.takeIf { it.scheme == "tel" }?.schemeSpecificPart ?: return
        val snapshot = CallControlBridge.journal(context).load() ?: return
        val config = snapshot.config
        val manifest = SecureControlStore.activeManifest(context) ?: return
        val instruction = runCatching {
            ExternalCallInstructionResolver.resolve(manifest, config.revisionId, config.blockId)
        }.getOrNull() ?: return
        if (!OutgoingOperatorOwnershipPolicy.allows(
                snapshot,
                sessionId,
                phoneNumber,
                BootIdentity.current(),
                instruction,
            )
        ) return
        if (OwnedCallRegistry.registerOperator(context, id, sessionId)) ownership.remember(id, sessionId)
    }

    companion object {
        private const val TAG = "IVRdroidTelecom"
        const val EXTRA_CONTROL_SESSION_ID = "ai.rx1.ivrdroid.extra.EXTERNAL_CALL_SESSION_ID"
    }
}

internal object OutgoingOperatorOwnershipPolicy {
    fun allows(
        snapshot: ExternalCallSessionSnapshot,
        sessionId: String,
        phoneNumber: String,
        currentBootId: String?,
        instruction: SignedExternalCallInstruction,
    ): Boolean {
        val config = snapshot.config
        return !snapshot.terminal && currentBootId != null &&
            config.sessionId == sessionId && config.phoneNumber == phoneNumber &&
            config.bootId == currentBootId && instruction.revisionId == config.revisionId &&
            instruction.blockId == config.blockId && instruction.phoneNumber == phoneNumber &&
            instruction.answerTimeoutMs == config.answerTimeoutMs
    }
}

/** Best-effort InCallService key. Cross-service and rebind correlation is handled separately. */
object TelecomCallKey {
    fun from(details: Call.Details): String {
        val account = details.accountHandle
        val source = buildString {
            append(details.creationTimeMillis).append('\n')
            append(details.callDirection).append('\n')
            append(account?.componentName?.flattenToString().orEmpty()).append('\n')
            append(account?.id.orEmpty())
        }
        return MessageDigest.getInstance("SHA-256")
            .digest(source.toByteArray(Charsets.UTF_8))
            .joinToString("") { "%02x".format(it) }
    }
}
