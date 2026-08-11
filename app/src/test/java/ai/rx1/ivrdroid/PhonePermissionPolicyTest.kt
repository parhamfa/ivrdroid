package ai.rx1.ivrdroid

import android.Manifest
import org.junit.Assert.assertTrue
import org.junit.Test

class PhonePermissionPolicyTest {
    @Test
    fun externalCallPermissionIsRequestedAtRuntime() {
        assertTrue(Manifest.permission.CALL_PHONE in PhonePermissionPolicy.required)
    }
}
