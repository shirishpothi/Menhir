package app.menhir

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.pm.PackageManager
import android.util.Log
import app.menhir.input.Hid
import app.menhir.input.LinuxKeys
import app.menhir.input.MacInput
import kotlinx.coroutines.delay
import java.io.File
import java.util.concurrent.Executors

/** The Mac that is running in this process, if any. One per process, as in the engine. */
object Session {
    @Volatile var mac: Mac? = null
    /** The RAM and CPUs it was started with, which a saved Mac must match. */
    @Volatile var ramMB = 0L
    @Volatile var cpus = 0

    val active get() = mac != null && Engine.state().let { it == Engine.RUNNING || it == Engine.PAUSED || it == Engine.CHANGING }

    fun title() = mac?.let { "${it.name} is running" } ?: "Menhir"

    private const val TAG = "menhir"

    /**
     * Starts [mac] with the display [mode], picking up a saved Mac when there is a whole one for these
     * settings (as SessionView.run does on iOS). Blocks; call off the main thread. Null, or the reason.
     */
    fun start(context: Context, mac: Mac, mode: DisplayMode): String? {
        var waited = 0
        while (Engine.state() == Engine.CHANGING && waited++ < 300) Thread.sleep(100)
        if (!Engine.canStart()) return "A Mac has run in this process already. Restart Menhir to start another."
        val ram = mac.ramMB.toLong()
        Sharing.prepare(context)
        Engine.setDisplay(mode.width, mode.height)
        Engine.setDisplayMm(mode.widthMm, mode.heightMm)
        Log.i(TAG, "session: ${mac.name}, $ram MB, ${mac.cpus} CPUs, screen ${mode.width}x${mode.height}, ${mode.widthMm}x${mode.heightMm} mm")
        val ident = InstantResume.ident(context, mac, ram, mac.cpus, mode)
        var err: String? = "no saved Mac"
        if (Engine.savedAt(mac.resumeFile.path, ident) >= 0) {
            val t0 = System.nanoTime()
            err = Engine.resume(mac.rom.path, mac.disk.path, mac.aux.path, mac.ecid, mac.seed, ram, mac.cpus, mac.resumeFile.path, ident)
            Log.i(TAG, if (err == null) "resumed in %.1f s".format(java.util.Locale.ROOT, (System.nanoTime() - t0) / 1e9) else "could not resume ($err); starting from cold")
        }
        InstantResume.discard(mac) // used, or out of date: the disk moves on from here
        if (err != null) err = Engine.boot(mac.rom.path, mac.disk.path, mac.aux.path, mac.ecid, mac.seed, ram, mac.cpus)
        if (err != null) return err
        this.mac = mac
        ramMB = ram
        cpus = mac.cpus
        InstantResume.ident = ident
        Machines.put(mac.copy(lastUsed = System.currentTimeMillis()))
        return null
    }

    fun ended() {
        mac = null
    }
}

/**
 * Instant resume, as ios/Henge/InstantResume.swift does it: when the session leaves the screen the Mac
 * is stopped where it is and written to its folder, so it picks up there even if Android closes
 * Menhir meanwhile; coming straight back lets it run on. On iOS this is a setting; here it is always
 * on, because a Mac nobody sees would otherwise run on and drain the battery.
 * The engine keeps a saved Mac only for its disk exactly as it was, and uses a file once.
 */
object InstantResume {
    private const val TAG = "menhir"
    /** Saving and running on take turns on one thread: a save takes seconds. */
    private val queue = Executors.newSingleThreadExecutor { r -> Thread(r, "menhir-resume") }
    @Volatile var ident = ""
    @Volatile private var saved = false

    /** What a saved Mac is for: this install of the app (a restored machine must meet the code that saved it), the Mac and its settings. */
    fun ident(context: Context, mac: Mac, ramMB: Long, cpus: Int, mode: DisplayMode): String {
        val info = try { context.packageManager.getPackageInfo(context.packageName, 0) } catch (_: PackageManager.NameNotFoundException) { null }
        return "Menhir ${info?.versionName} (${info?.longVersionCode}) ${info?.lastUpdateTime} mac ${mac.id} ram $ramMB cpus $cpus " +
            "screen ${mode.width}x${mode.height} ${mode.widthMm}x${mode.heightMm}mm fake ${Engine.isFake()}"
    }

    fun discard(mac: Mac) {
        mac.resumeFile.delete()
        File(mac.resumeFile.path + ".part").delete()
    }

    /** The session is off the screen: pause the Mac, then save it. */
    fun suspend() {
        val mac = Session.mac ?: return
        if (Engine.state() != Engine.RUNNING) return
        queue.execute {
            val t0 = System.nanoTime()
            val err = Engine.pause() ?: Engine.save(mac.resumeFile.path, ident)
            saved = err == null
            Log.i(TAG, if (err == null) "paused and saved the Mac in %.1f s".format(java.util.Locale.ROOT, (System.nanoTime() - t0) / 1e9) else "not saved: $err")
            MenhirService.update()
        }
    }

    /** Android is short of memory and the Mac is paused: make sure it is on storage. */
    fun saveIfPaused() {
        val mac = Session.mac ?: return
        queue.execute {
            if (Engine.state() != Engine.PAUSED || saved) return@execute
            saved = Engine.save(mac.resumeFile.path, ident) == null
        }
    }

    /** Back on the screen: the saved file is out of date the moment the Mac runs on. */
    fun cameBack() {
        val mac = Session.mac ?: return
        queue.execute {
            if (Engine.state() != Engine.PAUSED) return@execute
            discard(mac)
            saved = false
            Log.i(TAG, if (Engine.continueRunning()) "back: the Mac runs on" else "back: the Mac could not run on")
            MenhirService.update()
        }
    }
}

/**
 * The clipboard and the shared folder (host/share.c): the Mac mounts Storage.shared and follows the
 * clipboard through a small helper, tools/henge-agent.m, which it installs with [SET_UP_COMMAND].
 */
object Sharing {
    const val SET_UP_COMMAND = "curl -s http://10.0.2.4/.henge/install | sh"

    /** Before a Mac starts. */
    fun prepare(context: Context) {
        Storage.shared.mkdirs()
        // A Unix socket's path is short: the cache directory is.
        val socket = File(context.cacheDir, "s.sock").path
        Engine.share(Storage.shared.path, socket, agent(context)?.path)
    }

    /** The helper, if this build has it (docs/android/app.md): copied out of the APK for the engine to serve. */
    private fun agent(context: Context): File? = try {
        val out = File(context.filesDir, "henge-agent.gz")
        context.assets.open("henge-agent.gz").use { input -> out.outputStream().use { input.copyTo(it) } }
        out
    } catch (_: java.io.IOException) {
        null
    }

    /** The helper asks for news at most 25 s apart. */
    val helperRunning get() = Engine.agentSeen().let { it in 0..39 }

    /** What the Mac copied, onto Android's clipboard. Call every half second or so. */
    fun followMacClipboard(context: Context) {
        val bytes = Engine.takeClipboard() ?: return
        val text = String(bytes, Charsets.UTF_8)
        context.getSystemService(ClipboardManager::class.java).setPrimaryClip(ClipData.newPlainText("Mac", text))
    }

    /** Android's clipboard, pasted at the Mac's cursor. Without the helper it is typed, as far as the keys go. */
    suspend fun paste(context: Context, mac: MacInput) {
        val clip = context.getSystemService(ClipboardManager::class.java).primaryClip ?: return
        if (clip.itemCount == 0) return
        val text = clip.getItemAt(0).coerceToText(context).toString()
        if (helperRunning) {
            Engine.setClipboard(text.toByteArray(Charsets.UTF_8))
            delay(800) // for the helper to put it on the Mac's clipboard
            mac.keyHid(Hid.LEFT_COMMAND, true)
            mac.keyHid(Hid.V, true)
            mac.keyHid(Hid.V, false)
            mac.keyHid(Hid.LEFT_COMMAND, false)
        } else {
            typeSlowly(mac, text)
        }
    }

    /** One character at a time, for what the Mac has to keep up with. Never presses Return by itself for the command. */
    suspend fun typeSlowly(mac: MacInput, text: String) {
        for (c in text) {
            LinuxKeys.type(mac, c)
            delay(25)
        }
    }
}
