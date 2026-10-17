#!/usr/bin/env python3
"""Tile PNG screenshots (written by the port, filter 0 / stored) into one
image for quick review. Usage: montage.py out.png cols in1.png in2.png ..."""
import struct, sys, zlib

def readpng(p):
    d = open(p, 'rb').read(); pos = 8; w = h = 0; idat = b''
    while pos < len(d):
        ln = struct.unpack('>I', d[pos:pos+4])[0]; t = d[pos+4:pos+8]
        c = d[pos+8:pos+8+ln]
        if t == b'IHDR': w, h = struct.unpack('>II', c[:8])
        if t == b'IDAT': idat += c
        pos += 12 + ln
    raw = zlib.decompress(idat); st = w * 3 + 1
    return w, h, [raw[y*st+1:(y+1)*st] for y in range(h)]

def writepng(p, w, h, rows):
    raw = b''.join(b'\0' + r for r in rows)
    def chunk(t, c):
        return struct.pack('>I', len(c)) + t + c + struct.pack('>I', zlib.crc32(t + c) & 0xffffffff)
    with open(p, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b'IDAT', zlib.compress(raw, 6)))
        f.write(chunk(b'IEND', b''))

out, cols = sys.argv[1], int(sys.argv[2])
imgs = [readpng(p) for p in sys.argv[3:]]
cw = max(i[0] for i in imgs); ch = max(i[1] for i in imgs)
rows_n = (len(imgs) + cols - 1) // cols
W, H = cw * cols, ch * rows_n
canvas = [bytearray(W * 3) for _ in range(H)]
for k, (w, h, rows) in enumerate(imgs):
    ox, oy = (k % cols) * cw, (k // cols) * ch
    for y in range(h):
        canvas[oy + y][ox*3:(ox + w)*3] = rows[y]
writepng(out, W, H, [bytes(r) for r in canvas])
