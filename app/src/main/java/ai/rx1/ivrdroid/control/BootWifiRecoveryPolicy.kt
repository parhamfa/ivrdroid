package ai.rx1.ivrdroid.control

enum class BootWifiRecoveryOutcome(val wireValue: String) {
    ALREADY_CONNECTED("already_connected"),
    RECONNECT_RECOVERED("reconnect_recovered"),
    WIFI_LINK_UNVALIDATED("wifi_link_unvalidated"),
    GAVE_UP("gave_up"),
    GAVE_UP_ACTIVE_CALL("gave_up_active_call"),
}

data class BootWifiObservation(
    val elapsedSinceBootMs: Long,
    val wifiEnabled: Boolean,
    val wifiHasIp: Boolean,
    val internetValidated: Boolean,
    val wifiConnectionInProgress: Boolean,
    val safeToMutateWifi: Boolean,
)

data class BootWifiRecoveryResult(
    val outcome: BootWifiRecoveryOutcome,
    val elapsedSinceBootMs: Long,
    val internetValidated: Boolean,
    val reconnectAttempts: Int,
    val wifiEnableAttempts: Int,
)

sealed interface BootWifiRecoveryCommand {
    data class Wait(val delayMs: Long) : BootWifiRecoveryCommand
    data object EnableWifi : BootWifiRecoveryCommand
    data object RequestReconnect : BootWifiRecoveryCommand
    data class Finish(val result: BootWifiRecoveryResult) : BootWifiRecoveryCommand
}

/**
 * Pure decision policy for the bounded boot-time Wi-Fi guardian.
 *
 * All elapsed times are Android elapsed-realtime values measured from boot. The policy never
 * knows an SSID, credential, address, or caller identity, which keeps both its behavior and its
 * persisted result bounded and non-sensitive.
 */
class BootWifiRecoveryPolicy {
    private var reconnectAttempts = 0
    private var wifiEnableAttempts = 0
    private var lastReconnectAtMs: Long? = null
    private var lastEnableAtMs: Long? = null
    private var blockedByActiveCall = false
    private var completed: BootWifiRecoveryResult? = null

    fun evaluate(observation: BootWifiObservation): BootWifiRecoveryCommand {
        require(observation.elapsedSinceBootMs >= 0)
        completed?.let { return BootWifiRecoveryCommand.Finish(it) }

        val elapsed = observation.elapsedSinceBootMs

        if (observation.wifiHasIp) {
            if (observation.internetValidated) {
                return finish(
                    if (reconnectAttempts > 0 || wifiEnableAttempts > 0) {
                        BootWifiRecoveryOutcome.RECONNECT_RECOVERED
                    } else {
                        BootWifiRecoveryOutcome.ALREADY_CONNECTED
                    },
                    observation,
                )
            }
            // An associated Wi-Fi network with an address may have an upstream Internet problem.
            // Do not reset a healthy local link in response to failed Internet validation.
            if (elapsed >= HARD_DEADLINE_MS) {
                return finish(BootWifiRecoveryOutcome.WIFI_LINK_UNVALIDATED, observation)
            }
            return waitFor(elapsed, HARD_DEADLINE_MS)
        }

        if (elapsed >= HARD_DEADLINE_MS) {
            val outcome = if (blockedByActiveCall && reconnectAttempts == 0 && wifiEnableAttempts == 0) {
                BootWifiRecoveryOutcome.GAVE_UP_ACTIVE_CALL
            } else {
                BootWifiRecoveryOutcome.GAVE_UP
            }
            return finish(outcome, observation)
        }

        if (!observation.safeToMutateWifi) {
            blockedByActiveCall = true
            return waitFor(elapsed, HARD_DEADLINE_MS)
        }

        if (!observation.wifiEnabled) {
            val enableAt = nextAttemptAt(wifiEnableAttempts, lastEnableAtMs)
            if (enableAt != null && elapsed >= enableAt) {
                wifiEnableAttempts += 1
                lastEnableAtMs = elapsed
                return BootWifiRecoveryCommand.EnableWifi
            }
            return waitFor(elapsed, enableAt ?: HARD_DEADLINE_MS)
        }

        // Do not interrupt Android while it is scanning, associating, authenticating, or waiting
        // for IP configuration. A reconnect is useful only after that attempt has actually ended.
        if (observation.wifiConnectionInProgress) {
            return BootWifiRecoveryCommand.Wait(POLL_INTERVAL_MS)
        }

        val reconnectAt = nextAttemptAt(reconnectAttempts, lastReconnectAtMs)
        if (reconnectAt != null && elapsed >= reconnectAt) {
            reconnectAttempts += 1
            lastReconnectAtMs = elapsed
            return BootWifiRecoveryCommand.RequestReconnect
        }
        return waitFor(elapsed, reconnectAt ?: HARD_DEADLINE_MS)
    }

    private fun finish(
        outcome: BootWifiRecoveryOutcome,
        observation: BootWifiObservation,
    ): BootWifiRecoveryCommand.Finish {
        val result = BootWifiRecoveryResult(
            outcome = outcome,
            elapsedSinceBootMs = observation.elapsedSinceBootMs,
            internetValidated = observation.internetValidated,
            reconnectAttempts = reconnectAttempts,
            wifiEnableAttempts = wifiEnableAttempts,
        )
        completed = result
        return BootWifiRecoveryCommand.Finish(result)
    }

    private fun waitFor(elapsed: Long, deadline: Long): BootWifiRecoveryCommand.Wait =
        BootWifiRecoveryCommand.Wait((deadline - elapsed).coerceIn(1L, POLL_INTERVAL_MS))

    private fun nextAttemptAt(attempts: Int, lastAttemptAtMs: Long?): Long? {
        val scheduled = ATTEMPT_AT_MS.getOrNull(attempts) ?: return null
        return if (lastAttemptAtMs == null) scheduled else maxOf(scheduled, lastAttemptAtMs + MINIMUM_ATTEMPT_INTERVAL_MS)
    }

    companion object {
        const val RECONNECT_AT_MS = 45_000L
        const val HARD_DEADLINE_MS = 180_000L
        const val POLL_INTERVAL_MS = 5_000L
        const val MINIMUM_ATTEMPT_INTERVAL_MS = 30_000L
        private val ATTEMPT_AT_MS = longArrayOf(45_000L, 90_000L, 135_000L)
    }
}
