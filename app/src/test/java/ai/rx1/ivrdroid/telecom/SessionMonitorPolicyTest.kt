package ai.rx1.ivrdroid.telecom

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class SessionMonitorPolicyTest {
    @Test
    fun healthyConferenceBeyondTwentyMinutesRemainsBusyUntilHelperTerminal() {
        CallRuntimeState.setBusy(true)
        try {
            repeat(2_500) {
                assertFalse(SessionMonitorPolicy.isTerminal("RUNNING"))
                assertTrue(CallRuntimeState.isBusy())
            }
            assertTrue(SessionMonitorPolicy.isTerminal("READY"))
        } finally {
            CallRuntimeState.setBusy(false)
        }
    }
}
