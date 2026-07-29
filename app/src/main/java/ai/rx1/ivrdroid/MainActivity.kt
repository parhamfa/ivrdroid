package ai.rx1.ivrdroid

import android.Manifest
import android.app.Activity
import android.app.role.RoleManager
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.telecom.TelecomManager
import android.view.Gravity
import android.view.ViewGroup
import android.widget.Button
import android.widget.LinearLayout
import android.widget.Space
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast
import ai.rx1.ivrdroid.audio.RootAudioTrigger
import ai.rx1.ivrdroid.telecom.TestCallGate

class MainActivity : Activity() {
    private lateinit var roleStatus: TextView
    private lateinit var roleButton: Button
    private lateinit var permissionStatus: TextView
    private lateinit var autoAnswerSwitch: Switch
    private lateinit var audioBridgeStatus: TextView
    private val statusHandler = Handler(Looper.getMainLooper())
    private val statusPoll = object : Runnable {
        override fun run() {
            refreshAudioBridgeStatus()
            statusHandler.postDelayed(this, STATUS_POLL_INTERVAL_MS)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        RootAudioTrigger.initialize(this)
        setContentView(buildContent())
    }

    override fun onResume() {
        super.onResume()
        refreshState()
        statusHandler.post(statusPoll)
    }

    override fun onPause() {
        statusHandler.removeCallbacks(statusPoll)
        super.onPause()
    }

    @Deprecated("RoleManager reports its user decision through this callback on API 29–32.")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode == REQUEST_SCREENING_ROLE) refreshState()
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray,
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode != REQUEST_TEST_PERMISSIONS) return

        refreshState()
        if (hasRequiredPermissions()) {
            requestScreeningRole()
        } else {
            Toast.makeText(this, R.string.permissions_missing, Toast.LENGTH_LONG).show()
        }
    }

    private fun buildContent(): LinearLayout {
        val horizontalPadding = dp(24)
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_HORIZONTAL
            setPadding(horizontalPadding, dp(36), horizontalPadding, dp(24))
            setBackgroundColor(Color.rgb(246, 247, 250))
        }

        root.addView(TextView(this).apply {
            text = getString(R.string.app_name)
            textSize = 34f
            setTextColor(Color.rgb(16, 18, 23))
        }, matchWrap())

        root.addView(TextView(this).apply {
            text = getString(R.string.app_summary)
            textSize = 16f
            gravity = Gravity.CENTER
            setTextColor(Color.DKGRAY)
            setPadding(0, dp(6), 0, dp(28))
        }, matchWrap())

        roleStatus = TextView(this).apply {
            text = getString(R.string.role_status_unknown)
            textSize = 17f
            setTextColor(Color.rgb(16, 18, 23))
        }
        root.addView(roleStatus, matchWrap())

        permissionStatus = TextView(this).apply {
            setTextColor(Color.DKGRAY)
        }
        root.addView(permissionStatus, matchWrap(topMargin = 6))

        roleButton = Button(this).apply {
            text = getString(R.string.request_phone_role)
            setOnClickListener { enableGatedTesting() }
        }
        root.addView(roleButton, matchWrap(topMargin = 12))

        root.addView(TextView(this).apply {
            text = getString(R.string.allowlisted_caller)
            textSize = 14f
            setTextColor(Color.DKGRAY)
        }, matchWrap(topMargin = 30))

        root.addView(TextView(this).apply {
            text = TestCallGate.allowlistedCallerE164
                ?: getString(R.string.allowlist_not_configured)
            textSize = 22f
            setTextColor(Color.rgb(16, 18, 23))
        }, matchWrap(topMargin = 4))

        autoAnswerSwitch = Switch(this).apply {
            text = getString(R.string.auto_answer)
            isChecked = IvrPreferences.isAutoAnswerEnabled(this@MainActivity)
            setOnCheckedChangeListener { _, enabled ->
                IvrPreferences.setAutoAnswerEnabled(this@MainActivity, enabled)
            }
        }
        root.addView(autoAnswerSwitch, matchWrap(topMargin = 26))

        root.addView(TextView(this).apply {
            text = getString(R.string.auto_answer_summary)
            setTextColor(Color.DKGRAY)
        }, matchWrap(topMargin = 6))

        val telecom = getSystemService(Context.TELECOM_SERVICE) as TelecomManager
        root.addView(TextView(this).apply {
            text = getString(
                R.string.normal_dialer,
                telecom.defaultDialerPackage ?: getString(R.string.unknown_number),
            )
            setTextColor(Color.rgb(35, 105, 63))
        }, matchWrap(topMargin = 18))

        root.addView(Space(this), LinearLayout.LayoutParams(1, 0, 1f))

        audioBridgeStatus = TextView(this).apply {
            setTextColor(Color.rgb(166, 49, 49))
        }
        root.addView(audioBridgeStatus, matchWrap(topMargin = 12))

        return root
    }

    private fun refreshState() {
        val roleManager = getSystemService(RoleManager::class.java)
        val roleAvailable =
            roleManager?.isRoleAvailable(RoleManager.ROLE_CALL_SCREENING) == true
        val roleHeld =
            roleAvailable && roleManager.isRoleHeld(RoleManager.ROLE_CALL_SCREENING)
        val permissionsReady = hasRequiredPermissions()
        val allowlistReady = TestCallGate.allowlistedCallerE164 != null

        roleStatus.setText(
            when {
                roleHeld -> R.string.role_status_held
                roleAvailable -> R.string.role_status_missing
                else -> R.string.role_not_available
            },
        )
        permissionStatus.setText(
            if (permissionsReady) R.string.permissions_ready else R.string.permissions_missing,
        )
        roleButton.isEnabled = roleAvailable && (!roleHeld || !permissionsReady)
        autoAnswerSwitch.isEnabled = roleHeld && permissionsReady && allowlistReady
        refreshAudioBridgeStatus()
    }

    private fun refreshAudioBridgeStatus() {
        val state = RootAudioTrigger.readState(this)
        audioBridgeStatus.text = getString(
            R.string.audio_bridge_status,
            state.current,
            state.lastResult,
        )
        audioBridgeStatus.setTextColor(
            if (state.isIdle) {
                Color.rgb(35, 105, 63)
            } else {
                Color.rgb(166, 49, 49)
            },
        )
    }

    private fun enableGatedTesting() {
        if (!hasRequiredPermissions()) {
            requestPermissions(REQUIRED_PERMISSIONS, REQUEST_TEST_PERMISSIONS)
            return
        }
        requestScreeningRole()
    }

    @Suppress("DEPRECATION")
    private fun requestScreeningRole() {
        val roleManager = getSystemService(RoleManager::class.java)
        if (roleManager?.isRoleAvailable(RoleManager.ROLE_CALL_SCREENING) != true) {
            Toast.makeText(this, R.string.role_not_available, Toast.LENGTH_LONG).show()
            return
        }
        if (roleManager.isRoleHeld(RoleManager.ROLE_CALL_SCREENING)) {
            refreshState()
            return
        }
        startActivityForResult(
            roleManager.createRequestRoleIntent(RoleManager.ROLE_CALL_SCREENING),
            REQUEST_SCREENING_ROLE,
        )
    }

    private fun hasRequiredPermissions(): Boolean =
        REQUIRED_PERMISSIONS.all {
            checkSelfPermission(it) == PackageManager.PERMISSION_GRANTED
        }

    private fun matchWrap(topMargin: Int = 0) = LinearLayout.LayoutParams(
        ViewGroup.LayoutParams.MATCH_PARENT,
        ViewGroup.LayoutParams.WRAP_CONTENT,
    ).apply {
        this.topMargin = dp(topMargin)
    }

    private fun dp(value: Int): Int =
        (value * resources.displayMetrics.density).toInt()

    private companion object {
        const val REQUEST_SCREENING_ROLE = 1001
        const val REQUEST_TEST_PERMISSIONS = 1002
        const val STATUS_POLL_INTERVAL_MS = 1_000L
        val REQUIRED_PERMISSIONS = arrayOf(
            Manifest.permission.ANSWER_PHONE_CALLS,
            Manifest.permission.READ_CONTACTS,
        )
    }
}
