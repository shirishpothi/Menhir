package app.menhir

import android.content.Context
import android.os.PowerManager
import android.os.StatFs
import android.util.Log
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.io.File

/** What the installer is doing, for the Macs screen and the notification. */
sealed interface InstallStatus {
    data object Idle : InstallStatus
    data class Working(
        val macName: String,
        val phase: String,
        /** 0..1, or null when there is nothing to measure. */
        val progress: Double?,
        val detail: String = "",
        val downloading: Boolean = false,
    ) : InstallStatus
    data class Done(val text: String) : InstallStatus
    data class Failed(val text: String) : InstallStatus
}

/**
 * Downloads a restore image and installs it on a new Mac, as ios/Henge/Installer.swift does: fetch the
 * boot ROM, download the IPSW (resuming a .part file), make the machine's files, then boot the ROM in
 * DFU mode and run the restore. It runs in the app process, not in an activity, with a foreground
 * service and a partial wake lock, so it goes on with the screen off.
 */
object Installer {
    private const val TAG = "menhir"
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val state = MutableStateFlow<InstallStatus>(InstallStatus.Idle)
    val status: StateFlow<InstallStatus> = state
    private var job: Job? = null
    @Volatile private var fetchJob = 0L

    val busy get() = state.value is InstallStatus.Working

    fun reset() {
        if (!busy) state.value = InstallStatus.Idle
    }

    /** Free bytes where Macs are kept. */
    fun freeBytes(): Long = try { StatFs(Storage.root.path).availableBytes } catch (_: Exception) { 0 }

    fun spaceNeeded(r: Release): Long = (if (r.downloaded) 0 else (r.bytes - r.part.length()).coerceAtLeast(0)) + r.macBytes

    fun deleteImage(r: Release) {
        r.image.delete()
        r.part.delete()
    }

    /** Install [release] on [mac] (a new one, or one whose install did not finish). */
    fun start(context: Context, mac: Mac, release: Release, keepImage: Boolean) {
        if (busy) return
        val app = context.applicationContext
        state.value = InstallStatus.Working(mac.name, "Checking", null)
        MenhirService.refresh(app)
        job = scope.launch {
            val power = app.getSystemService(PowerManager::class.java)
            val lock = power.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "menhir:install")
            lock.acquire(12 * 60 * 60 * 1000L) // a long download and restore; released when done
            try {
                run(mac, release, keepImage)
            } catch (e: Exception) {
                Log.e(TAG, "install failed", e)
                // A failed install's disk and restore files are of no use; take them off the disk.
                if (Engine.state() == Engine.RUNNING) Engine.stop()
                val listed = Machines.get(mac.id)
                if (listed == null) Machines.removeFiles(mac.directory) else if (!listed.installed) Machines.delete(listed)
                state.value = InstallStatus.Failed(e.message ?: e.toString())
            } finally {
                if (lock.isHeld) lock.release()
                MenhirService.refresh(app)
            }
        }
    }

    /** Stops a download where it is; it goes on from there next time. A restore cannot be stopped halfway. */
    fun cancelDownload() {
        val j = fetchJob
        if (j != 0L) Engine.fetchCancel(j)
    }

    private fun working(mac: Mac, phase: String, progress: Double?, detail: String = "", downloading: Boolean = false) {
        state.value = InstallStatus.Working(mac.name, phase, progress, detail, downloading)
        MenhirService.update()
    }

    private suspend fun run(mac: Mac, release: Release, keepImage: Boolean) {
        val fake = Engine.isFake()
        val free = freeBytes()
        val needed = spaceNeeded(release)
        if (!fake && free in 1 until needed) {
            error("This install needs about ${bytesText(needed)} free; the tablet has ${bytesText(free)}.")
        }
        if (!Engine.canStart()) error("A Mac has run since Menhir started. Restart Menhir, then install.")

        working(mac, "Getting the boot ROM", null)
        Storage.bootRom.parentFile?.mkdirs()
        fetch(Engine.fetchRom(Storage.bootRom.path), mac, "Getting the boot ROM", false)
        Log.i(TAG, "boot ROM ready")

        Storage.images.mkdirs()
        if (!release.image.exists()) {
            var attempt = 1
            while (true) {
                val job = Engine.fetchIpsw(release.url, release.image.path)
                try {
                    fetch(job, mac, "Downloading ${release.name}", true)
                    break
                } catch (e: DownloadStopped) {
                    throw IllegalStateException("The download stopped. Install again to go on from where it was.")
                } catch (e: Exception) {
                    if (attempt++ >= 20) throw e
                    Log.w(TAG, "download interrupted (${e.message}); going on")
                    delay(3000)
                }
            }
        }

        working(mac, "Preparing the Mac", null)
        val prepared = mac.copy(osName = release.name, installed = false)
        Machines.prepareFiles(prepared, Storage.bootRom)
        Machines.put(prepared)

        // The restore needs 2 GB and gets at most 4 GB, whatever the Mac uses afterwards (Installer.swift).
        val ram = prepared.ramMB.coerceIn(2048, 4096).toLong()
        prepared.restoreWork.mkdirs()
        Engine.install(
            prepared.rom.path, prepared.disk.path, prepared.aux.path, prepared.ecid, release.image.path,
            prepared.restoreWork.path, ram, prepared.cpus,
        )?.let { error(it) }
        while (true) {
            delay(1000)
            val step = Engine.installStep()
            working(mac, stepName(step), Engine.installProgress())
            if (step == Engine.INSTALL_DONE) break
            if (step == Engine.INSTALL_FAILED) error("The restore failed. The log (adb logcat) says why.")
            if (Engine.state() == Engine.STOPPED) {
                // The restore ends with the machine restarting, and finishes a little after.
                repeat(90) { if (Engine.installStep() < Engine.INSTALL_DONE) delay(1000) }
                if (Engine.installStep() == Engine.INSTALL_DONE) break
                error("The machine stopped during the restore (${Engine.exitReason()}).")
            }
        }
        Machines.put(prepared.copy(installed = true))
        File(prepared.directory, "installed").writeText(release.name)
        if (Engine.state() == Engine.RUNNING) Engine.stop()
        prepared.restoreWork.deleteRecursively()
        if (!keepImage) release.image.delete()
        Log.i(TAG, "${mac.name}: installed ${release.name}")
        state.value = InstallStatus.Done("${release.name} is installed on ${mac.name}. Restart Menhir to start it.")
    }

    private class DownloadStopped : Exception()

    /** Polls a download job to its end, then frees it. */
    private suspend fun fetch(job: Long, mac: Mac, phase: String, downloading: Boolean) {
        check(job != 0L) { "The download could not start." }
        fetchJob = job
        val out = LongArray(3)
        try {
            while (scope.isActive) {
                val err = Engine.fetchPoll(job, out)
                val total = out[2]
                val detail = if (total > 0) "${bytesText(out[1])} of ${bytesText(total)}" else bytesText(out[1])
                when (out[0]) {
                    Engine.FETCH_DONE -> return
                    Engine.FETCH_CANCELLED -> throw DownloadStopped()
                    Engine.FETCH_FAILED -> error(err ?: "The download failed.")
                }
                working(mac, phase, if (total > 0) out[1].toDouble() / total else null, detail, downloading)
                delay(500)
            }
        } finally {
            fetchJob = 0
            Engine.fetchFree(job)
        }
    }

    fun stepName(step: Int) = when (step) {
        0 -> "Finding the Mac"
        1 -> "Starting the installer"
        2 -> "Writing macOS"
        3 -> "Checking macOS"
        4 -> "Installing firmware"
        6, 7 -> "Installing system files"
        else -> "Installing"
    }
}
