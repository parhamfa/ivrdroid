package ai.rx1.ivrdroid.telecom

import java.util.concurrent.atomic.AtomicInteger
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Test

class CallRuntimeStateTest {
    @After
    fun reset() {
        CallRuntimeState.setBusy(false)
    }

    @Test
    fun busyTransitionInterruptsRegisteredUploadOnce() {
        CallRuntimeState.setBusy(false)
        val interruptions = AtomicInteger()
        val registration = CallRuntimeState.interruptWhenBusy { interruptions.incrementAndGet() }

        CallRuntimeState.setBusy(true)
        CallRuntimeState.setBusy(true)
        assertEquals(1, interruptions.get())

        registration.close()
        CallRuntimeState.setBusy(false)
        CallRuntimeState.setBusy(true)
        assertEquals(1, interruptions.get())
    }

    @Test
    fun alreadyBusyRegistrationIsInterruptedImmediately() {
        CallRuntimeState.setBusy(true)
        val interruptions = AtomicInteger()

        CallRuntimeState.interruptWhenBusy { interruptions.incrementAndGet() }.close()

        assertEquals(1, interruptions.get())
    }
}
