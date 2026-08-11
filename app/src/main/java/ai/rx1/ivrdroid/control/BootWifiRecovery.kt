package ai.rx1.ivrdroid.control

import android.content.Context
import android.media.AudioManager
import android.net.ConnectivityManager
import android.net.LinkProperties
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.wifi.SupplicantState
import android.net.wifi.WifiManager
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.os.SystemClock
import android.util.Log
import ai.rx1.ivrdroid.audio.RootAudioTrigger
import ai.rx1.ivrdroid.telecom.CallRuntimeState
import java.time.Instant

class BootWifiRecovery(
    context: Context,
    private val onFinished: () -> Unit = {},
) {
    private val application = context.applicationContext
    private val connectivity = application.getSystemService(ConnectivityManager::class.java)
    private val wifi = application.getSystemService(WifiManager::class.java)
    private val audio = application.getSystemService(AudioManager::class.java)
    private val power = application.getSystemService(PowerManager::class.java)
    private val handler = Handler(Looper.getMainLooper())
    private val policy = BootWifiRecoveryPolicy()
    private val wakeLock = power.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, WAKE_LOCK_TAG).apply {
        setReferenceCounted(false)
    }
    private var started = false
    private var stopped = false
    private var callbackRegistered = false
    private val evaluation = Runnable { evaluate() }
    private val networkCallback = object : ConnectivityManager.NetworkCallback() {
        override fun onAvailable(network: Network) = schedule(0)
        override fun onCapabilitiesChanged(network: Network, capabilities: NetworkCapabilities) = schedule(0)
        override fun onLinkPropertiesChanged(network: Network, linkProperties: LinkProperties) = schedule(0)
        override fun onLost(network: Network) = schedule(0)
    }

    fun start() {
        if (started || stopped) return
        started = true
        BootWifiRecoveryStore.clear(application)
        runCatching {
            val request = NetworkRequest.Builder()
                .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
                .build()
            connectivity.registerNetworkCallback(request, networkCallback, handler)
            callbackRegistered = true
        }.onFailure { error ->
            Log.w(TAG, "Could not register the bounded Wi-Fi network callback (${error.javaClass.simpleName}).")
        }

        val remaining = (BootWifiRecoveryPolicy.HARD_DEADLINE_MS - SystemClock.elapsedRealtime())
            .coerceAtLeast(1L)
        runCatching { wakeLock.acquire(remaining + WAKE_LOCK_RELEASE_MARGIN_MS) }
            .onFailure { error ->
                Log.w(TAG, "Could not acquire the bounded Wi-Fi recovery wake lock (${error.javaClass.simpleName}).")
            }
        schedule(0)
    }

    fun stop() {
        if (stopped) return
        stopped = true
        handler.removeCallbacks(evaluation)
        if (callbackRegistered) {
            runCatching { connectivity.unregisterNetworkCallback(networkCallback) }
            callbackRegistered = false
        }
        if (wakeLock.isHeld) runCatching { wakeLock.release() }
    }

    @Suppress("DEPRECATION") // Required by the pinned Android 12 privileged system-app deployment.
    private fun evaluate() {
        if (stopped) return
        val observation = observe()
        when (val command = policy.evaluate(observation)) {
            is BootWifiRecoveryCommand.Wait -> schedule(command.delayMs)
            BootWifiRecoveryCommand.EnableWifi -> {
                execute("enable") { wifi.setWifiEnabled(true) }
                schedule(BootWifiRecoveryPolicy.POLL_INTERVAL_MS)
            }
            BootWifiRecoveryCommand.RequestReconnect -> {
                execute("reconnect") { wifi.reconnect() }
                schedule(BootWifiRecoveryPolicy.POLL_INTERVAL_MS)
            }
            is BootWifiRecoveryCommand.Finish -> finish(command.result)
        }
    }

    @Suppress("DEPRECATION") // getAllNetworks remains the bounded synchronous snapshot on Android 12.
    private fun observe(): BootWifiObservation {
        var wifiHasIp = false
        var internetValidated = false
        runCatching {
            connectivity.allNetworks.forEach { network ->
                val capabilities = connectivity.getNetworkCapabilities(network) ?: return@forEach
                if (!capabilities.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) return@forEach
                val hasAddress = connectivity.getLinkProperties(network)
                    ?.linkAddresses
                    ?.any { link -> !link.address.isLoopbackAddress && !link.address.isLinkLocalAddress }
                    ?: false
                if (hasAddress) {
                    wifiHasIp = true
                    if (capabilities.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED)) {
                        internetValidated = true
                    }
                }
            }
        }.onFailure { error ->
            Log.w(TAG, "Could not inspect Wi-Fi link state (${error.javaClass.simpleName}).")
        }
        val safeToMutate = !CallRuntimeState.isBusy() &&
            RootAudioTrigger.isIdle(application) &&
            audio.mode == AudioManager.MODE_NORMAL
        return BootWifiObservation(
            elapsedSinceBootMs = SystemClock.elapsedRealtime(),
            wifiEnabled = runCatching { wifi.isWifiEnabled }.getOrDefault(false),
            wifiHasIp = wifiHasIp,
            internetValidated = internetValidated,
            wifiConnectionInProgress = runCatching {
                wifi.connectionInfo.supplicantState in CONNECTION_IN_PROGRESS_STATES
            }.getOrDefault(false),
            safeToMutateWifi = safeToMutate,
        )
    }

    private fun execute(name: String, operation: () -> Boolean) {
        runCatching { operation() }
            .onSuccess { accepted -> Log.i(TAG, "Bounded Wi-Fi recovery action $name accepted=$accepted.") }
            .onFailure { error ->
                Log.w(TAG, "Bounded Wi-Fi recovery action $name failed (${error.javaClass.simpleName}).")
            }
    }

    private fun finish(result: BootWifiRecoveryResult) {
        BootWifiRecoveryStore.save(application, result, System.currentTimeMillis())
        Log.i(
            TAG,
                "Boot Wi-Fi recovery finished outcome=${result.outcome.wireValue} " +
                "elapsed_ms=${result.elapsedSinceBootMs} validated=${result.internetValidated} " +
                "reconnect_attempts=${result.reconnectAttempts} " +
                "enable_attempts=${result.wifiEnableAttempts}.",
        )
        stop()
        onFinished()
    }

    private fun schedule(delayMs: Long) {
        if (stopped) return
        handler.removeCallbacks(evaluation)
        handler.postDelayed(evaluation, delayMs.coerceAtLeast(0L))
    }

    companion object {
        private const val TAG = "IVRdroidWifi"
        private const val WAKE_LOCK_TAG = "IVRdroid:BootWifiRecovery"
        private const val WAKE_LOCK_RELEASE_MARGIN_MS = 5_000L
        private val CONNECTION_IN_PROGRESS_STATES = setOf(
            SupplicantState.SCANNING,
            SupplicantState.ASSOCIATING,
            SupplicantState.ASSOCIATED,
            SupplicantState.AUTHENTICATING,
            SupplicantState.FOUR_WAY_HANDSHAKE,
            SupplicantState.GROUP_HANDSHAKE,
            SupplicantState.COMPLETED,
        )
    }
}

data class BootWifiRecoveryReport(
    val result: BootWifiRecoveryResult,
    val completedAtEpochMs: Long,
) {
    fun completedAtIso8601(): String = Instant.ofEpochMilli(completedAtEpochMs).toString()
}

object BootWifiRecoveryStore {
    private const val PREFERENCES = "boot_wifi_recovery"
    private const val OUTCOME = "outcome"
    private const val COMPLETED_AT = "completed_at_epoch_ms"
    private const val ELAPSED = "elapsed_since_boot_ms"
    private const val VALIDATED = "internet_validated"
    private const val RECONNECT_ATTEMPTS = "reconnect_attempts"
    private const val ENABLE_ATTEMPTS = "wifi_enable_attempts"

    fun clear(context: Context) {
        context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE).edit().clear().apply()
    }

    fun save(context: Context, result: BootWifiRecoveryResult, completedAtEpochMs: Long) {
        context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE).edit()
            .putString(OUTCOME, result.outcome.wireValue)
            .putLong(COMPLETED_AT, completedAtEpochMs)
            .putLong(ELAPSED, result.elapsedSinceBootMs)
            .putBoolean(VALIDATED, result.internetValidated)
            .putInt(RECONNECT_ATTEMPTS, result.reconnectAttempts)
            .putInt(ENABLE_ATTEMPTS, result.wifiEnableAttempts)
            .apply()
    }

    fun latest(context: Context): BootWifiRecoveryReport? {
        val preferences = context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE)
        val wireOutcome = preferences.getString(OUTCOME, null) ?: return null
        val outcome = BootWifiRecoveryOutcome.entries.firstOrNull { it.wireValue == wireOutcome }
            ?: return null
        val completedAt = preferences.getLong(COMPLETED_AT, 0L).takeIf { it > 0 } ?: return null
        return BootWifiRecoveryReport(
            result = BootWifiRecoveryResult(
                outcome = outcome,
                elapsedSinceBootMs = preferences.getLong(ELAPSED, 0L).coerceAtLeast(0L),
                internetValidated = preferences.getBoolean(VALIDATED, false),
                reconnectAttempts = preferences.getInt(RECONNECT_ATTEMPTS, 0).coerceAtLeast(0),
                wifiEnableAttempts = preferences.getInt(ENABLE_ATTEMPTS, 0).coerceAtLeast(0),
            ),
            completedAtEpochMs = completedAt,
        )
    }
}
