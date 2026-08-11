package ai.rx1.ivrdroid.telecom.external

import ai.rx1.ivrdroid.control.CanonicalJson
import ai.rx1.ivrdroid.control.RevisionCompiler
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test
import java.security.MessageDigest

class ExternalCallInstructionResolverTest {
    @Test
    fun resolvesTheSharedSignedV4Fixture() {
        val manifest = JSONObject(resource("configuration_revision_v4.json"))
        val request = CallControlRequest.Dial(
            "11111111-1111-4111-8111-111111111111",
            86,
            "00000000-0000-4000-8000-000000000202",
            1,
            "33333333-3333-4333-8333-333333333333",
            1_000,
            "03136644636",
            30_000,
        )
        assertEquals(
            SignedExternalCallInstruction(86, request.blockId, request.phoneNumber, request.answerTimeoutMs),
            ExternalCallInstructionResolver.resolve(manifest, request),
        )
    }

    @Test
    fun resolvesExternalCallFromV41PromptBargeInManifest() {
        val manifest = v41ExternalCallManifest()
        val request = CallControlRequest.Dial(
            "11111111-1111-4111-8111-111111111111",
            87,
            "00000000-0000-4000-8000-00000000012e",
            1,
            "33333333-3333-4333-8333-333333333333",
            1_000,
            "03136644636",
            30_000,
        )

        assertEquals(
            SignedExternalCallInstruction(87, request.blockId, request.phoneNumber, request.answerTimeoutMs),
            ExternalCallInstructionResolver.resolve(manifest, request),
        )
    }

    @Test
    fun rejectsUnsupportedV4CompilerVersion() {
        val manifest = v4Manifest().put("compiler_version", "4.2.0")
        val request = CallControlRequest.Dial(
            "11111111-1111-4111-8111-111111111111",
            91,
            "00000000-0000-4000-8000-000000000202",
            1,
            "33333333-3333-4333-8333-333333333333",
            1_000,
            "03136644636",
            30_000,
        )

        assertThrows(IllegalArgumentException::class.java) {
            ExternalCallInstructionResolver.resolve(manifest, request)
        }
    }

    @Test
    fun compilesAndResolvesOnlyExactSignedV4InstructionScalars() {
        val manifest = v4Manifest()
        val compiled = RevisionCompiler.compile(manifest, digest(CanonicalJson.encode(manifest)))
        assertTrue(compiled.document.startsWith("IVRDROID_CONFIG_V4\n"))
        assertTrue(compiled.document.contains("MAX_AUTOMATED_SESSION_MS 61500\n"))
        assertTrue(
            compiled.document.contains(
                "INSTRUCTION 1 00000000-0000-4000-8000-000000000202 " +
                    "EXTERNAL_CALL 03136644636 30000 2 3 4",
            ),
        )

        val request = CallControlRequest.Dial(
            "11111111-1111-4111-8111-111111111111",
            91,
            "00000000-0000-4000-8000-000000000202",
            1,
            "33333333-3333-4333-8333-333333333333",
            1_000,
            "03136644636",
            30_000,
        )
        assertEquals("03136644636", ExternalCallInstructionResolver.resolve(manifest, request).phoneNumber)

        assertThrows(IllegalArgumentException::class.java) {
            ExternalCallInstructionResolver.resolve(manifest, request.copy(phoneNumber = "03136644637"))
        }
        assertThrows(IllegalArgumentException::class.java) {
            ExternalCallInstructionResolver.resolve(manifest, request.copy(answerTimeoutMs = 31_000))
        }
    }

    @Test
    fun rejectsExternalCallWithoutImmediateRecordingNoticePrompt() {
        val manifest = v4Manifest()
        manifest.getJSONObject("program").getJSONArray("instructions")
            .getJSONObject(0).put("op", "schedule_branch")
            .put("schedule_id", "business_hours")
            .put("on_open_pc", 1)
            .put("on_closed_pc", 1)
            .put("on_holiday_pc", 1)
            .remove("prompt_id")
        val program = manifest.getJSONObject("program")
        manifest.put("program_sha256", digest(CanonicalJson.encode(program)))

        assertThrows(IllegalArgumentException::class.java) {
            RevisionCompiler.compile(manifest, digest(CanonicalJson.encode(manifest)))
        }
    }

    private fun v4Manifest(): JSONObject {
        val base = JSONObject(resource("configuration_revision_v3.json"))
        val flow = JSONObject()
            .put(
                "root",
                JSONObject()
                    .put("block_id", "00000000-0000-4000-8000-000000000201")
                    .put("type", "play_prompt")
                    .put("prompt_id", "00000000-0000-4000-8000-000000000065")
                    .put(
                        "next",
                        JSONObject()
                            .put("block_id", "00000000-0000-4000-8000-000000000202")
                            .put("type", "external_call")
                            .put("phone_number", "03136644636")
                            .put("answer_timeout_seconds", 30),
                    ),
            )
        val instructions = JSONArray()
            .put(
                JSONObject()
                    .put("pc", 0)
                    .put("block_id", "00000000-0000-4000-8000-000000000201")
                    .put("op", "play_prompt")
                    .put("prompt_id", "00000000-0000-4000-8000-000000000065")
                    .put("next_pc", 1),
            )
            .put(
                JSONObject()
                    .put("pc", 1)
                    .put("block_id", "00000000-0000-4000-8000-000000000202")
                    .put("op", "external_call")
                    .put("phone_number", "03136644636")
                    .put("answer_timeout_ms", 30_000)
                    .put("next_pc", 2)
                    .put("on_not_connected_pc", 3)
                    .put("on_system_failure_pc", 4),
            )
        for (pc in 2..4) {
            instructions.put(
                JSONObject()
                    .put("pc", pc)
                    .put("block_id", "00000000-0000-4000-8000-00000000020${pc + 1}")
                    .put("op", "end_call"),
            )
        }
        val program = JSONObject()
            .put("version", 4)
            .put("entry_pc", 0)
            .put("maximum_automated_session_ms", 61_500)
            .put("instructions", instructions)
        val source = JSONObject()
            .put("flow", flow)
            .put("recording_behavior", base.getJSONObject("recording_behavior"))
        return base
            .put("schema_version", 4)
            .put("compiler_version", "4.0.0")
            .put("revision_id", 91)
            .put("flow", flow)
            .put("program", program)
            .put("source_sha256", digest(CanonicalJson.encode(source)))
            .put("program_sha256", digest(CanonicalJson.encode(program)))
    }

    private fun v41ExternalCallManifest(): JSONObject {
        val manifest = JSONObject(resource("configuration_revision_v41.json"))
        val flow = manifest.getJSONObject("flow")
        val external = flow.getJSONObject("root")
            .getJSONArray("branches")
            .getJSONObject(0)
            .getJSONObject("root")
        external.put("type", "external_call")
            .put("phone_number", "03136644636")
            .put("answer_timeout_seconds", 30)
            .put("next", endCall("00000000-0000-4000-8000-000000000133"))
            .put("on_not_connected", endCall("00000000-0000-4000-8000-000000000134"))
            .put("on_system_failure", endCall("00000000-0000-4000-8000-000000000135"))

        val program = manifest.getJSONObject("program")
        program.put("maximum_automated_session_ms", 61_000)
        val instructions = program.getJSONArray("instructions")
        instructions.getJSONObject(1)
            .put("op", "external_call")
            .put("phone_number", "03136644636")
            .put("answer_timeout_ms", 30_000)
            .put("next_pc", 6)
            .put("on_not_connected_pc", 7)
            .put("on_system_failure_pc", 8)
        listOf(
            "00000000-0000-4000-8000-000000000133",
            "00000000-0000-4000-8000-000000000134",
            "00000000-0000-4000-8000-000000000135",
        ).forEachIndexed { index, blockId ->
            instructions.put(
                JSONObject()
                    .put("pc", index + 6)
                    .put("block_id", blockId)
                    .put("op", "end_call"),
            )
        }

        val source = JSONObject()
            .put("flow", flow)
            .put("recording_behavior", manifest.getJSONObject("recording_behavior"))
        return manifest
            .put("source_sha256", digest(CanonicalJson.encode(source)))
            .put("program_sha256", digest(CanonicalJson.encode(program)))
    }

    private fun endCall(blockId: String): JSONObject = JSONObject()
        .put("block_id", blockId)
        .put("type", "end_call")

    private fun digest(value: String): String = MessageDigest.getInstance("SHA-256")
        .digest(value.toByteArray(Charsets.UTF_8))
        .joinToString("") { "%02x".format(it) }

    private fun resource(name: String): String = requireNotNull(javaClass.classLoader?.getResource(name)).readText()
}
