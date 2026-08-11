package ai.rx1.ivrdroid.control

import org.json.JSONArray
import org.json.JSONObject

object CanonicalJson {
    fun encode(value: Any?): String = buildString { appendValue(value) }

    private fun StringBuilder.appendValue(value: Any?) {
        when (value) {
            null, JSONObject.NULL -> append("null")
            is JSONObject -> {
                append('{')
                val keys = value.keys().asSequence().toList().sorted()
                keys.forEachIndexed { index, key ->
                    if (index > 0) append(',')
                    appendString(key)
                    append(':')
                    appendValue(value.get(key))
                }
                append('}')
            }
            is JSONArray -> {
                append('[')
                for (index in 0 until value.length()) {
                    if (index > 0) append(',')
                    appendValue(value.get(index))
                }
                append(']')
            }
            is String -> appendString(value)
            is Boolean -> append(if (value) "true" else "false")
            is Byte, is Short, is Int, is Long -> append(value.toString())
            else -> error("Unsupported canonical JSON value: ${value::class.java.simpleName}")
        }
    }

    private fun StringBuilder.appendString(value: String) {
        append('"')
        value.forEach { character ->
            when (character) {
                '"' -> append("\\\"")
                '\\' -> append("\\\\")
                '\b' -> append("\\b")
                '\u000c' -> append("\\f")
                '\n' -> append("\\n")
                '\r' -> append("\\r")
                '\t' -> append("\\t")
                else -> if (character.code < 0x20) {
                    append("\\u")
                    append(character.code.toString(16).padStart(4, '0'))
                } else {
                    append(character)
                }
            }
        }
        append('"')
    }
}
