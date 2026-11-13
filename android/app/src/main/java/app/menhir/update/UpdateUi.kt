package app.menhir.update

import android.content.Intent
import android.net.Uri
import android.provider.Settings
import androidx.compose.animation.animateContentSize
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import app.menhir.*
import kotlinx.coroutines.*

/** Checks only while the home screen is visible; installs outlive that screen. */
@Composable
fun WatchForUpdates() {
    val context = LocalContext.current.applicationContext
    val lifecycle = LocalLifecycleOwner.current.lifecycle
    var visible by remember { mutableStateOf(lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED)) }
    DisposableEffect(lifecycle) {
        val observer = LifecycleEventObserver { _, _ -> visible = lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED) }
        lifecycle.addObserver(observer)
        onDispose { lifecycle.removeObserver(observer) }
    }
    LaunchedEffect(visible) {
        if (!visible) return@LaunchedEffect
        delay(Updater.SETTLE_MS)
        launch { Updater.listen(context) }
        withContext(Dispatchers.IO) {
            while (isActive) {
                val ok = Updater.check(context)
                delay(if (ok) Updater.CHECK_EVERY_MS else Updater.RETRY_MS)
            }
        }
    }
}

@Composable
fun UpdatePill() {
    val state by Updater.state.collectAsState()
    if (state is Updater.State.Idle) return
    val context = LocalContext.current.applicationContext
    val release = when (val s = state) {
        is Updater.State.Available -> s.release
        is Updater.State.Failed -> s.release
        else -> null
    }
    val label = when (val s = state) {
        is Updater.State.Available -> "${if (s.newer) "Newer update" else "Update to"} ${s.release.versionName}"
        is Updater.State.Downloading -> "Downloading ${s.release.versionName} · ${s.percent}%"
        is Updater.State.Waiting -> s.reason
        is Updater.State.Installing -> "Installing ${s.release.versionName}…"
        is Updater.State.Failed -> "Update failed · Retry"
        else -> ""
    }
    val progress = when (val s = state) {
        is Updater.State.Downloading -> s.percent / 100f
        is Updater.State.Waiting, is Updater.State.Installing -> 1f
        else -> 0f
    }
    val fill = MaterialTheme.colorScheme.primary.copy(alpha = .22f)
    val shown by androidx.compose.animation.core.animateFloatAsState(progress, Springs.move(), label = "updateFill")
    Box(Modifier.padding(end = Space.m).animateContentSize(Springs.move()).clip(RoundedCornerShape(50))
        .background(MaterialTheme.colorScheme.secondaryContainer)
        .drawBehind { drawRect(fill, size = size.copy(width = size.width * shown.coerceIn(0f, 1f))) }
        .clickable(enabled = release != null) {
            // Application scope: leaving this screen must not interrupt an install session.
            release?.let { Thread({ Updater.install(context, it) }, "menhir-update").start() }
        }.padding(horizontal = Space.m, vertical = Space.m)) {
        Text(label, style = MaterialTheme.typography.labelLarge)
    }
}

@Composable
fun UpdateSettings() {
    val context = LocalContext.current
    var saved by remember { mutableStateOf(Updater.hasToken(context)) }
    var token by remember { mutableStateOf("") }
    val result by Updater.lastCheck.collectAsState()
    val scope = rememberCoroutineScope()
    Column(verticalArrangement = Arrangement.spacedBy(Space.s)) {
        Text("Menhir ${BuildConfig.VERSION_NAME} (${BuildConfig.VERSION_CODE})", style = Caption)
        Row(verticalAlignment = Alignment.CenterVertically) {
            OutlinedTextField(token, { token = it }, Modifier.weight(1f),
                label = { Text(if (saved) "Update token saved" else "Update token") },
                singleLine = true, visualTransformation = PasswordVisualTransformation())
            TextButton(enabled = token.isNotBlank(), onClick = {
                Updater.saveToken(context, token)
                token = ""
                saved = true
                scope.launch(Dispatchers.IO) { Updater.check(context) }
            }) { Text("Save") }
        }
        Text(result.ifEmpty { "Not checked yet" }, style = Caption)
        Text("To reopen after updating, allow Display over other apps, Battery usage → Allow background activity, and Display pop-ups while in background (search Settings for pop-ups).", style = Caption)
        TextButton(onClick = {
            context.startActivity(Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION, Uri.parse("package:${context.packageName}")))
        }) { Text("Allow reopening") }
    }
}
