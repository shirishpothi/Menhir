package app.menhir.input

import android.view.KeyEvent

/** USB HID keyboard usages (HID Usage Tables, page 0x07): what the Mac's USB keyboard sends. */
object Hid {
    const val ENTER = 0x28
    const val ESCAPE = 0x29
    const val BACKSPACE = 0x2A
    const val TAB = 0x2B
    const val HOME = 0x4A
    const val PAGE_UP = 0x4B
    const val FORWARD_DELETE = 0x4C
    const val END = 0x4D
    const val PAGE_DOWN = 0x4E
    const val RIGHT = 0x4F
    const val LEFT = 0x50
    const val DOWN = 0x51
    const val UP = 0x52
    const val LEFT_CONTROL = 0xE0
    const val LEFT_SHIFT = 0xE1
    const val LEFT_OPTION = 0xE2
    const val LEFT_COMMAND = 0xE3
    const val RIGHT_CONTROL = 0xE4
    const val RIGHT_SHIFT = 0xE5
    const val RIGHT_OPTION = 0xE6
    const val RIGHT_COMMAND = 0xE7
    const val C = 0x06
    const val V = 0x19

    fun isModifier(usage: Int) = usage in 0xE0..0xE7
}

/** Android key codes to HID usages. A PC keyboard's Windows key is Command and Alt is Option. */
object KeyMap {
    private val table: Map<Int, Int> = buildMap {
        for (i in 0..25) put(KeyEvent.KEYCODE_A + i, 0x04 + i)
        for (i in 1..9) put(KeyEvent.KEYCODE_1 + i - 1, 0x1E + i - 1)
        put(KeyEvent.KEYCODE_0, 0x27)
        put(KeyEvent.KEYCODE_ENTER, Hid.ENTER)
        put(KeyEvent.KEYCODE_ESCAPE, Hid.ESCAPE)
        put(KeyEvent.KEYCODE_DEL, Hid.BACKSPACE)
        put(KeyEvent.KEYCODE_TAB, Hid.TAB)
        put(KeyEvent.KEYCODE_SPACE, 0x2C)
        put(KeyEvent.KEYCODE_MINUS, 0x2D)
        put(KeyEvent.KEYCODE_EQUALS, 0x2E)
        put(KeyEvent.KEYCODE_LEFT_BRACKET, 0x2F)
        put(KeyEvent.KEYCODE_RIGHT_BRACKET, 0x30)
        put(KeyEvent.KEYCODE_BACKSLASH, 0x31)
        put(KeyEvent.KEYCODE_SEMICOLON, 0x33)
        put(KeyEvent.KEYCODE_APOSTROPHE, 0x34)
        put(KeyEvent.KEYCODE_GRAVE, 0x35)
        put(KeyEvent.KEYCODE_COMMA, 0x36)
        put(KeyEvent.KEYCODE_PERIOD, 0x37)
        put(KeyEvent.KEYCODE_SLASH, 0x38)
        put(KeyEvent.KEYCODE_CAPS_LOCK, 0x39)
        for (i in 0..11) put(KeyEvent.KEYCODE_F1 + i, 0x3A + i)
        put(KeyEvent.KEYCODE_SYSRQ, 0x46)
        put(KeyEvent.KEYCODE_SCROLL_LOCK, 0x47)
        put(KeyEvent.KEYCODE_BREAK, 0x48)
        put(KeyEvent.KEYCODE_INSERT, 0x49)
        put(KeyEvent.KEYCODE_MOVE_HOME, Hid.HOME)
        put(KeyEvent.KEYCODE_PAGE_UP, Hid.PAGE_UP)
        put(KeyEvent.KEYCODE_FORWARD_DEL, Hid.FORWARD_DELETE)
        put(KeyEvent.KEYCODE_MOVE_END, Hid.END)
        put(KeyEvent.KEYCODE_PAGE_DOWN, Hid.PAGE_DOWN)
        put(KeyEvent.KEYCODE_DPAD_RIGHT, Hid.RIGHT)
        put(KeyEvent.KEYCODE_DPAD_LEFT, Hid.LEFT)
        put(KeyEvent.KEYCODE_DPAD_DOWN, Hid.DOWN)
        put(KeyEvent.KEYCODE_DPAD_UP, Hid.UP)
        put(KeyEvent.KEYCODE_NUM_LOCK, 0x53)
        put(KeyEvent.KEYCODE_NUMPAD_DIVIDE, 0x54)
        put(KeyEvent.KEYCODE_NUMPAD_MULTIPLY, 0x55)
        put(KeyEvent.KEYCODE_NUMPAD_SUBTRACT, 0x56)
        put(KeyEvent.KEYCODE_NUMPAD_ADD, 0x57)
        put(KeyEvent.KEYCODE_NUMPAD_ENTER, 0x58)
        for (i in 1..9) put(KeyEvent.KEYCODE_NUMPAD_1 + i - 1, 0x59 + i - 1)
        put(KeyEvent.KEYCODE_NUMPAD_0, 0x62)
        put(KeyEvent.KEYCODE_NUMPAD_DOT, 0x63)
        put(KeyEvent.KEYCODE_MENU, 0x65)
        put(KeyEvent.KEYCODE_NUMPAD_EQUALS, 0x67)
        put(KeyEvent.KEYCODE_NUMPAD_COMMA, 0x85)
        put(KeyEvent.KEYCODE_RO, 0x87)
        put(KeyEvent.KEYCODE_YEN, 0x89)
        put(KeyEvent.KEYCODE_CTRL_LEFT, Hid.LEFT_CONTROL)
        put(KeyEvent.KEYCODE_SHIFT_LEFT, Hid.LEFT_SHIFT)
        put(KeyEvent.KEYCODE_ALT_LEFT, Hid.LEFT_OPTION)
        put(KeyEvent.KEYCODE_META_LEFT, Hid.LEFT_COMMAND)
        put(KeyEvent.KEYCODE_CTRL_RIGHT, Hid.RIGHT_CONTROL)
        put(KeyEvent.KEYCODE_SHIFT_RIGHT, Hid.RIGHT_SHIFT)
        put(KeyEvent.KEYCODE_ALT_RIGHT, Hid.RIGHT_OPTION)
        put(KeyEvent.KEYCODE_META_RIGHT, Hid.RIGHT_COMMAND)
    }

    /** With Fn held, as on a Mac keyboard: the arrows page and jump, delete deletes forward. */
    private val fnLayer: Map<Int, Int> = mapOf(
        KeyEvent.KEYCODE_DPAD_UP to Hid.PAGE_UP,
        KeyEvent.KEYCODE_DPAD_DOWN to Hid.PAGE_DOWN,
        KeyEvent.KEYCODE_DPAD_LEFT to Hid.HOME,
        KeyEvent.KEYCODE_DPAD_RIGHT to Hid.END,
        KeyEvent.KEYCODE_DEL to Hid.FORWARD_DELETE,
    )

    fun hid(keyCode: Int, fn: Boolean = false): Int? = (if (fn) fnLayer[keyCode] else null) ?: table[keyCode]
}

/**
 * A hardware keyboard. Each key goes to the Mac as its HID usage, down and up; Android's auto-repeat
 * is dropped, since macOS repeats a held key itself. Fn is not a key the Mac's USB keyboard has, so it
 * stays here and changes the keys pressed with it ([KeyMap.hid]). A key's release sends the usage its
 * press sent, even if Fn changed in between.
 */
class HardwareKeyboard(private val mac: MacInput, private val afterKey: () -> Unit = {}) {
    private val held = mutableMapOf<Int, Int>() // Android key code -> usage sent
    private var fn = false

    /** True if the key was the Mac's. Volume and system keys are not. */
    fun onKey(keyCode: Int, down: Boolean, repeat: Int): Boolean {
        if (keyCode == KeyEvent.KEYCODE_FUNCTION) {
            fn = down
            return true
        }
        if (down) {
            if (repeat > 0 || keyCode in held) return held.containsKey(keyCode) || KeyMap.hid(keyCode, fn) != null
            val usage = KeyMap.hid(keyCode, fn) ?: return false
            held[keyCode] = usage
            mac.keyHid(usage, true)
        } else {
            val usage = held.remove(keyCode) ?: return KeyMap.hid(keyCode, fn) != null
            mac.keyHid(usage, false)
            if (!Hid.isModifier(usage)) afterKey()
        }
        return true
    }

    /** Android kept a release (focus moved, a system shortcut took over): the Mac must not think it is down. */
    fun releaseAll() {
        for (usage in held.values) mac.keyHid(usage, false)
        held.clear()
        fn = false
    }

    val anyKeyDown get() = held.values.any { !Hid.isModifier(it) }
}

/** Linux input key codes, which the Mac's keyboard device also takes: for typing text and the key row. */
object LinuxKeys {
    const val ESCAPE = 1
    const val BACKSPACE = 14
    const val TAB = 15
    const val ENTER = 28
    const val CONTROL = 29
    const val SHIFT = 42
    const val OPTION = 56
    const val COMMAND = 125
    const val UP = 103
    const val LEFT = 105
    const val RIGHT = 106
    const val DOWN = 108

    private val letters = "qwertyuiop".mapIndexed { i, c -> c to 16 + i } +
        "asdfghjkl".mapIndexed { i, c -> c to 30 + i } +
        "zxcvbnm".mapIndexed { i, c -> c to 44 + i }
    private val letterCodes = letters.toMap()
    private val plain = mapOf(
        '1' to 2, '2' to 3, '3' to 4, '4' to 5, '5' to 6, '6' to 7, '7' to 8, '8' to 9, '9' to 10, '0' to 11,
        '-' to 12, '=' to 13, '\t' to 15, '[' to 26, ']' to 27, '\n' to 28, ';' to 39, '\'' to 40, '`' to 41,
        '\\' to 43, ',' to 51, '.' to 52, '/' to 53, ' ' to 57,
    )
    private val shifted = mapOf(
        '!' to 2, '@' to 3, '#' to 4, '$' to 5, '%' to 6, '^' to 7, '&' to 8, '*' to 9, '(' to 10, ')' to 11,
        '_' to 12, '+' to 13, '{' to 26, '}' to 27, ':' to 39, '"' to 40, '~' to 41, '|' to 43,
        '<' to 51, '>' to 52, '?' to 53,
    )

    /** The key and whether it needs Shift, for a character a U.S. keyboard can type; null otherwise. */
    fun forChar(c: Char): Pair<Int, Boolean>? {
        letterCodes[c]?.let { return it to false }
        letterCodes[c.lowercaseChar()]?.let { return it to true }
        plain[c]?.let { return it to false }
        shifted[c]?.let { return it to true }
        return null
    }

    fun press(mac: MacInput, code: Int, shift: Boolean = false) {
        if (shift) mac.key(SHIFT, true)
        mac.key(code, true)
        mac.key(code, false)
        if (shift) mac.key(SHIFT, false)
    }

    /** Types what it can of [c]; false if a U.S. keyboard has no key for it. */
    fun type(mac: MacInput, c: Char): Boolean {
        val (code, shift) = forChar(c) ?: return false
        press(mac, code, shift)
        return true
    }
}

/** Control, Option and Command on the key row stay down from a tap until the next key or click. */
class StickyKeys(private val mac: MacInput) {
    val down = mutableSetOf<Int>()
    var onChange: () -> Unit = {}

    fun toggle(code: Int) {
        if (down.remove(code)) mac.key(code, false) else {
            down.add(code)
            mac.key(code, true)
        }
        onChange()
    }

    fun releaseAll() {
        if (down.isEmpty()) return
        for (code in down) mac.key(code, false)
        down.clear()
        onChange()
    }
}

/**
 * Text from the on-screen keyboard, typed into the Mac. An input method may show a word as it is
 * being composed and change it before committing it, so what was typed for the composing text is
 * corrected with Backspace to whatever comes next.
 */
class SoftTyper(private val mac: MacInput, private val afterText: () -> Unit = {}) {
    private var composing = ""

    fun setComposing(text: String) {
        replace(composing, text)
        composing = text
    }

    fun commit(text: String) {
        replace(composing, text)
        composing = ""
        afterText()
    }

    fun finishComposing() {
        composing = ""
    }

    fun deleteBefore(count: Int) {
        if (composing.isNotEmpty()) {
            val keep = (composing.length - count).coerceAtLeast(0)
            setComposing(composing.substring(0, keep))
            return
        }
        repeat(count) { LinuxKeys.press(mac, LinuxKeys.BACKSPACE) }
        afterText()
    }

    private fun replace(old: String, new: String) {
        val common = old.commonPrefixWith(new).length
        repeat(old.length - common) { LinuxKeys.press(mac, LinuxKeys.BACKSPACE) }
        for (c in new.substring(common)) LinuxKeys.type(mac, c)
    }
}
