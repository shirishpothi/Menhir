package app.menhir

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.Checkbox
import androidx.compose.material3.FilterChip
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.darkColorScheme
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
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.testTagsAsResourceId
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.delay

/** The Macs, their installs, and the restore images on the tablet. */
class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            MaterialTheme(colorScheme = darkColorScheme()) {
                Surface(Modifier.fillMaxSize().semantics { testTagsAsResourceId = true }) { Home() }
            }
        }
    }
}

fun startSession(context: Context, mac: Mac) {
    context.startActivity(Intent(context, SessionActivity::class.java).putExtra(SessionActivity.MAC_ID, mac.id))
}

@Composable
private fun Home() {
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

    Row(Modifier.fillMaxSize().padding(24.dp), horizontalArrangement = Arrangement.spacedBy(24.dp)) {
        Column(Modifier.weight(1.4f)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("Macs", style = MaterialTheme.typography.headlineMedium, modifier = Modifier.weight(1f))
                Button(
                    onClick = { askNotifications(); creating = true },
                    enabled = !Installer.busy,
                    modifier = Modifier.testTag("newMac"),
                ) { Text("New Mac") }
            }
            if (Engine.isFake()) {
                Text(
                    "Test build: the engine is simulated. Nothing is downloaded and no macOS runs.",
                    style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.tertiary,
                )
            }
            Spacer(Modifier.padding(6.dp))
            if (macs.isEmpty()) Text("No Macs yet. Make one with New Mac: it downloads macOS from Apple and installs it.")
            LazyColumn(verticalArrangement = Arrangement.spacedBy(12.dp)) {
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
        Column(Modifier.weight(1f).verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(16.dp)) {
            InstallCard(install)
            if (!canStart && sessionMac == null && !Installer.busy) {
                Card {
                    Column(Modifier.padding(16.dp)) {
                        Text("The engine runs one Mac each time Menhir starts. Restart Menhir to start a Mac.")
                        Spacer(Modifier.padding(4.dp))
                        Button(onClick = { Device.restartApp(context) }, modifier = Modifier.testTag("restart")) { Text("Restart Menhir") }
                    }
                }
            }
            if (!batteryExempt) {
                Card {
                    Column(Modifier.padding(16.dp)) {
                        Text("Android may stop Menhir in the background, which stops the Mac or the install.")
                        Spacer(Modifier.padding(4.dp))
                        OutlinedButton(onClick = { Device.askBatteryExemption(context) }) { Text("Allow background use") }
                    }
                }
            }
            Images()
        }
    }

    if (creating) NewMacDialog(onDismiss = { creating = false }) { mac, release, keep ->
        creating = false
        Installer.start(context, mac, release, keep)
    }
    installing?.let { mac ->
        ReleaseDialog(mac, onDismiss = { installing = null }) { release, keep ->
            installing = null
            Installer.start(context, mac, release, keep)
        }
    }
    deleting?.let { mac ->
        AlertDialog(
            onDismissRequest = { deleting = null },
            title = { Text("Delete ${mac.name}?") },
            text = { Text("Its disk and everything on it go. This can't be undone.") },
            confirmButton = { TextButton(onClick = { Machines.delete(mac); deleting = null }) { Text("Delete") } },
            dismissButton = { TextButton(onClick = { deleting = null }) { Text("Cancel") } },
        )
    }
    memoryWarning?.let { (mac, text) ->
        AlertDialog(
            onDismissRequest = { memoryWarning = null },
            title = { Text("Not much memory free") },
            text = { Text(text) },
            confirmButton = { TextButton(onClick = { memoryWarning = null; startSession(context, mac) }) { Text("Start anyway") } },
            dismissButton = { TextButton(onClick = { memoryWarning = null }) { Text("Cancel") } },
        )
    }
}

@Composable
private fun MacCard(mac: Mac, running: Boolean, canStart: Boolean, onStart: () -> Unit, onInstall: () -> Unit, onDelete: () -> Unit) {
    Card(Modifier.fillMaxWidth()) {
        Row(Modifier.padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text(mac.name, style = MaterialTheme.typography.titleLarge)
                val os = if (mac.installed) mac.osName ?: "macOS" else "macOS not installed"
                Text("$os · ${mac.ramMB / 1024} GB memory · ${mac.cpus} CPUs · ${mac.diskGB} GB disk", style = MaterialTheme.typography.bodyMedium)
            }
            if (mac.installed) {
                Button(onClick = onStart, enabled = running || canStart, modifier = Modifier.testTag("start")) {
                    Text(if (running) "Open" else "Start")
                }
            } else {
                Button(onClick = onInstall, enabled = canStart, modifier = Modifier.testTag("install")) { Text("Install macOS") }
            }
            Spacer(Modifier.width(8.dp))
            TextButton(onClick = onDelete, enabled = !running) { Text("Delete") }
        }
    }
}

@Composable
private fun InstallCard(s: InstallStatus) {
    when (s) {
        InstallStatus.Idle -> return
        is InstallStatus.Working -> Card(Modifier.fillMaxWidth().testTag("installCard")) {
            Column(Modifier.padding(16.dp)) {
                Text("Installing macOS on ${s.macName}", style = MaterialTheme.typography.titleMedium)
                Text(s.phase + if (s.detail.isNotEmpty()) " · ${s.detail}" else "", Modifier.testTag("installPhase"))
                Spacer(Modifier.padding(4.dp))
                val p = s.progress
                if (p != null) LinearProgressIndicator(progress = { p.toFloat() }, modifier = Modifier.fillMaxWidth())
                else LinearProgressIndicator(Modifier.fillMaxWidth())
                Spacer(Modifier.padding(4.dp))
                Text("Keep the tablet plugged in. The screen can turn off.", style = MaterialTheme.typography.bodySmall)
                if (s.downloading) TextButton(onClick = { Installer.cancelDownload() }) { Text("Stop download") }
            }
        }
        is InstallStatus.Done -> Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp)) {
                Text(s.text, Modifier.testTag("installDone"))
                TextButton(onClick = { Installer.reset() }) { Text("OK") }
            }
        }
        is InstallStatus.Failed -> Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp)) {
                Text("The install stopped", style = MaterialTheme.typography.titleMedium)
                Text(s.text)
                TextButton(onClick = { Installer.reset() }) { Text("OK") }
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
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp)) {
            Text("Restore images", style = MaterialTheme.typography.titleMedium)
            for (r in present) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    val what = if (r.downloaded) bytesText(r.image.length()) else "stopped at ${bytesText(r.part.length())}"
                    Text("${r.name} · $what", Modifier.weight(1f))
                    TextButton(onClick = { Installer.deleteImage(r); tick++ }, enabled = !Installer.busy) { Text("Delete") }
                }
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
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("New Mac") },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(
                    name, { name = it }, label = { Text("Name") }, singleLine = true,
                    keyboardOptions = KeyboardOptions(capitalization = KeyboardCapitalization.Words),
                    modifier = Modifier.testTag("name"),
                )
                Choice("Memory", listOf(4, 6, 8), ram, { "$it GB" }) { ram = it }
                if (ram > total - 4) Text("$ram GB leaves Android little room on this ${total} GB tablet.", style = MaterialTheme.typography.bodySmall)
                Choice("CPUs", (1..cores).filter { it % 2 == 0 || it == 1 }, cpus, { "$it" }) { cpus = it }
                Choice("Disk", listOf(48, 64, 96, 128), disk, { "$it GB" }) { disk = it }
                Text("macOS", style = MaterialTheme.typography.labelLarge)
                for (r in Release.all) {
                    Row(
                        Modifier.fillMaxWidth().selectable(r == release, onClick = { release = r }),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        RadioButton(r == release, onClick = { release = r })
                        Column {
                            Text(r.name + if (r.downloaded) " · downloaded" else " · ${bytesText(r.bytes)}")
                            Text(r.note, style = MaterialTheme.typography.bodySmall)
                        }
                    }
                }
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Checkbox(keep, { keep = it })
                    Text("Keep the restore image afterwards")
                }
                val free = Installer.freeBytes()
                Text(
                    "Needs about ${bytesText(Installer.spaceNeeded(release))}; ${bytesText(free)} free.",
                    style = MaterialTheme.typography.bodySmall,
                )
            }
        },
        confirmButton = {
            TextButton(
                onClick = {
                    val mac = Mac(name = name.trim().ifEmpty { "Mac" }, ecid = Machines.newEcid(), ramMB = ram * 1024, cpus = cpus, diskGB = disk)
                    onCreate(mac, release, keep)
                },
                modifier = Modifier.testTag("create"),
            ) { Text("Create and install") }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

@Composable
private fun ReleaseDialog(mac: Mac, onDismiss: () -> Unit, onPick: (Release, Boolean) -> Unit) {
    var release by remember { mutableStateOf(Release.all.firstOrNull { it.name == mac.osName } ?: Release.recommended) }
    var keep by remember { mutableStateOf(false) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Install macOS on ${mac.name}") },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                Text("This erases the Mac's disk.")
                for (r in Release.all) {
                    Row(Modifier.fillMaxWidth().selectable(r == release, onClick = { release = r }), verticalAlignment = Alignment.CenterVertically) {
                        RadioButton(r == release, onClick = { release = r })
                        Text(r.name + if (r.downloaded) " · downloaded" else " · ${bytesText(r.bytes)}")
                    }
                }
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Checkbox(keep, { keep = it })
                    Text("Keep the restore image afterwards")
                }
            }
        },
        confirmButton = { TextButton(onClick = { onPick(release, keep) }) { Text("Install") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

@Composable
private fun <T> Choice(label: String, options: List<T>, selected: T, text: (T) -> String, onSelect: (T) -> Unit) {
    Column {
        Text(label, style = MaterialTheme.typography.labelLarge)
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            for (o in options) FilterChip(selected = o == selected, onClick = { onSelect(o) }, label = { Text(text(o)) })
        }
    }
}
