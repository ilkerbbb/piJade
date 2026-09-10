"""Item 39 measurement: when does the list scroll indicator reach the ends?

Why pixels: the indicator is a four-cell column; the lit cells are the only visible evidence.
Read display bytes rather than recalculating the code's arithmetic.
Usage (in the container): python3 t39_scrollbar.py <sock> <tag>
"""
import sys, time, jadectl

sock = sys.argv[1]
tag = sys.argv[2] if len(sys.argv) > 2 else "t39"
j = jadectl.Jade(sock)
W, H = j.w, j.h

# The bar occupies the rightmost 3% (LIST_SCROLLBAR_PERCENT, main/ui/dialogs.c), below the header.
BAR_X = W - 4
WHITE = 0xF79E


def bar_rows():
    d = j.display_bytes()
    return [(d[2 * (y * W + BAR_X)] << 8) | d[2 * (y * W + BAR_X) + 1] for y in range(H)]


def cells():
    """Return lit cells as a four-unit pattern."""
    rows = bar_rows()
    lit = [y for y, v in enumerate(rows) if v >= WHITE]
    if not lit:
        return "....", (None, None)
    # Use a fixed area to count unlit cells too; deriving it from the lit range would
    # always make the pattern look full.
    top, bot = BAR_TOP, H - 1
    span = bot - top + 1
    pat = ""
    for i in range(4):
        y0 = top + (span * i) // 4
        y1 = top + (span * (i + 1)) // 4 - 1
        pat += "#" if rows[(y0 + y1) // 2] >= WHITE else "."
    return pat, (min(lit), max(lit))


def press(ev, wait=0.35):
    j.btn(ev)
    time.sleep(wait)


# Measure the bar top: the topmost lit pixel in the initial frame is its top at offset 0.
rows0 = bar_rows()
lit0 = [y for y, v in enumerate(rows0) if v >= WHITE]
if not lit0:
    print("NO BAR; no scroll indicator found on the display")
    sys.exit(1)
BAR_TOP = min(lit0)
print("BAR_TOP=%d" % BAR_TOP)

seen = []
for step in range(0, 14):
    pat, rng = cells()
    seen.append(pat)
    print("step %2d: %s   (lit %s)" % (step, pat, rng))
    press("down")
print("PATTERNS: " + " ".join(seen))
