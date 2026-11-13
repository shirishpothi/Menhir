package app.menhir

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
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
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Checkbox
import androidx.compose.material3.ElevatedCard
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedCard
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.testTagsAsResourceId
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import kotlinx.coroutines.delay

/** The Macs, their installs, and the restore images on the tablet. */
class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge()
        super.onCreate(savedInstanceState)
        setContent {
            MenhirTheme {
                Surface(
                    Modifier.fillMaxSize().semantics { testTagsAsResourceId = true },
                    color = MaterialTheme.colorScheme.background,
                ) { Home() }
            }
        }
    }
}

fun startSession(context: Context, mac: Mac) {
    if (app.menhir.update.Updater.state.value is app.menhir.update.Updater.State.Installing) return
    context.startActivity(Intent(context, SessionActivity::class.java).putExtra(SessionActivity.MAC_ID, mac.id))
}

private val MaxContent = 1280.dp
private val SidePane = 360.dp

@Composable
private fun Home() {
    app.menhir.update.WatchForUpdates()
    val context = LocalContext.current
    val macs by Machines.all.collectAsState()
    val install by Installer.status.collectAsState()
    var creating by remember { mutableStateOf(false) }
    var deleting by remember { mutableStateOf<Mac?>(null) }
    var installing by remember { mutableStateOf<Mac?>(null) }
    var memoryWarning by remember { mutableStateOf<Pair<Mac, String>?>(null) }
    var tick by remember { mutableIntStateOf(0) }
    LaunchedEffect(Unit) {
        while (true) {
            delay(1000)
            tick++
        }
    }
    val canStart = remember(tick, install) { Engine.canStart() }
    val sessionMac = remember(tick) { Session.mac.takeIf { Session.active } }
    val batteryExempt = remember(tick) { Device.batteryExempt(context) }
    val notify = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) {}
    fun askNotifications() {
        if (context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            notify.launch(Manifest.permission.POST_NOTIFICATIONS)
        }
    }
    fun start(mac: Mac) {
        askNotifications()
        val warning = Device.memoryWarning(context, mac.ramMB)
        if (warning != null) memoryWarning = mac to warning else startSession(context, mac)
    }
    val newMac = { askNotifications(); creating = true }

    Box(Modifier.fillMaxSize().windowInsetsPadding(WindowInsets.safeDrawing), contentAlignment = Alignment.TopCenter) {
        Column(Modifier.widthIn(max = MaxContent).fillMaxSize().padding(horizontal = Space.xl, vertical = Space.l)) {
            // Header
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text("Macs", style = MaterialTheme.typography.headlineMedium)
                    if (Engine.isFake()) {
                        Text("Test build. The engine is simulated: nothing downloads and macOS does not run.", style = Caption)
                    }
                }
                app.menhir.update.UpdatePill()
                if (macs.isNotEmpty()) {
                    Button(
                        onClick = newMac,
                        enabled = !Installer.busy,
                        modifier = Modifier.heightIn(min = 48.dp).testTag("newMac"),
                    ) { Text("New Mac") }
                }
            }
            Spacer(Modifier.height(Space.l))

            Row(Modifier.fillMaxSize(), horizontalArrangement = Arrangement.spacedBy(Space.l)) {
                Box(Modifier.weight(1f).fillMaxHeight()) {
                    if (macs.isEmpty()) {
                        EmptyState(enabled = !Installer.busy, onNew = newMac)
                    } else {
                        LazyVerticalGrid(
                            columns = GridCells.Adaptive(minSize = 320.dp),
                            horizontalArrangement = Arrangement.spacedBy(Space.m),
                            verticalArrangement = Arrangement.spacedBy(Space.m),
                            contentPadding = PaddingValues(bottom = Space.l),
                        ) {
                            items(macs, key = { it.id }) { mac ->
                                MacCard(
                                    mac,
                                    running = sessionMac?.id == mac.id,
                                    canStart = canStart && !Installer.busy,
                                    onStart = { if (sessionMac?.id == mac.id) startSession(context, mac) else start(mac) },
                                    onInstall = { installing = mac },
                                    onDelete = { deleting = mac },
                                )
                            }
                        }
                    }
                }
                Column(
                    Modifier.width(SidePane).fillMaxHeight().verticalScroll(rememberScrollState()),
                    verticalArrangement = Arrangement.spacedBy(Space.m),
                ) {
                    InstallCard(install)
                    if (!canStart && sessionMac == null && !Installer.busy) {
                        Notice(
                            "Restart to start a Mac",
                            "Menhir runs one Mac each time it opens. Restart Menhir to start another.",
                        ) {
                            Button(onClick = { Device.restartApp(context) }, modifier = Modifier.testTag("restart")) { Text("Restart Menhir") }
                        }
                    }
                    if (!batteryExempt) {
                        Notice(
                            "Background use is limited",
                            "Android may close Menhir in the background, which stops a running Mac or an install.",
                        ) {
                            OutlinedButton(onClick = { Device.askBatteryExemption(context) }) { Text("Allow background use") }
                        }
                    }
                    app.menhir.update.UpdateSettings()
                    Images()
                }
            }
        }
    }

    if (creating) NewMacDialog(onDismiss = { creating = false }) { mac, release, keep ->
        creating = false
        if (app.menhir.update.Updater.state.value !is app.menhir.update.Updater.State.Installing) Installer.start(context, mac, release, keep)
    }
    installing?.let { mac ->
        ReleaseDialog(mac, onDismiss = { installing = null }) { release, keep ->
            installing = null
            if (app.menhir.update.Updater.state.value !is app.menhir.update.Updater.State.Installing) Installer.start(context, mac, release, keep)
        }
    }
    deleting?.let { mac ->
        AlertDialog(
            onDismissRequest = { deleting = null },
            title = { Text("Delete ${mac.name}?") },
            text = { Text("This removes its ${mac.diskGB} GB disk and everything on it. You can't undo this.") },
            confirmButton = {
                TextButton(
                    onClick = { Machines.delete(mac); deleting = null },
                    colors = ButtonDefaults.textButtonColors(contentColor = MaterialTheme.colorScheme.error),
                    modifier = Modifier.testTag("confirmDelete"),
                ) { Text("Delete") }
            },
            dismissButton = { TextButton(onClick = { deleting = null }) { Text("Cancel") } },
        )
    }
    memoryWarning?.let { (mac, text) ->
        AlertDialog(
            onDismissRequest = { memoryWarning = null },
            title = { Text("Memory is low") },
            text = { Text(text) },
            confirmButton = { TextButton(onClick = { memoryWarning = null; startSession(context, mac) }) { Text("Start anyway") } },
            dismissButton = { TextButton(onClick = { memoryWarning = null }) { Text("Cancel") } },
        )
    }
}

@Composable
private fun EmptyState(enabled: Boolean, onNew: () -> Unit) {
    OutlinedCard(Modifier.fillMaxWidth()) {
        Column(
            Modifier.fillMaxWidth().padding(vertical = 64.dp, horizontal = Space.xl),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Text("No Macs yet", style = MaterialTheme.typography.titleLarge)
            Spacer(Modifier.height(Space.s))
            Text(
                "Make a Mac to get started. Menhir downloads macOS from Apple and installs it, which takes a while.",
                style = Caption, textAlign = TextAlign.Center, modifier = Modifier.widthIn(max = 440.dp),
            )
            Spacer(Modifier.height(Space.l))
            Button(onClick = onNew, enabled = enabled, modifier = Modifier.heightIn(min = 48.dp).testTag("newMac")) { Text("New Mac") }
        }
    }
}

private enum class MacState(val label: String) { Running("Running"), Ready("Ready"), NotInstalled("Not installed") }

@Composable
private fun StateBadge(state: MacState) {
    val c = MaterialTheme.colorScheme
    val (bg, fg) = when (state) {
        MacState.Running -> c.primaryContainer to c.onPrimaryContainer
        MacState.Ready -> c.secondaryContainer to c.onSecondaryContainer
        MacState.NotInstalled -> c.surfaceContainerHighest to c.onSurfaceVariant
    }
    Row(
        Modifier.background(bg, CircleShape).padding(horizontal = 10.dp, vertical = Space.xs),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (state == MacState.Running) {
            Box(Modifier.size(Space.s).background(c.primary, CircleShape))
            Spacer(Modifier.width(6.dp))
        }
        Text(state.label, style = MaterialTheme.typography.labelMedium, color = fg)
    }
}

@Composable
private fun MacCard(mac: Mac, running: Boolean, canStart: Boolean, onStart: () -> Unit, onInstall: () -> Unit, onDelete: () -> Unit) {
    val state = when {
        running -> MacState.Running
        mac.installed -> MacState.Ready
        else -> MacState.NotInstalled
    }
    ElevatedCard(Modifier.fillMaxWidth(), colors = CardDefaults.elevatedCardColors(containerColor = MaterialTheme.colorScheme.surfaceContainerLow)) {
        Column(Modifier.padding(Space.l)) {
            Row(verticalAlignment = Alignment.Top) {
                Column(Modifier.weight(1f)) {
                    Text(mac.name, style = MaterialTheme.typography.titleLarge, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    Text(if (mac.installed) mac.osName ?: "macOS" else "macOS not installed", style = Caption)
                }
                Spacer(Modifier.width(Space.s))
                StateBadge(state)
            }
            Spacer(Modifier.height(Space.m))
            Row(horizontalArrangement = Arrangement.spacedBy(Space.l)) {
                Spec("${mac.ramMB / 1024} GB", "Memory")
                Spec("${mac.cpus}", if (mac.cpus == 1) "CPU" else "CPUs")
                Spec("${mac.diskGB} GB", "Disk")
            }
            Spacer(Modifier.height(Space.m))
            HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
            Spacer(Modifier.height(Space.m))
            Row(verticalAlignment = Alignment.CenterVertically) {
                TextButton(
                    onClick = onDelete,
                    enabled = !running,
                    colors = ButtonDefaults.textButtonColors(contentColor = MaterialTheme.colorScheme.error),
                    modifier = Modifier.heightIn(min = 48.dp).testTag("delete"),
                ) { Text("Delete") }
                Spacer(Modifier.weight(1f))
                if (mac.installed) {
                    Button(onClick = onStart, enabled = running || canStart, modifier = Modifier.heightIn(min = 48.dp).testTag("start")) {
                        Text(if (running) "Open" else "Start")
                    }
                } else {
                    FilledTonalButton(onClick = onInstall, enabled = canStart, modifier = Modifier.heightIn(min = 48.dp).testTag("install")) {
                        Text("Install macOS")
                    }
                }
            }
        }
    }
}

@Composable
private fun Spec(value: String, label: String) {
    Column {
        Text(value, style = MaterialTheme.typography.titleMedium)
        Text(label, style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

/** A side-pane card: a title, a sentence, and an action. */
@Composable
private fun Notice(title: String, text: String, action: @Composable () -> Unit) {
    Card(Modifier.fillMaxWidth(), colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainer)) {
        Column(Modifier.padding(Space.m + Space.xs), verticalArrangement = Arrangement.spacedBy(Space.s)) {
            Text(title, style = MaterialTheme.typography.titleMedium)
            Text(text, style = Caption)
            Box(Modifier.padding(top = Space.xs)) { action() }
        }
    }
}

@Composable
private fun InstallCard(s: InstallStatus) {
    val c = MaterialTheme.colorScheme
    when (s) {
        InstallStatus.Idle -> return
        is InstallStatus.Working -> Card(
            Modifier.fillMaxWidth().testTag("installCard"),
            colors = CardDefaults.cardColors(containerColor = c.surfaceContainerHigh),
        ) {
            Column(Modifier.padding(Space.m + Space.xs)) {
                Text("Installing macOS on ${s.macName}", style = Caption)
                Spacer(Modifier.height(Space.xs))
                Text(s.phase, style = MaterialTheme.typography.titleMedium, modifier = Modifier.testTag("installPhase"))
                Spacer(Modifier.height(Space.m))
                val p = s.progress
                if (p != null) {
                    LinearProgressIndicator(progress = { p.toFloat() }, modifier = Modifier.fillMaxWidth().height(6.dp))
                } else {
                    LinearProgressIndicator(Modifier.fillMaxWidth().height(6.dp))
                }
                Spacer(Modifier.height(Space.s))
                Row {
                    Text(s.detail, style = Caption, modifier = Modifier.weight(1f))
                    if (p != null) Text("${(p * 100).toInt()}%", style = MaterialTheme.typography.labelLarge)
                }
                Spacer(Modifier.height(Space.m))
                Text("Keep the tablet plugged in. The screen can turn off.", style = Caption)
                if (s.downloading) {
                    Spacer(Modifier.height(Space.s))
                    OutlinedButton(onClick = { Installer.cancelDownload() }, modifier = Modifier.testTag("stopDownload")) { Text("Stop download") }
                    Text("The download picks up where it stopped next time.", style = MaterialTheme.typography.labelMedium, color = c.onSurfaceVariant)
                }
            }
        }
        is InstallStatus.Done -> Card(Modifier.fillMaxWidth(), colors = CardDefaults.cardColors(containerColor = c.primaryContainer)) {
            Column(Modifier.padding(Space.m + Space.xs)) {
                Text("Install finished", style = MaterialTheme.typography.titleMedium, color = c.onPrimaryContainer)
                Spacer(Modifier.height(Space.xs))
                Text(s.text, color = c.onPrimaryContainer, modifier = Modifier.testTag("installDone"))
                TextButton(onClick = { Installer.reset() }, modifier = Modifier.padding(top = Space.s)) { Text("OK") }
            }
        }
        is InstallStatus.Failed -> Card(Modifier.fillMaxWidth(), colors = CardDefaults.cardColors(containerColor = c.errorContainer)) {
            Column(Modifier.padding(Space.m + Space.xs)) {
                Text("The install stopped", style = MaterialTheme.typography.titleMedium, color = c.onErrorContainer)
                Spacer(Modifier.height(Space.xs))
                Text(s.text, color = c.onErrorContainer, modifier = Modifier.testTag("installError"))
                TextButton(
                    onClick = { Installer.reset() },
                    colors = ButtonDefaults.textButtonColors(contentColor = c.onErrorContainer),
                    modifier = Modifier.padding(top = Space.s),
                ) { Text("OK") }
            }
        }
    }
}

@Composable
private fun Images() {
    var tick by remember { mutableIntStateOf(0) }
    val install by Installer.status.collectAsState()
    val present = remember(tick, install) { Release.all.filter { it.downloaded || it.part.exists() } }
    if (present.isEmpty()) return
    Card(Modifier.fillMaxWidth(), colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainer)) {
        Column(Modifier.padding(Space.m + Space.xs)) {
            Text("Downloaded macOS", style = MaterialTheme.typography.titleMedium)
            Text("Installers kept on the tablet. Delete them to free space.", style = Caption)
            Spacer(Modifier.height(Space.s))
            for (r in present) {
                Row(Modifier.heightIn(min = 48.dp), verticalAlignment = Alignment.CenterVertically) {
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

/** A large dialog sized for the tablet, with a title, a body and right-aligned actions. */
@Composable
private fun SheetDialog(
    title: String,
    onDismiss: () -> Unit,
    maxWidth: androidx.compose.ui.unit.Dp,
    actions: @Composable () -> Unit,
    body: @Composable () -> Unit,
) {
    Dialog(onDismissRequest = onDismiss, properties = DialogProperties(usePlatformDefaultWidth = false)) {
        Surface(
            Modifier.widthIn(max = maxWidth).fillMaxWidth(0.9f).padding(vertical = Space.l),
            shape = MaterialTheme.shapes.extraLarge,
            color = MaterialTheme.colorScheme.surfaceContainerHigh,
        ) {
            Column(Modifier.padding(Space.l)) {
                Text(title, style = MaterialTheme.typography.headlineSmall)
                Spacer(Modifier.height(Space.m))
                Box(Modifier.weight(1f, fill = false).verticalScroll(rememberScrollState())) { body() }
                Spacer(Modifier.height(Space.l))
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(Space.s, Alignment.End)) { actions() }
            }
        }
    }
}

@Composable
private fun NewMacDialog(onDismiss: () -> Unit, onCreate: (Mac, Release, Boolean) -> Unit) {
    val context = LocalContext.current
    val total = remember { Device.totalGB(context) }
    val cores = remember { Runtime.getRuntime().availableProcessors() }
    var name by remember { mutableStateOf("Mac") }
    var ram by remember { mutableIntStateOf(if (total >= 16) 8 else if (total >= 12) 6 else 4) }
    var cpus by remember { mutableIntStateOf(minOf(4, cores)) }
    var disk by remember { mutableIntStateOf(64) }
    var release by remember { mutableStateOf(Release.recommended) }
    var keep by remember { mutableStateOf(false) }
    val free = remember { Installer.freeBytes() }
    val needed = Installer.spaceNeeded(release)
    SheetDialog(
        title = "New Mac",
        onDismiss = onDismiss,
        maxWidth = 920.dp,
        actions = {
            TextButton(onClick = onDismiss, modifier = Modifier.heightIn(min = 48.dp)) { Text("Cancel") }
            Button(
                onClick = {
                    val mac = Mac(name = name.trim().ifEmpty { "Mac" }, ecid = Machines.newEcid(), ramMB = ram * 1024, cpus = cpus, diskGB = disk)
                    onCreate(mac, release, keep)
                },
                modifier = Modifier.heightIn(min = 48.dp).testTag("create"),
            ) { Text("Create and install") }
        },
    ) {
        Row(horizontalArrangement = Arrangement.spacedBy(Space.xl)) {
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(Space.l)) {
                OutlinedTextField(
                    name, { name = it }, label = { Text("Name") }, singleLine = true,
                    keyboardOptions = KeyboardOptions(capitalization = KeyboardCapitalization.Words),
                    modifier = Modifier.fillMaxWidth().testTag("name"),
                )
                Column {
                    Segmented("Memory", listOf(4, 6, 8), ram, { "$it GB" }, tag = "ram") { ram = it }
                    if (ram > total - 4) {
                        Text(
                            "$ram GB leaves Android little room on this $total GB tablet.",
                            style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.tertiary,
                            modifier = Modifier.padding(top = Space.s),
                        )
                    }
                }
                Segmented("CPUs", (1..cores).filter { it % 2 == 0 || it == 1 }, cpus, { "$it" }, tag = "cpus") { cpus = it }
                Segmented("Disk", listOf(48, 64, 96, 128), disk, { "$it GB" }, tag = "disk") { disk = it }
            }
            Column(Modifier.weight(1f)) {
                ReleasePicker(release, showNotes = true) { release = it }
                KeepImage(keep) { keep = it }
                Text(
                    "Needs about ${bytesText(needed)}. ${bytesText(free)} free.",
                    style = Caption,
                    color = if (needed > free) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(top = Space.s),
                )
            }
        }
    }
}

@Composable
private fun ReleaseDialog(mac: Mac, onDismiss: () -> Unit, onPick: (Release, Boolean) -> Unit) {
    var release by remember { mutableStateOf(Release.all.firstOrNull { it.name == mac.osName } ?: Release.recommended) }
    var keep by remember { mutableStateOf(false) }
    SheetDialog(
        title = "Install macOS on ${mac.name}",
        onDismiss = onDismiss,
        maxWidth = 560.dp,
        actions = {
            TextButton(onClick = onDismiss, modifier = Modifier.heightIn(min = 48.dp)) { Text("Cancel") }
            Button(onClick = { onPick(release, keep) }, modifier = Modifier.heightIn(min = 48.dp).testTag("installConfirm")) { Text("Install") }
        },
    ) {
        Column {
            Text("This erases the Mac's disk.", color = MaterialTheme.colorScheme.error, modifier = Modifier.padding(bottom = Space.m))
            ReleasePicker(release, showNotes = false) { release = it }
            KeepImage(keep) { keep = it }
        }
    }
}

@Composable
private fun ReleasePicker(selected: Release, showNotes: Boolean, onSelect: (Release) -> Unit) {
    Text("macOS version", style = MaterialTheme.typography.labelLarge)
    Spacer(Modifier.height(Space.s))
    Column(Modifier.selectableGroup(), verticalArrangement = Arrangement.spacedBy(Space.s)) {
        for (r in Release.all) {
            val on = r == selected
            OutlinedCard(
                Modifier.fillMaxWidth().selectable(on, role = Role.RadioButton, onClick = { onSelect(r) }),
                colors = CardDefaults.outlinedCardColors(
                    containerColor = if (on) MaterialTheme.colorScheme.secondaryContainer else Color.Transparent,
                ),
                border = BorderStroke(1.dp, if (on) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.outlineVariant),
            ) {
                Row(Modifier.heightIn(min = 56.dp).padding(end = Space.m), verticalAlignment = Alignment.CenterVertically) {
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

@Composable
private fun <T> Segmented(label: String, options: List<T>, selected: T, text: (T) -> String, tag: String, onSelect: (T) -> Unit) {
    Column {
        Text(label, style = MaterialTheme.typography.labelLarge)
        Spacer(Modifier.height(Space.s))
        SingleChoiceSegmentedButtonRow(Modifier.fillMaxWidth().testTag(tag)) {
            options.forEachIndexed { i, o ->
                SegmentedButton(
                    selected = o == selected,
                    onClick = { onSelect(o) },
                    shape = SegmentedButtonDefaults.itemShape(i, options.size),
                    modifier = Modifier.heightIn(min = 48.dp),
                ) { Text(text(o)) }
            }
        }
    }
}
