package app.menhir

import org.junit.Assert.assertEquals
import org.junit.Test

class DisplayModeTest {
    @Test fun everyPixelAndTheRealSize() {
        // A 12.1 inch 2800 x 2000 panel is about 284 ppi.
        val m = DisplayMode.forPanel(2800, 2000, 284f, 284f)
        assertEquals(2800, m.width)
        assertEquals(2000, m.height)
        assertEquals(250, m.widthMm)
        assertEquals(179, m.heightMm)
    }

    @Test fun aPortraitPanelIsTurnedOnItsSide() {
        val m = DisplayMode.forPanel(2000, 2800, 280f, 290f)
        assertEquals(2800, m.width)
        assertEquals(2000, m.height)
        assertEquals(Math.round(2800 / 290f * 25.4f), m.widthMm) // the long side's density is the panel's ydpi here
        assertEquals(Math.round(2000 / 280f * 25.4f), m.heightMm)
    }

    @Test fun sidesRoundDownToMultiplesOfEight() {
        val m = DisplayMode.forPanel(2560, 1605, 274f, 274f)
        assertEquals(2560, m.width)
        assertEquals(1600, m.height)
    }

    @Test fun anImplausibleDensityGivesTheDefault() {
        val m = DisplayMode.forPanel(2800, 2000, 0f, 0f)
        assertEquals(2800, m.width)
        assertEquals(0, m.widthMm)
        assertEquals(0, m.heightMm)
    }
}
