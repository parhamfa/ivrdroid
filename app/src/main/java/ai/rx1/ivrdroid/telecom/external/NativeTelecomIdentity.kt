package ai.rx1.ivrdroid.telecom.external

/** Pinned Android 12 Call.toString identity; never use a phone number as native ownership. */
object NativeTelecomIdentity {
    private val pattern = Regex("^Call \\[id: (TC@[A-Za-z0-9_-]{1,60}), state: .*")
    fun from(value: String): String? = pattern.matchEntire(value)?.groupValues?.get(1)
}
