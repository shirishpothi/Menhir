package app.menhir

import java.io.File

/** A macOS restore image Apple publishes for virtual Macs. The list is the iOS app's (Installer.swift). */
data class Release(val build: String, val name: String, val url: String, val bytes: Long, val note: String, val macBytes: Long = 24L shl 30) {
    val image get() = File(Storage.images, "$build.ipsw")
    val part get() = File(Storage.images, "$build.ipsw.part")
    val downloaded get() = image.exists()

    companion object {
        val all = listOf(
            Release(
                "22G120", "macOS Ventura 13.6",
                "https://updates.cdn-apple.com/2023FallFCS/fullrestores/042-55833/C0830847-A2F8-458F-B680-967991820931/UniversalMac_13.6_22G120_Restore.ipsw",
                12_297L shl 20, "The smallest download.", 23L shl 30,
            ),
            Release(
                "23G93", "macOS Sonoma 14.6.1",
                "https://updates.cdn-apple.com/2024SummerFCS/fullrestores/062-52859/932E0A8F-6644-4759-82DA-F8FA8DEA806A/UniversalMac_14.6.1_23G93_Restore.ipsw",
                14_106L shl 20, "A smaller download than Sequoia.", 26L shl 30,
            ),
            Release(
                "24G90", "macOS Sequoia 15.6.1",
                "https://updates.cdn-apple.com/2025SummerFCS/fullrestores/093-10809/CFD6DD38-DAF0-40DA-854F-31AAD1294C6F/UniversalMac_15.6.1_24G90_Restore.ipsw",
                16_035L shl 20, "Recommended: the version Henge is tested with most.",
            ),
            Release(
                "25G83", "macOS Tahoe 26.6.2",
                "https://updates.cdn-apple.com/2026SummerFCS/fullrestores/140-75212/A2A24B94-1FC1-45A3-93F7-C51B02AF1F4D/UniversalMac_26.6.2_25G83_Restore.ipsw",
                18_856L shl 20, "Its first setup is slow.",
            ),
            Release(
                "26B123", "macOS 27.2",
                "https://updates.cdn-apple.com/2026FallFCS/c9671975-52db-4fb5-9af2-00217169ba8d/UniversalMac_27.2_26B123_Restore.ipsw",
                24_409L shl 20, "The newest, and the slowest.",
            ),
        )
        val recommended = all.first { it.build == "24G90" }
    }
}

fun bytesText(n: Long): String = when {
    n >= 1L shl 30 -> "%.1f GB".format(n / (1L shl 30).toDouble())
    n >= 1L shl 20 -> "%.0f MB".format(n / (1L shl 20).toDouble())
    else -> "$n bytes"
}
