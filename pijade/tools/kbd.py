"""Drive Jade's word-entry keyboard; READ the selected cell from the screen.

Counting steps does not work here. The keyboard is restricted by the BIP39 prefix:
X is never drawn because no word starts with 'x'; after 'v', only A/E/I/O remain.
The distance between letters therefore cannot be known without reading the screen.
Read visible cells and selection, press direction keys to reach the target, and
verify every press with jadectl.Jade.press.

No color constant is used. Settings > Display > Theme can select any of five
accent colors (main/gui.c:230), so the criterion is color-independent:
a drawn cell's top edge is NOT black, and a selected cell's interior IS filled.

Usage:
    import jadectl, kbd
    j = jadectl.Jade(sock)
    kbd.word(j, 'AB', 0)   # 'abandon': A, B, then the first suggested word"""
import struct

W, H = 240, 240
BLACK = 0x0000
STEP = 24         # cell spacing (px)
EDGE_DX = 2      # probe point on the top edge
INNER_DX, INNER_DY = 3, 6  # inside the box, away from letter pixels
# Measured: each band's top-edge y and first-cell x.
BANDS = ((98, 2, 'ABCDEFGHIJ'), (146, 10, 'KLMNOPQRS'), (194, 18, 'TUVWXYZ'))
BACKSPACE = (194, 210, '<')  # backspace; separate cell to the right of the third band
_LETTER_BAND = {c: i for i, (_, _, letters) in enumerate(BANDS) for c in letters}
_LETTER_BAND[BACKSPACE[2]] = 2


def pixels(j):
    """Screen RGB565 contents as big-endian 16-bit values.

    Grid positions were measured on the 240x240 panel. main/Kconfig.projbuild:493
    also supports 240x135, so checking width alone is insufficient: another geometry
    can shift probe points and SILENTLY return the wrong letter."""
    if (j.w, j.h) != (W, H):
        raise RuntimeError('kbd grid measured for %dx%d px panel; display is %dx%d px' % (
            W, H, j.w, j.h))
    data = j.display_bytes()
    return struct.unpack('>%dH' % (len(data) // 2), data)


def _state(px, y0, x0):
    """(is_drawn, is_selected)."""
    drawn = px[y0 * W + x0 + EDGE_DX] != BLACK
    selected = px[(y0 + INNER_DY) * W + x0 + INNER_DX] != BLACK
    return drawn, selected


def cells(px):
    """Cells DRAWN on screen: [(letter, is_selected)]. Disabled letters are not drawn."""
    out = []
    for y0, x_start, letters in BANDS:
        for i, h in enumerate(letters):
            drawn, sel = _state(px, y0, x_start + i * STEP)
            if drawn:
                out.append((h, sel))
    y0, x0, h = BACKSPACE
    drawn, sel = _state(px, y0, x0)
    if drawn:
        out.append((h, sel))
    return out


def selected(px):
    for h, s in cells(px):
        if s:
            return h
    return None


def is_keyboard(px):
    """Is this the keyboard screen?

    Finding cells alone is insufficient: the word-selection carousel's highlight
    sits at the middle band's y, filling ALL nine probe points and causing it to
    be mistaken for a keyboard (measured 2026-08-27). A keyboard has exactly one
    selected cell and cells in at least two bands; the carousel satisfies neither."""
    h = cells(px)
    if sum(1 for _, s in h if s) != 1:
        return False
    return len({_LETTER_BAND[c] for c, _ in h}) >= 2


def forward_direction(j):
    """MEASURE which way 'right' moves through the list; cache the result.

    With Flip Orientation enabled (main/gui.c:309), gui_next() selects the previous
    item, so 'right' moves backward. Measure instead of assuming: keyboard selection
    is readable, allowing one direction measurement here to also drive the suggestion
    carousel, whose text cannot be read."""
    cached = getattr(j, 'kbd_forward', None)
    if cached is not None:
        return cached
    px = pixels(j)
    if not is_keyboard(px):
        raise RuntimeError('direction calibration requires the keyboard screen')
    items = [h for h, _ in cells(px)]
    before = items.index(selected(px))
    j.press('right')
    new = pixels(j)
    after = [h for h, _ in cells(new)].index(selected(new))
    n = len(items)
    if (after - before) % n == 1:
        j.kbd_forward = 'right'
    elif (before - after) % n == 1:
        j.kbd_forward = 'left'
    else:
        raise RuntimeError("'right' did not move one step in the list (%d -> %d)" % (before, after))
    return j.kbd_forward


def forget_calibration(j):
    """Discard the cached direction measurement; the next request measures again."""
    if hasattr(j, 'kbd_forward'):
        del j.kbd_forward


def _go_to_target(j, target):
    """Take the shortest path to the target; return the reached letter without selecting it."""
    forward = forward_direction(j)
    backward = 'left' if forward == 'right' else 'right'
    px = pixels(j)
    if not is_keyboard(px):
        raise RuntimeError('expected keyboard screen')
    items = [h for h, _ in cells(px)]
    if target not in items:
        raise RuntimeError('%r is not currently active; active cells: %s' % (target, ''.join(items)))
    n = len(items)
    distance = (items.index(target) - items.index(selected(px))) % n
    direction, steps = (forward, distance) if distance <= n - distance else (backward, n - distance)
    for _ in range(steps):
        j.press(direction)
    return selected(pixels(j))


def select_letter(j, target):
    """Navigate to and select the target letter. Raise if it is currently disabled.

    Settings > Display > Flip Orientation INVALIDATES the direction cache
    (main/gui.c:309): gui_next() reverses direction while the cache keeps the old
    measurement. No RPC reports the setting change, so detect it by FAILURE TO REACH
    the target: if the first attempt lands on a different letter, discard the cache,
    remeasure direction, and retry before. A second failure is a real error and is raised.
    Remeasuring every call would also work but adds one press and two full-frame reads
    per letter; direction changes during a session only when the setting changes."""
    for last_attempt in (False, True):
        reached = _go_to_target(j, target)
        if reached == target:
            j.press('click')
            return
        if last_attempt:
            raise RuntimeError('target was %r but reached %r' % (target, reached))
        forget_calibration(j)


def word(j, letters, suggestion_steps=0):
    """Enter letters, advance suggestion_steps positions in the suggestion list, and select.

    Once sufficiently narrowed, Jade leaves the keyboard for the 'Select word N'
    carousel; verify the keyboard ACTUALLY closed after entering the letters.
    The carousel is alphabetical; suggestion_steps is its position (0 = first match)."""
    # Measure direction on the KEYBOARD, since it cannot be measured in the carousel.
    # Calibrate here first, but select_letter may discard and remeasure the cache, so
    # read the direction for the carousel AFTER all letters are entered.
    forward_direction(j)
    for h in letters:
        select_letter(j, h)
    if is_keyboard(pixels(j)):
        raise RuntimeError('expected suggestion list, still on keyboard (%r was insufficient)' % letters)
    forward = forward_direction(j)
    for _ in range(suggestion_steps):
        j.press(forward)
    j.press('click')
