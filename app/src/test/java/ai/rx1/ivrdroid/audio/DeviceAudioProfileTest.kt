package ai.rx1.ivrdroid.audio

import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class DeviceAudioProfileTest {
    @Test
    fun bundledSmT585ProfileParsesWithTransactionalSafetyEnabled() {
        val rawJson = File(
            "src/main/assets/device_profiles/samsung_sm_t585.json",
        ).readText()

        val profile = DeviceAudioProfile.fromJson(rawJson)

        assertEquals("samsung-sm-t585-lineageos-19.1", profile.id)
        assertEquals("SM-T585", profile.model)
        assertEquals(32, profile.androidApi)
        assertEquals(48_000, profile.capture.sampleRateHz)
        assertEquals(48_000, profile.playback.sampleRateHz)
        assertTrue(profile.snapshotBeforeMutation)
        assertEquals(
            DeviceAudioProfile.MixerValue(
                control = "AudioMixer CH2 DOUT Select",
                value = "DMIX_OUT",
            ),
            profile.applyMixerValues.last(),
        )
    }
}
