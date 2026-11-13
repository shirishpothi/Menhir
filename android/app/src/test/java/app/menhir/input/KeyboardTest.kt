package app.menhir.input

import android.view.KeyEvent
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class KeyboardTest {
    @Test fun lettersDigitsAndPunctuation() {
        assertEquals(0x04, KeyMap.hid(KeyEvent.KEYCODE_A))
        assertEquals(0x1D, KeyMap.hid(KeyEvent.KEYCODE_Z))
        assertEquals(0x1E, KeyMap.hid(KeyEvent.KEYCODE_1))
        assertEquals(0x26, KeyMap.hid(KeyEvent.KEYCODE_9))
        assertEquals(0x27, KeyMap.hid(KeyEvent.KEYCODE_0))
        assertEquals(0x28, KeyMap.hid(KeyEvent.KEYCODE_ENTER))
        assertEquals(0x2A, KeyMap.hid(KeyEvent.KEYCODE_DEL))
        assertEquals(0x4C, KeyMap.hid(KeyEvent.KEYCODE_FORWARD_DEL))
        assertEquals(0x35, KeyMap.hid(KeyEvent.KEYCODE_GRAVE))
        assertEquals(0x3A, KeyMap.hid(KeyEvent.KEYCODE_F1))
        assertEquals(0x45, KeyMap.hid(KeyEvent.KEYCODE_F12))
        assertEquals(0x52, KeyMap.hid(KeyEvent.KEYCODE_DPAD_UP))
        assertEquals(0x62, KeyMap.hid(KeyEvent.KEYCODE_NUMPAD_0))
        assertNull(KeyMap.hid(KeyEvent.KEYCODE_VOLUME_UP))
        assertNull(KeyMap.hid(KeyEvent.KEYCODE_BACK))
    }

    @Test fun modifiersAreTheMacs() {
        assertEquals(Hid.LEFT_COMMAND, KeyMap.hid(KeyEvent.KEYCODE_META_LEFT))
        assertEquals(Hid.RIGHT_COMMAND, KeyMap.hid(KeyEvent.KEYCODE_META_RIGHT))
        assertEquals(Hid.LEFT_OPTION, KeyMap.hid(KeyEvent.KEYCODE_ALT_LEFT))
        assertEquals(Hid.RIGHT_OPTION, KeyMap.hid(KeyEvent.KEYCODE_ALT_RIGHT))
        assertEquals(Hid.LEFT_CONTROL, KeyMap.hid(KeyEvent.KEYCODE_CTRL_LEFT))
        assertEquals(Hid.RIGHT_CONTROL, KeyMap.hid(KeyEvent.KEYCODE_CTRL_RIGHT))
        assertEquals(Hid.LEFT_SHIFT, KeyMap.hid(KeyEvent.KEYCODE_SHIFT_LEFT))
    }

    @Test fun commandCIsAChord() {
        val r = Recorder()
        val k = HardwareKeyboard(r)
        k.onKey(KeyEvent.KEYCODE_META_LEFT, true, 0)
        k.onKey(KeyEvent.KEYCODE_C, true, 0)
        k.onKey(KeyEvent.KEYCODE_C, false, 0)
        k.onKey(KeyEvent.KEYCODE_META_LEFT, false, 0)
        assertEquals(listOf("hid 0xe3 down", "hid 0x06 down", "hid 0x06 up", "hid 0xe3 up"), r.take())
    }

    @Test fun androidRepeatIsDropped() {
        val r = Recorder()
        val k = HardwareKeyboard(r)
        assertTrue(k.onKey(KeyEvent.KEYCODE_A, true, 0))
        assertTrue(k.onKey(KeyEvent.KEYCODE_A, true, 1))
        assertTrue(k.onKey(KeyEvent.KEYCODE_A, true, 2))
        k.onKey(KeyEvent.KEYCODE_A, false, 0)
        assertEquals(listOf("hid 0x04 down", "hid 0x04 up"), r.take())
    }

    @Test fun fnLayerAndItsRelease() {
        val r = Recorder()
        val k = HardwareKeyboard(r)
        assertTrue(k.onKey(KeyEvent.KEYCODE_FUNCTION, true, 0))
        k.onKey(KeyEvent.KEYCODE_DPAD_UP, true, 0)
        k.onKey(KeyEvent.KEYCODE_FUNCTION, false, 0)
        k.onKey(KeyEvent.KEYCODE_DPAD_UP, false, 0) // released after Fn: still Page Up's release
        k.onKey(KeyEvent.KEYCODE_DEL, true, 0)
        k.onKey(KeyEvent.KEYCODE_DEL, false, 0)
        assertEquals(listOf("hid 0x4b down", "hid 0x4b up", "hid 0x2a down", "hid 0x2a up"), r.take())
        k.onKey(KeyEvent.KEYCODE_FUNCTION, true, 0)
        k.onKey(KeyEvent.KEYCODE_DEL, true, 0)
        assertEquals(listOf("hid 0x4c down"), r.take())
    }

    @Test fun keysThatAreNotTheMacsPassThrough() {
        val k = HardwareKeyboard(Recorder())
        assertFalse(k.onKey(KeyEvent.KEYCODE_VOLUME_DOWN, true, 0))
        assertFalse(k.onKey(KeyEvent.KEYCODE_BACK, true, 0))
    }

    @Test fun releaseAllLetsGo() {
        val r = Recorder()
        var after = 0
        val k = HardwareKeyboard(r) { after++ }
        k.onKey(KeyEvent.KEYCODE_SHIFT_LEFT, true, 0)
        k.onKey(KeyEvent.KEYCODE_Q, true, 0)
        r.take()
        k.releaseAll()
        assertEquals(setOf("hid 0xe1 up", "hid 0x14 up"), r.take().toSet())
        k.onKey(KeyEvent.KEYCODE_Q, false, 0) // the late release sends nothing more
        assertEquals(emptyList<String>(), r.take())
        assertEquals(0, after)
    }

    @Test fun aKeyAfterAStickyModifierReleasesIt() {
        val r = Recorder()
        val sticky = StickyKeys(r)
        val k = HardwareKeyboard(r) { sticky.releaseAll() }
        sticky.toggle(LinuxKeys.COMMAND)
        k.onKey(KeyEvent.KEYCODE_Q, true, 0)
        k.onKey(KeyEvent.KEYCODE_Q, false, 0)
        assertEquals(listOf("key 125 down", "hid 0x14 down", "hid 0x14 up", "key 125 up"), r.take())
    }

    @Test fun typingText() {
        val r = Recorder()
        assertTrue(LinuxKeys.type(r, 'a'))
        assertTrue(LinuxKeys.type(r, 'A'))
        assertTrue(LinuxKeys.type(r, '!'))
        assertFalse(LinuxKeys.type(r, 'é'))
        assertEquals(
            listOf("key 30 down", "key 30 up", "key 42 down", "key 30 down", "key 30 up", "key 42 up",
                "key 42 down", "key 2 down", "key 2 up", "key 42 up"),
            r.take(),
        )
    }

    @Test fun composingTextIsCorrected() {
        val r = Recorder()
        val t = SoftTyper(r)
        t.setComposing("he")
        t.setComposing("hel")
        t.commit("help")
        // h e, then l, then p: no Backspace while each step only adds.
        assertEquals(listOf("key 35", "key 18", "key 38", "key 25"), r.take().filter { it.endsWith("down") }.map { it.removeSuffix(" down") })
        t.setComposing("teh")
        t.commit("the")
        val downs = r.take().filter { it.endsWith("down") }.map { it.removeSuffix(" down") }
        // t e h, Backspace twice, h e.
        assertEquals(listOf("key 20", "key 18", "key 35", "key 14", "key 14", "key 35", "key 18"), downs)
    }
}
