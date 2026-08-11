package ai.rx1.ivrdroid.telecom.external

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import ai.rx1.ivrdroid.audio.HelperBridgeState

data class ExternalCallCapabilitySnapshot(
    val externalCallControlCapable: Boolean,
    val conversationRecordingCapable: Boolean,
    val protocolVersion: Int?,
) {
    val runtimeV4Capable: Boolean
        get() = externalCallControlCapable && conversationRecordingCapable &&
            protocolVersion == CallControlProtocol.VERSION
}

object ExternalCallCapabilities {
    private const val CONTROL_INCALL_EXPERIENCE = "android.permission.CONTROL_INCALL_EXPERIENCE"

    fun snapshot(context: Context, helper: HelperBridgeState): ExternalCallCapabilitySnapshot {
        val appControl = context.checkSelfPermission(Manifest.permission.CALL_PHONE) ==
            PackageManager.PERMISSION_GRANTED &&
            context.checkSelfPermission(CONTROL_INCALL_EXPERIENCE) == PackageManager.PERMISSION_GRANTED
        val callControl = appControl && helper.callControlCapable &&
            helper.runtimeVersions.contains(4) && BootIdentity.current() != null
        val conversationRecording = helper.recordingCapable && helper.conversationRecordingCapable
        return ExternalCallCapabilitySnapshot(
            externalCallControlCapable = callControl,
            conversationRecordingCapable = conversationRecording,
            protocolVersion = CallControlProtocol.VERSION.takeIf { helper.callControlCapable },
        )
    }
}
