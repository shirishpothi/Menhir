package app.menhir

import android.Manifest
import android.app.ActivityOptions
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Bundle
import android.view.animation.PathInterpolator
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.SizeTransform
import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.animateContentSize
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.tween
import androidx.compose.animation.expandVertically
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.scaleIn
import androidx.compose.animation.scaleOut
import androidx.compose.animation.shrinkVertically
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Checkbox
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.layout.boundsInRoot
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.testTagsAsResourceId
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.util.lerp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import app.menhir.update.UpdatePill
import app.menhir.update.UpdateSettings
import app.menhir.update.Updater
import app.menhir.update.WatchForUpdates
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** The one Mac: set it up, watch it install, start it. */
class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge()
        super.onCreate(savedInstanceState)
        // The platform splash (themes.xml) hands off by lifting its icon and fading as the home screen rises in.
        splashScreen.setOnExitAnimationListener { splash ->
            if (reduceMotion()) {
                splash.remove()
                return@setOnExitAnimationListener
            }
            val ease = PathInterpolator(0.2f, 0f, 0f, 1f)
            splash.iconView?.animate()?.scaleX(1.12f)?.scaleY(1.12f)?.translationY(-24f * resources.displayMetrics.density)
                ?.setDuration(260)?.setInterpolator(ease)?.start()
            splash.animate().alpha(0f).setStartDelay(60).setDuration(240).setInterpolator(ease)
                .withEndAction { splash.remove() }.start()
        }
        setContent {
            CompositionLocalProvider(LocalReduceMotion provides reduceMotion()) {
                MenhirTheme {
                    Surface(
                        Modifier.fillMaxSize().semantics { testTagsAsResourceId = true },
                        color = MaterialTheme.colorScheme.background,
                    ) { Home() }
                }
            }
        }
    }
}

fun startSession(context: Context, mac: Mac, options: Bundle? = null) {
    if (Updater.state.value is Updater.State.Installing) return
    context.startActivity(Intent(context, SessionActivity::class.java).putExtra(SessionActivity.MAC_ID, mac.id), options)
}

private val MaxContent = 1280.dp

/** What the home screen shows, from the one Mac and the installer. */
private enum class Phase { Setup, Installing, Failed, Ready, Running }

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun Home() {
    WatchForUpdates()
    val context = LocalContext.current
    val reduce = LocalReduceMotion.current
    val scope = rememberCoroutineScope()
    val macs by Machines.all.collectAsState()
    val mac = macs.firstOrNull() // One Mac. Any others on disk are ignored.
    val install by Installer.status.collectAsState()
    var tick by remember { mutableIntStateOf(0) }
    LaunchedEffect(Unit) {
        while (true) {
            delay(1000)
            tick++
        }
    }
    val canStart = remember(tick, install) { Engine.canStart() }
    val engineState = remember(tick) { Engine.state() }
    val running = remember(tick) { Session.active && Session.mac?.id == mac?.id }
    val batteryExempt = remember(tick) { Device.batteryExempt(context) }

    var settings by remember { mutableStateOf(false) }
    var editing by remember { mutableStateOf(false) }
    var reinstalling by remember { mutableStateOf(false) }
    var deleting by remember { mutableStateOf(false) }
    var stopping by remember { mutableStateOf(false) }
    var memoryWarning by remember { mutableStateOf<String?>(null) }

    val notify = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) {}
    fun askNotifications() {
        if (context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            notify.launch(Manifest.permission.POST_NOTIFICATIONS)
        }
    }
    fun install(m: Mac, release: Release, keep: Boolean) {
        askNotifications()
        if (Updater.state.value !is Updater.State.Installing) Installer.start(context, m, release, keep)
    }

    // The Mac's screen grows out of its card into the session, and shrinks back on return.
    val launch = remember { Animatable(if (HeroLaunch.pending && !reduce) 1f else 0f) }
    var hero by remember { mutableStateOf(Rect.Zero) }
    val lifecycle = LocalLifecycleOwner.current.lifecycle
    DisposableEffect(lifecycle) {
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_RESUME) {
                HeroLaunch.pending = false
                if (launch.value > 0f) scope.launch { delay(80); launch.animateTo(0f, spring(dampingRatio = 0.9f, stiffness = 320f)) }
            }
        }
        lifecycle.addObserver(observer)
        onDispose { lifecycle.removeObserver(observer) }
    }
    fun open(m: Mac) {
        Installer.reset()
        if (reduce || hero == Rect.Zero) {
            startSession(context, m)
            return
        }
        scope.launch {
            launch.animateTo(1f, spring(dampingRatio = 1f, stiffness = 520f))
            startSession(context, m, ActivityOptions.makeCustomAnimation(context, 0, 0).toBundle())
        }
    }
    fun start(m: Mac) {
        askNotifications()
        val warning = Device.memoryWarning(context, m.ramMB)
        if (warning != null) memoryWarning = warning else open(m)
    }

    val phase = when {
        install is InstallStatus.Working -> Phase.Installing
        install is InstallStatus.Failed -> Phase.Failed
        mac == null || !mac.installed -> Phase.Setup
        running -> Phase.Running
        else -> Phase.Ready
    }

    Box(Modifier.fillMaxSize()) {
        Box(Modifier.fillMaxSize().windowInsetsPadding(WindowInsets.safeDrawing), contentAlignment = Alignment.TopCenter) {
            Column(Modifier.widthIn(max = MaxContent).fillMaxSize().padding(horizontal = Space.xl, vertical = Space.l)) {
                Enter(0) {
                    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                        Column(Modifier.weight(1f)) {
                            Text("Menhir", style = MaterialTheme.typography.titleLarge)
                            if (Engine.isFake()) Text("Test build: the engine is simulated, nothing downloads and macOS does not run.", style = Caption)
                        }
                        UpdatePill()
                        TextButton(onClick = { settings = true }, modifier = Modifier.heightIn(min = 48.dp).testTag("settings")) { Text("Settings") }
                    }
                }
                AnimatedContent(
                    phase,
                    modifier = Modifier.weight(1f).fillMaxWidth(),
                    transitionSpec = {
                        (fadeIn(spring(stiffness = 380f)) + scaleIn(spring(dampingRatio = 0.9f, stiffness = 380f), initialScale = 0.97f))
                            .togetherWith(fadeOut(tween(90)) + scaleOut(tween(120), targetScale = 1.02f))
                            .using(SizeTransform(clip = false))
                    },
                    contentAlignment = Alignment.Center,
                    label = "phase",
                ) { p ->
                    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                        when (p) {
                            Phase.Setup -> SetUp(mac, onInstall = ::install)
                            Phase.Installing -> (install as? InstallStatus.Working)?.let { Installing(it) }
                            Phase.Failed -> Failed((install as? InstallStatus.Failed)?.text.orEmpty())
                            Phase.Ready, Phase.Running -> if (mac != null) MacHero(
                                mac = mac,
                                running = p == Phase.Running,
                                paused = engineState == Engine.PAUSED,
                                canStart = canStart,
                                done = (install as? InstallStatus.Done)?.text,
                                onScreen = { hero = it },
                                onStart = { if (p == Phase.Running) open(mac) else start(mac) },
                                onStop = { stopping = true },
                                onEdit = { editing = true },
                                onReinstall = { reinstalling = true },
                                onDelete = { deleting = true },
                            )
                        }
                    }
                }
                AnimatedVisibility(!batteryExempt, enter = fadeIn() + expandVertically(), exit = fadeOut() + shrinkVertically()) {
                    Row(Modifier.fillMaxWidth().padding(top = Space.s), verticalAlignment = Alignment.CenterVertically) {
                        Text(
                            "Android may close Menhir in the background, which stops a running Mac or an install.",
                            style = Caption, modifier = Modifier.weight(1f),
                        )
                        TextButton(onClick = { Device.askBatteryExemption(context) }) { Text("Allow background use") }
                    }
                }
            }
        }
        LaunchOverlay(hero, launch.value)
    }

    if (settings) {
        ModalBottomSheet(onDismissRequest = { settings = false }, sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)) {
            Column(
                Modifier.fillMaxWidth().padding(horizontal = Space.xl).padding(bottom = Space.xl).verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(Space.l),
            ) {
                Text("Settings", style = MaterialTheme.typography.headlineSmall)
                Row(horizontalArrangement = Arrangement.spacedBy(Space.xl)) {
                    Column(Modifier.weight(1f)) {
                        Text("Updates", style = MaterialTheme.typography.titleMedium)
                        Spacer(Modifier.height(Space.s))
                        UpdateSettings()
                    }
                    Column(Modifier.weight(1f)) { Images() }
                }
            }
        }
    }
    if (editing && mac != null) EditDialog(mac, locked = running) { editing = false }
    if (reinstalling && mac != null) ReinstallDialog(mac, onDismiss = { reinstalling = false }) { release, keep ->
        reinstalling = false
        install(mac.copy(installed = false), release, keep)
    }
    if (deleting && mac != null) {
        AlertDialog(
            onDismissRequest = { deleting = false },
            title = { Text("Delete ${mac.name}?") },
            text = { Text("This removes its ${mac.diskGB} GB disk and everything on it. You can't undo this.") },
            confirmButton = {
                TextButton(
                    onClick = { Machines.delete(mac); deleting = false },
                    colors = ButtonDefaults.textButtonColors(contentColor = MaterialTheme.colorScheme.error),
                    modifier = Modifier.testTag("confirmDelete"),
                ) { Text("Delete") }
            },
            dismissButton = { TextButton(onClick = { deleting = false }) { Text("Cancel") } },
        )
    }
    if (stopping && mac != null) {
        AlertDialog(
            onDismissRequest = { stopping = false },
            title = { Text("Stop ${mac.name}?") },
            text = { Text("This is like holding the power button: anything not saved on the Mac is lost. Shutting down from the Apple menu is safer.") },
            confirmButton = {
                TextButton(
                    onClick = {
                        stopping = false
                        val app = context.applicationContext
                        scope.launch(Dispatchers.IO) {
                            InstantResume.discard(mac)
                            Engine.stop()
                            InstantResume.discard(mac)
                            Session.ended()
                            withContext(Dispatchers.Main) { MenhirService.refresh(app) }
                        }
                    },
                    colors = ButtonDefaults.textButtonColors(contentColor = MaterialTheme.colorScheme.error),
                    modifier = Modifier.testTag("confirmStop"),
                ) { Text("Stop") }
            },
            dismissButton = { TextButton(onClick = { stopping = false }) { Text("Cancel") } },
        )
    }
    memoryWarning?.let { text ->
        AlertDialog(
            onDismissRequest = { memoryWarning = null },
            title = { Text("Memory is low") },
            text = { Text(text) },
            confirmButton = { TextButton(onClick = { memoryWarning = null; mac?.let(::open) }) { Text("Start anyway") } },
            dismissButton = { TextButton(onClick = { memoryWarning = null }) { Text("Cancel") } },
        )
    }
}

/** Black, from the Mac's screen in its card at [t] = 0 to the whole panel at 1. */
@Composable
private fun LaunchOverlay(from: Rect, t: Float) {
    if (t <= 0f || from == Rect.Zero) return
    Canvas(Modifier.fillMaxSize()) {
        val left = lerp(from.left, 0f, t)
        val top = lerp(from.top, 0f, t)
        val right = lerp(from.right, size.width, t)
        val bottom = lerp(from.bottom, size.height, t)
        val r = lerp(20.dp.toPx(), 0f, t)
        drawRoundRect(Color.Black, Offset(left, top), Size(right - left, bottom - top), CornerRadius(r, r))
    }
}

// ---- Set up ---------------------------------------------------------------------------------------

@Composable
private fun SetUp(existing: Mac?, onInstall: (Mac, Release, Boolean) -> Unit) {
    val context = LocalContext.current
    val total = remember { Device.totalGB(context) }
    val cores = remember { Runtime.getRuntime().availableProcessors() }
    var ram by remember { mutableIntStateOf(existing?.ramMB?.div(1024) ?: if (total >= 16) 8 else if (total >= 12) 6 else 4) }
    var cpus by remember { mutableIntStateOf(existing?.cpus ?: minOf(4, cores)) }
    var disk by remember { mutableIntStateOf(existing?.diskGB ?: 64) }
    var release by remember { mutableStateOf(Release.all.firstOrNull { it.name == existing?.osName } ?: Release.recommended) }
    var keep by remember { mutableStateOf(false) }
    val free = remember { Installer.freeBytes() }
    val needed = Installer.spaceNeeded(release)
    val short = !Engine.isFake() && needed > free

    Row(Modifier.fillMaxSize().padding(top = Space.l), horizontalArrangement = Arrangement.spacedBy(Space.xl * 2)) {
        Column(Modifier.weight(1f).fillMaxHeight(), verticalArrangement = Arrangement.spacedBy(Space.l)) {
            Enter(1) {
                Column {
                    Text("Set up your Mac", style = MaterialTheme.typography.displaySmall)
                    Spacer(Modifier.height(Space.s))
                    Text(
                        "Menhir downloads macOS from Apple and installs it on this tablet. It takes a while; the screen can turn off.",
                        style = Caption,
                    )
                }
            }
            Enter(2) {
                Column {
                    Segmented("Memory", listOf(4, 6, 8), ram, { "$it GB" }, tag = "ram") { ram = it }
                    AnimatedVisibility(ram > total - 4) {
                        Text(
                            "$ram GB leaves Android little room on this $total GB tablet.",
                            style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.tertiary,
                            modifier = Modifier.padding(top = Space.s),
                        )
                    }
                }
            }
            Enter(3) { Segmented("CPUs", (1..cores).filter { it % 2 == 0 || it == 1 }, cpus, { "$it" }, tag = "cpus") { cpus = it } }
            Enter(4) { Segmented("Disk", listOf(48, 64, 96, 128), disk, { "$it GB" }, tag = "disk") { disk = it } }
        }
        Column(Modifier.weight(1f).fillMaxHeight()) {
            Enter(3, Modifier.weight(1f, fill = false)) {
                Column(Modifier.verticalScroll(rememberScrollState())) {
                    ReleasePicker(release, showNotes = true) { release = it }
                    KeepImage(keep) { keep = it }
                }
            }
            Spacer(Modifier.height(Space.m))
            Enter(5) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        "Needs about ${bytesText(needed)}. ${bytesText(free)} free.",
                        style = Caption,
                        color = if (short) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.weight(1f),
                    )
                    PressButton(
                        onClick = {
                            val m = (existing ?: Mac(name = "Mac", ecid = Machines.newEcid(), ramMB = 0, cpus = 0, diskGB = 0))
                                .copy(ramMB = ram * 1024, cpus = cpus, diskGB = disk)
                            onInstall(m, release, keep)
                        },
                        large = true,
                        enabled = !Installer.busy,
                        modifier = Modifier.testTag("create"),
                    ) { Text("Download and install") }
                }
            }
        }
    }
}

@Composable
private fun ReleasePicker(selected: Release, showNotes: Boolean, onSelect: (Release) -> Unit) {
    val c = MaterialTheme.colorScheme
    Text("macOS version", style = MaterialTheme.typography.labelLarge)
    Spacer(Modifier.height(Space.s))
    Column(Modifier.selectableGroup(), verticalArrangement = Arrangement.spacedBy(Space.s)) {
        for (r in Release.all) {
            val on = r == selected
            val bg by animateColorAsState(if (on) c.secondaryContainer else Color.Transparent, Springs.move(), label = "release")
            val edge by animateColorAsState(if (on) c.primary else c.outlineVariant, Springs.move(), label = "releaseEdge")
            Row(
                Modifier.fillMaxWidth().clip(MaterialTheme.shapes.large).background(bg).border(BorderStroke(1.dp, edge), MaterialTheme.shapes.large)
                    .selectable(on, role = Role.RadioButton, onClick = { onSelect(r) })
                    .heightIn(min = 56.dp).padding(end = Space.m),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                RadioButton(on, onClick = null, modifier = Modifier.padding(horizontal = Space.m))
                Column(Modifier.weight(1f).padding(vertical = Space.s)) {
                    Text(r.name, style = MaterialTheme.typography.bodyLarge)
                    if (showNotes && r.note.isNotEmpty()) Text(r.note, style = Caption)
                }
                Text(if (r.downloaded) "Downloaded" else bytesText(r.bytes), style = Caption)
            }
        }
    }
}

@Composable
private fun KeepImage(keep: Boolean, onChange: (Boolean) -> Unit) {
    Row(
        Modifier.fillMaxWidth().heightIn(min = 48.dp).padding(top = Space.s)
            .toggleable(keep, role = Role.Checkbox, onValueChange = onChange),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Checkbox(keep, onCheckedChange = null, modifier = Modifier.padding(end = Space.m))
        Text("Keep the installer for next time")
    }
}

// ---- Installing -----------------------------------------------------------------------------------

@Composable
private fun Installing(s: InstallStatus.Working) {
    val c = MaterialTheme.colorScheme
    val p = s.progress
    val shown by animateFloatAsState((p ?: 0.0).toFloat(), spring(dampingRatio = 1f, stiffness = 120f), label = "progress")
    val eta = rememberEta(s)
    Column(Modifier.widthIn(max = 760.dp).fillMaxWidth()) {
        Enter(1) { Text("Installing macOS on ${s.macName}", style = Caption) }
        Spacer(Modifier.height(Space.s))
        Enter(2) {
            Row(verticalAlignment = Alignment.Bottom) {
                AnimatedContent(
                    s.phase,
                    transitionSpec = { (fadeIn(spring()) + androidx.compose.animation.slideInVertically { it / 3 }) togetherWith fadeOut(tween(90)) },
                    modifier = Modifier.weight(1f),
                    label = "step",
                ) { phase -> Text(phase, style = MaterialTheme.typography.displaySmall, modifier = Modifier.testTag("installPhase")) }
                if (p != null) Text("${(shown * 100).toInt()}%", style = MaterialTheme.typography.displaySmall, color = c.primary)
            }
        }
        Spacer(Modifier.height(Space.l))
        Enter(3) {
            val bar = Modifier.fillMaxWidth().height(10.dp).clip(CircleShape)
            if (p != null) LinearProgressIndicator(progress = { shown }, modifier = bar, gapSize = 0.dp, drawStopIndicator = {})
            else LinearProgressIndicator(bar, gapSize = 0.dp)
        }
        Spacer(Modifier.height(Space.m))
        Enter(4) {
            Row {
                Text(s.detail, style = Caption, modifier = Modifier.weight(1f))
                AnimatedVisibility(eta != null, enter = fadeIn(), exit = fadeOut()) { Text(eta.orEmpty(), style = MaterialTheme.typography.labelLarge) }
            }
        }
        Spacer(Modifier.height(Space.xl))
        Enter(5) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("Keep the tablet plugged in. The screen can turn off.", style = Caption, modifier = Modifier.weight(1f))
                AnimatedVisibility(s.downloading, enter = fadeIn(), exit = fadeOut()) {
                    OutlinedButton(onClick = { Installer.cancelDownload() }, modifier = Modifier.heightIn(min = 48.dp).testTag("stopDownload")) {
                        Text("Stop download")
                    }
                }
            }
        }
        if (s.downloading) Text("A stopped download picks up where it left off.", style = MaterialTheme.typography.labelMedium, color = c.onSurfaceVariant)
    }
}

/**
 * Time left, only while downloading: from the progress over the last 30 seconds, once there are 8
 * seconds of it. The restore's steps run at uneven speeds, so they get no estimate.
 */
@Composable
private fun rememberEta(s: InstallStatus.Working): String? {
    val samples = remember(s.phase) { ArrayDeque<Pair<Long, Double>>() }
    val p = s.progress ?: return null
    if (!s.downloading) return null
    val now = System.nanoTime()
    if (samples.lastOrNull()?.second != p) samples.addLast(now to p)
    while (samples.size > 2 && now - samples.first().first > 30_000_000_000L) samples.removeFirst()
    val (t0, p0) = samples.firstOrNull() ?: return null
    val seconds = (now - t0) / 1e9
    if (seconds < 8 || p <= p0) return null
    val left = ((1 - p) / ((p - p0) / seconds)).toLong()
    return when {
        left < 60 -> "Less than a minute left"
        left < 3600 -> "About ${(left + 30) / 60} min left"
        else -> "About ${left / 3600} h ${(left % 3600 + 30) / 60} min left"
    }
}

@Composable
private fun Failed(text: String) {
    val c = MaterialTheme.colorScheme
    Column(Modifier.widthIn(max = 640.dp)) {
        Enter(1) { Text("The install stopped", style = MaterialTheme.typography.displaySmall) }
        Spacer(Modifier.height(Space.m))
        Enter(2) {
            Surface(color = c.errorContainer, shape = MaterialTheme.shapes.large) {
                Text(text, color = c.onErrorContainer, modifier = Modifier.padding(Space.m).testTag("installError"))
            }
        }
        Spacer(Modifier.height(Space.m))
        Enter(3) {
            Text(
                "Nothing on the tablet is lost. A download that stopped carries on from where it was. If space ran out, delete downloaded installers in Settings.",
                style = Caption,
            )
        }
        Spacer(Modifier.height(Space.l))
        Enter(4) { PressButton(onClick = { Installer.reset() }, modifier = Modifier.testTag("tryAgain")) { Text("Try again") } }
    }
}

// ---- The Mac --------------------------------------------------------------------------------------

@Composable
private fun MacHero(
    mac: Mac,
    running: Boolean,
    paused: Boolean,
    canStart: Boolean,
    done: String?,
    onScreen: (Rect) -> Unit,
    onStart: () -> Unit,
    onStop: () -> Unit,
    onEdit: () -> Unit,
    onReinstall: () -> Unit,
    onDelete: () -> Unit,
) {
    val context = LocalContext.current
    val c = MaterialTheme.colorScheme
    val used by produceState(-1L, mac.id, running) {
        value = withContext(Dispatchers.IO) {
            try { android.system.Os.stat(mac.disk.path).st_blocks * 512L } catch (_: Exception) { -1L }
        }
    }
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(Space.xl * 2), verticalAlignment = Alignment.CenterVertically) {
        Enter(1, Modifier.weight(1.15f)) { MacScreen(mac, running, paused, onScreen) }
        Column(Modifier.weight(1f)) {
            Enter(2) {
                Column {
                    StateLine(running, paused)
                    Text(mac.name, style = MaterialTheme.typography.displayMedium, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    Text(mac.osName ?: "macOS", style = MaterialTheme.typography.titleMedium, color = c.onSurfaceVariant)
                }
            }
            Spacer(Modifier.height(Space.l))
            Enter(3) {
                Row(horizontalArrangement = Arrangement.spacedBy(Space.xl)) {
                    Spec("${mac.ramMB / 1024} GB", "Memory")
                    Spec("${mac.cpus}", if (mac.cpus == 1) "CPU" else "CPUs")
                    Spec(if (used >= 0) "${bytesText(used)} of ${mac.diskGB} GB" else "${mac.diskGB} GB", "Disk")
                }
            }
            Spacer(Modifier.height(Space.xl))
            Enter(4) {
                Column(Modifier.animateContentSize(Springs.move())) {
                    if (done != null && !running) {
                        Text(done, style = Caption, modifier = Modifier.padding(bottom = Space.m).testTag("installDone"))
                    }
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(Space.s)) {
                        when {
                            running -> {
                                PressButton(onClick = onStart, large = true, modifier = Modifier.testTag("start")) { Text("Resume") }
                                OutlinedButton(onClick = onStop, modifier = Modifier.heightIn(min = 64.dp).testTag("stop")) { Text("Stop") }
                            }
                            canStart -> PressButton(onClick = onStart, large = true, enabled = !Installer.busy, modifier = Modifier.testTag("start")) { Text("Start") }
                            else -> PressButton(onClick = { Device.restartApp(context) }, large = true, modifier = Modifier.testTag("restart")) { Text("Restart Menhir") }
                        }
                        Spacer(Modifier.weight(1f))
                        More(running, onEdit, onReinstall, onDelete)
                    }
                    if (!running && !canStart && done == null) {
                        Text(
                            "Menhir runs the Mac once each time it opens. Restart Menhir to start it again.",
                            style = Caption, modifier = Modifier.padding(top = Space.m),
                        )
                    }
                }
            }
        }
    }
}

/** A stand-in for the Mac's screen; the session grows from it. */
@Composable
private fun MacScreen(mac: Mac, running: Boolean, paused: Boolean, onScreen: (Rect) -> Unit) {
    val reduce = LocalReduceMotion.current
    val glow = if (running && !paused && !reduce) {
        rememberInfiniteTransition(label = "glow").animateFloat(
            0.55f, 1f, infiniteRepeatable(tween(1600), RepeatMode.Reverse), label = "glow",
        ).value
    } else 1f
    val dim by animateFloatAsState(if (paused) 0.55f else 1f, Springs.move(), label = "dim")
    Box(
        Modifier.fillMaxWidth().aspectRatio(3000f / 2120f)
            .onGloballyPositioned { onScreen(it.boundsInRoot()) }
            .clip(RoundedCornerShape(20.dp))
            .background(Brush.linearGradient(listOf(Color(0xFF1B2B5C), Color(0xFF3C2E73), Color(0xFF0E1A33))))
            .graphicsLayer { alpha = dim },
        contentAlignment = Alignment.Center,
    ) {
        Box(
            Modifier.size(180.dp).graphicsLayer { alpha = glow * 0.45f }
                .background(Brush.radialGradient(listOf(Color(0xFF7FA7FF), Color.Transparent)), CircleShape),
        )
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Text(mac.osName ?: "macOS", style = MaterialTheme.typography.headlineSmall, color = Color.White)
            Text("${mac.ramMB / 1024} GB · ${mac.cpus} CPUs", style = MaterialTheme.typography.labelLarge, color = Color.White.copy(alpha = 0.7f))
        }
    }
}

@Composable
private fun StateLine(running: Boolean, paused: Boolean) {
    val c = MaterialTheme.colorScheme
    val (label, dot) = when {
        running && paused -> "Paused" to c.tertiary
        running -> "Running" to c.primary
        else -> "Ready" to c.outline
    }
    val color by animateColorAsState(dot, Springs.move(), label = "state")
    Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.padding(bottom = Space.xs)) {
        Box(Modifier.size(Space.s).background(color, CircleShape))
        Spacer(Modifier.width(Space.s))
        AnimatedContent(label, transitionSpec = { fadeIn() togetherWith fadeOut() }, label = "stateText") {
            Text(it, style = MaterialTheme.typography.labelLarge, color = c.onSurfaceVariant)
        }
    }
}

@Composable
private fun More(running: Boolean, onEdit: () -> Unit, onReinstall: () -> Unit, onDelete: () -> Unit) {
    var open by remember { mutableStateOf(false) }
    Box {
        TextButton(onClick = { open = true }, modifier = Modifier.heightIn(min = 48.dp).testTag("more")) { Text("More") }
        DropdownMenu(open, onDismissRequest = { open = false }) {
            DropdownMenuItem(text = { Text("Memory and CPUs…") }, enabled = !running, onClick = { open = false; onEdit() })
            DropdownMenuItem(text = { Text("Reinstall macOS…") }, enabled = !running && !Installer.busy, onClick = { open = false; onReinstall() })
            HorizontalDivider()
            DropdownMenuItem(
                text = { Text("Delete Mac…", color = if (running) Color.Unspecified else MaterialTheme.colorScheme.error) },
                enabled = !running,
                onClick = { open = false; onDelete() },
                modifier = Modifier.testTag("delete"),
            )
        }
    }
}

@Composable
private fun Spec(value: String, label: String) {
    Column {
        Text(value, style = MaterialTheme.typography.titleLarge)
        Text(label, style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Composable
private fun EditDialog(mac: Mac, locked: Boolean, onDismiss: () -> Unit) {
    val cores = remember { Runtime.getRuntime().availableProcessors() }
    var ram by remember { mutableIntStateOf(mac.ramMB / 1024) }
    var cpus by remember { mutableIntStateOf(mac.cpus) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Memory and CPUs") },
        text = {
            Column(Modifier.widthIn(min = 420.dp), verticalArrangement = Arrangement.spacedBy(Space.l)) {
                Segmented("Memory", listOf(4, 6, 8), ram, { "$it GB" }, tag = "editRam", enabled = !locked) { ram = it }
                Segmented("CPUs", (1..cores).filter { it % 2 == 0 || it == 1 }, cpus, { "$it" }, tag = "editCpus", enabled = !locked) { cpus = it }
                Text("Changes take effect the next time the Mac starts.", style = Caption)
            }
        },
        confirmButton = {
            TextButton(enabled = !locked, onClick = { Machines.put(mac.copy(ramMB = ram * 1024, cpus = cpus)); onDismiss() }) { Text("Save") }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

@Composable
private fun ReinstallDialog(mac: Mac, onDismiss: () -> Unit, onPick: (Release, Boolean) -> Unit) {
    var release by remember { mutableStateOf(Release.all.firstOrNull { it.name == mac.osName } ?: Release.recommended) }
    var keep by remember { mutableStateOf(false) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Reinstall macOS") },
        text = {
            Column(Modifier.widthIn(min = 480.dp).verticalScroll(rememberScrollState())) {
                Text("This erases the Mac's disk.", color = MaterialTheme.colorScheme.error, modifier = Modifier.padding(bottom = Space.m))
                ReleasePicker(release, showNotes = false) { release = it }
                KeepImage(keep) { keep = it }
            }
        },
        confirmButton = {
            TextButton(
                onClick = { onPick(release, keep) },
                colors = ButtonDefaults.textButtonColors(contentColor = MaterialTheme.colorScheme.error),
                modifier = Modifier.testTag("installConfirm"),
            ) { Text("Erase and install") }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

@Composable
private fun Images() {
    var tick by remember { mutableLongStateOf(0) }
    val install by Installer.status.collectAsState()
    val present = remember(tick, install) { Release.all.filter { it.downloaded || it.part.exists() } }
    Text("Downloaded macOS", style = MaterialTheme.typography.titleMedium)
    Text(
        if (present.isEmpty()) "No installers are kept on the tablet." else "Installers kept on the tablet. Delete them to free space.",
        style = Caption,
    )
    Spacer(Modifier.height(Space.s))
    if (present.isNotEmpty()) Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainer)) {
        Column(Modifier.animateContentSize(Springs.move()).padding(horizontal = Space.m)) {
            for (r in present) {
                Row(Modifier.heightIn(min = 56.dp), verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text(r.name, style = MaterialTheme.typography.bodyLarge)
                        val what = if (r.downloaded) bytesText(r.image.length()) else "Partly downloaded, ${bytesText(r.part.length())}"
                        Text(what, style = Caption)
                    }
                    TextButton(onClick = { Installer.deleteImage(r); tick++ }, enabled = !Installer.busy) { Text("Delete") }
                }
            }
        }
    }
}
