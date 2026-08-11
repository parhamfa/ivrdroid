package ai.rx1.ivrdroid

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.app.role.RoleManager
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.telecom.TelecomManager
import android.text.InputType
import android.view.Gravity
import android.view.ViewGroup
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast
import ai.rx1.ivrdroid.audio.RootAudioTrigger
import ai.rx1.ivrdroid.control.RecoveryScheduler
import ai.rx1.ivrdroid.control.SecureControlStore
import ai.rx1.ivrdroid.control.SyncCoordinator
import ai.rx1.ivrdroid.control.SyncService
import java.text.DateFormat
import java.util.Date
import java.util.concurrent.Executors

internal object PhonePermissionPolicy {
    val required = listOf(
        Manifest.permission.ANSWER_PHONE_CALLS,
        Manifest.permission.CALL_PHONE,
        Manifest.permission.READ_CONTACTS,
    )
}

class MainActivity : Activity() {
    private lateinit var roleStatus: TextView
    private lateinit var roleButton: Button
    private lateinit var permissionStatus: TextView
    private lateinit var localIvrSwitch: Switch
    private lateinit var enrollmentStatus: TextView
    private lateinit var pairingCode: EditText
    private lateinit var pairButton: Button
    private lateinit var forgetButton: Button
    private lateinit var revisionStatus: TextView
    private lateinit var syncStatus: TextView
    private lateinit var syncButton: Button
    private lateinit var audioBridgeStatus: TextView
    private val background = Executors.newSingleThreadExecutor()
    private val statusHandler = Handler(Looper.getMainLooper())
    private val statusPoll = object : Runnable {
        override fun run() {
            refreshState()
            statusHandler.postDelayed(this, STATUS_POLL_INTERVAL_MS)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        RootAudioTrigger.initialize(this)
        RecoveryScheduler.ensure(this)
        setContentView(buildContent())
    }

    override fun onResume() {
        super.onResume()
        refreshState()
        if (SecureControlStore.enrollment(this) != null) SyncService.start(this, false)
        statusHandler.post(statusPoll)
    }

    override fun onPause() {
        statusHandler.removeCallbacks(statusPoll)
        super.onPause()
    }

    override fun onDestroy() {
        background.shutdownNow()
        super.onDestroy()
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
        if (requestCode != REQUEST_PERMISSIONS) return
        refreshState()
        if (hasRequiredPhonePermissions()) requestScreeningRole()
        else Toast.makeText(this, R.string.permissions_missing, Toast.LENGTH_LONG).show()
    }

    private fun buildContent(): ScrollView {
        val horizontalPadding = dp(24)
        val content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_HORIZONTAL
            setPadding(horizontalPadding, dp(30), horizontalPadding, dp(32))
            setBackgroundColor(Color.rgb(246, 248, 249))
        }
        content.addView(TextView(this).apply {
            text = getString(R.string.app_name)
            textSize = 32f
            setTextColor(Color.rgb(16, 36, 61))
        }, matchWrap())
        content.addView(TextView(this).apply {
            text = getString(R.string.app_summary)
            textSize = 15f
            gravity = Gravity.CENTER
            setTextColor(Color.DKGRAY)
            setPadding(0, dp(5), 0, dp(24))
        }, matchWrap())

        content.addView(sectionTitle(getString(R.string.tablet_safety)))
        roleStatus = statusText()
        content.addView(roleStatus, matchWrap())
        permissionStatus = secondaryText()
        content.addView(permissionStatus, matchWrap(5))
        roleButton = Button(this).apply {
            text = getString(R.string.request_phone_role)
            setOnClickListener { enableCallScreening() }
        }
        content.addView(roleButton, matchWrap(10))
        localIvrSwitch = Switch(this).apply {
            text = getString(R.string.local_ivr_switch)
            isChecked = IvrPreferences.isLocalIvrEnabled(this@MainActivity)
            setOnCheckedChangeListener { _, enabled ->
                IvrPreferences.setLocalIvrEnabled(this@MainActivity, enabled)
                refreshState()
            }
        }
        content.addView(localIvrSwitch, matchWrap(18))
        content.addView(TextView(this).apply {
            text = getString(R.string.local_ivr_summary)
            setTextColor(Color.rgb(153, 46, 52))
        }, matchWrap(4))

        content.addView(sectionTitle(getString(R.string.dashboard_connection), 27))
        enrollmentStatus = statusText()
        content.addView(enrollmentStatus, matchWrap())
        pairingCode = EditText(this).apply {
            hint = getString(R.string.pairing_code_hint)
            inputType = InputType.TYPE_CLASS_NUMBER
            maxLines = 1
        }
        content.addView(pairingCode, matchWrap(9))
        pairButton = Button(this).apply {
            text = getString(R.string.pair_tablet)
            setOnClickListener { pairTablet() }
        }
        content.addView(pairButton, matchWrap(8))
        forgetButton = Button(this).apply {
            text = getString(R.string.forget_dashboard)
            setOnClickListener { confirmForgetDashboard() }
        }
        content.addView(forgetButton, matchWrap(8))
        syncButton = Button(this).apply {
            text = getString(R.string.sync_now)
            setOnClickListener {
                SyncService.start(this@MainActivity, true)
                Toast.makeText(this@MainActivity, R.string.sync_requested, Toast.LENGTH_SHORT).show()
            }
        }
        content.addView(syncButton, matchWrap(8))
        revisionStatus = secondaryText()
        content.addView(revisionStatus, matchWrap(12))
        syncStatus = secondaryText()
        content.addView(syncStatus, matchWrap(5))

        content.addView(sectionTitle(getString(R.string.runtime_status), 27))
        audioBridgeStatus = statusText()
        content.addView(audioBridgeStatus, matchWrap())
        val telecom = getSystemService(Context.TELECOM_SERVICE) as TelecomManager
        content.addView(TextView(this).apply {
            text = getString(
                R.string.normal_dialer,
                telecom.defaultDialerPackage ?: getString(R.string.unknown_number),
            )
            setTextColor(Color.rgb(35, 105, 63))
        }, matchWrap(13))
        content.addView(TextView(this).apply {
            text = getString(R.string.dashboard_owned_lists)
            setTextColor(Color.DKGRAY)
        }, matchWrap(8))

        return ScrollView(this).apply { addView(content) }
    }

    private fun refreshState() {
        val roleManager = getSystemService(RoleManager::class.java)
        val roleAvailable = roleManager?.isRoleAvailable(RoleManager.ROLE_CALL_SCREENING) == true
        val roleHeld = roleAvailable && roleManager.isRoleHeld(RoleManager.ROLE_CALL_SCREENING)
        val permissionsReady = hasRequiredPhonePermissions()
        roleStatus.setText(when {
            roleHeld -> R.string.role_status_held
            roleAvailable -> R.string.role_status_missing
            else -> R.string.role_not_available
        })
        permissionStatus.setText(if (permissionsReady) R.string.permissions_ready else R.string.permissions_missing)
        roleButton.isEnabled = roleAvailable && (!roleHeld || !permissionsReady)

        val enrollment = SecureControlStore.enrollment(this)
        enrollmentStatus.text = if (enrollment == null) {
            getString(R.string.not_enrolled)
        } else {
            getString(R.string.enrolled_as, enrollment.deviceId.take(8))
        }
        pairingCode.isEnabled = enrollment == null
        pairButton.isEnabled = enrollment == null
        forgetButton.isEnabled = enrollment != null
        syncButton.isEnabled = enrollment != null

        val status = SyncCoordinator.status(this)
        revisionStatus.text = getString(
            R.string.revision_status,
            status.activeRevision?.toString() ?: getString(R.string.built_in_revision),
            status.stagedRevision?.toString() ?: getString(R.string.none),
        )
        val lastSync = if (status.lastSyncEpochMs > 0) {
            DateFormat.getDateTimeInstance().format(Date(status.lastSyncEpochMs))
        } else getString(R.string.never)
        syncStatus.text = if (status.lastError == null) {
            getString(R.string.last_sync, lastSync)
        } else {
            getString(R.string.last_sync_error, lastSync, status.lastError)
        }
        val helper = RootAudioTrigger.readState(this)
        audioBridgeStatus.text = getString(R.string.audio_bridge_status, helper.current, helper.lastResult)
        audioBridgeStatus.setTextColor(if (helper.isIdle) Color.rgb(35, 105, 63) else Color.rgb(166, 49, 49))
    }

    private fun pairTablet() {
        val code = pairingCode.text.toString().trim()
        if (!code.matches(Regex("[0-9]{8}"))) {
            pairingCode.error = getString(R.string.pairing_code_invalid)
            return
        }
        pairButton.isEnabled = false
        background.execute {
            val result = runCatching { SyncCoordinator.enroll(this, code) }
            runOnUiThread {
                result.onSuccess {
                    pairingCode.setText("")
                    Toast.makeText(this, R.string.pairing_complete, Toast.LENGTH_LONG).show()
                }.onFailure {
                    Toast.makeText(this, it.message ?: getString(R.string.pairing_failed), Toast.LENGTH_LONG).show()
                }
                refreshState()
            }
        }
    }

    private fun confirmForgetDashboard() {
        AlertDialog.Builder(this)
            .setTitle(R.string.forget_dashboard)
            .setMessage(R.string.forget_dashboard_confirm)
            .setNegativeButton(android.R.string.cancel, null)
            .setPositiveButton(R.string.forget_dashboard_action) { _, _ ->
                stopService(Intent(this, SyncService::class.java))
                RecoveryScheduler.cancel(this)
                SecureControlStore.clearEnrollment(this)
                refreshState()
                Toast.makeText(this, R.string.dashboard_forgotten, Toast.LENGTH_LONG).show()
            }
            .show()
    }

    private fun enableCallScreening() {
        val missing = requestedPermissions().filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }
        if (missing.isNotEmpty()) {
            requestPermissions(missing.toTypedArray(), REQUEST_PERMISSIONS)
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
        startActivityForResult(roleManager.createRequestRoleIntent(RoleManager.ROLE_CALL_SCREENING), REQUEST_SCREENING_ROLE)
    }

    private fun hasRequiredPhonePermissions(): Boolean =
        PhonePermissionPolicy.required.all { checkSelfPermission(it) == PackageManager.PERMISSION_GRANTED }

    private fun requestedPermissions(): List<String> = buildList {
        addAll(PhonePermissionPolicy.required)
        if (Build.VERSION.SDK_INT >= 33) add(Manifest.permission.POST_NOTIFICATIONS)
    }

    private fun sectionTitle(value: String, topMargin: Int = 0) = TextView(this).apply {
        text = value
        textSize = 13f
        setTextColor(Color.rgb(20, 125, 123))
        setPadding(0, dp(topMargin), 0, dp(8))
    }

    private fun statusText() = TextView(this).apply {
        textSize = 17f
        setTextColor(Color.rgb(16, 36, 61))
    }

    private fun secondaryText() = TextView(this).apply { setTextColor(Color.DKGRAY) }

    private fun matchWrap(topMargin: Int = 0) = LinearLayout.LayoutParams(
        ViewGroup.LayoutParams.MATCH_PARENT,
        ViewGroup.LayoutParams.WRAP_CONTENT,
    ).apply { this.topMargin = dp(topMargin) }

    private fun dp(value: Int): Int = (value * resources.displayMetrics.density).toInt()

    private companion object {
        const val REQUEST_SCREENING_ROLE = 1001
        const val REQUEST_PERMISSIONS = 1002
        const val STATUS_POLL_INTERVAL_MS = 1_000L
    }
}
