package ai.rx1.ivrdroid.telecom

import android.app.Activity
import android.graphics.Color
import android.os.Bundle
import android.telecom.Call
import android.telecom.VideoProfile
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import ai.rx1.ivrdroid.R

class CallActivity : Activity() {
    private lateinit var stateText: TextView
    private lateinit var numberText: TextView
    private lateinit var answerButton: Button
    private lateinit var endButton: Button

    private val listener = CallRegistry.Listener { renderCall(CallRegistry.primaryCall()) }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setShowWhenLocked(true)
        setTurnScreenOn(true)
        setContentView(buildContent())
    }

    override fun onStart() {
        super.onStart()
        CallRegistry.addListener(listener)
    }

    override fun onStop() {
        CallRegistry.removeListener(listener)
        super.onStop()
    }

    private fun buildContent(): LinearLayout {
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            setPadding(dp(32), dp(48), dp(32), dp(48))
            setBackgroundColor(Color.rgb(16, 18, 23))
        }

        stateText = TextView(this).apply {
            textSize = 22f
            gravity = Gravity.CENTER
            setTextColor(Color.LTGRAY)
        }
        root.addView(stateText, matchWrap())

        numberText = TextView(this).apply {
            textSize = 32f
            gravity = Gravity.CENTER
            setTextColor(Color.WHITE)
        }
        root.addView(numberText, matchWrap(topMargin = 18))

        answerButton = Button(this).apply {
            text = getString(R.string.answer_call)
            setOnClickListener {
                CallRegistry.primaryCall()?.takeIf { it.currentState == Call.STATE_RINGING }
                    ?.answer(VideoProfile.STATE_AUDIO_ONLY)
            }
        }
        root.addView(answerButton, matchWrap(topMargin = 48))

        endButton = Button(this).apply {
            text = getString(R.string.end_call)
            setOnClickListener { CallRegistry.primaryCall()?.disconnect() }
        }
        root.addView(endButton, matchWrap(topMargin = 12))
        return root
    }

    private fun renderCall(call: Call?) {
        if (call == null) {
            stateText.setText(R.string.no_active_call)
            numberText.text = ""
            answerButton.visibility = View.GONE
            endButton.visibility = View.GONE
            return
        }

        stateText.setText(
            when (call.currentState) {
                Call.STATE_RINGING -> R.string.incoming_call
                Call.STATE_DIALING, Call.STATE_CONNECTING -> R.string.outgoing_call
                Call.STATE_ACTIVE -> R.string.active_call
                Call.STATE_HOLDING -> R.string.held_call
                Call.STATE_DISCONNECTED, Call.STATE_DISCONNECTING -> R.string.call_ended
                else -> R.string.active_call
            },
        )
        numberText.text = call.details.handle?.schemeSpecificPart
            ?.takeIf(String::isNotBlank)
            ?: getString(R.string.unknown_number)
        answerButton.visibility =
            if (call.currentState == Call.STATE_RINGING) View.VISIBLE else View.GONE
        endButton.visibility = View.VISIBLE
    }

    private fun matchWrap(topMargin: Int = 0) = LinearLayout.LayoutParams(
        ViewGroup.LayoutParams.MATCH_PARENT,
        ViewGroup.LayoutParams.WRAP_CONTENT,
    ).apply {
        this.topMargin = dp(topMargin)
    }

    private fun dp(value: Int): Int =
        (value * resources.displayMetrics.density).toInt()
}
