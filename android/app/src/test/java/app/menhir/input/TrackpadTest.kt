package app.menhir.input

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

class TrackpadTest {
    private lateinit var r: Recorder
    private lateinit var s: ManualScheduler
    private lateinit var pad: Trackpad
    private var reveals = 0

    @Before fun setUp() {
        r = Recorder()
        s = ManualScheduler()
        reveals = 0
        pad = Trackpad(r, MouseButtons(r), s, tapsClick = true, tapDistance = 16f, onReveal = { reveals++ })
        pad.setSize(2800, 2000)
    }

    private fun f(id: Int, x: Float, y: Float) = Finger(id, x, y)

    @Test fun sensorMappingKeepsOneScaleAndCentres() {
        // 2800 x 2000 is squarer than the sensor (8134 x 5206): height fills it, width is centred.
        assertEquals(8134 / 2 to 5206 / 2, Trackpad.sensor(1400f, 1000f, 2800f, 2000f))
        val scale = 5206.0 / 2000
        val left = Trackpad.sensor(0f, 0f, 2800f, 2000f)
        assertEquals(((0 - 1400) * scale + 8134 / 2.0).toInt(), left.first)
        assertEquals(0, left.second)
        assertEquals(5206, Trackpad.sensor(1400f, 2000f, 2800f, 2000f).second)
        // Same distance on both axes, same sensor distance.
        val a = Trackpad.sensor(1000f, 1000f, 2800f, 2000f)
        val b = Trackpad.sensor(1100f, 1100f, 2800f, 2000f)
        assertTrue(Math.abs((b.first - a.first) - (b.second - a.second)) <= 1) // each axis truncates on its own
        // Clamped to the sensor.
        assertEquals(0 to 0, Trackpad.sensor(-500f, -500f, 2800f, 2000f))
    }

    @Test fun timestampsAreTheSamplesOwnAndTruncated() {
        assertEquals(1234, Trackpad.ts(1234))
        assertEquals((0x1_0000_0005L).toInt(), Trackpad.ts(0x1_0000_0005L))
        assertEquals(5, Trackpad.ts(0x1_0000_0005L))
    }

    @Test fun historicalSamplesAreFramesOfTheirOwn() {
        pad.down(f(7, 1400f, 1000f), 100)
        r.take()
        // A batched move: two historical samples and the latest, each with its own time.
        pad.move(listOf(f(7, 1410f, 1000f)), 108)
        pad.move(listOf(f(7, 1420f, 1000f)), 116)
        pad.move(listOf(f(7, 1430f, 1000f)), 124)
        val calls = r.take()
        assertEquals(listOf("frame 108", "frame 116", "frame 124"), calls.filter { it.startsWith("frame") })
        assertTrue(calls[0].startsWith("touch 1 1 ") && calls[0].endsWith(" 108"))
    }

    @Test fun fingersGetStableIdsAndMoveTogether() {
        pad.down(f(10, 1000f, 1000f), 0)
        pad.down(f(11, 1200f, 1000f), 5)
        r.take()
        pad.move(listOf(f(10, 1000f, 1100f), f(11, 1200f, 1100f)), 20)
        val calls = r.take()
        assertEquals(3, calls.size)
        assertTrue(calls[0].startsWith("touch 1 1"))
        assertTrue(calls[1].startsWith("touch 2 1"))
        assertEquals("frame 20", calls[2])
        pad.up(10, 30)
        pad.down(f(12, 1500f, 1000f), 40) // takes the free id 1
        assertTrue(r.take().last().startsWith("touch 1 0"))
    }

    @Test fun aFingerThatDidNotMoveIsNotSent() {
        pad.down(f(1, 100f, 100f), 0)
        r.take()
        pad.move(listOf(f(1, 100f, 100f)), 10)
        assertEquals(emptyList<String>(), r.take())
    }

    @Test fun aSixthFingerIsIgnored() {
        for (i in 0 until 6) pad.down(f(i, 100f * i, 100f), 0)
        assertEquals(5, pad.fingersDown)
    }

    @Test fun quickTapClicksLeft() {
        pad.down(f(1, 500f, 500f), 0)
        pad.up(1, 120)
        assertEquals(listOf("button 0 down", "button 0 up"), r.take().filter { it.startsWith("button") })
    }

    @Test fun twoFingerTapClicksRight() {
        pad.down(f(1, 500f, 500f), 0)
        pad.down(f(2, 600f, 500f), 10)
        pad.up(1, 100)
        pad.up(2, 110)
        assertEquals(listOf("button 1 down", "button 1 up"), r.take().filter { it.startsWith("button") })
    }

    @Test fun slowOrMovingTouchesDoNotClick() {
        pad.down(f(1, 500f, 500f), 0)
        pad.up(1, 350) // too slow (and the hold timer was never run)
        pad.down(f(1, 500f, 500f), 1000)
        pad.move(listOf(f(1, 540f, 500f)), 1050)
        pad.up(1, 1100)
        assertEquals(emptyList<String>(), r.take().filter { it.startsWith("button") })
    }

    @Test fun holdingStillDragsUntilLift() {
        pad.down(f(1, 500f, 500f), 0)
        s.advanceTo(400)
        assertEquals(listOf("button 0 down"), r.take().filter { it.startsWith("button") })
        pad.move(listOf(f(1, 900f, 700f)), 500)
        pad.up(1, 900)
        assertEquals(listOf("button 0 up"), r.take().filter { it.startsWith("button") })
    }

    @Test fun movingBeforeTheHoldCancelsIt() {
        pad.down(f(1, 500f, 500f), 0)
        pad.move(listOf(f(1, 560f, 500f)), 100)
        s.advanceTo(400)
        pad.up(1, 600)
        assertEquals(emptyList<String>(), r.take().filter { it.startsWith("button") })
    }

    @Test fun aSecondFingerTurnsADragIntoAGesture() {
        pad.down(f(1, 500f, 500f), 0)
        s.advanceTo(400)
        pad.down(f(2, 700f, 500f), 450)
        pad.up(1, 600)
        pad.up(2, 610)
        assertEquals(listOf("button 0 down", "button 0 up"), r.take().filter { it.startsWith("button") })
    }

    @Test fun fourFingerTapReveals() {
        for (i in 1..4) pad.down(f(i, 300f * i, 500f), i.toLong())
        for (i in 1..4) pad.up(i, 100L + i)
        assertEquals(1, reveals)
    }

    @Test fun volumeKeysAreButtonsHeldWhileTheKeyIs() {
        val buttons = MouseButtons(r)
        val v = VolumeButtons(buttons)
        assertTrue(v.onKey(android.view.KeyEvent.KEYCODE_VOLUME_UP, true, 0))
        v.onKey(android.view.KeyEvent.KEYCODE_VOLUME_UP, true, 1) // repeat
        v.onKey(android.view.KeyEvent.KEYCODE_VOLUME_UP, false, 0)
        v.onKey(android.view.KeyEvent.KEYCODE_VOLUME_DOWN, true, 0)
        v.onKey(android.view.KeyEvent.KEYCODE_VOLUME_DOWN, false, 0)
        assertEquals(listOf("button 0 down", "button 0 up", "button 1 down", "button 1 up"), r.take())
    }

    @Test fun aButtonHeldByTwoSourcesGoesUpWithTheLast() {
        val buttons = MouseButtons(r)
        buttons.press(0, MouseButtons.Source.VOLUME)
        buttons.click(0, MouseButtons.Source.TOUCH) // a tap while the volume key holds it: nothing new
        buttons.press(0, MouseButtons.Source.TOUCH)
        buttons.release(0, MouseButtons.Source.VOLUME)
        assertEquals(listOf("button 0 down"), r.take())
        buttons.release(0, MouseButtons.Source.TOUCH)
        assertEquals(listOf("button 0 up"), r.take())
    }

    @Test fun relativeMouseKeepsFractionsAndSendsButtonChanges() {
        val m = RelativeMouse(r, MouseButtons(r))
        m.move(0.4f, 0.4f)
        m.move(0.4f, -0.2f)
        m.move(0.4f, 0f)
        assertEquals(listOf("move 1 0"), r.take())
        m.buttonState(1)
        m.buttonState(3)
        m.buttonState(0)
        assertEquals(listOf("button 0 down", "button 1 down", "button 0 up", "button 1 up"), r.take())
        m.scroll(0f, 1f)
        m.scroll(0.5f, -0.5f)
        m.scroll(0.5f, 0f)
        assertEquals(listOf("scroll 0 1", "scroll 1 0"), r.take())
    }
}
