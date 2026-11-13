package app.menhir.update

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller
import android.os.SystemClock
import android.provider.Settings
import android.util.Log
import app.menhir.BuildConfig
import app.menhir.Installer
import app.menhir.Session
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.text.DateFormat
import java.util.Date

/**
 * Updates Menhir from the latest GitHub release of its private repository, which holds one
 * `menhir-<versionCode>.apk`. Android's installer applies it in place, so Macs and downloads are
 * untouched, and it refuses anything not signed with the same key. The repository's read-only token
 * is entered on the tablet and stays in app storage that's never backed up, so it isn't in the APK
 * or the repository. Replacing the app kills the engine, so an install never starts while a Mac is
 * running or macOS is installing: the download finishes and then waits ([State.Waiting]).
 */
object Updater {
    class Asset(val url: String, val size: Long)

    class Release(val versionName: String, val versionCode: Int, val apk: Asset)

    sealed interface State {
        data object Idle : State
        /** [newer]: it replaced an update that was already on offer, so the pill says so. */
        data class Available(val release: Release, val newer: Boolean = false) : State
        /** [newer]: a newer release came out mid-download, and this download switched to it. */
        data class Downloading(val release: Release, val percent: Int, val newer: Boolean = false) : State
        /** Downloaded, held back until no Mac is running and no install of macOS is in progress. */
        data class Waiting(val release: Release, val reason: String) : State
        data class Installing(val release: Release) : State
        data class Failed(val release: Release) : State
    }

    val state = MutableStateFlow<State>(State.Idle)

    /** The outcome of the last check, for the Macs screen. Empty until one has run. */
    val lastCheck = MutableStateFlow("")

    private const val LATEST = "https://api.github.com/repos/shirishpothi/Menhir/releases/latest"
    /**
     * A backup for when the push channel ([listen]) is down. Repeat checks send the last ETag, so an
     * unchanged release is an empty 304 that GitHub doesn't rate-limit.
     */
    const val CHECK_EVERY_MS = 5 * 60 * 1000L

    /**
     * ntfy.sh topic that `scripts/publish-update.sh` pings with the new versionCode once a release is up.
     * It's only a nudge to check GitHub now: a stray ping can't offer or install anything. The script
     * reads the topic from this line.
     */
    private const val PUSH_TOPIC = "menhir-0e2d62add5685a841a96f76b"

    /** The Macs screen waits this long before going online, so arriving there stays smooth. */
    const val SETTLE_MS = 1_000L

    /** A failed check (often Wi-Fi still reconnecting after the tablet wakes) tries again this soon. */
    const val RETRY_MS = 15 * 1000L

    /** Coming back to the Macs screen checks again after this long, without waiting for the next periodic check. */
    private const val MIN_GAP_MS = 30 * 1000L
    private val assetName = Regex("""menhir-(\d+)\.apk""")
    private var lastCheckAt = 0L
    private val checkLock = Any()
    private var etag: String? = null
    private var cached: Release? = null
    private var installSession = -1

    private fun tokenFile(context: Context) = File(context.noBackupFilesDir, "update-token")

    /** Left by an update started from the pill, so only that update reopens Menhir, not an adb install. */
    internal fun reopenMarker(context: Context) = File(context.noBackupFilesDir, "reopen-after-update")

    /**
     * Android kills Menhir to replace it and offers no way to relaunch it. The new build may only start
     * its own window from the background with "Display over other apps", which is granted once in Settings.
     * ColorOS also wants Battery usage → "Allow background activity" ([app.menhir.Device.batteryExempt])
     * and "Display pop-ups while in background", which apps can't read.
     */
    fun canReopen(context: Context) = Settings.canDrawOverlays(context)

    private fun token(context: Context) = tokenFile(context).takeIf { it.isFile }?.readText()?.trim().orEmpty()

    fun hasToken(context: Context) = token(context).isNotEmpty()

    fun saveToken(context: Context, token: String) {
        synchronized(checkLock) {
            tokenFile(context).writeText(token.trim())
            lastCheckAt = 0L
            etag = null
        }
    }

    /** Why an install can't start now, or null when it can. Replacing the app would kill the engine. */
    private fun busyReason(): String? = when {
        Session.active -> "Stop the Mac to install"
        Installer.busy -> "Waits for the macOS install"
        else -> null
    }

    private fun offeredCode(): Int? = when (val s = state.value) {
        State.Idle -> BuildConfig.VERSION_CODE
        is State.Available -> s.release.versionCode
        is State.Failed -> s.release.versionCode
        // Still checked while downloading or waiting, so a newer release can take over the download.
        is State.Downloading -> maxOf(s.release.versionCode, supersede?.versionCode ?: 0)
        is State.Waiting -> maxOf(s.release.versionCode, supersede?.versionCode ?: 0)
        is State.Installing -> null
    }

    /** A newer release found mid-download; the download stops and starts again with it. */
    @Volatile private var supersede: Release? = null

    private class Superseded : java.io.IOException("a newer release came out")

    private fun now() = DateFormat.getTimeInstance(DateFormat.SHORT).format(Date())

    /**
     * Looks for a newer build, at most every [MIN_GAP_MS]. An offered update is replaced by a newer one;
     * a download in progress is left alone. No network or a GitHub error just means no update for now.
     * Returns false when the check failed, so the caller can retry after [RETRY_MS].
     */
    // Checks serialize on their own lock: the network request must not hold the Updater lock, which
    // installer outcomes take on the main thread. The state change re-reads the state under that lock.
    fun check(context: Context, force: Boolean = false): Boolean = synchronized(checkLock) {
        val now = SystemClock.elapsedRealtime()
        val token = token(context)
        if (synchronized(this) { offeredCode() } == null) return true
        if (token.isEmpty()) { lastCheck.value = "No update token, so no checks"; return true }
        if (!force && lastCheckAt != 0L && now - lastCheckAt < MIN_GAP_MS) return true
        return runCatching { latest(token) }
            .onSuccess { r ->
                // Only a check that got an answer counts, so a failure doesn't hold off the next one.
                lastCheckAt = now
                lastCheck.value = when {
                    r == null -> "Checked ${now()}: the latest release has no APK"
                    r.versionCode > BuildConfig.VERSION_CODE -> "Checked ${now()}: ${r.versionName} (${r.versionCode}) is available"
                    else -> "Checked ${now()}: up to date"
                }
                synchronized(this) {
                    val offered = offeredCode() // null: an install started during the request
                    if (r != null && offered != null && r.versionCode > offered) {
                        when (val s = state.value) {
                            is State.Downloading, is State.Waiting -> supersede = r
                            // Replacing one already offered is flagged, so it doesn't pass for the same update.
                            else -> state.value = State.Available(r, newer = s is State.Available || s is State.Failed)
                        }
                    }
                }
            }
            .onFailure {
                Log.w("MenhirUpdate", "check failed: $it")
                lastCheck.value = "Check failed ${now()}: ${it.message ?: it.javaClass.simpleName}"
            }
            .isSuccess
    }

    /**
     * Holds one idle connection to the push topic while the caller is active (the Macs screen on
     * screen) and checks as soon as a release is announced. ntfy sends a keepalive every 45 s, so a
     * dead connection is noticed within the read timeout and reopened.
     */
    suspend fun listen(context: Context): Unit = withContext(Dispatchers.IO) {
        var backoff = 5_000L
        while (true) {
            if (!hasToken(context)) { delay(60_000); continue }
            runCatching {
                coroutineScope {
                    val conn = (URL("https://ntfy.sh/$PUSH_TOPIC/json").openConnection() as HttpURLConnection).apply {
                        connectTimeout = 15_000
                        readTimeout = 90_000
                    }
                    // A blocking read ignores cancellation; closing the connection ends it.
                    val closer = launch { try { awaitCancellation() } finally { conn.disconnect() } }
                    try {
                        check(conn.responseCode == 200) { "ntfy said ${conn.responseCode}" }
                        backoff = 5_000L
                        conn.inputStream.bufferedReader().useLines { lines ->
                            for (line in lines) {
                                val event = runCatching { JSONObject(line) }.getOrNull() ?: continue
                                if (event.optString("event") != "message") continue
                                announced(context, event.optString("message").trim().toIntOrNull() ?: 0)
                            }
                        }
                    } finally { closer.cancel() }
                }
            }.onFailure {
                if (it is kotlinx.coroutines.CancellationException) throw it
                Log.w("MenhirUpdate", "push channel: $it")
            }
            delay(backoff)
            backoff = (backoff * 2).coerceAtMost(60_000L)
        }
    }

    /** GitHub can take a moment to list a release it just published, so try a few times. */
    private suspend fun announced(context: Context, code: Int) {
        for (wait in longArrayOf(0, 2_000, 5_000, 10_000, 20_000)) {
            delay(wait)
            check(context, force = true)
            val offered = synchronized(this) { offeredCode() } ?: return
            if (code == 0 || offered >= code) return
        }
    }

    private fun latest(token: String): Release? {
        val json = github(LATEST, "application/vnd.github+json", token).run {
            etag?.let { setRequestProperty("If-None-Match", it) }
            try {
                if (responseCode == 304) return cached
                check(responseCode == 200) { "GitHub said $responseCode" }
                inputStream.bufferedReader().readText().also { etag = getHeaderField("ETag") }
            } finally { disconnect() }
        }
        val release = JSONObject(json)
        val assets = release.getJSONArray("assets")
        var code = 0
        var apk: Asset? = null
        for (i in 0 until assets.length()) {
            val asset = assets.getJSONObject(i)
            val match = assetName.matchEntire(asset.getString("name")) ?: continue
            apk = Asset(asset.getString("url"), asset.getLong("size"))
            code = match.groupValues[1].toInt()
        }
        return apk?.let { Release(release.getString("tag_name").removePrefix("v"), code, it) }
            .also { cached = it }
    }

    /** Streams the APK into an install session, then hands it to Android. Call off the main thread. */
    fun install(context: Context, release: Release, switched: Boolean = false) {
        val newer = switched || (state.value as? State.Available)?.newer == true
        val downloading = State.Downloading(release, 0, newer)
        synchronized(this) {
            // Admission is atomic with check()'s state change. Repeated taps cannot start concurrent sessions.
            val s = state.value
            if (s is State.Downloading || s is State.Waiting || s is State.Installing) return
            state.value = downloading
            installSession = -1
            supersede = null
        }
        val app = context.applicationContext
        val installer = app.packageManager.packageInstaller
        var session = -1
        try {
            val params = PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL).apply {
                setAppPackageName(app.packageName)
                setSize(release.apk.size)
                // Once Menhir is the app's installer, Android can skip the confirmation.
                setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_NOT_REQUIRED)
            }
            session = installer.createSession(params)
            synchronized(this) { installSession = session }
            val installing = State.Installing(release)
            installer.openSession(session).use { s ->
                val token = token(app)
                var done = 0L
                var shown = 0
                download(release.apk, token) { input ->
                    s.openWrite("menhir.apk", 0, release.apk.size).use { out ->
                        val buffer = ByteArray(64 * 1024)
                        while (true) {
                            if (supersede != null) throw Superseded()
                            val n = input.read(buffer)
                            if (n < 0) break
                            out.write(buffer, 0, n)
                            done += n
                            val percent = (done * 100 / release.apk.size).toInt()
                            if (percent != shown) { shown = percent; state.value = State.Downloading(release, percent, newer) }
                        }
                        s.fsync(out)
                    }
                }
                // Replacing the app kills the engine: hold the install while a Mac runs or macOS installs.
                while (true) {
                    if (supersede != null) throw Superseded()
                    val reason = busyReason() ?: break
                    if ((state.value as? State.Waiting)?.reason != reason) state.value = State.Waiting(release, reason)
                    Thread.sleep(2_000)
                }
                // Past this point the download is committed to; a newer release waits for the new build to find it.
                supersede = null

                // Rewritten, not createNewFile: a marker left from an earlier try would keep its old
                // time, look stale and stop the new build reopening.
                reopenMarker(app).writeText("${release.versionCode}\n$session")
                // The outcome comes back to an activity, not a receiver: once the new build is in, that
                // reopens Menhir. Android kills the app to replace it, and a broadcast to the new build may
                // never arrive (ColorOS holds back broadcasts to apps it just stopped), but the installer
                // always sends this. Menhir lends it its own right to start from the background, which
                // "Display over other apps" gives it; without that permission Android drops the start.
                val result = Intent(app, UpdateStatusActivity::class.java)
                    .putExtra(UpdateStatusActivity.CODE, release.versionCode)
                    .putExtra(SESSION, session)
                // Mutable because the installer fills in the outcome.
                val flags = PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_MUTABLE
                val options = android.app.ActivityOptions.makeBasic().apply {
                    setPendingIntentCreatorBackgroundActivityStartMode(
                        if (android.os.Build.VERSION.SDK_INT >= 36) android.app.ActivityOptions.MODE_BACKGROUND_ACTIVITY_START_ALLOW_ALWAYS
                        else @Suppress("DEPRECATION") android.app.ActivityOptions.MODE_BACKGROUND_ACTIVITY_START_ALLOWED,
                    )
                }.toBundle()
                // Starts and installs are admitted on the UI thread; no Mac can start between
                // this final busy check and handing the session to Android.
                kotlinx.coroutines.runBlocking(Dispatchers.Main) {
                    while (busyReason() != null) {
                        state.value = State.Waiting(release, busyReason() ?: "Waiting")
                        delay(500)
                    }
                    state.value = installing
                    s.commit(PendingIntent.getActivity(app, session, result, flags, options).intentSender)
                }
            }
            // If the outcome never arrives (its start was blocked) and the app is still here, the
            // install didn't happen: offer it again rather than showing "Installing…" for good.
            Thread({
                Thread.sleep(120_000)
                synchronized(this) {
                    // Identity matters: an old timeout must not fail a retry of the same release.
                    if (state.value === installing) {
                        state.value = State.Failed(release)
                        reopenMarker(app).delete()
                        installSession = -1
                    }
                }
            }, "menhir-update-watchdog").apply { isDaemon = true }.start()
        } catch (e: Superseded) {
            // Nothing is installed yet: drop this download and fetch the newer one instead, as already asked.
            if (session != -1) runCatching { installer.abandonSession(session) }
            val next = supersede
            synchronized(this) {
                supersede = null
                installSession = -1
                state.value = if (next != null) State.Available(next, newer = true) else State.Failed(release)
            }
            Log.i("MenhirUpdate", "switching download to ${next?.versionName}")
            if (next != null) install(context, next, switched = true)
        } catch (e: Exception) {
            Log.e("MenhirUpdate", "install failed", e)
            if (session != -1) runCatching { installer.abandonSession(session) }
            synchronized(this) {
                if (installSession == session) {
                    reopenMarker(app).delete()
                    installSession = -1
                    state.value = State.Failed(release)
                }
            }
        }
    }

    internal const val SESSION = "installSession"

    /** Ignore outcomes from superseded sessions, including a retry of the same version. */
    @Synchronized
    internal fun failInstall(context: Context, intent: Intent, aborted: Boolean) {
        val current = state.value as? State.Installing ?: return
        if (!matchesInstall(intent, current)) return
        reopenMarker(context).delete()
        installSession = -1
        state.value = if (aborted) State.Available(current.release) else State.Failed(current.release)
    }

    @Synchronized
    internal fun matchesInstall(intent: Intent, current: State.Installing): Boolean =
        intent.getIntExtra(UpdateStatusActivity.CODE, -1) == current.release.versionCode &&
            (!intent.hasExtra(SESSION) || intent.getIntExtra(SESSION, -1) == installSession)

    /** Confirmation can arrive after process recreation, when the in-memory state is Idle. */
    @Synchronized
    internal fun awaitingConfirmation(context: Context, intent: Intent): Boolean {
        val current = state.value as? State.Installing
        if (current != null) return matchesInstall(intent, current)
        if (state.value !is State.Idle) return false
        val marker = reopenMarker(context)
        val lines = runCatching { marker.readLines() }.getOrNull() ?: return false
        val age = System.currentTimeMillis() - marker.lastModified()
        return age in 0 until 10 * 60 * 1000L &&
            lines.firstOrNull()?.toIntOrNull() == intent.getIntExtra(UpdateStatusActivity.CODE, -1) &&
            (!intent.hasExtra(SESSION) || lines.getOrNull(1)?.toIntOrNull() == intent.getIntExtra(SESSION, -1))
    }

    private fun download(asset: Asset, token: String, write: (java.io.InputStream) -> Unit) {
        // GitHub redirects to a signed storage link, which must not get the token.
        val api = github(asset.url, "application/octet-stream", token)
        val link = try {
            check(api.responseCode in 300..399) { "GitHub said ${api.responseCode}" }
            api.getHeaderField("Location")
        } finally { api.disconnect() }
        val file = URL(link).openConnection() as HttpURLConnection
        try {
            file.connectTimeout = 15_000
            file.readTimeout = 30_000
            check(file.responseCode == 200) { "Download said ${file.responseCode}" }
            file.inputStream.use(write)
        } finally { file.disconnect() }
    }

    private fun github(url: String, accept: String, token: String) = (URL(url).openConnection() as HttpURLConnection).apply {
        instanceFollowRedirects = false
        connectTimeout = 15_000
        readTimeout = 30_000
        setRequestProperty("Authorization", "Bearer $token")
        setRequestProperty("Accept", accept)
        setRequestProperty("X-GitHub-Api-Version", "2022-11-28")
    }
}

/** Android's reply to an install: show its confirmation, or offer the update again if it didn't happen. */
internal fun handleInstallStatus(context: Context, intent: Intent, onSuccess: () -> Unit = {}) {
    when (val status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE)) {
        PackageInstaller.STATUS_PENDING_USER_ACTION -> {
            if (!Updater.awaitingConfirmation(context, intent)) return
            val confirm = intent.getParcelableExtra(Intent.EXTRA_INTENT, Intent::class.java) ?: return
            context.startActivity(confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
        }
        PackageInstaller.STATUS_SUCCESS -> {
            // This callback runs in the newly installed process. Ignore delayed older outcomes.
            if (intent.getIntExtra(UpdateStatusActivity.CODE, -1) != BuildConfig.VERSION_CODE) return
            Log.i("MenhirUpdate", "installed"); onSuccess()
        }
        else -> {
            Log.w("MenhirUpdate", "install ended: $status ${intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE)}")
            Updater.failInstall(context, intent, status == PackageInstaller.STATUS_FAILURE_ABORTED)
        }
    }
}

/**
 * Where the installer reports an in-app update. It shows nothing: it opens the Macs screen once the
 * new build is installed (this runs in the new build), shows Android's confirmation if one is needed,
 * or just closes after a failure.
 */
class UpdateStatusActivity : android.app.Activity() {
    override fun onCreate(savedInstanceState: android.os.Bundle?) {
        super.onCreate(savedInstanceState)
        handleInstallStatus(this, intent) {
            Updater.reopenMarker(this).delete() // reopening now, so the backup receiver needn't
            packageManager.getLaunchIntentForPackage(packageName)?.let { startActivity(it.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) }
        }
        finish()
    }

    companion object { const val CODE = "versionCode" }
}

/**
 * Backup for [UpdateStatusActivity]: Android tells the new build it replaced the old one. Only acts
 * when the update was started from the pill in the last 10 minutes and the activity hasn't reopened
 * Menhir already (it removes the marker), so `adb install` doesn't open the app.
 */
class UpdateReopen : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != Intent.ACTION_MY_PACKAGE_REPLACED) return
        val marker = Updater.reopenMarker(context)
        // A marker left by an install that never finished shouldn't reopen Menhir much later.
        val age = if (marker.isFile) System.currentTimeMillis() - marker.lastModified() else -1L
        val expectedCode = runCatching { marker.useLines { it.firstOrNull()?.toIntOrNull() } }.getOrNull()
        marker.delete()
        val allowed = Updater.canReopen(context)
        Log.i("MenhirUpdate", "replaced: marker age $age ms, overlay allowed $allowed")
        if (expectedCode != BuildConfig.VERSION_CODE || age !in 0 until 10 * 60 * 1000L || !allowed) return
        val launch = context.packageManager.getLaunchIntentForPackage(context.packageName) ?: return
        runCatching { context.startActivity(launch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) }
            .onFailure { Log.w("MenhirUpdate", "reopen failed: $it") }
    }
}

/** Compatibility receiver for installer outcomes sent to a broadcast PendingIntent. */
class UpdateResult : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) = handleInstallStatus(context, intent)
}
