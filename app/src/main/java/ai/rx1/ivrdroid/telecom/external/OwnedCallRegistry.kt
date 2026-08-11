package ai.rx1.ivrdroid.telecom.external

import android.content.Context
import android.system.Os
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.util.UUID

object OwnedCallRegistry {
    private const val VERSION = 2
    private const val MAXIMUM_BYTES = 32 * 1024L

    enum class Role { CALLER, OPERATOR }
    data class Ownership(
        val telecomCallId: String,
        val sessionId: String,
        val bootId: String,
        val role: Role,
        val callerHandleDigest: String? = null,
        val registeredElapsedMs: Long = 0,
        val confirmedByInCall: Boolean = role == Role.OPERATOR,
    )

    @Synchronized
    fun register(context: Context, telecomCallId: String, sessionId: String): Boolean =
        register(
            context,
            telecomCallId,
            sessionId,
            Role.CALLER,
            callerHandleDigest = null,
            registeredElapsedMs = android.os.SystemClock.elapsedRealtime(),
            confirmedByInCall = false,
        )

    @Synchronized
    fun registerScreenedCaller(
        context: Context,
        telecomCallId: String,
        sessionId: String,
        rawCallerHandle: String?,
        registeredElapsedMs: Long,
    ): Boolean {
        val bootId = BootIdentity.current() ?: return false
        return register(
            context,
            telecomCallId,
            sessionId,
            Role.CALLER,
            CallerHandleEvidence.digest(rawCallerHandle, bootId),
            registeredElapsedMs,
            confirmedByInCall = false,
        )
    }

    @Synchronized
    fun registerOperator(context: Context, telecomCallId: String, sessionId: String): Boolean =
        register(
            context,
            telecomCallId,
            sessionId,
            Role.OPERATOR,
            callerHandleDigest = null,
            registeredElapsedMs = 0,
            confirmedByInCall = true,
        )

    private fun register(
        context: Context,
        telecomCallId: String,
        sessionId: String,
        role: Role,
        callerHandleDigest: String?,
        registeredElapsedMs: Long,
        confirmedByInCall: Boolean,
    ): Boolean = runCatching {
        val bootId = requireNotNull(BootIdentity.current())
        requireSafeCallId(telecomCallId)
        require(UUID.fromString(sessionId).toString() == sessionId)
        require(registeredElapsedMs >= 0)
        callerHandleDigest?.let(::requireDigest)
        val entries = read(context, bootId).filterNot {
            it.telecomCallId == telecomCallId || (it.sessionId == sessionId && it.role == role)
        } + Ownership(
            telecomCallId,
            sessionId,
            bootId,
            role,
            callerHandleDigest,
            registeredElapsedMs,
            confirmedByInCall,
        )
        require(entries.size <= 8)
        write(context, bootId, entries)
        true
    }.getOrDefault(false)

    @Synchronized
    fun sessionFor(context: Context, telecomCallId: String): String? {
        val bootId = BootIdentity.current() ?: return null
        return read(context, bootId).singleOrNull {
            it.telecomCallId == telecomCallId &&
                (it.role == Role.OPERATOR || it.confirmedByInCall)
        }?.sessionId
    }

    @Synchronized
    fun callForSession(context: Context, sessionId: String): String? {
        val bootId = BootIdentity.current() ?: return null
        return read(context, bootId).singleOrNull {
            it.sessionId == sessionId && it.role == Role.CALLER && it.confirmedByInCall
        }?.telecomCallId
    }

    @Synchronized
    fun callerRegistration(context: Context, sessionId: String): RegisteredIncomingCaller? {
        val bootId = BootIdentity.current() ?: return null
        return read(context, bootId).singleOrNull {
            it.sessionId == sessionId && it.role == Role.CALLER
        }?.let {
            RegisteredIncomingCaller(
                it.sessionId,
                it.bootId,
                it.telecomCallId,
                it.callerHandleDigest,
                it.registeredElapsedMs,
                it.confirmedByInCall,
            )
        }
    }

    @Synchronized
    fun singleRebindableCaller(context: Context, nowElapsedMs: Long): RegisteredIncomingCaller? {
        val bootId = BootIdentity.current() ?: return null
        return read(context, bootId).filter {
            it.role == Role.CALLER && (
                it.confirmedByInCall ||
                    nowElapsedMs - it.registeredElapsedMs in
                    0..IncomingCallerClaimPolicy.INITIAL_CLAIM_WINDOW_MS
                )
        }.singleOrNull()?.let {
            RegisteredIncomingCaller(
                it.sessionId,
                it.bootId,
                it.telecomCallId,
                it.callerHandleDigest,
                it.registeredElapsedMs,
                it.confirmedByInCall,
            )
        }
    }

    @Synchronized
    fun confirmCaller(
        context: Context,
        sessionId: String,
        expectedCurrentCallId: String,
        inCallId: String,
    ): Boolean = runCatching {
        val bootId = requireNotNull(BootIdentity.current())
        requireSafeCallId(inCallId)
        val existing = read(context, bootId)
        val caller = existing.single {
            it.sessionId == sessionId && it.role == Role.CALLER &&
                it.telecomCallId == expectedCurrentCallId
        }
        require(existing.none {
            it.telecomCallId == inCallId &&
                !(it.sessionId == sessionId && it.role == Role.CALLER)
        })
        val updated = existing.filterNot { it === caller } + caller.copy(
            telecomCallId = inCallId,
            confirmedByInCall = true,
        )
        write(context, bootId, updated)
        true
    }.getOrDefault(false)

    @Synchronized
    fun removeCall(context: Context, telecomCallId: String) {
        val bootId = BootIdentity.current() ?: return
        write(context, bootId, read(context, bootId).filterNot { it.telecomCallId == telecomCallId })
    }

    @Synchronized
    fun removeSession(context: Context, sessionId: String) {
        val bootId = BootIdentity.current() ?: return
        write(context, bootId, read(context, bootId).filterNot { it.sessionId == sessionId })
    }

    private fun read(context: Context, currentBootId: String): List<Ownership> {
        val file = file(context)
        if (!file.isFile || Files.isSymbolicLink(file.toPath()) || file.length() !in 2..MAXIMUM_BYTES) return emptyList()
        return runCatching {
            val document = JSONObject(file.readText(Charsets.UTF_8))
            val version = document.getInt("version")
            require(version in 1..VERSION)
            if (document.getString("boot_id") != currentBootId) return@runCatching emptyList()
            val entries = document.getJSONArray("entries")
            buildList {
                for (index in 0 until entries.length()) {
                    val entry = entries.getJSONObject(index)
                    val telecomId = entry.getString("telecom_call_id").also(::requireSafeCallId)
                    val sessionId = entry.getString("session_id").also {
                        require(UUID.fromString(it).toString() == it)
                    }
                    val role = entry.optString("role", Role.CALLER.name).let(Role::valueOf)
                    val digest = entry.optString("caller_handle_digest").takeIf { it.isNotEmpty() }
                        ?.also(::requireDigest)
                    val registeredElapsedMs = entry.optLong("registered_elapsed_ms", 0)
                        .also { require(it >= 0) }
                    val confirmed = if (version >= 2) {
                        entry.optBoolean("confirmed_by_incall", role == Role.OPERATOR)
                    } else {
                        role == Role.OPERATOR
                    }
                    add(
                        Ownership(
                            telecomId,
                            sessionId,
                            currentBootId,
                            role,
                            digest,
                            registeredElapsedMs,
                            confirmed,
                        ),
                    )
                }
            }.also { values ->
                require(values.size <= 8)
                require(values.map { it.telecomCallId }.distinct().size == values.size)
                require(values.map { it.sessionId to it.role }.distinct().size == values.size)
            }
        }.getOrDefault(emptyList())
    }

    private fun write(context: Context, bootId: String, entries: List<Ownership>) {
        val destination = file(context)
        val directory = requireNotNull(destination.parentFile)
        require(directory.isDirectory || directory.mkdirs())
        Os.chmod(directory.absolutePath, 0b111000000)
        val document = JSONObject()
            .put("version", VERSION)
            .put("boot_id", bootId)
            .put(
                "entries",
                JSONArray().also { array ->
                    entries.forEach { entry ->
                        array.put(
                            JSONObject()
                                .put("telecom_call_id", entry.telecomCallId)
                                .put("session_id", entry.sessionId)
                                .put("role", entry.role.name)
                                .put("caller_handle_digest", entry.callerHandleDigest ?: "")
                                .put("registered_elapsed_ms", entry.registeredElapsedMs)
                                .put("confirmed_by_incall", entry.confirmedByInCall),
                        )
                    }
                },
            )
        val bytes = document.toString().toByteArray(Charsets.UTF_8)
        require(bytes.size <= MAXIMUM_BYTES)
        val temporary = File(directory, ".${destination.name}.tmp")
        FileOutputStream(temporary).use { output ->
            output.write(bytes)
            output.fd.sync()
        }
        Os.chmod(temporary.absolutePath, 0b110000000)
        Files.move(
            temporary.toPath(),
            destination.toPath(),
            StandardCopyOption.ATOMIC_MOVE,
            StandardCopyOption.REPLACE_EXISTING,
        )
    }

    private fun file(context: Context): File = File(context.filesDir, "external-call/ownership.json")

    private fun requireSafeCallId(value: String) {
        require(value.length in 1..256 && value.none { it.isISOControl() })
    }

    private fun requireDigest(value: String) {
        require(value.matches(Regex("[0-9a-f]{64}")))
    }
}
