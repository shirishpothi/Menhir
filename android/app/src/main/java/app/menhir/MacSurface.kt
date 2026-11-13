package app.menhir

import android.annotation.SuppressLint
import android.content.Context
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.view.HapticFeedbackConstants
import android.view.InputDevice
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import app.menhir.input.Cancellable
import app.menhir.input.EngineInput
import app.menhir.input.Finger
import app.menhir.input.MouseButtons
import app.menhir.input.RelativeMouse
import app.menhir.input.Scheduler
import app.menhir.input.SoftTyper
import app.menhir.input.Trackpad

/**
 * The Mac's screen, and its trackpad. The engine draws into this view's surface (at the guest's own
 * size: the compositor scales it to the panel). Fingers on it are the Magic Trackpad 2's fingers; a
 * captured mouse or touchpad comes here too ([onCapturedPointerEvent]); and while [typing] it is the
 * text field the on-screen keyboard types into.
 */
@SuppressLint("ViewConstructor")
class MacSurface(
    context: Context,
    val buttons: MouseButtons,
    onReveal: () -> Unit,
    private val afterInput: () -> Unit,
) : SurfaceView(context), SurfaceHolder.Callback {

    private val main = Handler(Looper.getMainLooper())
    private val scheduler = Scheduler { delay, block ->
        val r = Runnable(block)
        main.postDelayed(r, delay)
        Cancellable { main.removeCallbacks(r) }
    }
    private val density = resources.displayMetrics.density
    private fun haptic() { performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK) }

    val screen = Trackpad(EngineInput, buttons, scheduler, tapsClick = true, tapDistance = 8 * density,
        onReveal = onReveal, onGestureEnd = afterInput, onHaptic = ::haptic)
    /** The keyboard case's touchpad, captured: its fingers go to the Mac's pad too; it clicks by itself. */
    private val touchpad = Trackpad(EngineInput, buttons, scheduler, tapsClick = false, tapDistance = 8 * density)
    private var touchpadDevice = -1
    private var touchpadButton = false
    val mouse = RelativeMouse(EngineInput, buttons)
    val typer = SoftTyper(EngineInput, afterInput)

    /** Keys for the Mac: true if taken. The soft keyboard's Enter and Delete come here as key events too. */
    var keyHandler: (android.view.KeyEvent) -> Boolean = { false }

    override fun dispatchKeyEvent(event: android.view.KeyEvent): Boolean = keyHandler(event) || super.dispatchKeyEvent(event)

    /** Wanted by the session while the on-screen keyboard is up. */
    var typing = false

    init {
        holder.addCallback(this)
        isFocusable = true
        isFocusableInTouchMode = true
    }

    override fun surfaceCreated(holder: SurfaceHolder) = Engine.setDisplaySurface(holder.surface)
    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) = Engine.setDisplaySurface(holder.surface)
    /** The surface goes when the app is in the background; the engine must let go of it before this returns. */
    override fun surfaceDestroyed(holder: SurfaceHolder) = Engine.setDisplaySurface(null)

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        screen.setSize(w, h)
    }

    @SuppressLint("ClickableViewAccessibility") // the whole view is a trackpad; there is nothing to click
    override fun onTouchEvent(e: MotionEvent): Boolean {
        if (Engine.state() != Engine.RUNNING) return true
        route(screen, e, toolOk = { it == MotionEvent.TOOL_TYPE_FINGER || it == MotionEvent.TOOL_TYPE_STYLUS })
        return true
    }

    /** One MotionEvent onto a [Trackpad]: every historical sample of a batched move is its own frame, at its own time. */
    private fun route(pad: Trackpad, e: MotionEvent, toolOk: (Int) -> Boolean) {
        fun fingers(h: Int?): List<Finger> = (0 until e.pointerCount).filter { toolOk(e.getToolType(it)) }.map {
            if (h == null) Finger(e.getPointerId(it), e.getX(it), e.getY(it))
            else Finger(e.getPointerId(it), e.getHistoricalX(it, h), e.getHistoricalY(it, h))
        }
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                val i = e.actionIndex
                if (toolOk(e.getToolType(i))) pad.down(Finger(e.getPointerId(i), e.getX(i), e.getY(i)), e.eventTime)
            }
            MotionEvent.ACTION_MOVE -> {
                for (h in 0 until e.historySize) pad.move(fingers(h), e.getHistoricalEventTime(h))
                pad.move(fingers(null), e.eventTime)
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> pad.up(e.getPointerId(e.actionIndex), e.eventTime)
            MotionEvent.ACTION_CANCEL -> pad.cancel(e.eventTime)
        }
    }

    /** A mouse or touchpad while the pointer is captured: only the Mac's pointer moves. */
    override fun onCapturedPointerEvent(e: MotionEvent): Boolean {
        if (Engine.state() != Engine.RUNNING) return true
        if (e.isFromSource(InputDevice.SOURCE_TOUCHPAD) && e.getToolType(0) == MotionEvent.TOOL_TYPE_FINGER) {
            if (touchpadDevice != e.deviceId) {
                touchpadDevice = e.deviceId
                val d = e.device
                val w = d?.getMotionRange(MotionEvent.AXIS_X, e.source)?.max ?: 1000f
                val h = d?.getMotionRange(MotionEvent.AXIS_Y, e.source)?.max ?: 700f
                touchpad.setSize(w.toInt(), h.toInt())
            }
            route(touchpad, e) { true }
            val pressed = e.buttonState and MotionEvent.BUTTON_PRIMARY != 0
            if (pressed != touchpadButton) {
                touchpadButton = pressed
                Engine.touchButton(pressed) // the pad's own click, as a real Magic Trackpad's
                if (!pressed) afterInput()
            }
            return true
        }
        when (e.actionMasked) {
            MotionEvent.ACTION_MOVE, MotionEvent.ACTION_HOVER_MOVE -> {
                var dx = 0f
                var dy = 0f
                for (h in 0 until e.historySize) {
                    dx += e.getHistoricalX(h)
                    dy += e.getHistoricalY(h)
                }
                mouse.move(dx + e.x, dy + e.y)
            }
            MotionEvent.ACTION_SCROLL -> mouse.scroll(e.getAxisValue(MotionEvent.AXIS_HSCROLL), e.getAxisValue(MotionEvent.AXIS_VSCROLL))
        }
        val before = buttons.isDown(0, MouseButtons.Source.MOUSE)
        mouse.buttonState(e.buttonState)
        if (before && !buttons.isDown(0, MouseButtons.Source.MOUSE)) afterInput()
        return true
    }

    /** All input lets go: focus moved away, or the session is leaving the screen. */
    fun releaseAll() {
        screen.cancel(android.os.SystemClock.uptimeMillis())
        touchpad.cancel(android.os.SystemClock.uptimeMillis())
        mouse.releaseAll()
        if (touchpadButton) {
            touchpadButton = false
            Engine.touchButton(false)
        }
    }

    override fun onCheckIsTextEditor() = typing

    override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection? {
        if (!typing) return null
        // Visible-password text: no suggestions or autocorrection, and letters arrive as they are typed.
        outAttrs.inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD or
            InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
        outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI or EditorInfo.IME_FLAG_NO_FULLSCREEN or EditorInfo.IME_ACTION_NONE
        return object : BaseInputConnection(this, false) {
            override fun commitText(text: CharSequence, newCursorPosition: Int): Boolean {
                typer.commit(text.toString())
                return true
            }

            override fun setComposingText(text: CharSequence, newCursorPosition: Int): Boolean {
                typer.setComposing(text.toString())
                return true
            }

            override fun finishComposingText(): Boolean {
                typer.finishComposing()
                return true
            }

            override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean {
                typer.deleteBefore(beforeLength)
                return true
            }

        }
    }
}
