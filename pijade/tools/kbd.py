"""Drive Jade's word-entry keyboard; READ the selected cell from the screen.

Counting steps does not work here. The keyboard is restricted by the BIP39 prefix:
X is never drawn because no word starts with 'x'; after 'v', only A/E/I/O remain.
The distance between letters therefore cannot be known without reading the screen.
Read visible cells and selection, press direction keys to reach the target, and
verify every press with jadectl.Jade.press.

No color constant is used. Settings > Display > Theme can select any of five
accent colors (main/gui.c:45-49), so the criterion is color-independent:
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

    With Flip Orientation enabled (main/gui.c:2849-2856), gui_next() selects the previous
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
    (main/gui.c:2849-2856): gui_next() reverses direction while the cache keeps the old
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


# --- The passphrase keyboard (main/ui/keyboard.c) ---
#
# This is a DIFFERENT keyboard from the word one above and keeps its own table: the word
# keyboard draws only the letters a bip39 prefix still allows, while this grid is fixed and
# every cell of the current page is always drawn.  Geometry follows the code: the title bar
# takes the top 20% (main/ui/dialogs.c:9, :191) and the rest is a four-row vertical split
# (keyboard.c:31) whose first row is the typed text; each keyboard row is ten 24 px cells,
# shifted right by half the cells its line does not use, and each button keeps a 2 px margin
# (keyboard.c:71-73, :84-87).  That puts the three rows at the same y the word keyboard's
# bands sit at, which is why the probe offsets above are reused rather than measured again.
ASCII_PAGES = (
    ('abcdefghij', 'klmnopqrst', 'uvwxyz |>S'),
    ('ABCDEFGHIJ', 'KLMNOPQRST', 'UVWXYZ |>S'),
    ('1234567890', '!"#$%&\'()', '*+,-./ |>S'),
    (':;<=>?@', '[\\]^_`~', '{|} |>S'),
)
ASCII_CELL_W = 24
ASCII_ROW_TOP = (98, 146, 194)
ASCII_COLS = 10

# The last three cells of the last row are backspace, next-keyboard and enter.  The C source
# writes them as '|', '>' and 'S' because their fonts draw symbols in place of those letters
# (keyboard.c:48-49), and '|' is also a real character on the fourth page, so they are found by
# POSITION and never by character.
ASCII_ACTIONS = 3


def ascii_cell_x(row, page):
    """Left edge of the first button of a row on this page."""
    return (ASCII_COLS - len(ASCII_PAGES[page][row])) * ASCII_CELL_W // 2 + 2


def ascii_cells(px, page):
    """[(row, col, is_selected)] for the cells this page draws."""
    out = []
    for row, line in enumerate(ASCII_PAGES[page]):
        x0 = ascii_cell_x(row, page)
        for col in range(len(line)):
            drawn, sel = _state(px, ASCII_ROW_TOP[row], x0 + col * ASCII_CELL_W)
            if drawn:
                out.append((row, col, sel))
    return out


def ascii_selected(px, page):
    """(row, col) of the selected cell, or None when the screen is not this page."""
    cells_here = ascii_cells(px, page)
    if len(cells_here) != sum(len(line) for line in ASCII_PAGES[page]):
        return None
    chosen = [(r, c) for r, c, s in cells_here if s]
    return chosen[0] if len(chosen) == 1 else None


def ascii_find(page, char):
    """(row, col) of a character on this page; the three action cells are never returned."""
    for row, line in enumerate(ASCII_PAGES[page]):
        limit = len(line) - ASCII_ACTIONS if row == len(ASCII_PAGES[page]) - 1 else len(line)
        for col in range(limit):
            if line[col] == char:
                return row, col
    raise RuntimeError('%r is not on keyboard page %d' % (char, page))


def ascii_action(page, which):
    """(row, col) of an action cell: 0 backspace, 1 next keyboard, 2 enter."""
    row = len(ASCII_PAGES[page]) - 1
    return row, len(ASCII_PAGES[page][row]) - ASCII_ACTIONS + which


_ASCII_OPPOSITE = {'down': 'up', 'up': 'down', 'right': 'left', 'left': 'right'}


def _ascii_calibrate(j, page):
    """MEASURE which key moves the selection one cell forward on each axis; cache the pair.

    Flip Orientation reverses both axes (main/gui.c:2849-2868) and no RPC reports the setting,
    so the meaning of 'down' and 'right' is read off the screen once rather than assumed.  Two
    presses cost less than being wrong silently, and the calibration cell is left where it is:
    the caller navigates from wherever the selection ends up."""
    cached = getattr(j, 'kbd_ascii_dirs', None)
    if cached is not None:
        return cached
    rows = len(ASCII_PAGES[page])
    start = ascii_selected(pixels(j), page)
    if start is None:
        raise RuntimeError('direction calibration requires keyboard page %d' % page)
    j.press('down')
    moved = ascii_selected(pixels(j), page)
    if moved is None or moved[1] != start[1]:
        raise RuntimeError("'down' did not stay in its column (%s -> %s)" % (start, moved))
    vertical = 'down' if (moved[0] - start[0]) % rows == 1 else 'up'
    cols = len(ASCII_PAGES[page][moved[0]])
    j.press('right')
    again = ascii_selected(pixels(j), page)
    if again is None or again[0] != moved[0]:
        raise RuntimeError("'right' did not stay in its row (%s -> %s)" % (moved, again))
    horizontal = 'right' if (again[1] - moved[1]) % cols == 1 else 'left'
    j.kbd_ascii_dirs = {'v': vertical, 'h': horizontal}
    return j.kbd_ascii_dirs


def ascii_click(j, page, target):
    """Reach a cell and select it, reading the selection back after every press.

    The step is recomputed from the screen each time rather than counted out in advance: a
    vertical move onto a shorter row also moves the column (the number pages have a nine cell
    row), so a plan made at the start would land one cell out and click the wrong key."""
    dirs = _ascii_calibrate(j, page)
    for _ in range(2 * (len(ASCII_PAGES[page]) + ASCII_COLS)):
        cur = ascii_selected(pixels(j), page)
        if cur is None:
            raise RuntimeError('keyboard page %d left the screen mid-move' % page)
        if cur == target:
            j.press('click')
            return
        if cur[0] != target[0]:
            axis, size, wanted = 'v', len(ASCII_PAGES[page]), target[0] - cur[0]
        else:
            axis, size, wanted = 'h', len(ASCII_PAGES[page][cur[0]]), target[1] - cur[1]
        distance = wanted % size
        j.press(dirs[axis] if distance <= size - distance else _ASCII_OPPOSITE[dirs[axis]])
    raise RuntimeError('cell %s not reached on page %d' % (target, page))


def ascii_text(j, text, page=0):
    """Type text on the passphrase keyboard and press its enter key.

    The page is COUNTED, not read: pages 0 and 1 draw the same ten-ten-ten grid and differ
    only in glyphs, which nothing here reads.  Every page change is still checked, because
    ascii_selected() returns None whenever the cells on screen are not the ones the counted
    page should draw."""
    for char in text:
        target_page = next((p for p in range(len(ASCII_PAGES)) if _ascii_has(p, char)), None)
        if target_page is None:
            raise RuntimeError('%r is on no keyboard page' % char)
        while page != target_page:
            ascii_click(j, page, ascii_action(page, 1))
            page = (page + 1) % len(ASCII_PAGES)
        ascii_click(j, page, ascii_find(page, char))
    ascii_click(j, page, ascii_action(page, 2))
    return page


def _ascii_has(page, char):
    try:
        ascii_find(page, char)
        return True
    except RuntimeError:
        return False
