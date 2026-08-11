package ai.rx1.ivrdroid.telecom

import android.content.Context
import ai.rx1.ivrdroid.IvrPreferences
import ai.rx1.ivrdroid.control.SecureControlStore

data class CallerPolicyDecision(
    val shouldHandle: Boolean,
    val reason: String,
    val canonicalCaller: String?,
)

object CallerPolicyEngine {
    fun decide(context: Context, rawCaller: String?): CallerPolicyDecision {
        val caller = canonicalize(rawCaller)
        if (!IvrPreferences.isLocalIvrEnabled(context)) {
            return CallerPolicyDecision(false, "LOCAL_KILL_SWITCH", caller)
        }
        val manifest = SecureControlStore.activeManifest(context)
            ?: return CallerPolicyDecision(false, "NO_ACTIVE_CONFIGURATION", caller)
        return runCatching {
            val policy = manifest.getJSONObject("caller_policy")
            val routeUnknown = policy.getBoolean("route_unknown_callers")
            val mode = policy.getString("mode")
            if (caller == null) {
                val enabledMode = mode in setOf(
                    "ALLOWLIST_ONLY",
                    "ACCEPT_ALL",
                    "ACCEPT_ALL_EXCEPT_BLOCKLIST",
                )
                return@runCatching CallerPolicyDecision(
                    routeUnknown && enabledMode,
                    if (routeUnknown && enabledMode) "IVR_HANDLED" else "UNKNOWN_TO_STOCK_DIALER",
                    null,
                )
            }
            val allowlist = policy.getJSONArray("allowlist")
            val blocklist = policy.getJSONArray("blocklist")
            val allowed = when (mode) {
                "IVR_DISABLED" -> false
                "ALLOWLIST_ONLY" -> allowlist.containsPhone(caller)
                "ACCEPT_ALL" -> true
                "ACCEPT_ALL_EXCEPT_BLOCKLIST" -> !blocklist.containsPhone(caller)
                else -> false
            }
            CallerPolicyDecision(
                allowed,
                if (allowed) "IVR_HANDLED" else when (mode) {
                    "IVR_DISABLED" -> "IVR_DISABLED"
                    "ALLOWLIST_ONLY" -> "NOT_ALLOWLISTED"
                    "ACCEPT_ALL_EXCEPT_BLOCKLIST" -> "EXCLUDED_CALLER"
                    else -> "INVALID_POLICY"
                },
                caller,
            )
        }.getOrElse { CallerPolicyDecision(false, "INVALID_POLICY", caller) }
    }

    internal fun canonicalize(rawCaller: String?): String? {
        val raw = rawCaller?.trim().orEmpty()
        if (raw.isEmpty()) return null
        if (raw.any { character ->
                character !in '0'..'9' &&
                    character != '+' &&
                    character != ' ' &&
                    character != '-' &&
                    character != '(' &&
                    character != ')'
            }
        ) {
            return null
        }
        if (raw.count { it == '+' } > 1 ||
            ('+' in raw && !raw.startsWith("+"))
        ) {
            return null
        }

        val asciiDigits = raw.filter { it in '0'..'9' }
        return when {
            raw.startsWith("+") && asciiDigits.length in 8..15 -> "+$asciiDigits"
            asciiDigits.startsWith("0098") && asciiDigits.length == 14 ->
                "+${asciiDigits.drop(2)}"
            asciiDigits.startsWith("98") && asciiDigits.length == 12 ->
                "+$asciiDigits"
            asciiDigits.startsWith("0") && asciiDigits.length == 11 ->
                "+98${asciiDigits.drop(1)}"
            else -> null
        }
    }

    private fun org.json.JSONArray.containsPhone(caller: String): Boolean {
        for (index in 0 until length()) {
            if (optJSONObject(index)?.optString("e164") == caller) return true
        }
        return false
    }
}
