#!/usr/bin/env python3
"""Render the committed t44 QR matrices as grayscale camera frames, with no QR library.

pijade/tools/t44_matrix.py needs segno to build pijade/tools/t44_matrix.h, and segno is not
installed anywhere in this project.  The header is committed, so the matrices themselves are
available: each is packed row by row, most significant bit leftmost.  This renders one of them
at any frame size, which is what a resolution change needs -- the same scene sampled twice.

The QR is centred and scaled to `--fill` of the frame HEIGHT.  The default 0.93 is the geometry
a user actually produces: main/camera.c draws a guide box of 224 screen pixels out of 240, and
the screen always shows a square crop of the camera frame, so a QR held to fill the guide covers
224/240 of the frame height at any camera resolution.  Scaling by height (not width) keeps the
same scene when the aspect ratio changes.

    t44_frames.py dense 320 240 dense320.gray
    t44_frames.py dense 640 480 dense640.gray
"""
import os
import re
import sys


def load_matrix(header_path, name):
    text = open(header_path, encoding='utf-8').read()
    entry = re.search(r'\{\s*"%s"\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\w+)\s*\}' % re.escape(name), text)
    if not entry:
        raise SystemExit('no matrix named %r in %s' % (name, header_path))
    version, n, symbol = int(entry.group(1)), int(entry.group(2)), entry.group(3)
    body = re.search(r'%s\[(\d+)\]\s*=\s*\{(.*?)\};' % re.escape(symbol), text, re.S)
    if not body:
        raise SystemExit('no array body for %r' % symbol)
    data = [int(b, 16) for b in re.findall(r'0x([0-9a-fA-F]{2})', body.group(2))]
    stride = (n + 7) // 8
    if len(data) != int(body.group(1)) or len(data) != stride * n:
        raise SystemExit('array %s is %d bytes; %d rows of %d expected' % (symbol, len(data), n, stride))
    rows = [[bool(data[r * stride + c // 8] & (0x80 >> (c % 8))) for c in range(n)] for r in range(n)]
    return version, n, rows


def render(rows, n, width, height, fill):
    scale = int(height * fill) // n
    if scale < 1:
        raise SystemExit('frame %dx%d is too small for %d modules at fill %.2f' % (width, height, n, fill))
    side = n * scale
    x0, y0 = (width - side) // 2, (height - side) // 2
    frame = bytearray(b'\xff' * (width * height))
    for r, row in enumerate(rows):
        for c, dark in enumerate(row):
            if not dark:
                continue
            for dy in range(scale):
                start = (y0 + r * scale + dy) * width + x0 + c * scale
                frame[start:start + scale] = b'\x00' * scale
    return frame, scale, side


def main():
    if len(sys.argv) not in (5, 6):
        raise SystemExit(__doc__)
    name, width, height, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
    fill = float(sys.argv[5]) if len(sys.argv) == 6 else 0.93
    header = os.path.join(os.path.dirname(os.path.abspath(__file__)), 't44_matrix.h')
    version, n, rows = load_matrix(header, name)
    frame, scale, side = render(rows, n, width, height, fill)
    open(out, 'wb').write(frame)
    print('%s: version %d, %d modules -> %dx%d frame, %d px/module, QR side %d px'
          % (out, version, n, width, height, scale, side))


main()
