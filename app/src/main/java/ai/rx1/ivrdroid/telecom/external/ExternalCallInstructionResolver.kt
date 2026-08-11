package ai.rx1.ivrdroid.telecom.external

import ai.rx1.ivrdroid.control.CanonicalJson
import ai.rx1.ivrdroid.control.RevisionCompiler
import org.json.JSONObject
import java.security.MessageDigest

data class SignedExternalCallInstruction(
    val revisionId: Long,
    val blockId: String,
    val phoneNumber: String,
    val answerTimeoutMs: Int,
)

object ExternalCallInstructionResolver {
    fun resolve(manifest: JSONObject, request: CallControlRequest.Dial): SignedExternalCallInstruction {
        val instruction = resolve(manifest, request.revisionId, request.blockId)
        require(instruction.phoneNumber == request.phoneNumber) {
            "Helper phone number differs from the signed instruction."
        }
        require(instruction.answerTimeoutMs == request.answerTimeoutMs) {
            "Helper timeout differs from the signed instruction."
        }
        return instruction
    }

    fun resolve(manifest: JSONObject, revisionId: Long, blockId: String): SignedExternalCallInstruction {
        require(manifest.getInt("schema_version") == 4)
        require(manifest.getLong("revision_id") == revisionId)
        // The encrypted active-manifest slot is written only after signature verification. Re-running
        // the complete deterministic compiler here both enforces the supported V4 compiler versions
        // and prevents a bridge record from bypassing any graph, digest, recording-notice, or scalar
        // validation performed at activation time.
        val manifestDigest = MessageDigest.getInstance("SHA-256")
            .digest(CanonicalJson.encode(manifest).toByteArray(Charsets.UTF_8))
            .joinToString("") { "%02x".format(it) }
        RevisionCompiler.compile(manifest, manifestDigest)

        val instructions = manifest.getJSONObject("program").getJSONArray("instructions")
        val matches = buildList {
            for (index in 0 until instructions.length()) {
                val candidate = instructions.getJSONObject(index)
                if (candidate.getString("block_id") == blockId) add(candidate)
            }
        }
        require(matches.size == 1)
        val instruction = matches.single()
        require(instruction.getString("op") == "external_call")
        return SignedExternalCallInstruction(
            revisionId,
            blockId,
            instruction.getString("phone_number"),
            instruction.getInt("answer_timeout_ms"),
        )
    }
}
