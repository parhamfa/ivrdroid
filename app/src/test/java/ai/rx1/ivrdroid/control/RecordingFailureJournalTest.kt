package ai.rx1.ivrdroid.control

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.IOException

class RecordingFailureJournalTest {
    @Test
    fun keepsOriginalCauseWithRecordingIdentityAndElapsedDuration() {
        val error = IllegalStateException("Recording save failed", IOException("fsync: ENOSPC"))
        val entry = RecordingFailureJournal.entry("commit_encrypted_pair", "call", "recording.00000",
            210841000, 37_900, 34_560_044, 0, error)
        assertTrue(entry.getString("exception").contains("Caused by: java.io.IOException: fsync: ENOSPC"))
        assertEquals("recording.00000", entry.getString("recording_key"))
        assertEquals(37900L, entry.getLong("duration_ms"))
        assertEquals("commit_encrypted_pair", entry.getString("stage"))
    }
}
