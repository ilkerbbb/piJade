# -*- coding: utf-8 -*-
"""Convert a QR in a display dump to a grayscale frame for the emulator camera.

Why redraw: the 240x240 display dump leaves a 20 px border around the QR, or 2.5 modules;
quirc expects a 4-module quiet zone and SILENTLY fails on the raw dump (measured 2026-08-27:
200x200 bounding box, quirc_count=0). Read each module and redraw with a clean quiet zone.

Write two files: <output> (320x240 grayscale, for set_camera_bytes) and <output>.rgb565
(for independent verification with screen_qr_decode).

Usage:
    screen_qr_to_camera.py <source.rgb565> <x0> <y0> <module_count> <module_px> <output.gray>
Read x0/y0/module_px from the dark pixel bounding box printed by screen_qr_decode
(example: 20 20 25 8 -> v2; 18 18 29 7 -> v3).
"""
import struct, sys

src, x0, y0, mods, ppm, out = (sys.argv[1], int(sys.argv[2]), int(sys.argv[3]),
                               int(sys.argv[4]), int(sys.argv[5]), sys.argv[6])
W = H = 240
CAM_W, CAM_H = 320, 240   # same contract as jadectl.CAM_FRAME_W/H
QUIET = 4
# Use the largest scale that fits, capped at 6. At a fixed 6, a 33-module code
# took (33+8)*6 = 246 px and exceeded the 240 px frame height; address QRs are this size.
# Smaller codes still use 6, preserving earlier measurement anchors.
SCALE = min(6, CAM_H // (mods + 2 * QUIET))

raw = open(src, 'rb').read()
assert len(raw) == W * H * 2, 'expected a %dx%d display dump' % (W, H)
px = struct.unpack('>%dH' % (W * H), raw)

side = (mods + 2 * QUIET) * SCALE
assert side <= CAM_H, 'frame does not fit camera height: %d px' % side
ox, oy = (CAM_W - side) // 2, (CAM_H - side) // 2

frame = bytearray(b'\xff' * (CAM_W * CAM_H))
dark = 0
for r in range(mods):
    for c in range(mods):
        if px[(y0 + r * ppm + ppm // 2) * W + (x0 + c * ppm + ppm // 2)] != 0x0000:
            continue
        dark += 1
        for dy in range(SCALE):
            base = (oy + (QUIET + r) * SCALE + dy) * CAM_W + ox + (QUIET + c) * SCALE
            frame[base:base + SCALE] = b'\x00' * SCALE

open(out, 'wb').write(bytes(frame))
rgb = bytearray()
for v in frame:
    rgb += struct.pack('>H', ((v >> 3) << 11) | ((v >> 2) << 5) | (v >> 3))
open(out + '.rgb565', 'wb').write(bytes(rgb))
print('frame: %s  dark modules %d/%d' % (out, dark, mods * mods))
