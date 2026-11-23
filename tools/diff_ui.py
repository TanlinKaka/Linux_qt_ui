#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Compare rendered shot against mockup.

usage: diff_ui.py <mock.png> <shot.png> <out.png> [thr]
prints overall diff % and the worst 16x16 tiles.
"""
import sys
from PIL import Image

mock_p, shot_p, out_p = sys.argv[1:4]
thr = int(sys.argv[4]) if len(sys.argv) > 4 else 24

a = Image.open(mock_p).convert('RGB')
b = Image.open(shot_p).convert('RGB')
if a.size != b.size:
    print("SIZE MISMATCH", a.size, b.size)
    sys.exit(1)

w, h = a.size
da, db = a.tobytes(), b.tobytes()

diff = bytearray(w * h)
n = 0
for i in range(w * h):
    j = i * 3
    if (abs(da[j] - db[j]) > thr or abs(da[j + 1] - db[j + 1]) > thr
            or abs(da[j + 2] - db[j + 2]) > thr):
        diff[i] = 1
        n += 1

print("total %dx%d  diff=%d  %.2f%%" % (w, h, n, 100.0 * n / (w * h)))

# 16x16 tiles
TS = 32
tiles = []
for ty in range(0, h - TS + 1, TS):
    for tx in range(0, w - TS + 1, TS):
        c = 0
        for y in range(ty, ty + TS):
            base = y * w
            for x in range(tx, tx + TS):
                c += diff[base + x]
        if c:
            tiles.append((c, tx, ty))
tiles.sort(reverse=True)
print("worst tiles (count, x, y):")
for t in tiles[:14]:
    print("  %5d  (%4d,%4d)" % t)

# per-band summary
bands = [("header", 0, 112), ("cards", 112, 673), ("readout", 673, 960),
         ("gap", 960, 1064), ("footer", 1064, 1200)]
for name, y0, y1 in bands:
    c = 0
    for y in range(y0, min(y1, h)):
        base = y * w
        for x in range(w):
            c += diff[base + x]
    print("  %-8s y%4d-%4d  %6d  %.2f%%" % (name, y0, y1, c, 100.0 * c / (w * (y1 - y0))))

# diff image
im = Image.new('RGB', (w, h))
px = im.load()
for y in range(h):
    base = y * w
    for x in range(w):
        if diff[base + x]:
            px[x, y] = (255, 60, 60)
        else:
            j = base * 3 + x * 3
            g = (da[j] + da[j + 1] + da[j + 2]) // 3
            px[x, y] = (g // 3, g // 3, g // 3)
im.save(out_p)
print("diff image ->", out_p)
