package ai.rx1.ivrdroid.telecom

object SessionMonitorPolicy {
    fun isTerminal(helperState: String): Boolean = helperState in setOf("READY", "ERROR", "STOPPED")
}
