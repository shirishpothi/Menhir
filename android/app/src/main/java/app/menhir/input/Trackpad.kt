package app.menhir.input

import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min

/** One finger's position in a frame, in view pixels. */
data class Finger(val pointerId: Int, val x: Float, val y: Float)

/**
 * A surface that is the Mac's trackpad: the engine presents a Magic Trackpad 2 on the Mac's USB bus, so
 * macOS's own multitouch driver sees the fingers and does the pointing, scrolling and gestures. This
 * side sends only where the fingers are, in the pad's sensor units, with each sample's own time.
 *
 * As in ios/Henge/Trackpad.swift (touchscreen mode), the surface also clicks when [tapsClick]: a quick
 * tap is a left click, a quick two-finger tap a right click, and a finger that stays put for 0.4 s
 * presses the left button so moving it drags. A quick four-finger tap calls [onReveal]. The keyboard
 * case's touchpad, captured, uses the same class with [tapsClick] off: it has its own click.
 *
 * Times are in ms on the events' clock (MotionEvent.getEventTime), truncated to 32 bits for the engine.
 */
class Trackpad(
    private val mac: MacInput,
    private val buttons: MouseButtons,
    private val scheduler: Scheduler,
    private val tapsClick: Boolean,
    /** How far fingers may travel, in view pixels, and still be a tap (8 dp). */
    private val tapDistance: Float,
    private val onReveal: () -> Unit = {},
    private val onGestureEnd: () -> Unit = {},
    private val onHaptic: () -> Unit = {},
) {
    companion object {
        const val SENSOR_W = 8134
        const val SENSOR_H = 5206
        const val HOLD_MS = 400L
        const val TAP_MS = 300L
        const val MAX_FINGERS = 5 // as on iOS; a sixth finger is ignored

        const val DOWN = 0
        const val MOVE = 1
        const val UP = 2

        /**
         * Where a point in a [w] x [h] view is on the pad's sensor. One scale for both axes: the view
         * and the sensor are different shapes, and stretching one onto the other would make a finger
         * move the pointer further one way than the other. The view maps to a centred part of the sensor.
         */
        fun sensor(x: Float, y: Float, w: Float, h: Float): Pair<Int, Int> {
            val vw = max(w.toDouble(), 1.0)
            val vh = max(h.toDouble(), 1.0)
            val scale = min(SENSOR_W / vw, SENSOR_H / vh)
            val sx = (x - vw / 2) * scale + SENSOR_W / 2.0
            val sy = (y - vh / 2) * scale + SENSOR_H / 2.0
            return sx.coerceIn(0.0, SENSOR_W.toDouble()).toInt() to sy.coerceIn(0.0, SENSOR_H.toDouble()).toInt()
        }

        /** The engine takes the sample clock as a 32-bit ms count; it only looks at differences. */
        fun ts(ms: Long): Int = ms.toInt()
    }

    private var width = 1f
    private var height = 1f
    private val ids = mutableMapOf<Int, Int>() // pointer id -> finger 1..5
    private val last = mutableMapOf<Int, Pair<Float, Float>>()
    private var started = 0L
    private var travelled = 0f
    private var mostFingers = 0
    private var holding = false
    private var holdTimer: Cancellable? = null

    fun setSize(w: Int, h: Int) {
        width = w.toFloat()
        height = h.toFloat()
    }

    val fingersDown get() = ids.size

    fun down(f: Finger, timeMs: Long) {
        if (ids.containsKey(f.pointerId)) return
        val id = (1..MAX_FINGERS).firstOrNull { it !in ids.values } ?: return
        if (ids.isEmpty()) {
            started = timeMs
            travelled = 0f
            mostFingers = 0
        }
        ids[f.pointerId] = id
        last[f.pointerId] = f.x to f.y
        val (sx, sy) = sensor(f.x, f.y, width, height)
        mac.touch(id, DOWN, sx, sy, ts(timeMs)) // a finger down or up is its own report: no frame
        mostFingers = max(mostFingers, ids.size)
        if (!tapsClick) return
        if (ids.size >= 2) {
            cancelHold()
            if (holding) { // a drag becomes a gesture
                holding = false
                buttons.release(0, MouseButtons.Source.TOUCH)
            }
        } else {
            armHold()
        }
    }

    /** One sample of every finger that is down: a historical one from a batched event, or the latest. */
    fun move(fingers: List<Finger>, timeMs: Long) {
        var moved = false
        for (f in fingers) {
            val id = ids[f.pointerId] ?: continue
            val before = last[f.pointerId]
            if (before != null) {
                if (before.first == f.x && before.second == f.y) continue
                travelled += abs(f.x - before.first) + abs(f.y - before.second)
            }
            last[f.pointerId] = f.x to f.y
            val (sx, sy) = sensor(f.x, f.y, width, height)
            mac.touch(id, MOVE, sx, sy, ts(timeMs))
            moved = true
        }
        if (tapsClick && travelled >= tapDistance && !holding) cancelHold()
        // One frame with every finger that moved, so a gesture's fingers move together.
        if (moved) mac.touchFrame(ts(timeMs))
    }

    fun up(pointerId: Int, timeMs: Long) {
        val id = ids.remove(pointerId) ?: return
        last.remove(pointerId)
        mac.touch(id, UP, 0, 0, ts(timeMs))
        if (ids.isNotEmpty()) return // the gesture goes on
        val quick = timeMs - started < TAP_MS
        val still = travelled < tapDistance
        if (tapsClick) {
            cancelHold()
            if (holding) {
                holding = false
                buttons.release(0, MouseButtons.Source.TOUCH) // let go of the drag
            } else if (quick && still) {
                if (mostFingers == 1) click(0)
                else if (mostFingers == 2) click(1)
            }
        }
        if (mostFingers >= 4 && travelled < tapDistance * 3 && timeMs - started < TAP_MS * 2) onReveal()
        onGestureEnd()
    }

    /** Every finger is gone at once (the gesture was cancelled, or the surface went away). */
    fun cancel(timeMs: Long) {
        cancelHold()
        if (holding) {
            holding = false
            buttons.release(0, MouseButtons.Source.TOUCH)
        }
        if (ids.isEmpty()) return
        for (id in ids.values) mac.touch(id, UP, 0, 0, ts(timeMs))
        ids.clear()
        last.clear()
    }

    private fun click(button: Int) {
        buttons.click(button, MouseButtons.Source.TOUCH)
        onHaptic()
    }

    private fun armHold() {
        cancelHold()
        holdTimer = scheduler.schedule(HOLD_MS) {
            holdTimer = null
            if (ids.size == 1 && travelled < tapDistance) {
                holding = true
                buttons.press(0, MouseButtons.Source.TOUCH)
                onHaptic()
            }
        }
    }

    private fun cancelHold() {
        holdTimer?.cancel()
        holdTimer = null
    }
}

/**
 * The volume keys are the pad's buttons: up is the left button, down the right. Android reports a real
 * press and release, so the button is down exactly while the key is (the iOS app's "intercept" mode):
 * a press is a click, and holding one while a finger moves drags. Auto-repeat is dropped.
 */
class VolumeButtons(private val buttons: MouseButtons, private val onHaptic: () -> Unit = {}) {
    /** keyCode: KeyEvent.KEYCODE_VOLUME_UP or _DOWN. True if it was one of them. */
    fun onKey(keyCode: Int, down: Boolean, repeat: Int): Boolean {
        val button = when (keyCode) {
            android.view.KeyEvent.KEYCODE_VOLUME_UP -> 0
            android.view.KeyEvent.KEYCODE_VOLUME_DOWN -> 1
            else -> return false
        }
        if (down) {
            if (repeat == 0 && !buttons.isDown(button, MouseButtons.Source.VOLUME)) {
                buttons.press(button, MouseButtons.Source.VOLUME)
                onHaptic()
            }
        } else {
            buttons.release(button, MouseButtons.Source.VOLUME)
        }
        return true
    }

    fun releaseAll() = buttons.releaseAll(MouseButtons.Source.VOLUME)
}

/**
 * A mouse, or a touchpad Android reports as one, with the pointer captured: relative motion, buttons
 * and the wheel. What does not make a whole step yet waits for the next event, as on iOS.
 */
class RelativeMouse(private val mac: MacInput, private val buttons: MouseButtons) {
    private var restX = 0f
    private var restY = 0f
    private var restScrollX = 0f
    private var restScrollY = 0f
    private var lastState = 0

    fun move(dx: Float, dy: Float) {
        restX += dx
        restY += dy
        val x = restX.toInt()
        val y = restY.toInt()
        if (x == 0 && y == 0) return
        restX -= x
        restY -= y
        mac.mouseMove(x, y)
    }

    /**
     * Android's MotionEvent.getButtonState bits: 1 primary, 2 secondary, 4 tertiary. Sends what changed.
     */
    fun buttonState(state: Int) {
        val changed = state xor lastState
        for ((bit, button) in listOf(1 to 0, 2 to 1, 4 to 2)) {
            if (changed and bit == 0) continue
            if (state and bit != 0) buttons.press(button, MouseButtons.Source.MOUSE)
            else buttons.release(button, MouseButtons.Source.MOUSE)
        }
        lastState = state
    }

    /** AXIS_VSCROLL is positive away from the user (up), AXIS_HSCROLL positive to the right, as the engine's. */
    fun scroll(h: Float, v: Float) {
        restScrollX += h
        restScrollY += v
        val x = restScrollX.toInt()
        val y = restScrollY.toInt()
        if (x == 0 && y == 0) return
        restScrollX -= x
        restScrollY -= y
        mac.mouseScroll(x, y)
    }

    fun releaseAll() {
        buttons.releaseAll(MouseButtons.Source.MOUSE)
        lastState = 0
    }
}
