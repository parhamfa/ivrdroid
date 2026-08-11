package ai.rx1.ivrdroid.control

import org.json.JSONArray
import org.json.JSONObject
import java.security.MessageDigest
import java.time.Instant
import java.time.LocalTime
import java.time.OffsetDateTime
import java.time.ZoneId

object RevisionCompiler {
    private val identifier = Regex("[a-z][a-z0-9_-]{0,31}")
    private val promptIdentifier = Regex("[0-9a-f-]{36}")
    private val blockIdentifier = Regex("[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}")
    private val digest = Regex("[0-9a-f]{64}")
    private const val HORIZON_DAYS = 365L

    data class Compiled(
        val document: String,
        val promptAssets: List<PromptAsset>,
    )

    data class PromptAsset(
        val id: String,
        val sha256: String,
        val sizeBytes: Long,
        val durationMs: Int,
    )

    private data class Window(
        val scheduleId: String,
        val state: Char,
        val start: Long,
        val end: Long,
    )

    fun compile(
        manifest: JSONObject,
        manifestSha256: String,
        now: Instant = publishedAt(manifest),
    ): Compiled {
        val schemaVersion = manifest.getInt("schema_version")
        require(schemaVersion in 1..4) { "Unsupported configuration schema." }
        val revisionId = manifest.getLong("revision_id")
        require(revisionId > 0 && digest.matches(manifestSha256))
        validateCallerPolicy(manifest.getJSONObject("caller_policy"))
        val horizonStart = now.minusSeconds(86_400).epochSecond
        val horizonEnd = now.plusSeconds(HORIZON_DAYS * 86_400).epochSecond
        val prompts = manifest.getJSONArray("prompts").objects().map { prompt ->
            val id = prompt.getString("id")
            val hash = prompt.getString("content_hash")
            val size = prompt.getLong("size_bytes")
            require(promptIdentifier.matches(id) && digest.matches(hash))
            require(prompt.getString("name").length in 1..120)
            require(prompt.getInt("version") > 0)
            require(prompt.getInt("duration_ms") in 1..300_000)
            require(size in 45..64L * 1024L * 1024L)
            require(prompt.getString("format") == "audio/wav;codec=pcm_s16le;rate=48000;channels=2")
            PromptAsset(id, hash, size, prompt.getInt("duration_ms"))
        }.sortedBy { it.id }
        require(prompts.size <= 64)
        require(prompts.map { it.id }.distinct().size == prompts.size)
        require(prompts.map { it.sha256 }.distinct().size == prompts.size)
        require(prompts.sumOf { it.sizeBytes } <= 128L * 1024L * 1024L)

        val schedules = manifest.getJSONArray("schedules").objects().sortedBy { it.getString("id") }
        require(schedules.size <= 32)
        require(schedules.map { checkedIdentifier(it.getString("id")) }.distinct().size == schedules.size)
        val windows = schedules.flatMap { schedule ->
            expandSchedule(schedule, horizonStart, horizonEnd)
        }.sortedWith(compareBy<Window> { it.scheduleId }.thenBy { it.start }.thenBy { it.state })
        val merged = mergeWindows(windows)
        require(merged.size <= 65_536)

        val output = StringBuilder()
        output.append("IVRDROID_CONFIG_V").append(schemaVersion).append('\n')
        output.append("REVISION ").append(revisionId).append('\n')
        output.append("MANIFEST ").append(manifestSha256).append('\n')
        output.append("HORIZON ").append(horizonStart).append(' ').append(horizonEnd).append('\n')
        schedules.forEach { output.append("SCHEDULE ").append(checkedIdentifier(it.getString("id"))).append('\n') }
        merged.forEach { window ->
            output.append("WINDOW ").append(window.scheduleId).append(' ')
                .append(window.state).append(' ').append(window.start).append(' ')
                .append(window.end).append('\n')
        }
        prompts.forEach { prompt ->
            output.append("PROMPT ").append(prompt.id).append(' ')
                .append(prompt.sha256).append(' ').append(prompt.sizeBytes).append('\n')
        }
        if (schemaVersion == 1) {
            val flow = manifest.getJSONObject("flow")
            val root = checkedIdentifier(flow.getString("root_node"))
            val nodes = flow.getJSONArray("nodes").objects()
            require(nodes.size in 1..64)
            output.append("ROOT ").append(root).append('\n')
            nodes.forEach { node -> appendNode(output, node) }
        } else {
            val expectedCompiler = "$schemaVersion.0.0"
            val compilerVersion = manifest.getString("compiler_version")
            val supportedCompiler = compilerVersion == expectedCompiler ||
                (schemaVersion == 4 && compilerVersion == "4.1.0")
            require(supportedCompiler) {
                "Unsupported V$schemaVersion compiler version."
            }
            val flow = manifest.getJSONObject("flow")
            val sourceSha256 = checkedDigest(manifest.getString("source_sha256"))
            val recordingBehavior = if (schemaVersion >= 3) {
                manifest.getJSONObject("recording_behavior").also(::validateRecordingBehavior)
            } else {
                null
            }
            val source = if (recordingBehavior == null) {
                flow
            } else {
                JSONObject()
                    .put("flow", flow)
                    .put("recording_behavior", recordingBehavior)
            }
            require(sha256(CanonicalJson.encode(source)) == sourceSha256) {
                "V$schemaVersion source tree digest verification failed."
            }
            val program = manifest.getJSONObject("program")
            val programSha256 = checkedDigest(manifest.getString("program_sha256"))
            require(sha256(CanonicalJson.encode(program)) == programSha256) {
                "V$schemaVersion runtime program digest verification failed."
            }
            output.append("SOURCE ").append(sourceSha256).append('\n')
            output.append("PROGRAM ").append(programSha256).append('\n')
            appendProgram(
                output,
                program,
                prompts.map { it.id }.toSet(),
                schedules.map { it.getString("id") }.toSet(),
                schemaVersion,
                recordingBehavior,
                compilerVersion,
            )
        }
        output.append("END_CONFIG\n")
        require(output.length <= 8 * 1024 * 1024) { "Compiled revision is too large." }
        return Compiled(output.toString(), prompts)
    }

    /**
     * V1 expanded schedule windows were historically anchored to device download time.
     * Accept an existing cache only when the complete document can be reproduced from
     * the same signed manifest and the anchor embedded in that cache. New compilations
     * are anchored to published_at and are therefore deterministic.
     */
    fun verifyCachedCompilation(
        manifest: JSONObject,
        manifestSha256: String,
        document: String,
        now: Instant = Instant.now(),
    ): Compiled? = runCatching {
        val deterministic = compile(manifest, manifestSha256)
        if (document == deterministic.document) return@runCatching deterministic
        if (manifest.getInt("schema_version") != 1) return@runCatching null

        val horizonLines = document.lineSequence().filter { it.startsWith("HORIZON ") }.toList()
        if (horizonLines.size != 1) return@runCatching null
        val fields = horizonLines.single().split(' ')
        if (fields.size != 3) return@runCatching null
        val legacyAnchor = Instant.ofEpochSecond(Math.addExact(fields[1].toLong(), 86_400L))
        val earliestAllowed = publishedAt(manifest).minusSeconds(300)
        if (legacyAnchor.isBefore(earliestAllowed) || legacyAnchor.isAfter(now.plusSeconds(300))) {
            return@runCatching null
        }

        compile(manifest, manifestSha256, legacyAnchor)
            .takeIf { it.document == document }
    }.getOrNull()

    private fun publishedAt(manifest: JSONObject): Instant =
        OffsetDateTime.parse(manifest.getString("published_at")).toInstant()

    private data class OwnedEdge(val target: Int, val returnLimit: Boolean = false)

    private fun appendProgram(
        output: StringBuilder,
        program: JSONObject,
        promptIds: Set<String>,
        scheduleIds: Set<String>,
        schemaVersion: Int,
        recordingBehavior: JSONObject?,
        compilerVersion: String,
    ) {
        require(program.getInt("version") == schemaVersion)
        if (schemaVersion == 3) {
            val maximumSession = program.getInt("maximum_session_ms")
            require(maximumSession in 0..600_000) { "V3 session duration is unbounded." }
            output.append("MAX_SESSION ").append(maximumSession).append('\n')
        } else if (schemaVersion == 4) {
            val maximumSession = program.getInt("maximum_automated_session_ms")
            require(maximumSession in 0..600_000) { "V4 automated session duration is unbounded." }
            output.append("MAX_AUTOMATED_SESSION_MS ").append(maximumSession).append('\n')
        }
        val instructions = program.getJSONArray("instructions").objects()
        require(instructions.size in 1..64)
        val entry = program.getInt("entry_pc")
        require(entry in instructions.indices)
        val blockIds = mutableSetOf<String>()
        val incoming = IntArray(instructions.size)
        val edges = Array(instructions.size) { mutableListOf<OwnedEdge>() }
        val operations = Array(instructions.size) { "" }
        val providesPromptNotice = BooleanArray(instructions.size)
        val returnTargets = IntArray(instructions.size) { -1 }
        val incomingParents = IntArray(instructions.size) { -1 }
        var promptBargeInInstructions = 0

        fun checkedPc(value: Int): Int = value.also { require(it in instructions.indices) }
        fun addEdge(source: Int, target: Int, returnLimit: Boolean = false) {
            val checked = checkedPc(target)
            incoming[checked] += 1
            if (incomingParents[checked] == -1) incomingParents[checked] = source
            edges[source].add(OwnedEdge(checked, returnLimit))
        }

        instructions.forEachIndexed { index, instruction ->
            require(instruction.getInt("pc") == index) { "V2 instruction PCs must be contiguous." }
            val blockId = checkedBlock(instruction.getString("block_id"))
            require(blockIds.add(blockId)) { "V2 block UUIDs must be unique." }
            val operation = instruction.getString("op")
            operations[index] = operation
            output.append("INSTRUCTION ").append(index).append(' ').append(blockId).append(' ')
            when (operation) {
                "play_prompt" -> {
                    val promptId = checkedPrompt(instruction.getString("prompt_id"))
                    require(promptId in promptIds) { "PLAY references a missing prompt asset." }
                    providesPromptNotice[index] = true
                    val next = checkedPc(instruction.getInt("next_pc"))
                    addEdge(index, next)
                    output.append("PLAY ").append(promptId).append(' ').append(next)
                }
                "collect_digit" -> {
                    val promptId = if (instruction.isNull("prompt_id")) "-" else checkedPrompt(instruction.getString("prompt_id"))
                    require(promptId == "-" || promptId in promptIds) { "COLLECT references a missing prompt asset." }
                    providesPromptNotice[index] = promptId != "-"
                    val timeout = instruction.getInt("timeout_ms")
                    val attempts = instruction.getInt("maximum_attempts")
                    val maximumReturns = instruction.getInt("maximum_menu_returns")
                    require(timeout in 1000..15000 && attempts in 1..3 && maximumReturns in 1..3)
                    val onTimeout = checkedPc(instruction.getInt("on_timeout_pc"))
                    val onInvalid = checkedPc(instruction.getInt("on_invalid_pc"))
                    val onReturnLimit = checkedPc(instruction.getInt("on_return_limit_pc"))
                    val branches = instruction.getJSONArray("branches").objects()
                    val allowPromptBargeIn = when {
                        schemaVersion == 4 && compilerVersion == "4.1.0" -> {
                            require(instruction.has("allow_prompt_barge_in") &&
                                !instruction.isNull("allow_prompt_barge_in"))
                            instruction.getBoolean("allow_prompt_barge_in")
                        }
                        else -> {
                            require(!instruction.has("allow_prompt_barge_in"))
                            false
                        }
                    }
                    require(!allowPromptBargeIn || promptId != "-") {
                        "Prompt interruption requires a menu prompt."
                    }
                    if (allowPromptBargeIn) promptBargeInInstructions += 1
                    require(branches.size in 1..12)
                    val digits = mutableSetOf<Char>()
                    var previousRank = -1
                    output.append("COLLECT ").append(promptId).append(' ')
                        .append(timeout).append(' ').append(attempts).append(' ')
                        .append(maximumReturns).append(' ').append(onTimeout).append(' ')
                        .append(onInvalid).append(' ').append(onReturnLimit).append(' ')
                        .append(branches.size)
                    branches.forEach { branch ->
                        val digit = branch.getString("digit")
                        require(digit.length == 1 && digit[0] in "0123456789*#" && digits.add(digit[0]))
                        val rank = "0123456789*#".indexOf(digit[0])
                        require(rank > previousRank) { "V2 digit branches are not canonical." }
                        previousRank = rank
                        val target = checkedPc(branch.getInt("target_pc"))
                        addEdge(index, target)
                        output.append(' ').append(digit).append(' ').append(target)
                    }
                    if (allowPromptBargeIn) output.append(" BARGE_IN")
                    addEdge(index, onTimeout)
                    addEdge(index, onInvalid)
                    addEdge(index, onReturnLimit, returnLimit = true)
                }
                "schedule_branch" -> {
                    val scheduleId = checkedIdentifier(instruction.getString("schedule_id"))
                    require(scheduleId in scheduleIds) { "SCHEDULE references a missing schedule." }
                    val onOpen = checkedPc(instruction.getInt("on_open_pc"))
                    val onClosed = checkedPc(instruction.getInt("on_closed_pc"))
                    val onHoliday = checkedPc(instruction.getInt("on_holiday_pc"))
                    addEdge(index, onOpen)
                    addEdge(index, onClosed)
                    addEdge(index, onHoliday)
                    output.append("SCHEDULE ").append(scheduleId).append(' ')
                        .append(onOpen).append(' ').append(onClosed).append(' ').append(onHoliday)
                }
                "return_to_menu" -> {
                    val menu = checkedPc(instruction.getInt("menu_pc"))
                    returnTargets[index] = menu
                    output.append("RETURN ").append(menu)
                }
                "record_message" -> {
                    require(schemaVersion >= 3 && recordingBehavior != null)
                    val maximumDuration = instruction.getInt("maximum_duration_ms")
                    val expectedMaximum = recordingBehavior.getInt("maximum_duration_seconds") * 1000
                    require(maximumDuration == expectedMaximum && maximumDuration in 10_000..180_000)
                    val finishKey = if (instruction.isNull("finish_key")) {
                        "-"
                    } else {
                        instruction.getString("finish_key").also {
                            require(it.length == 1 && it[0] in "0123456789*#")
                        }
                    }
                    val expectedFinish = if (recordingBehavior.isNull("finish_key")) {
                        "-"
                    } else {
                        recordingBehavior.getString("finish_key")
                    }
                    require(finishKey == expectedFinish)
                    val next = checkedPc(instruction.getInt("next_pc"))
                    val unavailable = checkedPc(instruction.getInt("on_unavailable_pc"))
                    addEdge(index, next)
                    addEdge(index, unavailable)
                    output.append("RECORD ").append(maximumDuration).append(' ')
                        .append(finishKey).append(' ').append(next).append(' ')
                        .append(unavailable)
                }
                "external_call" -> {
                    require(schemaVersion == 4)
                    val phoneNumber = instruction.getString("phone_number").also {
                        require(it.matches(Regex("\\+?[0-9]{8,15}")))
                    }
                    val answerTimeout = instruction.getInt("answer_timeout_ms").also {
                        require(it in 5_000..120_000 && it % 1_000 == 0)
                    }
                    val next = checkedPc(instruction.getInt("next_pc"))
                    val notConnected = checkedPc(instruction.getInt("on_not_connected_pc"))
                    val systemFailure = checkedPc(instruction.getInt("on_system_failure_pc"))
                    addEdge(index, next)
                    addEdge(index, notConnected)
                    addEdge(index, systemFailure)
                    output.append("EXTERNAL_CALL ").append(phoneNumber).append(' ')
                        .append(answerTimeout).append(' ').append(next).append(' ')
                        .append(notConnected).append(' ').append(systemFailure)
                }
                "end_call" -> output.append("END")
                else -> error("Unsupported V$schemaVersion instruction operation.")
            }
            output.append('\n')
        }

        require(incoming[entry] == 0) { "V2 entry instruction cannot be owned by another block." }
        if (schemaVersion == 4 && compilerVersion == "4.1.0") {
            require(promptBargeInInstructions > 0) {
                "V4.1 requires at least one prompt-interruptible collector."
            }
        }
        instructions.indices.filter { it != entry }.forEach { pc ->
            require(incoming[pc] == 1) { "Every non-entry V2 instruction must have exactly one owner." }
        }
        if (schemaVersion == 3) {
            instructions.indices.filter { operations[it] == "record_message" }.forEach { pc ->
                val parent = incomingParents[pc]
                require(parent >= 0 && operations[parent] == "play_prompt") {
                    "Every V3 recording must immediately follow a Play prompt greeting."
                }
            }
        }

        val visited = mutableSetOf<Int>()
        fun visit(
            pc: Int,
            depth: Int,
            ownerMenu: Int?,
            forbiddenReturnMenu: Int?,
            promptNoticeAvailable: Boolean,
        ) {
            require(depth <= 8) { "V2 program exceeds depth 8." }
            require(visited.add(pc)) { "V2 program ownership contains a cycle." }
            if (schemaVersion == 4 && !promptNoticeAvailable) {
                when (operations[pc]) {
                    "record_message" -> require(false) {
                        "Record message requires an earlier Play prompt or menu prompt notice on this path."
                    }
                    "external_call" -> require(false) {
                        "External call requires an earlier Play prompt or menu prompt notice on this path."
                    }
                }
            }
            val nextPromptNoticeAvailable = promptNoticeAvailable || providesPromptNotice[pc]
            when (operations[pc]) {
                "return_to_menu" -> {
                    require(ownerMenu != null && returnTargets[pc] == ownerMenu) {
                        "RETURN does not target its structurally containing menu."
                    }
                    require(returnTargets[pc] != forbiddenReturnMenu) {
                        "A menu return-limit path cannot return to the same menu."
                    }
                }
                "end_call" -> Unit
                "collect_digit" -> edges[pc].forEach { edge ->
                    visit(
                        edge.target,
                        depth + 1,
                        pc,
                        if (edge.returnLimit) pc else null,
                        nextPromptNoticeAvailable,
                    )
                }
                else -> edges[pc].forEach { edge ->
                    visit(
                        edge.target,
                        depth + 1,
                        ownerMenu,
                        forbiddenReturnMenu,
                        nextPromptNoticeAvailable,
                    )
                }
            }
        }
        visit(entry, 1, null, null, false)
        require(visited.size == instructions.size) { "V2 program contains unreachable instructions." }
        output.append("ENTRY ").append(entry).append('\n')
    }

    private fun validateRecordingBehavior(behavior: JSONObject) {
        require(behavior.getInt("maximum_duration_seconds") in 10..180)
        if (!behavior.isNull("finish_key")) {
            val key = behavior.getString("finish_key")
            require(key.length == 1 && key[0] in "0123456789*#")
        }
    }

    private fun validateCallerPolicy(policy: JSONObject) {
        require(
            policy.getString("mode") in setOf(
                "IVR_DISABLED",
                "ALLOWLIST_ONLY",
                "ACCEPT_ALL",
                "ACCEPT_ALL_EXCEPT_BLOCKLIST",
            ),
        )
        policy.getBoolean("route_unknown_callers")
        listOf("allowlist", "blocklist").forEach { key ->
            val entries = policy.getJSONArray(key)
            require(entries.length() <= 1000)
            val numbers = entries.objects().map { entry ->
                require(entry.getString("label").length in 1..120)
                entry.getString("e164").also {
                    require(it.matches(Regex("\\+[1-9][0-9]{7,14}")))
                }
            }
            require(numbers.distinct().size == numbers.size)
        }
    }

    private fun expandSchedule(schedule: JSONObject, horizonStart: Long, horizonEnd: Long): List<Window> {
        val id = checkedIdentifier(schedule.getString("id"))
        val zone = ZoneId.of(schedule.getString("timezone"))
        val weekly = schedule.getJSONArray("weekly").objects().groupBy { it.getInt("weekday") }
        val exceptions = schedule.getJSONArray("exceptions").objects().associateBy { it.getString("date") }
        val startDate = Instant.ofEpochSecond(horizonStart).atZone(zone).toLocalDate().minusDays(1)
        val endDate = Instant.ofEpochSecond(horizonEnd).atZone(zone).toLocalDate().plusDays(1)
        val result = mutableListOf<Window>()
        var date = startDate
        while (!date.isAfter(endDate)) {
            val exception = exceptions[date.toString()]
            if (exception != null) {
                val state = exception.getString("state")
                if (state == "open" || state == "holiday") {
                    addWindow(
                        result,
                        id,
                        if (state == "holiday") 'H' else 'O',
                        date.atStartOfDay(zone).toInstant().epochSecond,
                        date.plusDays(1).atStartOfDay(zone).toInstant().epochSecond,
                        horizonStart,
                        horizonEnd,
                    )
                } else {
                    require(state == "closed")
                }
            } else {
                val weekday = date.dayOfWeek.value - 1
                weekly[weekday].orEmpty().forEach { interval ->
                    addWindow(
                        result,
                        id,
                        'O',
                        date.atTime(LocalTime.parse(interval.getString("start"))).atZone(zone).toInstant().epochSecond,
                        date.atTime(LocalTime.parse(interval.getString("end"))).atZone(zone).toInstant().epochSecond,
                        horizonStart,
                        horizonEnd,
                    )
                }
            }
            date = date.plusDays(1)
        }
        return result
    }

    private fun addWindow(
        output: MutableList<Window>,
        scheduleId: String,
        state: Char,
        start: Long,
        end: Long,
        horizonStart: Long,
        horizonEnd: Long,
    ) {
        val clippedStart = maxOf(start, horizonStart)
        val clippedEnd = minOf(end, horizonEnd)
        if (clippedStart < clippedEnd) output.add(Window(scheduleId, state, clippedStart, clippedEnd))
    }

    private fun mergeWindows(windows: List<Window>): List<Window> {
        val merged = mutableListOf<Window>()
        windows.forEach { window ->
            val previous = merged.lastOrNull()
            if (previous != null && previous.scheduleId == window.scheduleId &&
                previous.state == window.state && window.start <= previous.end) {
                merged[merged.lastIndex] = previous.copy(end = maxOf(previous.end, window.end))
            } else {
                merged.add(window)
            }
        }
        return merged
    }

    private fun appendNode(output: StringBuilder, node: JSONObject) {
        val id = checkedIdentifier(node.getString("id"))
        output.append("NODE ").append(id).append(' ')
        when (node.getString("type")) {
            "play_prompt" -> output.append("PLAY ")
                .append(checkedPrompt(node.getString("prompt_id"))).append(' ')
                .append(checkedIdentifier(node.getString("next")))
            "collect_digit" -> {
                val prompt = if (node.isNull("prompt_id")) "-" else checkedPrompt(node.getString("prompt_id"))
                val branches = node.getJSONObject("branches")
                val digits = branches.keys().asSequence().toList().sortedBy { "0123456789*#".indexOf(it) }
                require(digits.isNotEmpty() && digits.size <= 12 && digits.all { it.length == 1 && it[0] in "0123456789*#" })
                output.append("COLLECT ").append(prompt).append(' ')
                    .append(node.getInt("timeout_ms")).append(' ')
                    .append(node.getInt("maximum_attempts")).append(' ')
                    .append(checkedIdentifier(node.getString("on_timeout"))).append(' ')
                    .append(checkedIdentifier(node.getString("on_invalid"))).append(' ')
                    .append(digits.size)
                digits.forEach { digit -> output.append(' ').append(digit).append(' ').append(checkedIdentifier(branches.getString(digit))) }
            }
            "schedule_branch" -> output.append("SCHEDULE ")
                .append(checkedIdentifier(node.getString("schedule_id"))).append(' ')
                .append(checkedIdentifier(node.getString("on_open"))).append(' ')
                .append(checkedIdentifier(node.getString("on_closed"))).append(' ')
                .append(checkedIdentifier(node.getString("on_holiday")))
            "repeat_menu" -> output.append("REPEAT ")
                .append(checkedIdentifier(node.getString("target"))).append(' ')
                .append(node.getInt("maximum_repeats")).append(' ')
                .append(checkedIdentifier(node.getString("on_exhausted")))
            "end_call" -> output.append("END")
            else -> error("Unsupported flow node type.")
        }
        output.append('\n')
    }

    private fun checkedIdentifier(value: String): String = value.also { require(identifier.matches(it)) }
    private fun checkedPrompt(value: String): String = value.also { require(promptIdentifier.matches(it)) }
    private fun checkedBlock(value: String): String = value.also { require(blockIdentifier.matches(it)) }
    private fun checkedDigest(value: String): String = value.also { require(digest.matches(it)) }
    private fun sha256(value: String): String =
        MessageDigest.getInstance("SHA-256").digest(value.toByteArray(Charsets.UTF_8)).toHex()

    private fun JSONArray.objects(): List<JSONObject> =
        buildList { for (index in 0 until length()) add(getJSONObject(index)) }
}
