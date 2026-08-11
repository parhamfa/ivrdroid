package ai.rx1.ivrdroid.telecom.external

import java.io.File
import java.util.UUID

object BootIdentity {
    private val cached: String? by lazy {
        runCatching {
            val value = File("/proc/sys/kernel/random/boot_id").readText(Charsets.US_ASCII).trim()
            UUID.fromString(value).toString().takeIf { it == value }
        }.getOrNull()
    }

    fun current(): String? = cached
}
