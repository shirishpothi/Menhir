package app.menhir

import android.view.Surface

/**
 * The engine as Kotlin sees it: android/native/jni.c over android/native/henge-vm.h, whose functions
 * keep the iOS app's names (henge_vm_boot is [boot], and so on). Calls that can fail return the
 * engine's reason, or null when they worked.
 */
object Engine {
    init {
        System.loadLibrary("menhir")
    }

    const val NOT_STARTED = 0
    const val RUNNING = 1
    const val STOPPED = 2
    const val CHANGING = 3
    const val PAUSED = 4

    const val OUTCOME_ERROR = 0
    const val OUTCOME_SHUTDOWN = 1
    const val OUTCOME_RESTART = 2
    const val OUTCOME_HOST_STOP = 3
    const val OUTCOME_PANIC = 4

    const val INSTALL_DONE = 100
    const val INSTALL_FAILED = 101

    @JvmStatic external fun boot(rom: String, disk: String, aux: String, ecid: Long, seed: Long, ramMB: Long, cpus: Int): String?
    @JvmStatic external fun install(
        rom: String, disk: String, aux: String, ecid: Long, ipsw: String, workdir: String, ramMB: Long, cpus: Int,
    ): String?
    @JvmStatic external fun installStep(): Int
    @JvmStatic external fun installProgress(): Double
    @JvmStatic external fun makeAux(path: String): Boolean

    @JvmStatic external fun pause(): String?
    @JvmStatic external fun save(path: String, ident: String): String?
    @JvmStatic external fun continueRunning(): Boolean
    @JvmStatic external fun resume(
        rom: String, disk: String, aux: String, ecid: Long, seed: Long, ramMB: Long, cpus: Int, path: String, ident: String,
    ): String?
    /** Seconds since 1970 when the saved Mac at [path] was saved, or -1 if it is not a whole one for [ident]. */
    @JvmStatic external fun savedAt(path: String, ident: String): Long

    @JvmStatic external fun state(): Int
    @JvmStatic external fun generation(): Long
    @JvmStatic external fun canStart(): Boolean
    @JvmStatic external fun exitReason(): String
    @JvmStatic external fun exitOutcome(): Int
    @JvmStatic external fun stop()
    @JvmStatic external fun blocks(): Long
    @JvmStatic external fun netFrames(sent: Boolean): Long
    @JvmStatic external fun cpusOn(): Int

    @JvmStatic external fun share(folder: String, socket: String, agent: String?)
    @JvmStatic external fun setClipboard(utf8: ByteArray)
    @JvmStatic external fun takeClipboard(): ByteArray?
    @JvmStatic external fun agentSeen(): Long

    @JvmStatic external fun setDisplay(width: Int, height: Int)
    @JvmStatic external fun setDisplayMm(widthMm: Int, heightMm: Int)

    @JvmStatic external fun key(code: Int, down: Boolean)
    @JvmStatic external fun keyHid(usage: Int, down: Boolean)
    @JvmStatic external fun mouseMove(dx: Int, dy: Int)
    @JvmStatic external fun mouseButton(button: Int, down: Boolean)
    @JvmStatic external fun mouseScroll(dx: Int, dy: Int)
    @JvmStatic external fun touch(finger: Int, phase: Int, x: Int, y: Int, tsMs: Int)
    @JvmStatic external fun touchFrame(tsMs: Int)
    @JvmStatic external fun touchButton(down: Boolean)
    @JvmStatic external fun pointer(): IntArray?

    /** The SurfaceView's surface, or null once it is destroyed. */
    @JvmStatic external fun setDisplaySurface(surface: Surface?)
    @JvmStatic external fun gpuFrames(): Long
    @JvmStatic external fun availableMemory(): Long
    @JvmStatic external fun largestMappingMb(): Long

    @JvmStatic external fun fetchRom(destination: String): Long
    @JvmStatic external fun fetchIpsw(url: String, destination: String): Long
    /** Fills [out] with state, bytes downloaded and total; returns the error text, if any. */
    @JvmStatic external fun fetchPoll(job: Long, out: LongArray): String?
    @JvmStatic external fun fetchCancel(job: Long)
    @JvmStatic external fun fetchFree(job: Long)

    /** True in the build with android/native/fake_vm.c in place of the engine. */
    @JvmStatic external fun isFake(): Boolean

    const val FETCH_RUNNING = 0L
    const val FETCH_DONE = 1L
    const val FETCH_FAILED = 2L
    const val FETCH_CANCELLED = 3L
}
