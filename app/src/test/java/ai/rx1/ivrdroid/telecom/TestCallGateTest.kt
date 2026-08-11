package ai.rx1.ivrdroid.telecom

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class TestCallGateTest {
    @Test
    fun canonicalizesCommonIranianRepresentationsToOneExactIdentity() {
        val expected = "+989999999999"

        assertEquals(expected, CallerPolicyEngine.canonicalize("+98 999 999 9999"))
        assertEquals(expected, CallerPolicyEngine.canonicalize("0098-999-999-9999"))
        assertEquals(expected, CallerPolicyEngine.canonicalize("989999999999"))
        assertEquals(expected, CallerPolicyEngine.canonicalize("09999999999"))
    }

    @Test
    fun rejectsSuffixMatchesAndUnavailableCallerIds() {
        assertNull(CallerPolicyEngine.canonicalize(null))
        assertNull(CallerPolicyEngine.canonicalize("anonymous"))
        assertNull(CallerPolicyEngine.canonicalize("+989999999999;12"))
        assertNull(CallerPolicyEngine.canonicalize("+98+9999999999"))
        assertNull(CallerPolicyEngine.canonicalize("tel:+989999999999"))
        assertNull(CallerPolicyEngine.canonicalize("9999999"))
    }
}
