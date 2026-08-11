package ai.rx1.ivrdroid

import android.content.Context

object IvrPreferences {
    private const val FILE_NAME = "ivrdroid"
    private const val KEY_LOCAL_IVR_ENABLED = "local_ivr_enabled"

    fun isLocalIvrEnabled(context: Context): Boolean =
        context.getSharedPreferences(FILE_NAME, Context.MODE_PRIVATE)
            .getBoolean(KEY_LOCAL_IVR_ENABLED, true)

    fun setLocalIvrEnabled(context: Context, enabled: Boolean) {
        context.getSharedPreferences(FILE_NAME, Context.MODE_PRIVATE)
            .edit()
            .putBoolean(KEY_LOCAL_IVR_ENABLED, enabled)
            .apply()
    }
}
