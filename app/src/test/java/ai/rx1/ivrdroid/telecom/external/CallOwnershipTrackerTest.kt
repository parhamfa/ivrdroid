package ai.rx1.ivrdroid.telecom.external

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class CallOwnershipTrackerTest {
    @Test
    fun operatorRemainsOwnedWhenTelecomDropsTransientExtras() {
        val tracker = CallOwnershipTracker()
        val session = "11111111-1111-4111-8111-111111111111"
        tracker.remember("outgoing-fingerprint", session)

        // A later Details update has no session extra; ownership is still the validated value.
        assertEquals(session, tracker.owner("outgoing-fingerprint"))
        tracker.remove("outgoing-fingerprint")
        assertNull(tracker.owner("outgoing-fingerprint"))
    }
}
