package app.menhir

import android.content.Intent
import android.hardware.input.InputManager
import android.os.Bundle
import android.util.Log
import android.view.InputDevice
import android.view.KeyEvent
import android.view.View
import android.view.WindowInsets as PlatformInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.view.inputmethod.InputMethodManager
import android.widget.FrameLayout
import androidx.activity.ComponentActivity
import androidx.activity.OnBackPressedCallback
import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.spring
import androidx.compose.foundation.background
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.platform.LocalView
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.displayCutout
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.Surface
import androidx.compose.material3.VerticalDivider
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.unit.Dp
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.ime
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.testTagsAsResourceId
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.lifecycleScope
import app.menhir.input.EngineInput
import app.menhir.input.HardwareKeyboard
import app.menhir.input.LinuxKeys
import app.menhir.input.MouseButtons
import app.menhir.input.StickyKeys
import app.menhir.input.VolumeButtons
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * A running Mac, full screen. The Mac's screen fills the panel; a key row and controls sit over it
 * when asked for. Leaving the screen pauses and saves the Mac (InstantResume); coming back runs it on.
 */
class SessionActivity : ComponentActivity() {
    companion object {
        const val MAC_ID = "mac"
        private const val TAG = "menhir"
    }

    private lateinit var mac: Mac
    private lateinit var surface: MacSurface
    private lateinit var mode: DisplayMode
    private val buttons = MouseButtons(EngineInput)
    private val sticky = StickyKeys(EngineInput)
    private val keyboard = HardwareKeyboard(EngineInput) { sticky.releaseAll() }
    private val volume = VolumeButtons(buttons) { surface.performHapticFeedback(android.view.HapticFeedbackConstants.CONFIRM) }

    // What the overlay shows.
    private var engineState by mutableStateOf(Engine.NOT_STARTED)
    private var error by mutableStateOf<String?>(null)
    private var controls by mutableStateOf(false)
    private var typing by mutableStateOf(false)
    private var confirmStop by mutableStateOf(false)
    private var stickyDown by mutableStateOf(emptySet<Int>())
    private var reason by mutableStateOf("")
    private var starting = false

    private val devices = object : InputManager.InputDeviceListener {
        override fun onInputDeviceAdded(deviceId: Int) = capturePointer()
        override fun onInputDeviceRemoved(deviceId: Int) {
            surface.mouse.releaseAll()
        }
        override fun onInputDeviceChanged(deviceId: Int) = capturePointer()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // The home screen grows the Mac's screen to full black before this opens and shrinks it after; no window animation between.
        overrideActivityTransition(OVERRIDE_TRANSITION_OPEN, 0, 0)
        overrideActivityTransition(OVERRIDE_TRANSITION_CLOSE, 0, 0)
        val id = intent.getStringExtra(MAC_ID) ?: Session.mac?.id
        val found = id?.let { Machines.get(it) }
        if (found == null) {
            finish()
            return
        }
        mac = found
        mode = displayMode()
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        immersive()

        sticky.onChange = { stickyDown = sticky.down.toSet() }
        surface = MacSurface(this, buttons, onReveal = { controls = true }, afterInput = { sticky.releaseAll() })
        surface.holder.setFixedSize(mode.width, mode.height)
        surface.keyHandler = ::onKey
        val overlay = ComposeView(this).apply {
            setContent {
                MenhirTheme(dark = true) {
                    Box(Modifier.fillMaxSize().semantics { testTagsAsResourceId = true }) { Overlay() }
                }
            }
        }
        setContentView(FrameLayout(this).apply {
            setBackgroundColor(android.graphics.Color.BLACK)
            addView(surface, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
            addView(overlay, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
        })
        surface.requestFocus()

        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                if (engineState == Engine.STOPPED || error != null) finish() else controls = !controls
            }
        })
        getSystemService(InputManager::class.java).registerInputDeviceListener(devices, null)

        if (Session.mac?.id == mac.id && Session.active) engineState = Engine.state() else boot()
        lifecycleScope.launch { watch() }
    }

    override fun finish() {
        HeroLaunch.pending = !reduceMotion()
        super.finish()
    }

    override fun onDestroy() {
        if (::surface.isInitialized) getSystemService(InputManager::class.java).unregisterInputDeviceListener(devices)
        super.onDestroy()
    }

    /** The panel's real pixels and density, read now. */
    private fun displayMode(): DisplayMode {
        val d = display
        val m = d.mode
        val metrics = resources.displayMetrics
        return DisplayMode.forPanel(m.physicalWidth, m.physicalHeight, metrics.xdpi, metrics.ydpi)
    }

    private fun immersive() {
        window.insetsController?.let {
            it.hide(PlatformInsets.Type.systemBars())
            it.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
    }

    private fun boot() {
        if (starting) return
        starting = true
        error = null
        engineState = Engine.CHANGING
        lifecycleScope.launch {
            val err = withContext(Dispatchers.IO) { Session.start(this@SessionActivity, mac, mode) }
            starting = false
            if (err != null) {
                error = err
                Log.w(TAG, "the Mac did not start: $err")
            }
            MenhirService.refresh(this@SessionActivity)
        }
    }

    /** Follows the engine: its state for the overlay, and the Mac's clipboard. */
    private suspend fun watch() {
        var lastState = -1
        while (lifecycleScope.isActive) {
            val s = Engine.state()
            if (!starting) engineState = s
            if (s == Engine.RUNNING) Sharing.followMacClipboard(this)
            if (s != lastState) {
                if (s == Engine.STOPPED && lastState != -1 && Session.mac != null) stopped()
                lastState = s
                MenhirService.update()
            }
            delay(500)
        }
    }

    private fun stopped() {
        reason = Engine.exitReason()
        Log.i(TAG, "the Mac stopped: $reason")
        typing = false
        showKeyboard(false)
        releaseInput()
        val restart = Engine.exitOutcome() == Engine.OUTCOME_RESTART && Engine.canStart()
        Session.ended()
        MenhirService.refresh(this)
        if (restart) boot() // macOS asked to restart: start it again, as iOS does
    }

    override fun onStart() {
        super.onStart()
        InstantResume.cameBack()
    }

    override fun onStop() {
        super.onStop()
        releaseInput()
        if (!isChangingConfigurations) InstantResume.suspend()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) {
            immersive()
            surface.requestFocus()
            capturePointer()
        } else {
            releaseInput() // Android may keep a key's or button's release when something else takes over
        }
    }

    /** A mouse or touchpad drives only the Mac's pointer. Without one, nothing is captured. */
    private fun capturePointer() {
        if (!::surface.isInitialized || !hasWindowFocus()) return
        val pointing = InputDevice.getDeviceIds().any { id ->
            val d = InputDevice.getDevice(id) ?: return@any false
            !d.isVirtual && d.isEnabled &&
                (d.supportsSource(InputDevice.SOURCE_MOUSE) || d.supportsSource(InputDevice.SOURCE_TOUCHPAD)) &&
                !d.supportsSource(InputDevice.SOURCE_TOUCHSCREEN)
        }
        if (pointing && !surface.hasPointerCapture()) surface.requestPointerCapture()
        if (!pointing && surface.hasPointerCapture()) surface.releasePointerCapture()
    }

    private fun releaseInput() {
        if (!::surface.isInitialized) return
        keyboard.releaseAll()
        volume.releaseAll()
        surface.releaseAll()
        buttons.releaseAll(MouseButtons.Source.ROW)
        buttons.releaseAll(MouseButtons.Source.TOUCH)
        sticky.releaseAll()
    }

    /** Keys reach the focused MacSurface, which asks here first: hardware keys, the volume keys, and the soft keyboard's key events. */
    private fun onKey(event: KeyEvent): Boolean {
        if ((event.action != KeyEvent.ACTION_DOWN && event.action != KeyEvent.ACTION_UP) || Engine.state() != Engine.RUNNING) return false
        val down = event.action == KeyEvent.ACTION_DOWN
        if (volume.onKey(event.keyCode, down, event.repeatCount)) return true
        return keyboard.onKey(event.keyCode, down, event.repeatCount)
    }

    private fun showKeyboard(on: Boolean) {
        surface.typing = on
        val imm = getSystemService(InputMethodManager::class.java)
        imm.restartInput(surface)
        if (on) {
            surface.requestFocus()
            window.insetsController?.show(PlatformInsets.Type.ime())
        } else {
            window.insetsController?.hide(PlatformInsets.Type.ime())
        }
    }

    // ---- The overlay -----------------------------------------------------------------------------

    @Composable
    private fun Overlay() {
        val s = engineState
        when {
            error != null -> Message("${mac.name} didn't start", error ?: "") {
                TextButton(onClick = { finish() }, modifier = Modifier.heightIn(min = 48.dp)) { Text("Close") }
                if (Engine.canStart()) Button(onClick = { boot() }, modifier = Modifier.heightIn(min = 48.dp)) { Text("Try again") }
            }
            s == Engine.STOPPED && Session.mac == null && !starting -> Message("${mac.name} stopped", reason) {
                Button(onClick = { finish() }, modifier = Modifier.heightIn(min = 48.dp).testTag("close")) { Text("Close") }
            }
            s == Engine.CHANGING || starting -> Message("Starting ${mac.name}", "", busy = true) {}
        }
        if (controls && s == Engine.RUNNING) Controls()
        if (typing && s == Engine.RUNNING) {
            Box(
                Modifier.fillMaxSize().windowInsetsPadding(WindowInsets.ime).padding(bottom = Space.s),
                contentAlignment = Alignment.BottomCenter,
            ) { KeyRow() }
        }
        if (confirmStop) {
            AlertDialog(
                onDismissRequest = { confirmStop = false },
                title = { Text("Stop ${mac.name}?") },
                text = { Text("This is like holding the power button: anything not saved on the Mac is lost. Shutting down from the Apple menu is safer.") },
                confirmButton = {
                    TextButton(
                        onClick = {
                            confirmStop = false
                            controls = false
                            InstantResume.discard(mac)
                            Engine.stop()
                        },
                        colors = ButtonDefaults.textButtonColors(contentColor = MaterialTheme.colorScheme.error),
                        modifier = Modifier.testTag("confirmStop"),
                    ) { Text("Stop") }
                },
                dismissButton = { TextButton(onClick = { confirmStop = false }) { Text("Cancel") } },
            )
        }
    }

    @Composable
    private fun Message(title: String, text: String, busy: Boolean = false, actions: @Composable () -> Unit) {
        Box(Modifier.fillMaxSize().background(Color.Black.copy(alpha = 0.4f)), contentAlignment = Alignment.Center) {
            Surface(
                Modifier.widthIn(min = 320.dp, max = 480.dp),
                shape = MaterialTheme.shapes.extraLarge,
                color = MaterialTheme.colorScheme.surfaceContainerHigh,
            ) {
                Column(Modifier.padding(Space.l)) {
                    Text(title, style = MaterialTheme.typography.titleLarge)
                    if (busy) {
                        LinearProgressIndicator(Modifier.fillMaxWidth().padding(top = Space.m))
                    }
                    if (text.isNotEmpty()) Text(text, style = Caption, modifier = Modifier.padding(top = Space.s).testTag("message"))
                    Row(
                        Modifier.fillMaxWidth().padding(top = Space.m),
                        horizontalArrangement = Arrangement.spacedBy(Space.s, Alignment.End),
                    ) { actions() }
                }
            }
        }
    }

    /** Shown by a four-finger tap or Back. */
    @Composable
    private fun Controls() {
        Box(
            Modifier.fillMaxSize().windowInsetsPadding(WindowInsets.displayCutout).padding(top = Space.m),
            contentAlignment = Alignment.TopCenter,
        ) {
            Surface(shape = CircleShape, color = MaterialTheme.colorScheme.surfaceContainerHigh, shadowElevation = 6.dp) {
                Row(
                    Modifier.padding(horizontal = Space.s, vertical = Space.xs),
                    horizontalArrangement = Arrangement.spacedBy(Space.xs),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    BarButton(if (typing) "Hide keyboard" else "Keyboard", "keyboard") { typing = !typing; showKeyboard(typing); controls = false }
                    BarButton("Paste") { controls = false; lifecycleScope.launch { Sharing.paste(this@SessionActivity, EngineInput) } }
                    BarButton("Type setup command") {
                        controls = false
                        lifecycleScope.launch { Sharing.typeSlowly(EngineInput, Sharing.SET_UP_COMMAND) }
                    }
                    BarButton("Home") {
                        HeroLaunch.pending = !reduceMotion()
                        startActivity(
                            Intent(this@SessionActivity, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT),
                            android.app.ActivityOptions.makeCustomAnimation(this@SessionActivity, 0, 0).toBundle(),
                        )
                    }
                    VerticalDivider(Modifier.height(24.dp).padding(horizontal = Space.xs), color = MaterialTheme.colorScheme.outlineVariant)
                    BarButton("Stop", "stop", MaterialTheme.colorScheme.error) { confirmStop = true }
                    Button(onClick = { controls = false }, modifier = Modifier.heightIn(min = 48.dp)) { Text("Done") }
                }
            }
        }
    }

    @Composable
    private fun BarButton(label: String, tag: String? = null, color: Color = Color.Unspecified, onClick: () -> Unit) {
        TextButton(
            onClick = onClick,
            colors = if (color == Color.Unspecified) ButtonDefaults.textButtonColors() else ButtonDefaults.textButtonColors(contentColor = color),
            modifier = Modifier.heightIn(min = 48.dp).then(if (tag != null) Modifier.testTag(tag) else Modifier),
        ) { Text(label) }
    }

    /** The keys an on-screen keyboard doesn't have, above it. ⌃ ⌥ ⌘ stay down until the next key or click. */
    @Composable
    private fun KeyRow() {
        Surface(
            shape = RoundedCornerShape(16.dp),
            color = Color(0xF01C1C1E),
            shadowElevation = 4.dp,
        ) {
            Row(
                Modifier.padding(6.dp),
                horizontalArrangement = Arrangement.spacedBy(6.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                HeldKey("esc", LinuxKeys.ESCAPE)
                HeldKey("tab", LinuxKeys.TAB)
                KeyGap()
                StickyKey("⌃", "control", LinuxKeys.CONTROL)
                StickyKey("⌥", "option", LinuxKeys.OPTION)
                StickyKey("⌘", "command", LinuxKeys.COMMAND)
                KeyGap()
                HeldKey("←", LinuxKeys.LEFT, KeyNarrow)
                HeldKey("↑", LinuxKeys.UP, KeyNarrow)
                HeldKey("↓", LinuxKeys.DOWN, KeyNarrow)
                HeldKey("→", LinuxKeys.RIGHT, KeyNarrow)
                KeyGap()
                KeyCap("hide", null, KeyUp, KeyWide, Modifier.pointerInput(Unit) {
                    detectTapGestures { typing = false; showKeyboard(false) }
                }, labelColor = Color(0xFFAEAEB2))
            }
        }
    }

    @Composable
    private fun KeyGap() = Spacer(Modifier.width(6.dp))

    @Composable
    private fun KeyCap(
        label: String,
        caption: String?,
        color: Color,
        width: Dp,
        modifier: Modifier,
        tag: String = label,
        labelColor: Color = Color.White,
    ) {
        val shown by animateColorAsState(color, spring(dampingRatio = 0.8f, stiffness = 900f), label = "key")
        val lit = color == KeyOn
        val scale by animateFloatAsState(if (lit) 1.06f else 1f, Springs.snappy(), label = "keyScale")
        Box(
            Modifier.width(width).height(48.dp).graphicsLayer { scaleX = scale; scaleY = scale }
                .background(shown, RoundedCornerShape(10.dp)).testTag("key-$tag").then(modifier),
            contentAlignment = Alignment.Center,
        ) {
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                Text(label, fontSize = if (caption != null) 18.sp else 15.sp, fontWeight = FontWeight.SemiBold, color = labelColor, lineHeight = 18.sp)
                if (caption != null) Text(caption, fontSize = 10.sp, color = labelColor.copy(alpha = 0.8f), lineHeight = 12.sp)
            }
        }
    }

    /** Down while touched, up when let go, so holding it repeats as on a Mac. */
    @Composable
    private fun HeldKey(label: String, code: Int, width: Dp = KeyWide) {
        var down by androidx.compose.runtime.remember { mutableStateOf(false) }
        KeyCap(label, null, if (down) KeyOn else KeyUp, width, Modifier.pointerInput(code) {
            detectTapGestures(onPress = {
                down = true
                EngineInput.key(code, true)
                tryAwaitRelease()
                EngineInput.key(code, false)
                down = false
                sticky.releaseAll()
            })
        })
    }

    /** Lit while it is held for the next key. */
    @Composable
    private fun StickyKey(symbol: String, name: String, code: Int) {
        val on = code in stickyDown
        val view = LocalView.current
        KeyCap(
            symbol, name, if (on) KeyOn else KeyUp, KeyWide,
            Modifier.semantics { stateDescription = if (on) "on" else "off" }.pointerInput(code) {
                detectTapGestures {
                    view.performHapticFeedback(android.view.HapticFeedbackConstants.CLOCK_TICK)
                    sticky.toggle(code)
                }
            },
            name,
        )
    }
}

private val KeyUp = Color(0xFF3A3A3C)
private val KeyOn = Color(0xFF0A84FF)
private val KeyWide = 64.dp
private val KeyNarrow = 48.dp
