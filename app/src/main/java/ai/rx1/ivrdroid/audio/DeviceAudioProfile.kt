package ai.rx1.ivrdroid.audio

import android.content.Context
import android.os.Build
import org.json.JSONObject

data class DeviceAudioProfile(
    val id: String,
    val manufacturer: String,
    val model: String,
    val device: String,
    val androidApi: Int,
    val buildFingerprint: String,
    val buildDisplayId: String,
    val capture: PcmEndpoint,
    val playback: PcmEndpoint,
    val snapshotBeforeMutation: Boolean,
    val applyMixerValues: List<MixerValue>,
) {
    fun matchesCurrentDevice(): Boolean =
        Build.MANUFACTURER.equals(manufacturer, ignoreCase = true) &&
            Build.MODEL.equals(model, ignoreCase = true) &&
            Build.DEVICE.equals(device, ignoreCase = true) &&
            Build.VERSION.SDK_INT == androidApi &&
            Build.FINGERPRINT == buildFingerprint &&
            Build.DISPLAY == buildDisplayId &&
            snapshotBeforeMutation

    data class PcmEndpoint(
        val card: Int,
        val device: Int,
        val sampleRateHz: Int,
        val channels: Int,
        val format: String,
    )

    data class MixerValue(
        val control: String,
        val value: String,
    )

    companion object {
        private const val SM_T585_PROFILE_ASSET =
            "device_profiles/samsung_sm_t585.json"

        fun forCurrentDevice(context: Context): DeviceAudioProfile? =
            runCatching {
                context.assets.open(SM_T585_PROFILE_ASSET)
                    .bufferedReader()
                    .use { fromJson(it.readText()) }
            }.getOrNull()?.takeIf(DeviceAudioProfile::matchesCurrentDevice)

        internal fun fromJson(rawJson: String): DeviceAudioProfile {
            val root = JSONObject(rawJson)
            val mixer = root.getJSONObject("mixer")
            val apply = mixer.getJSONArray("apply")

            return DeviceAudioProfile(
                id = root.getString("id"),
                manufacturer = root.getString("manufacturer"),
                model = root.getString("model"),
                device = root.getString("device"),
                androidApi = root.getInt("androidApi"),
                buildFingerprint = root.getString("buildFingerprint"),
                buildDisplayId = root.getString("buildDisplayId"),
                capture = root.getJSONObject("capture").toPcmEndpoint(),
                playback = root.getJSONObject("playback").toPcmEndpoint(),
                snapshotBeforeMutation = mixer.getBoolean("snapshotBeforeMutation"),
                applyMixerValues = List(apply.length()) { index ->
                    val row = apply.getJSONArray(index)
                    MixerValue(
                        control = row.getString(0),
                        value = row.getString(1),
                    )
                },
            )
        }

        private fun JSONObject.toPcmEndpoint() = PcmEndpoint(
            card = getInt("card"),
            device = getInt("device"),
            sampleRateHz = getInt("sampleRateHz"),
            channels = getInt("channels"),
            format = getString("pcmFormat"),
        )
    }
}
