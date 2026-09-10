import unittest

import kbd


class FakeJade:
    def __init__(self, w, h):
        self.w = w
        self.h = h

    def display_bytes(self):
        return bytes(self.w * self.h * 2)


class KbdGeometryTest(unittest.TestCase):
    def test_unmeasured_geometries_raise_explicit_errors(self):
        for w, h in ((240, 135), (240, 241), (320, 240)):
            with self.subTest(w=w, h=h):
                with self.assertRaises(RuntimeError) as error:
                    kbd.pixels(FakeJade(w, h))
                self.assertIn('240x240', str(error.exception))
                self.assertIn('%dx%d' % (w, h), str(error.exception))

    def test_measured_geometry_passes(self):
        self.assertEqual(len(kbd.pixels(FakeJade(240, 240))), 240 * 240)


def _frame(drawn, selected_letter):
    """Create a 240x240 RGB565 frame with the given cells DRAWN and one SELECTED.

    Fill only kbd's probe points, without drawing letter pixels: the criterion is
    color-independent (nonblack top edge and filled box interior)."""
    buf = bytearray(kbd.W * kbd.H * 2)

    def put(y, x):
        i = (y * kbd.W + x) * 2
        buf[i] = 0xFF
        buf[i + 1] = 0xFF

    all_cells = [(y0, x_start + i * kbd.STEP, h)
             for y0, x_start, letters in kbd.BANDS
             for i, h in enumerate(letters)]
    all_cells.append((kbd.BACKSPACE[0], kbd.BACKSPACE[1], kbd.BACKSPACE[2]))
    for y0, x0, h in all_cells:
        if h in drawn:
            put(y0, x0 + kbd.EDGE_DX)
            if h == selected_letter:
                put(y0 + kbd.INNER_DY, x0 + kbd.INNER_DX)
    return bytes(buf)


class FakeKeyboard:
    """Fake device responding to direction keys like a real keyboard.

    actual_forward specifies which list direction the 'right' event takes; on the
    device with Flip Orientation it becomes 'left' (main/gui.c:309).

    If close_after_clicks is set, leave the keyboard after that many selections and
    show a single band. kbd.is_keyboard returns False there, simulating the real
    'Select word N' carousel."""

    KEYBOARD = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ'
    CAROUSEL = 'KLMNOPQRS'     # one band; is_keyboard does not classify it as a keyboard

    def __init__(self, actual_forward='right', close_after_clicks=None):
        self.w, self.h = kbd.W, kbd.H
        self.idx = 0
        self.actual_forward = actual_forward
        self.close_after_clicks = close_after_clicks
        self.clicks = []
        self.frozen = False   # True: direction keys do nothing

    @property
    def letters(self):
        if self.close_after_clicks is not None and len(self.clicks) >= self.close_after_clicks:
            return list(self.CAROUSEL)
        return list(self.KEYBOARD)

    def display_bytes(self):
        letters = self.letters
        return _frame(letters, letters[self.idx % len(letters)])

    def press(self, event):
        letters = self.letters
        if event == 'click':
            self.clicks.append(letters[self.idx % len(letters)])
            self.idx = 0     # selection returns to the start when the screen changes
            return
        if self.frozen:
            return
        steps = 1 if event == self.actual_forward else -1
        self.idx = (self.idx + steps) % len(letters)


class KbdDirectionTest(unittest.TestCase):
    def test_reaches_target_in_both_directions(self):
        for actual in ('right', 'left'):
            with self.subTest(actual=actual):
                j = FakeKeyboard(actual_forward=actual)
                kbd.select_letter(j, 'D')
                self.assertEqual(j.clicks, ['D'])
                self.assertEqual(j.kbd_forward, actual)

    def test_stale_cache_repairs_itself(self):
        """After Flip Orientation, the cache still holds the old direction.

        The old behavior raised RuntimeError and blocked progress; the new behavior
        discards the measurement, recalibrates, and reaches the target."""
        j = FakeKeyboard(actual_forward='right')
        j.kbd_forward = 'left'          # measurement left over from the previous orientation
        kbd.select_letter(j, 'D')
        self.assertEqual(j.clicks, ['D'])
        self.assertEqual(j.kbd_forward, 'right')

    def test_real_error_still_raises(self):
        """Self-repair must not silently swallow a broken device."""
        j = FakeKeyboard()
        j.kbd_forward = 'right'
        j.frozen = True
        with self.assertRaises(RuntimeError):
            kbd.select_letter(j, 'D')
        self.assertEqual(j.clicks, [])

    def test_word_uses_corrected_carousel_direction(self):
        """word() must read carousel direction AFTER entering the letters.

        The cache can be corrected during letter entry; the old code captured direction
        before entering letters and therefore used a stale value in the carousel."""
        j = FakeKeyboard(actual_forward='right', close_after_clicks=1)
        j.kbd_forward = 'left'          # measurement left over from the previous orientation
        kbd.word(j, 'B', 2)
        self.assertEqual(j.kbd_forward, 'right')
        self.assertEqual(j.clicks[0], 'B')
        # Carousel starts at K; two steps in the corrected direction reach M.
        self.assertEqual(j.clicks[1], 'M')


if __name__ == '__main__':
    unittest.main()
