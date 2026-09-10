#!/usr/bin/env python3
"""Raw RGB565 dump -> PNG (no dependencies; zlib + struct).

Byte order: the first byte is the high byte. Jade stores colors in panel wire order
(main/display.c: TFT_RED = 0x00F8, meaning 0xF800). Reversing this makes red appear blue.

Usage: rgb2png.py <source.rgb565> <target.png> [WIDTHxHEIGHT]
Display size is now a build option (--display WxH), so it is not fixed.
"""
import struct, sys, zlib

SCALE = 2
W, H = 320, 200  # default; overridden by the 3rd argument

def chunk(tag, data):
    c = tag + data
    return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)

def main(src, dst):
    raw = open(src, 'rb').read()
    assert len(raw) == W * H * 2, f'expected {W*H*2}, got {len(raw)}'
    rows = []
    for y in range(H):
        px = bytearray()
        for x in range(W):
            v = (raw[(y * W + x) * 2] << 8) | raw[(y * W + x) * 2 + 1]
            r = ((v >> 11) & 0x1f) * 255 // 31
            g = ((v >> 5) & 0x3f) * 255 // 63
            b = (v & 0x1f) * 255 // 31
            px += bytes((r, g, b)) * SCALE
        rows += [b'\x00' + bytes(px)] * SCALE
    idat = zlib.compress(b''.join(rows), 9)
    hdr = struct.pack('>IIBBBBB', W * SCALE, H * SCALE, 8, 2, 0, 0, 0)
    open(dst, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', hdr) + chunk(b'IDAT', idat) + chunk(b'IEND', b''))
    print(f'{dst} written')

if len(sys.argv) > 3:
    W, H = (int(v) for v in sys.argv[3].lower().split('x'))
main(sys.argv[1], sys.argv[2])
