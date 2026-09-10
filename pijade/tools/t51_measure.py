#!/usr/bin/env python3
"""Measurement side of task 51: the camera screen top strip.

Inputs are display dumps (RGB565, 240x240); t51_strip.sh drives the device. This is separate
because the guide pixel set comes from SOURCE: reproduce render_qrguide() rectangles from
main/gui.c exactly to measure corner visibility numerically.

A positive control checks this set by painting pre-change opaque button cells onto a copy
of the dump; upper corners must drop from 120 to 6. This is computed without a fixture file
whose absence could silently skip the control and remove the measurement baseline.

Usage: python3 t51_measure.py <flat80.rgb565> <flat40.rgb565> <scene.rgb565>
"""
import os
import re
import sys

W = H = 240
# Middle strip cell: horizontal split 20/60/20 gives 60% of 240, with the strip's height.
# The title is drawn here (make_camera_activity, main/ui/camera.c).
CELL_X, CELL_WIDTH, CELL_HEIGHT = (W * 20) // 100, (W * 60) // 100, (H * 20) // 100
# Title of the screen visited in this run (Set Clock > Continue -> handle_scan_qr, dashboard.c).
EXPECTED_TITLE = 'Clock QR'
FONT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'main', 'fonts',
                        'DejaVuSans18.c')
WHITE = (255, 255, 255)

# Old button cell: the top row is 20% of the screen (CAMERA_HEADER_PCNT, main/ui.h), with the
# upstream horizontal split 15/70/15, so 15% of 240 = 36 columns. Opaque buttons left only
# 6 guide-corner pixels visible at x 36-39. The actual pre-change dump on 2026-09-08 measured
# exactly 6/120, independently confirming the number below by calculation and measurement.
OLD_CELL_WIDTH = (W * 15) // 100
OLD_CELL_HEIGHT = (H * 20) // 100
OLD_CORNER_REMAINING = 6


def load(path):
    d = open(path, 'rb').read()
    if len(d) != W * H * 2:
        raise SystemExit('%s: %d bytes, expected %d' % (path, len(d), W * H * 2))
    px = []
    for i in range(0, len(d), 2):
        v = (d[i] << 8) | d[i + 1]
        px.append((((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31))
    return px


def guide_set():
    """Pixel set of render_qrguide() (main/gui.c) rectangles, corner by corner."""
    gwidth, glength, gnubbin = 2, 30, 2
    x1, y1, x2, y2 = 0, 0, W, H
    square = min(x2 - x1, y2 - y1)
    inset = square // 30
    left, right = x1 + (x2 - x1 - square) // 2 + inset, x2 - (x2 - x1 - square) // 2 - inset
    top, bottom = y1 + (y2 - y1 - square) // 2 + inset, y2 - (y2 - y1 - square) // 2 - inset

    def rect(x, y, w, h, target):
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                if 0 <= xx < W and 0 <= yy < H:
                    target.add((xx, yy))

    corners = {}
    for name, rectangles in (
        ('left-top', [(left, top, gwidth, glength), (left, top, glength, gwidth),
                     (left + glength, top, gnubbin, gwidth // 2), (left, top + glength, gwidth // 2, gnubbin)]),
        ('right-top', [(right - gwidth, top, gwidth, glength), (right - glength, top, glength, gwidth),
                     (right - glength - gnubbin, top, gnubbin, gwidth // 2),
                     (right - gwidth // 2, top + glength, gwidth // 2, gnubbin)]),
        ('left-bottom', [(left, bottom - glength, gwidth, glength), (left, bottom - gwidth, glength, gwidth),
                     (left + glength, bottom - gwidth // 2, gnubbin, gwidth // 2),
                     (left, bottom - glength - gnubbin, gwidth // 2, gnubbin)]),
        ('right-bottom', [(right - gwidth, bottom - glength, gwidth, glength),
                     (right - glength, bottom - gwidth, glength, gwidth),
                     (right - glength - gnubbin, bottom - gwidth // 2, gnubbin, gwidth // 2),
                     (right - gwidth // 2, bottom - glength - gnubbin, gwidth // 2, gnubbin)]),
    ):
        s = set()
        for d in rectangles:
            rect(d[0], d[1], d[2], d[3], s)
        corners[name] = s
    return corners


def font():
    """Decode tft_Dejavu18 records: character -> (width, height, yOffset, xOffset, xDelta, data)."""
    source = open(FONT, encoding='utf-8').read()
    body = source.index('tft_Dejavu18[]')
    block = source[source.index('{', body):source.index('};', body)]
    b = [int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', block)]
    glyphs, i = {}, 4
    while i < len(b) and b[i] != 0xff:
        code, yoffset, glyph_width, glyph_height, xoffset, xdelta = b[i:i + 6]
        start = i + 6
        glyphs[chr(code)] = (glyph_width, glyph_height, yoffset, xoffset if xoffset < 0x80 else -(0xFF - xoffset), xdelta, start)
        i = start + ((((glyph_width * glyph_height) - 1) // 8) + 1 if glyph_width else 0)
    return glyphs, b


def expected_ink(text):
    """Box display_print_in_area() paints for this string in the strip's middle cell.

    Width matches display_get_string_width() (main/display.c:664-681); placement centers
    horizontally and vertically as print_proportional_char() does. Verified exactly against
    an emulator dump on 2026-09-08, rather than estimated by the measurement tool.
    """
    glyphs, b = font()
    height = max(v[2] + v[1] for v in glyphs.values())
    width = sum(max(glyphs[c][0], glyphs[c][4]) + 1 for c in text) - 1
    x = CELL_X + (CELL_WIDTH - width) // 2
    y = (CELL_HEIGHT - height) // 2
    box = [10 ** 9, 10 ** 9, -1, -1]
    for c in text:
        glyph_width, glyph_height, yoffset, xoffset, xdelta, start = glyphs[c]
        for j in range(glyph_height):
            for k in range(glyph_width):
                bit = j * glyph_width + k
                if b[start + bit // 8] & (0x80 >> (bit % 8)):
                    cx, cy = x + k + xoffset, y + j + yoffset
                    box = [min(box[0], cx), min(box[1], cy), max(box[2], cx), max(box[3], cy)]
        x += max(glyph_width, xdelta) + 1
    return width, tuple(box)


def gray(p):
    return (p[0] + p[1] + p[2]) // 3


def row_median(px, y0, y1):
    v = sorted(gray(px[y * W + x]) for y in range(y0, y1) for x in range(W))
    return v[len(v) // 2]


def visible(px, group):
    return sum(1 for p in group if px[p[1] * W + p[0]] == WHITE)


def main():
    error = 0

    def report(m):
        nonlocal error
        print('ERROR: ' + m)
        error = 1

    corners = guide_set()
    total = sum(len(s) for s in corners.values())
    print('guide corner set: ' + ', '.join('%s=%d' % (k, len(v)) for k, v in corners.items())
          + ' (total %d)' % total)

    flat80, flat40, scene = (load(a) for a in sys.argv[1:4])

    # Positive control: paint old opaque button cells onto a copy of the dump and recount corners.
    # If the set matches the guide corners, the top two must drop to 6 and the bottom two must be
    # unchanged. Otherwise the "120/120" measurement below is meaningless.
    old = list(flat80)
    for x0 in (0, W - OLD_CELL_WIDTH):
        for y in range(OLD_CELL_HEIGHT):
            for x in range(x0, x0 + OLD_CELL_WIDTH):
                old[y * W + x] = (0, 0, 0)
    previous = {k: visible(old, v) for k, v in corners.items()}
    print('positive control (old button cells repainted): '
          + ', '.join('%s=%d/%d' % (k, previous[k], len(corners[k])) for k in corners))
    for k in corners:
        if k.endswith('top') and previous[k] != OLD_CORNER_REMAINING:
            report('positive control: %s should show %d pixels in old layout, showed %d'
                   % (k, OLD_CORNER_REMAINING, previous[k]))
        if k.endswith('bottom') and previous[k] != visible(flat80, corners[k]):
            report('positive control: %s affected by top strip, set is misplaced' % k)

    # 1. Are all four guide corners fully visible?
    # NO tolerance for the upper pair: this is exactly the defect addressed. For the lower pair,
    # only progress-bar border occlusion is accepted. Measure its rectangle from the dump and check
    # that missing pixels lie on that border; anything elsewhere is a finding.
    border_gray = (32, 32, 32)
    bar = [(i % W, i // W) for i, p in enumerate(flat80) if p == border_gray and i // W >= 190]
    bar_x = {min(x for x, _ in bar), max(x for x, _ in bar)} if bar else set()
    bar_y = range(min(y for _, y in bar), max(y for _, y in bar) + 1) if bar else range(0)
    if bar:
        print('progress bar border: x %d-%d, y %d-%d'
              % (min(bar_x), max(bar_x), bar_y.start, bar_y.stop - 1))
    current = {k: visible(flat80, v) for k, v in corners.items()}
    print('guide now: ' + ', '.join('%s=%d/%d' % (k, current[k], len(corners[k])) for k in corners))
    for k, v in corners.items():
        missing = [p for p in sorted(v) if flat80[p[1] * W + p[0]] != WHITE]
        if not missing:
            continue
        if k.endswith('top'):
            report('%s corner has %d covered pixels: %s' % (k, len(missing), missing[:6]))
            continue
        unrelated = [p for p in missing if not (p[0] in bar_x and p[1] in bar_y)]
        if unrelated:
            report('%s corner has %d covered pixels not attributable to progress bar: %s'
                   % (k, len(unrelated), unrelated[:6]))
        else:
            print('%s: %d pixels under progress bar border (outside this task, geometry '
                  'unchanged)' % (k, len(missing)))

    # 2. Locate the selected back button fill.
    highlight = (0, 137, 98)
    nk = [(i % W, i // W) for i, p in enumerate(flat80) if p == highlight]
    if not nk:
        report('selected button fill not found (highlight color %s)' % (highlight,))
    else:
        box = (min(x for x, _ in nk), min(y for _, y in nk), max(x for x, _ in nk), max(y for _, y in nk))
        print('selected fill box: x %d-%d, y %d-%d (%dx%d)'
              % (box[0], box[2], box[1], box[3], box[2] - box[0] + 1, box[3] - box[1] + 1))
        if box != (12, 12, 35, 35):
            report('fill box should be (12,12)-(35,35)')

    # 3. Did the title move to the strip and leave the center? Text presence alone is insufficient:
    # Jade wraps strings that do not fit at character boundaries and omits a second line outside the
    # cell (display_print_in_area, main/display.c), so even a cropped title would count as present.
    # Calculate the expected ink box from the font and compare it with the dump; a cropped title
    # has a narrower box and cannot pass.
    white_strip = [(x, y) for y in range(CELL_HEIGHT) for x in range(CELL_X, CELL_X + CELL_WIDTH)
                   if flat80[y * W + x] == WHITE]
    whole_guide = set().union(*corners.values())
    middle = sum(1 for y in range(60, 180) for x in range(W)
               if flat80[y * W + x] == WHITE and (x, y) not in whole_guide)
    if not white_strip:
        report('no text in strip center')
    else:
        measured = (min(x for x, _ in white_strip), min(y for _, y in white_strip),
                   max(x for x, _ in white_strip), max(y for _, y in white_strip))
        width, expected = expected_ink(EXPECTED_TITLE)
        print('title %r: string width %d px (cell %d), ink box measured %s expected %s'
              % (EXPECTED_TITLE, width, CELL_WIDTH, measured, expected))
        if measured != expected:
            report('title ink box differs from expected: cropped, displaced or on another screen')
    print('white pixels in image center: %d' % middle)
    if middle:
        report('image center still has %d white pixels' % middle)

    # 4. Dimming: for two different flat tones, the top strip must be half the base.
    for name, px in (('0x80', flat80), ('0x40', flat40)):
        u, a = row_median(px, 0, 9), row_median(px, 190, 199)
        print('%s frame: top strip %d, rest %d (ratio %.2f)' % (name, u, a, (a / u) if u else 0))
        if abs(u * 2 - a) > 8:
            report('%s frame top strip is not half the base (%d and %d)' % (name, u, a))

    # 5. Scene frame: is the image preserved UNDER the strip? An opaque strip would leave the top
    # 48 rows in one tone. First verify the camera is still open: a scannable code closes it and
    # causes measurement on another screen (happened once, 2026-09-08).
    lower_tones = len({gray(scene[y * W + x]) for y in range(60, 180) for x in range(W)})
    tones = len({gray(scene[y * W + x]) for y in range(0, 48) for x in range(W)})
    print('scene frame tone count: strip %d, rest of image %d' % (tones, lower_tones))
    if lower_tones < 8:
        report('scene frame never reached the display (camera may have closed)')
    elif tones < 8:
        report('strip appears to cover the image (tone count %d)' % tones)

    print('51: measurement complete' if not error else '51: FINDINGS PRESENT')
    return error


if __name__ == '__main__':
    sys.exit(main())
