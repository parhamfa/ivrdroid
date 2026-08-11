package ai.rx1.ivrdroid.telecom

import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.CopyOnWriteArraySet

object CallRuntimeState {
    private class Interruption(private val action: () -> Unit) {
        private val fired = AtomicBoolean(false)
        fun fire() {
            if (fired.compareAndSet(false, true)) runCatching(action)
        }
    }

    private val busy = AtomicBoolean(false)
    private val interruptions = CopyOnWriteArraySet<Interruption>()

    fun isBusy(): Boolean = busy.get()

    fun setBusy(value: Boolean) {
        val changed = busy.getAndSet(value) != value
        if (value && changed) interruptions.forEach(Interruption::fire)
    }

    fun interruptWhenBusy(interrupt: () -> Unit): AutoCloseable {
        val registration = Interruption(interrupt)
        interruptions.add(registration)
        if (busy.get()) registration.fire()
        return AutoCloseable { interruptions.remove(registration) }
    }
}
