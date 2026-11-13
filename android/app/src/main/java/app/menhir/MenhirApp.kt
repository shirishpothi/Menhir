package app.menhir

import android.app.ActivityManager
import android.app.Application
import android.content.ComponentCallbacks2
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.PowerManager
import android.provider.Settings
import android.util.Log

class MenhirApp : Application() {
    override fun onCreate() {
        super.onCreate()
        Storage.init(this)
        Machines.load()
        MenhirService.init(this)
        Log.i("menhir", "Menhir started; fake engine: ${Engine.isFake()}; storage ${Storage.root}")
    }

    /**
     * Android is short of memory. Since Android 14 only the UI-hidden and background levels arrive. A
     * Mac paused in the background is saved by then already (InstantResume); one that is running is
     * an install, which nothing here can shrink, so this only says so in the log.
     */
    override fun onTrimMemory(level: Int) {
        super.onTrimMemory(level)
        Log.i("menhir", "trim memory, level $level; Mac state ${Engine.state()}; available ${Engine.availableMemory() shr 20} MB")
        if (level >= ComponentCallbacks2.TRIM_MEMORY_BACKGROUND) InstantResume.saveIfPaused()
    }
}

/** What the device can give a Mac. */
object Device {
    fun memory(context: Context): ActivityManager.MemoryInfo {
        val info = ActivityManager.MemoryInfo()
        context.getSystemService(ActivityManager::class.java).getMemoryInfo(info)
        return info
    }

    /**
     * A warning if a Mac with [ramMB] will not fit in what Android has free now (with 1 GB to spare
     * for Android and the app itself), or null if it fits.
     */
    fun memoryWarning(context: Context, ramMB: Int): String? {
        val info = memory(context)
        val needed = (ramMB.toLong() shl 20) + (1L shl 30)
        if (info.availMem >= needed) return null
        return "This Mac uses ${ramMB / 1024} GB, and Android has ${bytesText(info.availMem)} free right now. " +
            "Android may close Menhir when macOS gets busy. Close other apps, or give the Mac less memory."
    }

    fun totalGB(context: Context) = (memory(context).totalMem shr 30).toInt() + 1

    fun batteryExempt(context: Context) =
        context.getSystemService(PowerManager::class.java).isIgnoringBatteryOptimizations(context.packageName)

    fun askBatteryExemption(context: Context) {
        context.startActivity(
            Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, Uri.parse("package:${context.packageName}"))
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
        )
    }

    /** Starts the app again in a new process: the engine runs one machine per process. */
    fun restartApp(context: Context) {
        val intent = context.packageManager.getLaunchIntentForPackage(context.packageName) ?: return
        context.startActivity(Intent.makeRestartActivityTask(intent.component))
        Runtime.getRuntime().exit(0)
    }
}
