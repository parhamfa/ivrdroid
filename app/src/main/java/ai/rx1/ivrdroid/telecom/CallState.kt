package ai.rx1.ivrdroid.telecom

import android.os.Build
import android.telecom.Call

val Call.currentState: Int
    get() = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
        details.state
    } else {
        @Suppress("DEPRECATION")
        state
    }

