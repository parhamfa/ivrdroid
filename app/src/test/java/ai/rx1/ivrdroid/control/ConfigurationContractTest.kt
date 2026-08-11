package ai.rx1.ivrdroid.control

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.assertThrows
import org.junit.Test
import java.security.MessageDigest
import java.time.Instant

class ConfigurationContractTest {
    @Test
    fun canonicalJsonMatchesServerDigestAndSignature() {
        val manifest = JSONObject(resource("configuration_revision_v1.json"))
        val metadata = JSONObject(resource("configuration_revision_v1.meta.json"))
        val canonical = CanonicalJson.encode(manifest).toByteArray(Charsets.UTF_8)
        assertEquals(
            metadata.getString("manifest_sha256"),
            MessageDigest.getInstance("SHA-256").digest(canonical).toHex(),
        )
        val response = JSONObject()
            .put("manifest", manifest)
            .put("manifest_sha256", metadata.getString("manifest_sha256"))
            .put("signature_b64", metadata.getString("signature_b64"))
        val verified = RevisionVerifier.verify(
            response,
            42,
            metadata.getString("manifest_sha256"),
            metadata.getString("signature_b64"),
        )
        assertEquals(42, verified.revisionId)
    }

    @Test
    fun rejectsAManifestChangedAfterItWasSigned() {
        val manifest = JSONObject(resource("configuration_revision_v1.json"))
        val metadata = JSONObject(resource("configuration_revision_v1.meta.json"))
        manifest.getJSONObject("caller_policy").put("mode", "ACCEPT_ALL")
        val response = JSONObject()
            .put("manifest", manifest)
            .put("manifest_sha256", metadata.getString("manifest_sha256"))
            .put("signature_b64", metadata.getString("signature_b64"))

        assertThrows(IllegalArgumentException::class.java) {
            RevisionVerifier.verify(
                response,
                42,
                metadata.getString("manifest_sha256"),
                metadata.getString("signature_b64"),
            )
        }
    }

    @Test
    fun compilesOnlyBoundedHelperCommandsAndUtcWindows() {
        val manifest = JSONObject(resource("configuration_revision_v1.json"))
        val metadata = JSONObject(resource("configuration_revision_v1.meta.json"))
        val compiled = RevisionCompiler.compile(
            manifest,
            metadata.getString("manifest_sha256"),
            Instant.parse("2026-08-05T12:00:00Z"),
        )
        assertTrue(compiled.document.startsWith("IVRDROID_CONFIG_V1\nREVISION 42\n"))
        assertTrue(compiled.document.contains("NODE menu COLLECT"))
        assertTrue(compiled.document.contains("NODE repeat REPEAT menu 2 end"))
        assertTrue(compiled.document.contains("WINDOW business_hours H "))
        assertTrue(compiled.document.endsWith("END_CONFIG\n"))
        assertEquals(1, compiled.promptAssets.size)
    }

    @Test
    fun defaultCompilationIsAnchoredToTheSignedPublishTime() {
        val manifest = JSONObject(resource("configuration_revision_v1.json"))
        val metadata = JSONObject(resource("configuration_revision_v1.meta.json"))

        val first = RevisionCompiler.compile(manifest, metadata.getString("manifest_sha256"))
        val second = RevisionCompiler.compile(manifest, metadata.getString("manifest_sha256"))

        assertEquals(first.document, second.document)
        val publishedAt = Instant.parse(manifest.getString("published_at"))
        assertTrue(first.document.contains("HORIZON ${publishedAt.minusSeconds(86_400).epochSecond} "))
    }

    @Test
    fun acceptsProductionPublishTimestampsWithAnExplicitUtcOffset() {
        val manifest = JSONObject(resource("configuration_revision_v1.json"))
            .put("published_at", "2026-08-07T13:38:23.380454+00:00")

        val compiled = RevisionCompiler.compile(manifest, "a".repeat(64))

        assertTrue(compiled.document.contains("HORIZON 1786023503 "))
    }

    @Test
    fun acceptsOnlyReproducibleLegacyV1CacheWindows() {
        val manifest = JSONObject(resource("configuration_revision_v1.json"))
        val metadata = JSONObject(resource("configuration_revision_v1.meta.json"))
        val publishedAt = Instant.parse(manifest.getString("published_at"))
        val legacy = RevisionCompiler.compile(
            manifest,
            metadata.getString("manifest_sha256"),
            publishedAt.plusSeconds(25),
        )

        assertEquals(
            legacy.document,
            RevisionCompiler.verifyCachedCompilation(
                manifest,
                metadata.getString("manifest_sha256"),
                legacy.document,
                publishedAt.plusSeconds(60),
            )?.document,
        )
        assertEquals(
            null,
            RevisionCompiler.verifyCachedCompilation(
                manifest,
                metadata.getString("manifest_sha256"),
                legacy.document.replace("NODE menu COLLECT", "NODE menu END"),
                publishedAt.plusSeconds(60),
            ),
        )
    }

    @Test
    fun rejectsAnUnsupportedCallerPolicyEvenWhenTheDocumentIsOtherwiseWellFormed() {
        val manifest = JSONObject(resource("configuration_revision_v1.json"))
        manifest.getJSONObject("caller_policy").put("mode", "ARBITRARY")
        assertThrows(IllegalArgumentException::class.java) {
            RevisionCompiler.compile(manifest, "a".repeat(64), Instant.parse("2026-08-05T12:00:00Z"))
        }
    }

    @Test
    fun verifiesAndCompilesTheOwnedV2InstructionTape() {
        val manifest = JSONObject(resource("configuration_revision_v2.json"))
        val metadata = JSONObject(resource("configuration_revision_v2.meta.json"))
        val canonical = CanonicalJson.encode(manifest).toByteArray(Charsets.UTF_8)
        assertEquals(
            metadata.getString("manifest_sha256"),
            MessageDigest.getInstance("SHA-256").digest(canonical).toHex(),
        )
        val response = JSONObject()
            .put("manifest", manifest)
            .put("manifest_sha256", metadata.getString("manifest_sha256"))
            .put("signature_b64", metadata.getString("signature_b64"))
        val verified = RevisionVerifier.verify(
            response,
            84,
            metadata.getString("manifest_sha256"),
            metadata.getString("signature_b64"),
            metadata.getString("signing_public_key_b64"),
        )
        val compiled = RevisionCompiler.compile(
            verified.manifest,
            verified.manifestSha256,
            Instant.parse("2026-08-07T12:00:00Z"),
        )
        assertTrue(compiled.document.startsWith("IVRDROID_CONFIG_V2\nREVISION 84\n"))
        assertTrue(compiled.document.contains("SOURCE 2e4e3be3f2d332c7d54d5d32170a4fc6d597163019771a46a7734fed5a8d4e45"))
        assertTrue(compiled.document.contains("INSTRUCTION 1 00000000-0000-4000-8000-000000000002 COLLECT - 5000 3 2"))
        assertTrue(compiled.document.contains("INSTRUCTION 3 00000000-0000-4000-8000-000000000004 RETURN 1"))
        assertTrue(compiled.document.contains("ENTRY 0\nEND_CONFIG\n"))
        assertEquals(2, compiled.promptAssets.size)
    }

    @Test
    fun rejectsAHashConsistentV2ProgramThatAliasesTwoBranches() {
        val manifest = JSONObject(resource("configuration_revision_v2.json"))
        val program = manifest.getJSONObject("program")
        val branches = program.getJSONArray("instructions")
            .getJSONObject(1)
            .getJSONArray("branches")
        branches.getJSONObject(1).put("target_pc", 2)
        manifest.put(
            "program_sha256",
            MessageDigest.getInstance("SHA-256")
                .digest(CanonicalJson.encode(program).toByteArray(Charsets.UTF_8))
                .toHex(),
        )
        assertThrows(IllegalArgumentException::class.java) {
            RevisionCompiler.compile(
                manifest,
                "a".repeat(64),
                Instant.parse("2026-08-07T12:00:00Z"),
            )
        }
    }

    @Test
    fun verifiesAndCompilesTheExplicitV3RecordingTape() {
        val manifest = JSONObject(resource("configuration_revision_v3.json"))
        val metadata = JSONObject(resource("configuration_revision_v3.meta.json"))
        val canonical = CanonicalJson.encode(manifest).toByteArray(Charsets.UTF_8)
        assertEquals(
            metadata.getString("manifest_sha256"),
            MessageDigest.getInstance("SHA-256").digest(canonical).toHex(),
        )
        val response = JSONObject()
            .put("manifest", manifest)
            .put("manifest_sha256", metadata.getString("manifest_sha256"))
            .put("signature_b64", metadata.getString("signature_b64"))
        val verified = RevisionVerifier.verify(
            response,
            85,
            metadata.getString("manifest_sha256"),
            metadata.getString("signature_b64"),
            metadata.getString("signing_public_key_b64"),
        )
        val compiled = RevisionCompiler.compile(verified.manifest, verified.manifestSha256)
        assertEquals(resource("configuration_revision_v3.compiled.txt"), compiled.document)
        assertTrue(compiled.document.startsWith("IVRDROID_CONFIG_V3\nREVISION 85\n"))
        assertTrue(compiled.document.contains("MAX_SESSION 61500\n"))
        assertTrue(
            compiled.document.contains(
                "INSTRUCTION 1 00000000-0000-4000-8000-000000000102 RECORD 60000 # 2 3",
            ),
        )
        assertTrue(compiled.document.endsWith("ENTRY 0\nEND_CONFIG\n"))
    }

    @Test
    fun verifiesAndCompilesTheSharedV4ExternalCallTapeExactly() {
        val manifest = JSONObject(resource("configuration_revision_v4.json"))
        val metadata = JSONObject(resource("configuration_revision_v4.meta.json"))
        val canonical = CanonicalJson.encode(manifest).toByteArray(Charsets.UTF_8)
        assertEquals(
            metadata.getString("manifest_sha256"),
            MessageDigest.getInstance("SHA-256").digest(canonical).toHex(),
        )
        val verified = RevisionVerifier.verify(
            JSONObject()
                .put("manifest", manifest)
                .put("manifest_sha256", metadata.getString("manifest_sha256"))
                .put("signature_b64", metadata.getString("signature_b64")),
            86,
            metadata.getString("manifest_sha256"),
            metadata.getString("signature_b64"),
            metadata.getString("signing_public_key_b64"),
        )
        val compiled = RevisionCompiler.compile(verified.manifest, verified.manifestSha256)
        assertEquals(resource("configuration_revision_v4.compiled.txt"), compiled.document)
        assertTrue(compiled.document.contains("MAX_AUTOMATED_SESSION_MS 61000\n"))
        assertTrue(compiled.document.contains("EXTERNAL_CALL 03136644636 30000 2 3 4\n"))
    }

    @Test
    fun verifiesAndCompilesTheSharedV41PromptBargeInTapeExactly() {
        val manifest = JSONObject(resource("configuration_revision_v41.json"))
        val metadata = JSONObject(resource("configuration_revision_v41.meta.json"))
        val canonical = CanonicalJson.encode(manifest).toByteArray(Charsets.UTF_8)
        assertEquals(
            metadata.getString("manifest_sha256"),
            MessageDigest.getInstance("SHA-256").digest(canonical).toHex(),
        )
        val verified = RevisionVerifier.verify(
            JSONObject()
                .put("manifest", manifest)
                .put("manifest_sha256", metadata.getString("manifest_sha256"))
                .put("signature_b64", metadata.getString("signature_b64")),
            87,
            metadata.getString("manifest_sha256"),
            metadata.getString("signature_b64"),
            metadata.getString("signing_public_key_b64"),
        )
        val compiled = RevisionCompiler.compile(verified.manifest, verified.manifestSha256)
        assertEquals(resource("configuration_revision_v41.compiled.txt"), compiled.document)
        assertTrue(compiled.document.contains(" 2 1 1 2 2 BARGE_IN\n"))
    }

    @Test
    fun compilesV41RecordedActionsAfterAPromptedMenuWithoutABranchPrompt() {
        listOf("external_call", "record_message").forEach { operation ->
            val manifest = JSONObject(resource("configuration_revision_v41.json"))
            val menuPrompt = manifest.getJSONObject("program")
                .getJSONArray("instructions")
                .getJSONObject(0)
                .getString("prompt_id")
            replaceProgram(
                manifest,
                recordedBranchProgram(
                    version = 4,
                    operation = operation,
                    menuPromptId = menuPrompt,
                    allowPromptBargeIn = true,
                ),
            )

            val compiled = RevisionCompiler.compile(manifest, "a".repeat(64))

            assertTrue(compiled.document.contains("INSTRUCTION 0 ${testBlock(0)} COLLECT $menuPrompt"))
            assertTrue(compiled.document.contains(" BARGE_IN\n"))
            assertTrue(
                compiled.document.contains(
                    if (operation == "external_call") {
                        "INSTRUCTION 1 ${testBlock(1)} EXTERNAL_CALL 03136644636 30000 2 3 4"
                    } else {
                        "INSTRUCTION 1 ${testBlock(1)} RECORD 60000 # 2 3"
                    },
                ),
            )
            assertTrue(
                compiled.document.lineSequence().none {
                    it.startsWith("INSTRUCTION ") && it.contains(" PLAY ")
                },
            )
        }
    }

    @Test
    fun rejectsV4RecordedActionsWhenTheEntireOwnedPathHasNoPrompt() {
        val expected = mapOf(
            "record_message" to
                "Record message requires an earlier Play prompt or menu prompt notice on this path.",
            "external_call" to
                "External call requires an earlier Play prompt or menu prompt notice on this path.",
        )
        expected.forEach { (operation, message) ->
            val manifest = JSONObject(resource("configuration_revision_v4.json"))
            replaceProgram(
                manifest,
                recordedBranchProgram(
                    version = 4,
                    operation = operation,
                    menuPromptId = null,
                    allowPromptBargeIn = false,
                ),
            )

            val failure = assertThrows(IllegalArgumentException::class.java) {
                RevisionCompiler.compile(manifest, "a".repeat(64))
            }
            assertEquals(message, failure.message)
        }
    }

    @Test
    fun rejectsNonCanonicalV41AndBargeInFieldsOnV40() {
        val v41WithoutEnabledCollector = JSONObject(resource("configuration_revision_v4.json"))
            .put("compiler_version", "4.1.0")
        assertThrows(IllegalArgumentException::class.java) {
            RevisionCompiler.compile(v41WithoutEnabledCollector, "a".repeat(64))
        }

        val v40WithBargeInField = JSONObject(resource("configuration_revision_v41.json"))
            .put("compiler_version", "4.0.0")
        assertThrows(IllegalArgumentException::class.java) {
            RevisionCompiler.compile(v40WithBargeInField, "a".repeat(64))
        }
    }

    @Test
    fun rejectsV3RecordingWithoutAnImmediateGreetingEvenWhenRehashed() {
        val manifest = JSONObject(resource("configuration_revision_v3.json"))
        val greeting = manifest.getJSONObject("program")
            .getJSONArray("instructions")
            .getJSONObject(0)
            .getString("prompt_id")
        replaceProgram(
            manifest,
            recordedBranchProgram(
                version = 3,
                operation = "record_message",
                menuPromptId = greeting,
                allowPromptBargeIn = false,
            ),
        )
        val failure = assertThrows(IllegalArgumentException::class.java) {
            RevisionCompiler.compile(manifest, "a".repeat(64))
        }
        assertEquals("Every V3 recording must immediately follow a Play prompt greeting.", failure.message)
    }

    private fun recordedBranchProgram(
        version: Int,
        operation: String,
        menuPromptId: String?,
        allowPromptBargeIn: Boolean,
    ): JSONObject {
        require(operation == "record_message" || operation == "external_call")
        val recoveryStart = if (operation == "external_call") 5 else 4
        val collect = JSONObject()
            .put("pc", 0)
            .put("block_id", testBlock(0))
            .put("op", "collect_digit")
            .put("prompt_id", menuPromptId ?: JSONObject.NULL)
            .put("timeout_ms", 5_000)
            .put("maximum_attempts", 3)
            .put("maximum_menu_returns", 2)
            .put("on_timeout_pc", recoveryStart)
            .put("on_invalid_pc", recoveryStart + 1)
            .put("on_return_limit_pc", recoveryStart + 2)
            .put(
                "branches",
                JSONArray().put(JSONObject().put("digit", "0").put("target_pc", 1)),
            )
        if (allowPromptBargeIn) collect.put("allow_prompt_barge_in", true)

        val recorded = JSONObject()
            .put("pc", 1)
            .put("block_id", testBlock(1))
            .put("op", operation)
        if (operation == "external_call") {
            recorded.put("phone_number", "03136644636")
                .put("answer_timeout_ms", 30_000)
                .put("next_pc", 2)
                .put("on_not_connected_pc", 3)
                .put("on_system_failure_pc", 4)
        } else {
            recorded.put("maximum_duration_ms", 60_000)
                .put("finish_key", "#")
                .put("next_pc", 2)
                .put("on_unavailable_pc", 3)
        }

        val instructions = JSONArray().put(collect).put(recorded)
        (2..recoveryStart + 2).forEach { pc ->
            instructions.put(
                JSONObject()
                    .put("pc", pc)
                    .put("block_id", testBlock(pc))
                    .put("op", "end_call"),
            )
        }
        return JSONObject()
            .put("version", version)
            .put("entry_pc", 0)
            .put(
                if (version == 3) "maximum_session_ms" else "maximum_automated_session_ms",
                61_000,
            )
            .put("instructions", instructions)
    }

    private fun replaceProgram(manifest: JSONObject, program: JSONObject) {
        manifest.put("program", program)
        manifest.put(
            "program_sha256",
            MessageDigest.getInstance("SHA-256")
                .digest(CanonicalJson.encode(program).toByteArray(Charsets.UTF_8))
                .toHex(),
        )
    }

    private fun testBlock(pc: Int): String =
        "00000000-0000-4000-8000-%012x".format(0x400 + pc)

    private fun resource(name: String): String =
        requireNotNull(javaClass.classLoader?.getResource(name)).readText()
}
