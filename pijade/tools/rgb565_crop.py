"""Crop and enlarge a region of a raw RGB565 dump into a PNG (no dependencies).

For inspecting small text (such as the "Grid: B2" label on the parts screen) pixel by pixel.
Usage: rgb565_crop.py <source.rgb565> <target.png> <x0> <y0> <width> <height> <scale>
"""
import struct, sys, zlib
src, dst, W, H, x0, y0, cw, ch, scale = sys.argv[1], sys.argv[2], 240, 240, *map(int, sys.argv[3:])
raw = open(src,'rb').read()
rows=[]
for y in range(y0, y0+ch):
    row=bytearray([0])
    for _ in range(scale):
        pass
    row=bytearray([0])
    line=bytearray()
    for x in range(x0, x0+cw):
        i=(y*W+x)*2
        px=(raw[i]<<8)|raw[i+1]
        r=((px>>11)&0x1f)<<3; g=((px>>5)&0x3f)<<2; b=(px&0x1f)<<3
        line += bytes([r,g,b])*scale
    for _ in range(scale):
        rows.append(b'\x00'+bytes(line))
w,h=cw*scale,ch*scale
def chunk(t,d):
    c=t+d
    return struct.pack('>I',len(d))+c+struct.pack('>I',zlib.crc32(c)&0xffffffff)
png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(b''.join(rows)))+chunk(b'IEND',b'')
open(dst,'wb').write(png)
print('written', dst, w, h)
