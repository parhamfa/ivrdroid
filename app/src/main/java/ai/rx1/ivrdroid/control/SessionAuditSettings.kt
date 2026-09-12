package ai.rx1.ivrdroid.control

import android.content.Context
import android.os.SystemClock
import ai.rx1.ivrdroid.audio.RootAudioTrigger
import ai.rx1.ivrdroid.telecom.CallRuntimeState
import org.json.JSONObject
import java.io.File

object SessionAuditSettings {
    private fun policyFile(context: Context) = File(context.filesDir, "session-audit-policy.json")

    fun verified(context: Context): SessionAuditPolicy = runCatching {
        SessionAuditProtocol.verifyPolicy(JSONObject(String(SessionAuditFiles.read(policyFile(context), 4096), Charsets.UTF_8)))
    }.getOrDefault(SessionAuditPolicy())

    fun applied(context: Context): SessionAuditPolicy {
        val expected = verified(context)
        val actual = runCatching { String(SessionAuditFiles.read(File(context.filesDir, "bridge/audit-state"), 160), Charsets.US_ASCII) }.getOrNull()
        if (actual == expected.bridgeText()) return expected
        val prior = runCatching { SessionAuditProtocol.verifyPolicy(JSONObject(String(SessionAuditFiles.read(
            File(context.filesDir, "session-audit-applied-policy.json"), 4096), Charsets.UTF_8))) }.getOrNull()
        return prior?.takeIf { it.bridgeText() == actual } ?: SessionAuditPolicy()
    }

    fun apply(context: Context, envelope: JSONObject) {
        val incoming = SessionAuditProtocol.verifyPolicy(envelope)
        val previous = verified(context)
        require(incoming.version >= previous.version) { "Audit policy rollback was rejected." }
        require(incoming.version != previous.version || incoming == previous) { "Audit policy changed without a new version." }
        if (CallRuntimeState.isBusy() || !RootAudioTrigger.isIdle(context)) return
        if (!RootAudioTrigger.readState(context).sessionAuditCapable) return
        if (applied(context) == previous && policyFile(context).exists()) {
            SessionAuditFiles.write(File(context.filesDir, "session-audit-applied-policy.json"), SessionAuditFiles.read(policyFile(context), 4096))
        }
        SessionAuditFiles.write(policyFile(context), envelope.toString().toByteArray(Charsets.UTF_8))
        SessionAuditFiles.write(File(context.filesDir, "bridge/audit-policy"), incoming.bridgeText().toByteArray(Charsets.US_ASCII))
        val deadline = SystemClock.elapsedRealtime() + 2500
        while (!CallRuntimeState.isBusy() && SystemClock.elapsedRealtime() < deadline) {
            if (applied(context) == incoming) return
            Thread.sleep(50)
        }
    }

    /** Restore only a verified document; an absent or corrupt document always restores disabled. */
    fun restore(context: Context) {
        if (CallRuntimeState.isBusy() || !RootAudioTrigger.isIdle(context)) return
        val policy = verified(context)
        SessionAuditFiles.write(File(context.filesDir, "bridge/audit-policy"), policy.bridgeText().toByteArray(Charsets.US_ASCII))
    }
}
