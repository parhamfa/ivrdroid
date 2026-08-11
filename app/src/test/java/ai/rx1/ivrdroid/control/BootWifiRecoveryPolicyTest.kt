package ai.rx1.ivrdroid.control

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class BootWifiRecoveryPolicyTest {
    @Test
    fun anAlreadyValidatedConnectionFinishesWithoutIntervention() {
        val command = BootWifiRecoveryPolicy().evaluate(observation(30_000, hasIp = true, validated = true))
        val result = (command as BootWifiRecoveryCommand.Finish).result

        assertEquals(BootWifiRecoveryOutcome.ALREADY_CONNECTED, result.outcome)
        assertEquals(0, result.reconnectAttempts)
        assertEquals(0, result.wifiEnableAttempts)
    }

    @Test
    fun requestsOneReconnectBeforeTheNextBoundedRetry() {
        val policy = BootWifiRecoveryPolicy()
        assertTrue(policy.evaluate(observation(44_999)) is BootWifiRecoveryCommand.Wait)
        assertEquals(
            BootWifiRecoveryCommand.RequestReconnect,
            policy.evaluate(observation(BootWifiRecoveryPolicy.RECONNECT_AT_MS)),
        )
        assertTrue(policy.evaluate(observation(60_000)) is BootWifiRecoveryCommand.Wait)

        val result = policy.evaluate(observation(70_000, hasIp = true, validated = true))
            .let { (it as BootWifiRecoveryCommand.Finish).result }
        assertEquals(BootWifiRecoveryOutcome.RECONNECT_RECOVERED, result.outcome)
        assertEquals(1, result.reconnectAttempts)
        assertEquals(0, result.wifiEnableAttempts)
    }

    @Test
    fun neverInterruptsAnAssociationOrAuthenticationAlreadyInProgress() {
        val policy = BootWifiRecoveryPolicy()
        assertTrue(
            policy.evaluate(observation(45_000, connecting = true)) is BootWifiRecoveryCommand.Wait,
        )
        assertEquals(
            BootWifiRecoveryCommand.RequestReconnect,
            policy.evaluate(observation(55_000, connecting = false)),
        )
    }

    @Test
    fun makesThreeBoundedReconnectRequestsButNeverDisablesWifi() {
        val policy = BootWifiRecoveryPolicy()
        assertEquals(
            BootWifiRecoveryCommand.RequestReconnect,
            policy.evaluate(observation(45_000)),
        )
        assertEquals(
            BootWifiRecoveryCommand.RequestReconnect,
            policy.evaluate(observation(90_000)),
        )
        assertEquals(
            BootWifiRecoveryCommand.RequestReconnect,
            policy.evaluate(observation(135_000)),
        )
        assertTrue(policy.evaluate(observation(140_000)) is BootWifiRecoveryCommand.Wait)

        val result = policy.evaluate(observation(150_000, hasIp = true, validated = true))
            .let { (it as BootWifiRecoveryCommand.Finish).result }
        assertEquals(BootWifiRecoveryOutcome.RECONNECT_RECOVERED, result.outcome)
        assertEquals(3, result.reconnectAttempts)
    }

    @Test
    fun activeCallDefersEveryWifiMutationAndEventuallyGivesUp() {
        val policy = BootWifiRecoveryPolicy()
        assertTrue(policy.evaluate(observation(45_000, safe = false)) is BootWifiRecoveryCommand.Wait)
        assertTrue(policy.evaluate(observation(90_000, safe = false)) is BootWifiRecoveryCommand.Wait)

        val result = policy.evaluate(observation(180_000, safe = false))
            .let { (it as BootWifiRecoveryCommand.Finish).result }
        assertEquals(BootWifiRecoveryOutcome.GAVE_UP_ACTIVE_CALL, result.outcome)
        assertEquals(0, result.reconnectAttempts)
        assertEquals(0, result.wifiEnableAttempts)
    }

    @Test
    fun aCallMayEndWithinTheWindowBeforeReconnectStarts() {
        val policy = BootWifiRecoveryPolicy()
        assertTrue(policy.evaluate(observation(45_000, safe = false)) is BootWifiRecoveryCommand.Wait)
        assertEquals(
            BootWifiRecoveryCommand.RequestReconnect,
            policy.evaluate(observation(70_000)),
        )
        assertTrue(policy.evaluate(observation(75_000, safe = false)) is BootWifiRecoveryCommand.Wait)
    }

    @Test
    fun upstreamFailureNeverResetsAWifiLinkThatAlreadyHasAnAddress() {
        val policy = BootWifiRecoveryPolicy()
        assertTrue(
            policy.evaluate(observation(100_000, hasIp = true, validated = false))
                is BootWifiRecoveryCommand.Wait,
        )
        val result = policy.evaluate(observation(180_000, hasIp = true, validated = false))
            .let { (it as BootWifiRecoveryCommand.Finish).result }
        assertEquals(BootWifiRecoveryOutcome.WIFI_LINK_UNVALIDATED, result.outcome)
        assertEquals(0, result.reconnectAttempts)
    }

    @Test
    fun aDisabledRadioGetsOnlyBoundedEnableRequests() {
        val policy = BootWifiRecoveryPolicy()
        assertEquals(
            BootWifiRecoveryCommand.EnableWifi,
            policy.evaluate(observation(45_000, enabled = false)),
        )
        assertTrue(policy.evaluate(observation(50_000, enabled = false)) is BootWifiRecoveryCommand.Wait)
        assertEquals(
            BootWifiRecoveryCommand.EnableWifi,
            policy.evaluate(observation(90_000, enabled = false)),
        )
        assertTrue(policy.evaluate(observation(100_000, enabled = false)) is BootWifiRecoveryCommand.Wait)
    }

    @Test
    fun aLateStartStillSpacesReconnectAttempts() {
        val policy = BootWifiRecoveryPolicy()
        assertEquals(
            BootWifiRecoveryCommand.RequestReconnect,
            policy.evaluate(observation(120_000)),
        )
        assertTrue(policy.evaluate(observation(130_000)) is BootWifiRecoveryCommand.Wait)
        assertEquals(
            BootWifiRecoveryCommand.RequestReconnect,
            policy.evaluate(observation(150_000)),
        )
    }

    private fun observation(
        elapsed: Long,
        enabled: Boolean = true,
        hasIp: Boolean = false,
        validated: Boolean = false,
        connecting: Boolean = false,
        safe: Boolean = true,
    ) = BootWifiObservation(
        elapsedSinceBootMs = elapsed,
        wifiEnabled = enabled,
        wifiHasIp = hasIp,
        internetValidated = validated,
        wifiConnectionInProgress = connecting,
        safeToMutateWifi = safe,
    )
}
