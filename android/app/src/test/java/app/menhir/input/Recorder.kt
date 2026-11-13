package app.menhir.input

/** A MacInput that writes down what it is sent, and a Scheduler the test runs by hand. */
class Recorder : MacInput {
    val calls = mutableListOf<String>()
    override fun key(code: Int, down: Boolean) { calls += "key $code ${if (down) "down" else "up"}" }
    override fun keyHid(usage: Int, down: Boolean) { calls += "hid 0x%02x %s".format(usage, if (down) "down" else "up") }
    override fun mouseMove(dx: Int, dy: Int) { calls += "move $dx $dy" }
    override fun mouseButton(button: Int, down: Boolean) { calls += "button $button ${if (down) "down" else "up"}" }
    override fun mouseScroll(dx: Int, dy: Int) { calls += "scroll $dx $dy" }
    override fun touch(finger: Int, phase: Int, x: Int, y: Int, tsMs: Int) { calls += "touch $finger $phase $x $y $tsMs" }
    override fun touchFrame(tsMs: Int) { calls += "frame $tsMs" }
    override fun touchButton(down: Boolean) { calls += "pad ${if (down) "down" else "up"}" }
    fun take(): List<String> = calls.toList().also { calls.clear() }
}

class ManualScheduler : Scheduler {
    private class Task(val at: Long, val block: () -> Unit) { var cancelled = false }
    private val tasks = mutableListOf<Task>()
    var now = 0L

    override fun schedule(delayMs: Long, block: () -> Unit): Cancellable {
        val t = Task(now + delayMs, block)
        tasks += t
        return Cancellable { t.cancelled = true }
    }

    fun advanceTo(ms: Long) {
        now = ms
        val due = tasks.filter { it.at <= ms && !it.cancelled }
        tasks.removeAll(due)
        due.forEach { it.block() }
    }
}
