package ai.rx1.ivrdroid.telecom

import ai.rx1.ivrdroid.BuildConfig

object TestCallGate {
    val allowlistedCallerE164: String? =
        canonicalize(BuildConfig.TEST_CALLER_E164)

    fun matches(rawCaller: String?): Boolean {
        val expected = allowlistedCallerE164 ?: return false
        return canonicalize(rawCaller) == expected
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
}
