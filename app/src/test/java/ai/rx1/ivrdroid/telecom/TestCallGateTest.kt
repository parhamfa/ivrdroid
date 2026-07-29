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

        assertEquals(expected, TestCallGate.canonicalize("+98 999 999 9999"))
        assertEquals(expected, TestCallGate.canonicalize("0098-999-999-9999"))
        assertEquals(expected, TestCallGate.canonicalize("989999999999"))
        assertEquals(expected, TestCallGate.canonicalize("09999999999"))
    }

    @Test
    fun rejectsSuffixMatchesAndUnavailableCallerIds() {
        assertNull(TestCallGate.canonicalize(null))
        assertNull(TestCallGate.canonicalize("anonymous"))
        assertNull(TestCallGate.canonicalize("+989999999999;12"))
        assertNull(TestCallGate.canonicalize("+98+9999999999"))
        assertNull(TestCallGate.canonicalize("tel:+989999999999"))
        assertFalse(TestCallGate.matches("+449999999999"))
        assertFalse(TestCallGate.matches("9999999"))
    }

    @Test
    fun configuredBuildMatchesOnlyItsCanonicalAllowlist() {
        val configured = TestCallGate.allowlistedCallerE164
        if (configured == null) {
            assertFalse(TestCallGate.matches("+989999999999"))
        } else {
            assertTrue(TestCallGate.matches(configured))
        }
    }
}
