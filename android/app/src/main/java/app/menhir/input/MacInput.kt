package app.menhir.input

import app.menhir.Engine

/** Where input goes: the engine, or a recorder in the unit tests. The calls are the engine's own. */
interface MacInput {
    fun key(code: Int, down: Boolean)
    fun keyHid(usage: Int, down: Boolean)
    fun mouseMove(dx: Int, dy: Int)
    fun mouseButton(button: Int, down: Boolean)
    fun mouseScroll(dx: Int, dy: Int)
    fun touch(finger: Int, phase: Int, x: Int, y: Int, tsMs: Int)
    fun touchFrame(tsMs: Int)
    fun touchButton(down: Boolean)
}

object EngineInput : MacInput {
    override fun key(code: Int, down: Boolean) = Engine.key(code, down)
    override fun keyHid(usage: Int, down: Boolean) = Engine.keyHid(usage, down)
    override fun mouseMove(dx: Int, dy: Int) = Engine.mouseMove(dx, dy)
    override fun mouseButton(button: Int, down: Boolean) = Engine.mouseButton(button, down)
    override fun mouseScroll(dx: Int, dy: Int) = Engine.mouseScroll(dx, dy)
    override fun touch(finger: Int, phase: Int, x: Int, y: Int, tsMs: Int) = Engine.touch(finger, phase, x, y, tsMs)
    override fun touchFrame(tsMs: Int) = Engine.touchFrame(tsMs)
    override fun touchButton(down: Boolean) = Engine.touchButton(down)
}

/** Runs a block later on the main thread; the tests run it by hand. */
fun interface Scheduler {
    fun schedule(delayMs: Long, block: () -> Unit): Cancellable
}

fun interface Cancellable {
    fun cancel()
}

/**
 * The Mac's mouse buttons, which more than one thing can hold at once: the volume keys, a finger
 * holding to drag, a mouse. A button goes down when the first holder presses it and up when the last
 * lets go, so one source never releases what another still holds.
 */
class MouseButtons(private val mac: MacInput) {
    enum class Source { TOUCH, VOLUME, MOUSE, ROW }

    private val holders = Array(3) { mutableSetOf<Source>() }

    fun press(button: Int, source: Source) {
        if (button !in 0..2) return
        val set = holders[button]
        if (set.isEmpty()) mac.mouseButton(button, true)
        set.add(source)
    }

    fun release(button: Int, source: Source) {
        if (button !in 0..2) return
        val set = holders[button]
        if (!set.remove(source)) return
        if (set.isEmpty()) mac.mouseButton(button, false)
    }

    fun isDown(button: Int, source: Source) = button in 0..2 && source in holders[button]

    /** A brief press: down then up, unless something else holds the button. */
    fun click(button: Int, source: Source) {
        press(button, source)
        release(button, source)
    }

    fun releaseAll(source: Source) {
        for (b in 0..2) release(b, source)
    }
}
