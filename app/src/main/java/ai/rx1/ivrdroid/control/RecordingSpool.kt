package ai.rx1.ivrdroid.control

import android.content.Context
import android.os.Process
import android.os.SystemClock
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.system.Os
import android.system.OsConstants
import ai.rx1.ivrdroid.audio.RootAudioTrigger
import ai.rx1.ivrdroid.telecom.external.ExternalCallAuditStore
import ai.rx1.ivrdroid.telecom.CallRuntimeState
import org.json.JSONObject
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.nio.file.Files
import java.security.KeyStore
import java.security.MessageDigest
import java.time.Instant
import java.util.UUID
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey

data class ConversationHandoffIdentity(
    val callId: String,
    val revisionId: Long,
    val blockId: String,
)

data class ConversationHandoffResult(
    val identity: ConversationHandoffIdentity,
    val recordingId: String,
    val segmentIndex: Int,
    val succeeded: Boolean,
    val reason: String,
)

object ConversationHandoffSelection {
    fun matchesCurrentReceipt(
        receipt: JSONObject,
        identity: ConversationHandoffIdentity,
        recordingId: String,
        segmentIndex: Int,
    ): Boolean = runCatching {
        receipt.getInt("version") == 2 && receipt.getString("kind") == "conversation" &&
            receipt.getString("call_id") == identity.callId &&
            receipt.getLong("revision_id") == identity.revisionId &&
            receipt.getString("block_id") == identity.blockId &&
            receipt.getString("recording_id") == recordingId &&
            receipt.getInt("segment_index") == segmentIndex && receipt.getInt("sequence") == segmentIndex
    }.getOrDefault(false)
}

object ConversationCaptureBoundary {
    fun permits(conferenceEvidenceAt: Instant, capturedAt: Instant): Boolean =
        !capturedAt.isBefore(conferenceEvidenceAt)
}

object RecordingSpool {
    private class DeferredForCall : Exception("Recording processing deferred until the call ends.")
    private const val KEY_ALIAS = "ivrdroid-recording-spool-v1"
    private const val MAXIMUM_VOICEMAIL_BYTES = 512L * 1024L * 1024L
    private const val MAXIMUM_CONVERSATION_BYTES = 4L * 1024L * 1024L * 1024L
    private const val MINIMUM_FILESYSTEM_RESERVE_BYTES = 512L * 1024L * 1024L
    private const val MAXIMUM_RECORDING_BYTES = 40L * 1024L * 1024L
    private const val MAXIMUM_RECEIPT_BYTES = 16L * 1024L
    private const val ENCRYPTION_OVERHEAD_BYTES = RecordingEnvelope.OVERHEAD_BYTES
    private val uuid = Regex("[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}")
    private val storageKey = Regex("${uuid.pattern}(?:\\.[0-9]{5})?")
    private val conversationStorageKey = Regex("(${uuid.pattern})\\.([0-9]{5})")
    private val digest = Regex("[0-9a-f]{64}")

    @Synchronized
    fun reconcile(context: Context) {
        try { reconcileInternal(context, onlyStorageKey = null) } catch (_: DeferredForCall) { }
    }

    /**
     * Encrypts at most one complete conversation pair. A helper may publish the WAV and receipt in
     * either order, so an unpaired file is deliberately left untouched for the next worker tick.
     */
    @Synchronized
    fun handoffNextConversation(
        context: Context,
        fallbackIdentity: ConversationHandoffIdentity,
    ): ConversationHandoffResult? {
        if (CallRuntimeState.isBusy()) return null
        val inbox = inbox(context)
        val candidate = inbox.listFiles { file ->
            file.name.endsWith(".json") && conversationStorageKey.matches(file.name.removeSuffix(".json"))
        }.orEmpty().sortedBy(File::getName).firstOrNull { receipt ->
            val candidateKey = receipt.name.removeSuffix(".json")
            val candidateMatch = requireNotNull(conversationStorageKey.matchEntire(candidateKey))
            File(inbox, "$candidateKey.wav").isFile && runCatching {
                requireSafeInbound(receipt, MAXIMUM_RECEIPT_BYTES)
                ConversationHandoffSelection.matchesCurrentReceipt(
                    JSONObject(receipt.readText(Charsets.UTF_8)),
                    fallbackIdentity,
                    candidateMatch.groupValues[1],
                    candidateMatch.groupValues[2].toInt(),
                )
            }.getOrDefault(false)
        } ?: return null
        val key = candidate.name.removeSuffix(".json")
        val match = requireNotNull(conversationStorageKey.matchEntire(key))
        val recordingId = match.groupValues[1]
        val segmentIndex = match.groupValues[2].toInt()
        return try {
            reconcileInternal(context, onlyStorageKey = key)
            val pending = pendingByStorageKey(context, key)
            require(pending.kind == "conversation" && pending.recordingId == recordingId &&
                pending.segmentIndex == segmentIndex)
            ConversationHandoffResult(
                ConversationHandoffIdentity(pending.callId, pending.revisionId, pending.blockId),
                recordingId,
                segmentIndex,
                true,
                "-",
            )
        } catch (_: DeferredForCall) {
            null
        } catch (error: Exception) {
            RecordingFailureJournal.capture(context, "handoff", fallbackIdentity.callId, key, error = error)
            ConversationHandoffResult(
                fallbackIdentity,
                recordingId,
                segmentIndex,
                false,
                "APP_HANDOFF_FAILED",
            )
        }
    }

    private fun reconcileInternal(context: Context, onlyStorageKey: String?) {
        if (CallRuntimeState.isBusy()) throw DeferredForCall()
        val inbox = inbox(context)
        val spool = spool(context)
        val acknowledgedPattern = Regex("^\\.(${storageKey.pattern})\\.(?:audio|meta)\\.acked\\.tmp$")
        spool.listFiles { file -> acknowledgedPattern.matches(file.name) }
            .orEmpty()
            .forEach { marker ->
                val key = requireNotNull(acknowledgedPattern.matchEntire(marker.name)).groupValues[1]
                File(spool, "$key.audio").delete()
                File(spool, "$key.meta").delete()
                File(spool, ".$key.audio.acked.tmp").delete()
                File(spool, ".$key.meta.acked.tmp").delete()
            }
        spool.listFiles { file -> file.name.startsWith(".") && file.name.endsWith(".tmp") }
            .orEmpty()
            .forEach { it.delete() }
        val spoolPattern = Regex("^(${storageKey.pattern})\\.(audio|meta)$")
        spool.listFiles { file -> !file.name.startsWith(".") }
            .orEmpty()
            .forEach { file ->
                val match = requireNotNull(spoolPattern.matchEntire(file.name)) {
                    "Encrypted recording spool contains an unknown file."
                }
                requireSafeSpool(file)
                val key = match.groupValues[1]
                val counterpart = File(
                    spool,
                    "$key.${if (match.groupValues[2] == "audio") "meta" else "audio"}",
                )
                if (!counterpart.exists()) require(file.delete())
            }
        syncDirectory(spool)
        inbox.listFiles { file ->
            file.name.endsWith(".json") &&
                (onlyStorageKey == null || file.name == "$onlyStorageKey.json")
        }
            .orEmpty()
            .sortedBy { it.name }
            .forEach { receiptFile ->
                if (CallRuntimeState.isBusy()) throw DeferredForCall()
                val key = receiptFile.name.removeSuffix(".json")
                val source = File(inbox, "$key.wav")
                val audioFinal = File(spool, "$key.audio")
                val metadataFinal = File(spool, "$key.meta")
                val audioExisted = audioFinal.exists()
                val metadataExisted = metadataFinal.exists()
                var handoffDurable = false
                var validated = false
                var callId: String? = null
                var stage = "validate"
                val started = SystemClock.elapsedRealtime()
                try {
                    require(storageKey.matches(key))
                    requireSafeInbound(receiptFile, MAXIMUM_RECEIPT_BYTES)
                    requireSafeInbound(source, MAXIMUM_RECORDING_BYTES)
                    val receipt = JSONObject(receiptFile.readText(Charsets.UTF_8))
                    val pending = parseReceipt(receipt, source)
                    callId = pending.callId
                    require(pending.storageKey() == key)
                    requireSignedRecordingStep(context, pending)
                    validated = true
                    stage = "recover_encrypted_copy"
                    val receiptBytes = receipt.toString().toByteArray(Charsets.UTF_8)
                    if (audioFinal.exists()) {
                        requireSafeSpool(audioFinal)
                        val recovered = decryptFile(audioFinal, key)
                        require(recovered.size.toLong() == pending.sizeBytes)
                        require(MessageDigest.getInstance("SHA-256").digest(recovered).toHex() == pending.sha256)
                    }
                    if (metadataFinal.exists()) {
                        requireSafeSpool(metadataFinal)
                        val recovered = JSONObject(String(decryptFile(metadataFinal, "$key:meta"), Charsets.UTF_8))
                        require(parseReceipt(recovered, audioFinal, encrypted = true) == pending)
                    }
                    val additionalBytes =
                        (if (audioFinal.exists()) 0 else source.length() + ENCRYPTION_OVERHEAD_BYTES) +
                        (if (metadataFinal.exists()) 0 else receiptBytes.size + ENCRYPTION_OVERHEAD_BYTES)
                    val maximumKindBytes = if (pending.kind == "conversation") {
                        MAXIMUM_CONVERSATION_BYTES
                    } else {
                        MAXIMUM_VOICEMAIL_BYTES
                    }
                    stage = "capacity"
                    require(usageBytes(context, pending.kind) + additionalBytes <= maximumKindBytes) {
                        "Encrypted ${pending.kind} spool is full."
                    }
                    if (pending.kind == "conversation") {
                        require(spool.usableSpace - additionalBytes >= MINIMUM_FILESYSTEM_RESERVE_BYTES) {
                            "Conversation spool would consume the filesystem reserve."
                        }
                    }

                    if (!audioFinal.exists()) {
                        stage = "encrypt_audio"
                        encryptFile(source, File(spool, ".$key.audio.tmp"), audioFinal, key)
                    }
                    if (!metadataFinal.exists()) {
                        stage = "encrypt_metadata"
                        encryptBytes(
                            receiptBytes,
                            File(spool, ".$key.meta.tmp"),
                            metadataFinal,
                            "$key:meta",
                        )
                    }
                    stage = "commit_encrypted_pair"
                    requireSafeSpool(audioFinal)
                    requireSafeSpool(metadataFinal)
                    syncDirectory(spool)
                    handoffDurable = true
                    stage = "remove_plaintext"
                    require(!source.exists() || source.delete())
                    require(!receiptFile.exists() || receiptFile.delete())
                    syncDirectory(inbox)
                    if (pending.kind == "conversation" && pending.stopReason != "segment_boundary") {
                        ExternalCallAuditStore.releaseConferenceEvidence(
                            context,
                            pending.callId,
                            pending.revisionId,
                            pending.blockId,
                        )
                    }
                } catch (error: Exception) {
                    if (error !is DeferredForCall) {
                        RecordingFailureJournal.capture(context, stage, callId, key,
                            durationMs = SystemClock.elapsedRealtime() - started,
                            sourceBytes = source.length(), freeBytes = spool.usableSpace, error = error)
                    }
                    File(spool, ".$key.audio.tmp").delete()
                    File(spool, ".$key.meta.tmp").delete()
                    if (!handoffDurable && !audioExisted) audioFinal.delete()
                    if (!handoffDurable && !metadataExisted) metadataFinal.delete()
                    // Reject untrusted PCM. Keep a validated private pair for a later retry
                    // after a transient encryption/storage failure; it remains quota bounded.
                    if (!validated || handoffDurable) {
                        source.delete()
                        receiptFile.delete()
                    }
                    syncDirectory(spool)
                    syncDirectory(inbox)
                    if (error is DeferredForCall) throw error
                    throw IllegalStateException("Recording save failed at $stage.", error)
                }
            }
        if (onlyStorageKey == null) {
            val inboxPattern = Regex("^(${storageKey.pattern})\\.(wav|json)$")
            val now = System.currentTimeMillis()
            inbox.listFiles { file -> !file.name.startsWith(".") }
                .orEmpty()
                .forEach { file ->
                    val match = requireNotNull(inboxPattern.matchEntire(file.name)) {
                        "Recording bridge contains an unknown file."
                    }
                    requireSafeInbound(
                        file,
                        if (match.groupValues[2] == "wav") MAXIMUM_RECORDING_BYTES else MAXIMUM_RECEIPT_BYTES,
                    )
                    val key = match.groupValues[1]
                    val counterpart = File(
                        inbox,
                        "$key.${if (match.groupValues[2] == "wav") "json" else "wav"}",
                    )
                    if (!counterpart.exists() && now - file.lastModified() >= ORPHAN_GRACE_MS) {
                        require(file.delete())
                    }
                }
            syncDirectory(inbox)
        }
        publishCapacity(context)
    }

    @Synchronized
    fun pending(context: Context): List<PendingRecording> {
        val directory = spool(context)
        return directory.listFiles { file -> file.name.endsWith(".meta") }
            .orEmpty()
            .sortedBy { it.name }
            .map { metadata -> pendingByStorageKey(context, metadata.name.removeSuffix(".meta")) }
    }

    @Synchronized
    fun readAudio(context: Context, recording: PendingRecording): ByteArray {
        val key = recording.storageKey()
        val path = File(spool(context), "$key.audio")
        val content = decryptFile(path, key)
        require(content.size.toLong() == recording.sizeBytes)
        require(MessageDigest.getInstance("SHA-256").digest(content).toHex() == recording.sha256)
        return content
    }

    @Synchronized
    fun acknowledge(context: Context, recording: PendingRecording, receipt: JSONObject) {
        require(receipt.getString("id") == recording.recordingId)
        if (recording.kind == "conversation") {
            require(receipt.getInt("segment_index") == recording.segmentIndex)
        }
        require(receipt.getString("source_sha256") == recording.sha256)
        require(receipt.getLong("expected_size_bytes") == recording.sizeBytes)
        require(receipt.getBoolean("acknowledged"))
        require(RecordingUploadPolicy.isAcknowledgedTerminalStatus(recording, receipt.getString("status")))
        val directory = spool(context)
        val key = recording.storageKey()
        val audio = File(directory, "$key.audio")
        val metadata = File(directory, "$key.meta")
        requireSafeSpool(audio)
        requireSafeSpool(metadata)
        val audioTrash = File(directory, ".$key.audio.acked.tmp")
        val metadataTrash = File(directory, ".$key.meta.acked.tmp")
        audioTrash.delete()
        metadataTrash.delete()
        require(audio.renameTo(audioTrash))
        if (!metadata.renameTo(metadataTrash)) {
            require(audioTrash.renameTo(audio))
            syncDirectory(directory)
            error("Could not atomically acknowledge the recording spool.")
        }
        syncDirectory(directory)
        audioTrash.delete()
        metadataTrash.delete()
        syncDirectory(directory)
        publishCapacity(context)
    }

    fun usageBytes(context: Context): Long = spool(context).listFiles()
        .orEmpty()
        .filter { it.isFile && !Files.isSymbolicLink(it.toPath()) }
        .sumOf { it.length() }

    fun voicemailUsageBytes(context: Context): Long = usageBytes(context, "voicemail")

    fun conversationUsageBytes(context: Context): Long = usageBytes(context, "conversation")

    fun voicemailCount(context: Context): Int = countByKind(context, "voicemail")

    fun conversationCount(context: Context): Int = countByKind(context, "conversation")

    private fun parseReceipt(
        receipt: JSONObject,
        audio: File,
        encrypted: Boolean = false,
    ): PendingRecording {
        val version = receipt.getInt("version").also { require(it in 1..2) }
        val recordingId = canonicalUuid(receipt.getString("recording_id"))
        val callId = canonicalUuid(receipt.getString("call_id"))
        val blockId = canonicalUuid(receipt.getString("block_id"))
        val revisionId = receipt.getLong("revision_id").also { require(it > 0) }
        val sequence = receipt.getInt("sequence").also {
            require(it in 0..if (version == 1) 63 else 65_535)
        }
        val kind = if (version == 1) "voicemail" else receipt.getString("kind").also {
            require(it == "conversation")
        }
        val segmentIndex = if (version == 1) null else receipt.getInt("segment_index").also {
            require(it == sequence)
        }
        val capturedAt = receipt.getString("captured_at").also { Instant.parse(it) }
        val durationMs = receipt.getInt("duration_ms").also { require(it in 1..180_000) }
        val stopReason = receipt.getString("stop_reason").also {
            val accepted = if (version == 1) {
                setOf("finish_key", "maximum_duration", "caller_hangup")
            } else {
                setOf("segment_boundary", "operator_hangup", "caller_hangup", "recording_failure")
            }
            require(it in accepted)
        }
        val partial = if (version == 1) false else receipt.getBoolean("partial").also {
            require(it == (stopReason == "recording_failure"))
        }
        val sizeBytes = receipt.getLong("size_bytes").also {
            require(it in 45..MAXIMUM_RECORDING_BYTES)
            require(!encrypted || audio.isFile)
            if (!encrypted) require(audio.length() == it)
        }
        val sha256 = receipt.getString("sha256").also { require(digest.matches(it)) }
        if (!encrypted) require(hash(audio) == sha256)
        return PendingRecording(
            recordingId,
            callId,
            revisionId,
            blockId,
            sequence,
            capturedAt,
            durationMs,
            stopReason,
            sizeBytes,
            sha256,
            kind,
            segmentIndex,
            partial,
        )
    }

    private fun pendingByStorageKey(context: Context, key: String): PendingRecording {
        require(storageKey.matches(key))
        val directory = spool(context)
        val metadata = File(directory, "$key.meta")
        val audio = File(directory, "$key.audio")
        requireSafeSpool(metadata)
        requireSafeSpool(audio)
        val document = JSONObject(String(decryptFile(metadata, "$key:meta"), Charsets.UTF_8))
        return parseReceipt(document, audio, encrypted = true).also { require(it.storageKey() == key) }
    }

    private fun requireSignedRecordingStep(context: Context, recording: PendingRecording) {
        val manifest = requireNotNull(SecureControlStore.activeManifest(context))
        val schemaVersion = manifest.getInt("schema_version")
        require(manifest.getLong("revision_id") == recording.revisionId)
        val instructions = manifest.getJSONObject("program").getJSONArray("instructions")
        val matching = (0 until instructions.length())
            .map { instructions.getJSONObject(it) }
            .filter {
                it.getString("op") == (if (recording.kind == "conversation") "external_call" else "record_message") &&
                    it.getString("block_id") == recording.blockId
            }
        require(matching.size == 1)
        val instruction = matching.single()
        if (recording.kind == "conversation") {
            require(schemaVersion == 4 && recording.segmentIndex == recording.sequence)
            require(recording.durationMs <= 180_000)
            val conferenceAt = requireNotNull(
                ExternalCallAuditStore.verifiedConferenceAt(
                    context,
                    recording.callId,
                    recording.revisionId,
                    recording.blockId,
                ),
            ) { "Conversation PCM has no verified conference." }
            require(ConversationCaptureBoundary.permits(conferenceAt, Instant.parse(recording.capturedAt))) {
                "Conversation PCM predates a verified conference."
            }
            return
        }
        require(schemaVersion in 3..4)
        require(recording.durationMs <= instruction.getInt("maximum_duration_ms"))
        val behavior = manifest.getJSONObject("recording_behavior")
        require(instruction.getInt("maximum_duration_ms") == behavior.getInt("maximum_duration_seconds") * 1000)
        val instructionFinish = if (instruction.isNull("finish_key")) null else instruction.getString("finish_key")
        val behaviorFinish = if (behavior.isNull("finish_key")) null else behavior.getString("finish_key")
        require(instructionFinish == behaviorFinish)
        if (recording.stopReason == "finish_key") {
            require(behaviorFinish != null)
        }
    }

    private fun canonicalUuid(value: String): String = UUID.fromString(value).toString().also {
        require(it == value && uuid.matches(value))
    }

    private fun PendingRecording.storageKey(): String = if (kind == "conversation") {
        "$recordingId.${requireNotNull(segmentIndex).toString().padStart(5, '0')}"
    } else {
        recordingId
    }

    private fun requireSafeInbound(file: File, maximumBytes: Long) {
        require(file.canonicalFile.parentFile == file.parentFile?.canonicalFile)
        require(!Files.isSymbolicLink(file.toPath()))
        val state = Os.lstat(file.absolutePath)
        require(OsConstants.S_ISREG(state.st_mode))
        require(state.st_uid == Process.myUid())
        require(state.st_mode and (OsConstants.S_IRWXG or OsConstants.S_IRWXO) == 0)
        require(state.st_size in 1..maximumBytes)
    }

    private fun requireSafeSpool(file: File) {
        require(file.canonicalFile.parentFile == file.parentFile?.canonicalFile)
        require(!Files.isSymbolicLink(file.toPath()))
        val state = Os.lstat(file.absolutePath)
        require(OsConstants.S_ISREG(state.st_mode))
        require(state.st_uid == Process.myUid())
        require(state.st_mode and (OsConstants.S_IRWXG or OsConstants.S_IRWXO) == 0)
        require(state.st_size > ENCRYPTION_OVERHEAD_BYTES)
    }

    private fun encryptFile(source: File, temporary: File, destination: File, aad: String) {
        FileOutputStream(temporary).use { raw ->
            FileInputStream(source).use { input ->
                RecordingEnvelope.encryptStream(secretKey(), aad, input, raw) {
                    if (CallRuntimeState.isBusy()) throw DeferredForCall()
                }
            }
            raw.fd.sync()
        }
        Os.chmod(temporary.absolutePath, 0b110000000)
        require(temporary.renameTo(destination))
        syncDirectory(requireNotNull(destination.parentFile))
    }

    private fun encryptBytes(value: ByteArray, temporary: File, destination: File, aad: String) {
        val encrypted = RecordingEnvelope.encrypt(secretKey(), aad, value)
        FileOutputStream(temporary).use { raw ->
            raw.write(encrypted)
            raw.fd.sync()
        }
        Os.chmod(temporary.absolutePath, 0b110000000)
        require(temporary.renameTo(destination))
        syncDirectory(requireNotNull(destination.parentFile))
    }

    private fun decryptFile(file: File, aad: String): ByteArray {
        require(file.isFile && !Files.isSymbolicLink(file.toPath()))
        return RecordingEnvelope.decrypt(secretKey(), aad, file.readBytes())
    }

    private fun secretKey(): SecretKey {
        val keyStore = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (keyStore.getKey(KEY_ALIAS, null) as? SecretKey)?.let { return it }
        return KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore").run {
            init(
                KeyGenParameterSpec.Builder(
                    KEY_ALIAS,
                    KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT,
                )
                    .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                    .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                    .setRandomizedEncryptionRequired(true)
                    .build(),
            )
            generateKey()
        }
    }

    private fun inbox(context: Context): File = privateDirectory(File(context.filesDir, "bridge/recordings"))

    private fun spool(context: Context): File = privateDirectory(File(context.filesDir, "recording-spool"))

    private fun privateDirectory(directory: File): File = directory.apply {
        require(exists() || mkdirs())
        require(!Files.isSymbolicLink(toPath()))
        val state = Os.lstat(absolutePath)
        require(OsConstants.S_ISDIR(state.st_mode))
        require(state.st_uid == Process.myUid())
        Os.chmod(absolutePath, 0b111000000)
        val secured = Os.lstat(absolutePath)
        require(secured.st_mode and (OsConstants.S_IRWXG or OsConstants.S_IRWXO) == 0)
    }

    private fun syncDirectory(directory: File) {
        val descriptor = Os.open(
            directory.absolutePath,
            OsConstants.O_RDONLY or OsConstants.O_CLOEXEC,
            0,
        )
        try {
            Os.fsync(descriptor)
        } finally {
            Os.close(descriptor)
        }
    }

    private fun publishCapacity(context: Context) {
        RootAudioTrigger.publishRecordingCapacity(
            context,
            voicemailUsageBytes(context),
            voicemailCount(context),
            conversationUsageBytes(context),
            conversationCount(context),
            spool(context).usableSpace.coerceAtLeast(0),
        )
    }

    private fun countByKind(context: Context, kind: String): Int {
        require(kind in setOf("voicemail", "conversation"))
        val pattern = if (kind == "conversation") {
            Regex("^${uuid.pattern}\\.[0-9]{5}\\.meta$")
        } else {
            Regex("^${uuid.pattern}\\.meta$")
        }
        return spool(context).listFiles().orEmpty().count {
            it.isFile && !Files.isSymbolicLink(it.toPath()) && pattern.matches(it.name)
        }
    }

    private fun usageBytes(context: Context, kind: String): Long {
        require(kind in setOf("voicemail", "conversation"))
        val conversation = Regex("^${uuid.pattern}\\.[0-9]{5}\\.(?:audio|meta)$")
        return spool(context).listFiles()
            .orEmpty()
            .filter { file ->
                file.isFile && !Files.isSymbolicLink(file.toPath()) &&
                    (conversation.matches(file.name) == (kind == "conversation")) &&
                    (conversation.matches(file.name) || Regex("^${uuid.pattern}\\.(?:audio|meta)$").matches(file.name))
            }
            .sumOf(File::length)
    }

    private fun hash(file: File): String {
        val digest = MessageDigest.getInstance("SHA-256")
        FileInputStream(file).use { input ->
            val buffer = ByteArray(64 * 1024)
            while (true) {
                val count = input.read(buffer)
                if (count < 0) break
                digest.update(buffer, 0, count)
            }
        }
        return digest.digest().toHex()
    }

    private const val ORPHAN_GRACE_MS = 30_000L
}
