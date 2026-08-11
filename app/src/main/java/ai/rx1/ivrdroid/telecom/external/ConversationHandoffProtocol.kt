package ai.rx1.ivrdroid.telecom.external

import java.util.UUID

enum class ConversationHandoffResultKind { OK, FAILED }

data class ConversationHandoffRecord(
    val sessionId: String,
    val revisionId: Long,
    val blockId: String,
    val recordingId: String,
    val segmentIndex: Int,
    val bootId: String,
    val elapsedMs: Long,
    val result: ConversationHandoffResultKind,
    val reason: String,
)

object ConversationHandoffProtocol {
    const val VERSION = 1
    const val PREFIX = "IVRDROID_CONVERSATION_HANDOFF_V1"
    const val MAXIMUM_RECORD_BYTES = 512
    private val canonicalUuid = Regex(
        "[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}",
    )
    private val reason = Regex("(?:-|[A-Z][A-Z0-9_]{0,63})")

    fun format(record: ConversationHandoffRecord): String {
        validate(record)
        val value = buildString {
            append(PREFIX).append(' ')
            append(record.sessionId).append(' ')
            append(record.revisionId).append(' ')
            append(record.blockId).append(' ')
            append(record.recordingId).append(' ')
            append(record.segmentIndex).append(' ')
            append(record.bootId).append(' ')
            append(record.elapsedMs).append(' ')
            append(record.result.name).append(' ')
            append(record.reason).append('\n')
        }
        requireCanonical(value)
        return value
    }

    fun parse(value: String): ConversationHandoffRecord {
        requireCanonical(value)
        val fields = value.dropLast(1).split(' ')
        require(fields.size == 10 && fields[0] == PREFIX)
        return ConversationHandoffRecord(
            fields[1],
            canonicalLong(fields[2], positive = true),
            fields[3],
            fields[4],
            canonicalInt(fields[5], positive = false),
            fields[6],
            canonicalLong(fields[7], positive = false),
            ConversationHandoffResultKind.valueOf(fields[8]),
            fields[9],
        ).also(::validate)
    }

    private fun validate(record: ConversationHandoffRecord) {
        canonical(record.sessionId)
        require(record.revisionId > 0)
        canonical(record.blockId)
        canonical(record.recordingId)
        require(record.segmentIndex in 0..65_535)
        canonical(record.bootId)
        require(record.elapsedMs >= 0)
        require(reason.matches(record.reason))
        require(
            (record.result == ConversationHandoffResultKind.OK && record.reason == "-") ||
                (record.result == ConversationHandoffResultKind.FAILED && record.reason != "-"),
        )
    }

    private fun canonical(value: String) {
        require(canonicalUuid.matches(value) && UUID.fromString(value).toString() == value)
    }

    private fun canonicalLong(value: String, positive: Boolean): Long {
        require(value.matches(if (positive) Regex("[1-9][0-9]*") else Regex("(?:0|[1-9][0-9]*)")))
        return value.toLong().also { require(!positive || it > 0) }
    }

    private fun canonicalInt(value: String, positive: Boolean): Int {
        canonicalLong(value, positive)
        return value.toInt()
    }

    private fun requireCanonical(value: String) {
        val bytes = value.toByteArray(Charsets.US_ASCII)
        require(bytes.size in 1..MAXIMUM_RECORD_BYTES)
        require(String(bytes, Charsets.US_ASCII) == value)
        require(value.endsWith('\n'))
        require(value.dropLast(1).none { it == '\n' || it == '\r' || it == '\t' })
        require(!value.startsWith(' ') && !value.contains("  "))
    }
}
