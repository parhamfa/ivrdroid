package ai.rx1.ivrdroid.control

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import org.json.JSONArray
import org.json.JSONObject
import java.security.KeyStore
import java.time.Instant
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

object SecureControlStore {
    private const val PREFERENCES = "ivrdroid_secure_control"
    private const val KEY_ALIAS = "ivrdroid-control-v1"
    private const val KEY_ENROLLMENT = "enrollment"
    private const val KEY_ACTIVE_MANIFEST = "active_manifest"
    private const val KEY_EVENTS = "pending_events"
    private const val KEY_LAST_SYNC = "last_sync"
    private const val KEY_LAST_ERROR = "last_error"

    @Synchronized
    fun enrollment(context: Context): Enrollment? {
        val document = decrypt(context, KEY_ENROLLMENT)?.let(::JSONObject) ?: return null
        return runCatching {
            Enrollment(
                deviceId = document.getString("device_id"),
                deviceToken = document.getString("device_token"),
                serviceClientId = document.getString("service_client_id"),
                serviceClientSecret = document.getString("service_client_secret"),
                serverUrl = document.getString("server_url"),
            )
        }.getOrNull()
    }

    @Synchronized
    fun saveEnrollment(context: Context, enrollment: Enrollment) {
        val document = JSONObject()
            .put("device_id", enrollment.deviceId)
            .put("device_token", enrollment.deviceToken)
            .put("service_client_id", enrollment.serviceClientId)
            .put("service_client_secret", enrollment.serviceClientSecret)
            .put("server_url", enrollment.serverUrl)
        encrypt(context, KEY_ENROLLMENT, document.toString())
    }

    @Synchronized
    fun clearEnrollment(context: Context) {
        preferences(context).edit()
            .remove(KEY_ENROLLMENT)
            .remove(KEY_ACTIVE_MANIFEST)
            .remove(KEY_EVENTS)
            .remove(KEY_LAST_SYNC)
            .remove(KEY_LAST_ERROR)
            .apply()
    }

    @Synchronized
    fun activeManifest(context: Context): JSONObject? =
        decrypt(context, KEY_ACTIVE_MANIFEST)?.let { runCatching { JSONObject(it) }.getOrNull() }

    @Synchronized
    fun saveActiveManifest(context: Context, manifest: JSONObject) {
        encrypt(context, KEY_ACTIVE_MANIFEST, CanonicalJson.encode(manifest))
    }

    @Synchronized
    fun enqueueCall(context: Context, event: PendingCallEvent) {
        val events = readEvents(context)
        val existing = events.indexOfFirst { it.callId == event.callId }
        if (existing >= 0) {
            events[existing] = CallEventPayload.preserveEvents(events[existing], event)
        } else events.add(event)
        writeEvents(context, events)
    }

    @Synchronized
    fun appendCallEvent(context: Context, callId: String, event: PendingCallSubEvent) {
        val calls = readEvents(context)
        val index = calls.indexOfFirst { it.callId == callId }
        if (index < 0) return
        val bounded = (calls[index].events + event).distinct().takeLast(128)
        calls[index] = calls[index].copy(events = bounded)
        writeEvents(context, calls)
    }

    @Synchronized
    fun pendingCalls(context: Context): List<PendingCallEvent> = readEvents(context)

    @Synchronized
    fun acknowledgeCalls(context: Context, accepted: Set<String>) {
        if (accepted.isEmpty()) return
        writeEvents(context, readEvents(context).filterNot { accepted.contains(it.callId) })
    }

    @Synchronized
    fun updateSyncStatus(context: Context, error: String?) {
        preferences(context).edit()
            .putLong(KEY_LAST_SYNC, if (error == null) System.currentTimeMillis() else lastSyncEpochMs(context))
            .apply()
        if (error == null) {
            preferences(context).edit().remove(KEY_LAST_ERROR).apply()
        } else {
            encrypt(context, KEY_LAST_ERROR, error.take(500))
        }
    }

    fun lastSyncEpochMs(context: Context): Long =
        preferences(context).getLong(KEY_LAST_SYNC, 0L)

    fun lastError(context: Context): String? = decrypt(context, KEY_LAST_ERROR)

    private fun readEvents(context: Context): MutableList<PendingCallEvent> {
        val array = decrypt(context, KEY_EVENTS)
            ?.let { runCatching { JSONArray(it) }.getOrNull() }
            ?: JSONArray()
        return buildList {
            for (index in 0 until array.length()) {
                val item = array.optJSONObject(index) ?: continue
                runCatching {
                    add(
                        PendingCallEvent(
                            callId = item.getString("call_id"),
                            startedAt = item.getString("started_at"),
                            caller = item.optString("caller").takeIf { it.isNotEmpty() },
                            policyDecision = item.getString("policy_decision"),
                            revisionId = item.optLong("revision_id", 0).takeIf { it > 0 },
                            menuPath = item.optJSONArray("menu_path")?.let { path ->
                                buildList { for (position in 0 until path.length()) add(path.getString(position)) }
                            } ?: emptyList(),
                            result = item.getString("result"),
                            durationSeconds = item.optInt("duration_seconds", 0),
                            events = CallEventPayload.decodeEvents(item.optJSONArray("events")),
                            sessionAudit = item.optJSONObject("session_audit"),
                            auditPolicyVersion = item.optLong("_audit_policy_version", 0).takeIf { it > 0 },
                            auditQuotaBytes = item.optLong("_audit_quota_bytes", 0).takeIf { it > 0 },
                        ),
                    )
                }
            }
        }.toMutableList()
    }

    private fun writeEvents(context: Context, events: List<PendingCallEvent>) {
        val array = JSONArray()
        events.forEach { event ->
            array.put(CallEventPayload.encode(event).put("caller", event.caller ?: "")
                .put("_audit_policy_version", event.auditPolicyVersion ?: JSONObject.NULL)
                .put("_audit_quota_bytes", event.auditQuotaBytes ?: JSONObject.NULL))
        }
        encrypt(context, KEY_EVENTS, array.toString())
    }

    private fun preferences(context: Context) =
        context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE)

    private fun secretKey(): SecretKey {
        val keyStore = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (keyStore.getKey(KEY_ALIAS, null) as? SecretKey)?.let { return it }
        val generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore")
        generator.init(
            KeyGenParameterSpec.Builder(
                KEY_ALIAS,
                KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT,
            )
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setRandomizedEncryptionRequired(true)
                .build(),
        )
        return generator.generateKey()
    }

    private fun encrypt(context: Context, key: String, plaintext: String) {
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, secretKey())
        cipher.updateAAD(key.toByteArray(Charsets.UTF_8))
        val encrypted = cipher.doFinal(plaintext.toByteArray(Charsets.UTF_8))
        val payload = cipher.iv + encrypted
        preferences(context).edit()
            .putString(key, Base64.encodeToString(payload, Base64.NO_WRAP))
            .apply()
    }

    private fun decrypt(context: Context, key: String): String? {
        val encoded = preferences(context).getString(key, null) ?: return null
        return runCatching {
            val payload = Base64.decode(encoded, Base64.NO_WRAP)
            require(payload.size >= 12 + 16)
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(Cipher.DECRYPT_MODE, secretKey(), GCMParameterSpec(128, payload.copyOfRange(0, 12)))
            cipher.updateAAD(key.toByteArray(Charsets.UTF_8))
            String(cipher.doFinal(payload.copyOfRange(12, payload.size)), Charsets.UTF_8)
        }.getOrElse {
            preferences(context).edit().remove(key).apply()
            null
        }
    }
}
