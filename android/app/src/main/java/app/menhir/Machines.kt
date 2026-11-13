package app.menhir

import android.content.Context
import android.util.Log
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.util.UUID

/**
 * Where everything lives: the app's external files directory, so Macs survive app updates and can be
 * reached over adb (/sdcard/Android/data/app.menhir/files). Android bind-mounts an app's own
 * Android/data straight onto the underlying file system, so the disk images do not go through FUSE.
 */
object Storage {
    lateinit var root: File
        private set

    fun init(context: Context) {
        root = context.getExternalFilesDir(null) ?: context.filesDir
    }

    val machines get() = File(root, "Machines")
    val images get() = File(root, "Restore images")
    val bootRom get() = File(root, "BootROM/AVPBooter.vmapple2.bin")
    val shared get() = File(root, "Shared")
}

/** A virtual Mac: its identity, what it gets to run with, and its files. */
data class Mac(
    val id: String = UUID.randomUUID().toString(),
    val name: String,
    val ecid: Long,
    val seed: Long = 0,
    val ramMB: Int,
    val cpus: Int,
    val diskGB: Int,
    val osName: String? = null,
    val installed: Boolean = false,
    val added: Long = System.currentTimeMillis(),
    val lastUsed: Long = 0,
) {
    val directory get() = File(Storage.machines, id)
    val rom get() = File(directory, "AVPBooter.vmapple2.bin")
    val disk get() = File(directory, "disk.img")
    val aux get() = File(directory, "auxiliary.bin")
    val resumeFile get() = File(directory, "resume.hsnap")
    val restoreWork get() = File(directory, "restore")

    fun toJson(): JSONObject = JSONObject()
        .put("id", id).put("name", name).put("ecid", ecid).put("seed", seed).put("ramMB", ramMB)
        .put("cpus", cpus).put("diskGB", diskGB).put("osName", osName ?: JSONObject.NULL)
        .put("installed", installed).put("added", added).put("lastUsed", lastUsed)

    companion object {
        fun fromJson(o: JSONObject) = Mac(
            id = o.getString("id"), name = o.getString("name"), ecid = o.getLong("ecid"), seed = o.optLong("seed"),
            ramMB = o.getInt("ramMB"), cpus = o.optInt("cpus", 4), diskGB = o.optInt("diskGB", 64),
            osName = if (o.isNull("osName")) null else o.optString("osName"),
            installed = o.optBoolean("installed"), added = o.optLong("added"), lastUsed = o.optLong("lastUsed"),
        )
    }
}

/** The list of Macs, in Machines/machines.json. */
object Machines {
    private const val TAG = "menhir"
    private val list get() = File(Storage.machines, "machines.json")
    private val state = MutableStateFlow<List<Mac>>(emptyList())
    val all: StateFlow<List<Mac>> = state

    fun load() {
        val loaded = try {
            if (!list.exists()) emptyList() else {
                val a = JSONArray(list.readText())
                (0 until a.length()).map { Mac.fromJson(a.getJSONObject(it)) }
            }
        } catch (e: Exception) {
            Log.e(TAG, "the list of Macs could not be read", e)
            emptyList()
        }
        // load() runs at startup, when no install can be in progress: a Mac whose install did not
        // finish, and any folder no Mac owns, are leftovers of a failed install, so remove them from disk.
        val kept = loaded.filter { it.installed }
        if (kept.size != loaded.size) save(kept)
        val owned = kept.map { it.id }.toSet()
        Storage.machines.listFiles()?.filter { it.isDirectory && it.name !in owned }?.forEach { removeFiles(it) }
        state.value = kept
    }

    /** Deletes [dir] and everything in it, and logs if anything is left behind. */
    fun removeFiles(dir: File) {
        if (!dir.deleteRecursively() && dir.exists()) Log.e(TAG, "could not delete all of ${dir.path}")
    }

    fun get(id: String) = state.value.firstOrNull { it.id == id }

    @Synchronized
    fun put(mac: Mac) {
        val next = state.value.filter { it.id != mac.id } + mac
        save(next)
        state.value = next
    }

    @Synchronized
    fun delete(mac: Mac) {
        removeFiles(mac.directory)
        val next = state.value.filter { it.id != mac.id }
        save(next)
        state.value = next
    }

    private fun save(macs: List<Mac>) {
        Storage.machines.mkdirs()
        val a = JSONArray()
        macs.forEach { a.put(it.toJson()) }
        val part = File(list.path + ".part")
        part.writeText(a.toString(2))
        part.renameTo(list)
    }

    /**
     * A new machine's files, as Virtualization.framework makes them: a random ECID, a sparse disk of
     * the chosen size, blank auxiliary storage, the boot ROM, and machine.json in the format
     * tools/export_ios.sh writes, so a Mac can be carried between Henge and Menhir.
     */
    fun prepareFiles(mac: Mac, rom: File) {
        val dir = mac.directory
        dir.mkdirs()
        rom.copyTo(mac.rom, overwrite = true)
        mac.disk.delete()
        java.io.RandomAccessFile(mac.disk, "rw").use { it.setLength(mac.diskGB.toLong() shl 30) }
        check(Engine.makeAux(mac.aux.path)) { "Cannot create the auxiliary storage." }
        File(dir, "machine.json").writeText(
            JSONObject().put("name", mac.name).put("ecid", mac.ecid).put("seed", mac.seed).put("ramMB", mac.ramMB).toString(),
        )
    }

    /** A new ECID, as VZMacMachineIdentifier makes one. */
    fun newEcid(): Long = (1L shl 40) + (Math.random() * ((1L shl 62) - (1L shl 40))).toLong()
}
