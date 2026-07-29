package ai.rx1.ivrdroid

import android.content.Context

object IvrPreferences {
    private const val FILE_NAME = "ivrdroid"
    private const val KEY_AUTO_ANSWER = "auto_answer"

    fun isAutoAnswerEnabled(context: Context): Boolean =
        context.getSharedPreferences(FILE_NAME, Context.MODE_PRIVATE)
            .getBoolean(KEY_AUTO_ANSWER, true)

    fun setAutoAnswerEnabled(context: Context, enabled: Boolean) {
        context.getSharedPreferences(FILE_NAME, Context.MODE_PRIVATE)
            .edit()
            .putBoolean(KEY_AUTO_ANSWER, enabled)
            .apply()
    }
}
