"""Combine rgb565 frames (240x240) into an NxM mosaic. Usage: mosaic.py <output.rgb565> <columns> <frames...>"""
import sys
out, cols, files = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
W = H = 240
frames = [open(f, 'rb').read() for f in files]
rows = (len(frames) + cols - 1) // cols
buf = bytearray(b'\x00' * (W * cols * H * rows * 2))
for k, fr in enumerate(frames):
    r, c = divmod(k, cols)
    for y in range(H):
        src = fr[y * W * 2:(y + 1) * W * 2]
        off = ((r * H + y) * (W * cols) + c * W) * 2
        buf[off:off + W * 2] = src
open(out, 'wb').write(buf)
print(f'{W*cols}x{H*rows}')
