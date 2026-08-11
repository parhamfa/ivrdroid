package ai.rx1.ivrdroid.telecom.external

import org.junit.Assert.assertTrue
import org.junit.Test
import java.lang.reflect.Modifier

class ExternalCallCoordinatorPolicyTest {
    @Test
    fun telecomTicksAndBackgroundHandoffCallbacksShareOneMonitor() {
        listOf("tick", "conversationHandoffSession", "recordingHandoffFailed").forEach { name ->
            val method = ExternalCallCoordinator::class.java.declaredMethods.single { it.name == name }
            assertTrue("$name must remain synchronized", Modifier.isSynchronized(method.modifiers))
        }
    }
}
