package app.menhir

import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

/**
 * The screen macOS is told it has: every pixel of the panel, on its side, and the panel's real size,
 * so that macOS sees its true density and draws at 2x on a dense panel (sharp, at half the points).
 * Read from the device at run time, so the Mac's screen is the panel's shape and there are no bars.
 */
data class DisplayMode(val width: Int, val height: Int, val widthMm: Int, val heightMm: Int) {
    companion object {
        /** Pixels per inch outside this range are a device lying about its panel (emulators often do). */
        private val plausiblePpi = 90f..700f

        /**
         * [pxW] x [pxH]: the panel's pixels (Display.Mode physical size); [xdpi], [ydpi]: its density
         * along those axes (DisplayMetrics). Sides round down to a multiple of 8, which the virtual GPU
         * wants. Without a believable density the size is 0 x 0: the engine's 1x default.
         */
        fun forPanel(pxW: Int, pxH: Int, xdpi: Float, ydpi: Float): DisplayMode {
            val landscape = pxW >= pxH
            val long = max(pxW, pxH)
            val short = min(pxW, pxH)
            val longDpi = if (landscape) xdpi else ydpi
            val shortDpi = if (landscape) ydpi else xdpi
            val w = long / 8 * 8
            val h = short / 8 * 8
            if (longDpi !in plausiblePpi || shortDpi !in plausiblePpi) return DisplayMode(w, h, 0, 0)
            return DisplayMode(w, h, (w / longDpi * 25.4f).roundToInt(), (h / shortDpi * 25.4f).roundToInt())
        }
    }
}
