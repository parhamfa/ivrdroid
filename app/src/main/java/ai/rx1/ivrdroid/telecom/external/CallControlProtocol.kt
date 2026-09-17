package ai.rx1.ivrdroid.telecom.external

import java.util.UUID

sealed interface CallControlRequest {
    val sessionId: String
    val revisionId: Long
    val blockId: String
    val sequence: Long
    val bootId: String
    val elapsedMs: Long

    data class Dial(
        override val sessionId: String,
        override val revisionId: Long,
        override val blockId: String,
        override val sequence: Long,
        override val bootId: String,
        override val elapsedMs: Long,
        val phoneNumber: String,
        val answerTimeoutMs: Int,
    ) : CallControlRequest

    data class RecorderReady(
        override val sessionId: String,
        override val revisionId: Long,
        override val blockId: String,
        override val sequence: Long,
        override val bootId: String,
        override val elapsedMs: Long,
    ) : CallControlRequest

    data class Cancel(
        override val sessionId: String,
        override val revisionId: Long,
        override val blockId: String,
        override val sequence: Long,
        override val bootId: String,
        override val elapsedMs: Long,
        val reason: String,
    ) : CallControlRequest
}

enum class CallControlStatus {
    ACK,
    CALLER_HELD,
    DIALING,
    OPERATOR_ANSWERED,
    MERGING,
    CONFERENCED,
    COMPLETED,
    NOT_CONNECTED,
    SYSTEM_FAILURE,
}

data class CallControlStatusRecord(
    val status: CallControlStatus,
    val sessionId: String,
    val revisionId: Long,
    val blockId: String,
    val sequence: Long,
    val bootId: String,
    val elapsedMs: Long,
    val reason: String,
)

object CallControlProtocol {
    const val VERSION = 2
    const val PREFIX = "IVRDROID_CALL_CONTROL_V2"
    const val MAXIMUM_RECORD_BYTES = 512
    private val phoneNumber = Regex("\\+?[0-9]{8,15}")
    private val reason = Regex("(?:-|[A-Z][A-Z0-9_]{0,63})")
    private val blockUuid = Regex(
        "[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}",
    )

    fun parseRequest(record: String): CallControlRequest {
        requireCanonicalRecord(record)
        val fields = record.dropLast(1).split(' ')
        require(fields.firstOrNull() == PREFIX)
        return when (fields.getOrNull(1)) {
            "DIAL" -> {
                require(fields.size == 10)
                CallControlRequest.Dial(
                    canonicalUuid(fields[2]),
                    positiveRevision(fields[3]),
                    canonicalBlock(fields[4]),
                    positiveSequence(fields[5]),
                    canonicalUuid(fields[6]),
                    nonnegativeElapsed(fields[7]),
                    fields[8].also { require(phoneNumber.matches(it)) },
                    canonicalInt(fields[9], positive = true).also {
                        require(it in 5_000..120_000 && it % 1_000 == 0)
                    },
                )
            }
            "RECORDER_READY" -> {
                require(fields.size == 8)
                CallControlRequest.RecorderReady(
                    canonicalUuid(fields[2]),
                    positiveRevision(fields[3]),
                    canonicalBlock(fields[4]),
                    positiveSequence(fields[5]),
                    canonicalUuid(fields[6]),
                    nonnegativeElapsed(fields[7]),
                )
            }
            "CANCEL" -> {
                require(fields.size == 9)
                CallControlRequest.Cancel(
                    canonicalUuid(fields[2]),
                    positiveRevision(fields[3]),
                    canonicalBlock(fields[4]),
                    positiveSequence(fields[5]),
                    canonicalUuid(fields[6]),
                    nonnegativeElapsed(fields[7]),
                    fields[8].also { require(reason.matches(it) && it != "-") },
                )
            }
            else -> error("Unsupported call-control request.")
        }
    }

    fun formatStatus(record: CallControlStatusRecord): String {
        require(record.sequence > 0 && record.elapsedMs >= 0)
        val value = buildString {
            append(PREFIX).append(' ').append(record.status.name).append(' ')
            append(canonicalUuid(record.sessionId)).append(' ')
            append(record.revisionId.also { require(it > 0) }).append(' ')
            append(canonicalBlock(record.blockId)).append(' ')
            append(record.sequence).append(' ')
            append(canonicalUuid(record.bootId)).append(' ')
            append(record.elapsedMs).append(' ')
            append(record.reason.also { require(reason.matches(it)) }).append('\n')
        }
        requireCanonicalRecord(value)
        return value
    }

    fun parseStatus(record: String): CallControlStatusRecord {
        requireCanonicalRecord(record)
        val fields = record.dropLast(1).split(' ')
        require(fields.size == 9 && fields[0] == PREFIX)
        return CallControlStatusRecord(
            CallControlStatus.valueOf(fields[1]),
            canonicalUuid(fields[2]),
            positiveRevision(fields[3]),
            canonicalBlock(fields[4]),
            canonicalLong(fields[5], positive = true),
            canonicalUuid(fields[6]),
            canonicalLong(fields[7], positive = false),
            fields[8].also { require(reason.matches(it)) },
        )
    }

    private fun requireCanonicalRecord(record: String) {
        val bytes = record.toByteArray(Charsets.US_ASCII)
        require(bytes.size in 1..MAXIMUM_RECORD_BYTES)
        require(String(bytes, Charsets.US_ASCII) == record)
        require(record.endsWith('\n') && record.dropLast(1).none { it == '\n' || it == '\r' || it == '\t' })
        require(!record.startsWith(' ') && !record.contains("  "))
    }

    private fun positiveRevision(value: String): Long = canonicalLong(value, positive = true)

    private fun positiveSequence(value: String): Long = canonicalLong(value, positive = true)

    private fun nonnegativeElapsed(value: String): Long = canonicalLong(value, positive = false)

    private fun canonicalLong(value: String, positive: Boolean): Long {
        require(value.matches(if (positive) Regex("[1-9][0-9]*") else Regex("(?:0|[1-9][0-9]*)")))
        return value.toLong().also { require(!positive || it > 0) }
    }

    private fun canonicalInt(value: String, positive: Boolean): Int {
        canonicalLong(value, positive)
        return value.toInt()
    }

    private fun canonicalBlock(value: String): String = value.also { require(blockUuid.matches(it)) }

    private fun canonicalUuid(value: String): String = UUID.fromString(value).toString().also {
        require(it == value)
    }
}
