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
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.ime
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.darkColorScheme
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
                MaterialTheme(colorScheme = darkColorScheme()) {
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
            error != null -> Message("${mac.name} did not start", error ?: "") {
                if (Engine.canStart()) Button(onClick = { boot() }) { Text("Try again") }
                TextButton(onClick = { finish() }) { Text("Close") }
            }
            s == Engine.STOPPED && Session.mac == null && !starting -> Message("${mac.name} stopped", reason) {
                Button(onClick = { finish() }, modifier = Modifier.testTag("close")) { Text("Close") }
            }
            s == Engine.CHANGING || starting -> Message("Starting ${mac.name}…", "") {}
        }
        if (controls && s == Engine.RUNNING) Controls()
        if (typing && s == Engine.RUNNING) {
            Box(Modifier.fillMaxSize().windowInsetsPadding(WindowInsets.ime), contentAlignment = Alignment.BottomCenter) { KeyRow() }
        }
        if (confirmStop) {
            AlertDialog(
                onDismissRequest = { confirmStop = false },
                title = { Text("Stop ${mac.name}?") },
                text = { Text("It's like holding a Mac's power button: what isn't saved on the Mac is lost. Shutting down from the Apple menu is safer.") },
                confirmButton = {
                    TextButton(onClick = {
                        confirmStop = false
                        controls = false
                        InstantResume.discard(mac)
                        Engine.stop()
                    }, modifier = Modifier.testTag("confirmStop")) { Text("Stop") }
                },
                dismissButton = { TextButton(onClick = { confirmStop = false }) { Text("Cancel") } },
            )
        }
    }

    @Composable
    private fun Message(title: String, text: String, actions: @Composable () -> Unit) {
        Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
            Card {
                Column(Modifier.padding(24.dp), horizontalAlignment = Alignment.CenterHorizontally) {
                    Text(title, style = MaterialTheme.typography.titleLarge)
                    if (text.isNotEmpty()) Text(text, Modifier.padding(top = 8.dp).testTag("message"))
                    Row(Modifier.padding(top = 12.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) { actions() }
                }
            }
        }
    }

    /** Shown by a four-finger tap or Back. */
    @Composable
    private fun Controls() {
        Box(Modifier.fillMaxSize().padding(top = 16.dp), contentAlignment = Alignment.TopCenter) {
            Card {
                Row(Modifier.padding(8.dp), horizontalArrangement = Arrangement.spacedBy(4.dp), verticalAlignment = Alignment.CenterVertically) {
                    TextButton(onClick = { typing = !typing; showKeyboard(typing); controls = false }, modifier = Modifier.testTag("keyboard")) {
                        Text(if (typing) "Hide keyboard" else "Keyboard")
                    }
                    TextButton(onClick = { controls = false; lifecycleScope.launch { Sharing.paste(this@SessionActivity, EngineInput) } }) { Text("Paste") }
                    TextButton(onClick = {
                        controls = false
                        lifecycleScope.launch { Sharing.typeSlowly(EngineInput, Sharing.SET_UP_COMMAND) }
                    }) { Text("Type the set up command") }
                    TextButton(onClick = { startActivity(Intent(this@SessionActivity, MainActivity::class.java)) }) { Text("Macs") }
                    TextButton(onClick = { confirmStop = true }, modifier = Modifier.testTag("stop")) { Text("Stop") }
                    TextButton(onClick = { controls = false }) { Text("Done") }
                }
            }
        }
    }

    /** The keys an on-screen keyboard doesn't have, above it. ⌃ ⌥ ⌘ stay down until the next key or click. */
    @Composable
    private fun KeyRow() {
        Row(
            Modifier.fillMaxWidth().height(52.dp).background(Color(0xE0202022)).padding(6.dp),
            horizontalArrangement = Arrangement.spacedBy(5.dp),
        ) {
            HeldKey("esc", LinuxKeys.ESCAPE)
            HeldKey("tab", LinuxKeys.TAB)
            StickyKey("⌃", LinuxKeys.CONTROL, "control")
            StickyKey("⌥", LinuxKeys.OPTION, "option")
            StickyKey("⌘", LinuxKeys.COMMAND, "command")
            HeldKey("←", LinuxKeys.LEFT)
            HeldKey("↑", LinuxKeys.UP)
            HeldKey("↓", LinuxKeys.DOWN)
            HeldKey("→", LinuxKeys.RIGHT)
            KeyCap("hide", Color(0xFF3A3A3C), Modifier.pointerInput(Unit) {
                detectTapGestures { typing = false; showKeyboard(false) }
            })
        }
    }

    @Composable
    private fun androidx.compose.foundation.layout.RowScope.KeyCap(label: String, color: Color, modifier: Modifier, tag: String = label) {
        Box(
            Modifier.weight(1f).fillMaxSize().background(color, RoundedCornerShape(9.dp)).testTag("key-$tag").then(modifier),
            contentAlignment = Alignment.Center,
        ) { Text(label, fontSize = 15.sp, fontWeight = FontWeight.SemiBold, color = Color.White) }
    }

    /** Down while touched, up when let go, so holding it repeats as on a Mac. */
    @Composable
    private fun androidx.compose.foundation.layout.RowScope.HeldKey(label: String, code: Int) {
        var down by androidx.compose.runtime.remember { mutableStateOf(false) }
        KeyCap(label, if (down) Color(0xFF0A84FF) else Color(0xFF3A3A3C), Modifier.pointerInput(code) {
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

    @Composable
    private fun androidx.compose.foundation.layout.RowScope.StickyKey(label: String, code: Int, tag: String) {
        val on = code in stickyDown
        KeyCap(label, if (on) Color(0xFF0A84FF) else Color(0xFF3A3A3C), Modifier.pointerInput(code) {
            detectTapGestures { sticky.toggle(code) }
        }, tag)
    }
}
