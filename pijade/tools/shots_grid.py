"""Arrange multiple RGB565 display dumps into a single PNG grid (no dependencies).

An audit produces dozens of screens; view them on one page instead of opening each one.
Usage: shots_grid.py <target.png> <WIDTHxHEIGHT> <columns> <source.rgb565...>
"""
import struct
import sys
import zlib

GAP = 4
BG = (40, 40, 40)


def rgb565_to_rows(path, w, h):
    data = open(path, 'rb').read()
    assert len(data) == w * h * 2, f'{path}: {len(data)} bytes, expected {w*h*2}'
    rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            i = (y * w + x) * 2
            v = (data[i] << 8) | data[i + 1]
            row += bytes((((v >> 11) & 0x1F) << 3, ((v >> 5) & 0x3F) << 2, (v & 0x1F) << 3))
        rows.append(row)
    return rows


def main():
    out, size, cols = sys.argv[1], sys.argv[2], int(sys.argv[3])
    srcs = sys.argv[4:]
    w, h = (int(v) for v in size.lower().split('x'))
    rows_n = (len(srcs) + cols - 1) // cols
    W = cols * w + (cols + 1) * GAP
    H = rows_n * h + (rows_n + 1) * GAP
    canvas = [bytearray(bytes(BG) * W) for _ in range(H)]
    for idx, src in enumerate(srcs):
        r, c = divmod(idx, cols)
        x0 = GAP + c * (w + GAP)
        y0 = GAP + r * (h + GAP)
        for y, row in enumerate(rgb565_to_rows(src, w, h)):
            canvas[y0 + y][x0 * 3:(x0 + w) * 3] = row
    raw = b''.join(b'\x00' + bytes(row) for row in canvas)

    def chunk(tag, payload):
        return (struct.pack('>I', len(payload)) + tag + payload
                + struct.pack('>I', zlib.crc32(tag + payload) & 0xFFFFFFFF))

    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(raw, 9))
           + chunk(b'IEND', b''))
    open(out, 'wb').write(png)
    print(f'{out} written ({len(srcs)} screens, {W}x{H})')


if __name__ == '__main__':
    main()
