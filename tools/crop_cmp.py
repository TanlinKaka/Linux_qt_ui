#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Make a side-by-side (mock | shot) zoom of one region.

usage: crop_cmp.py <mock> <shot> <x0> <y0> <x1> <y1> <scale> <out>
"""
import sys
from PIL import Image, ImageDraw

mp, sp = sys.argv[1], sys.argv[2]
x0, y0, x1, y1 = [int(v) for v in sys.argv[3:7]]
sc = float(sys.argv[7])
out = sys.argv[8]

a = Image.open(mp).convert('RGB').crop((x0, y0, x1, y1))
b = Image.open(sp).convert('RGB').crop((x0, y0, x1, y1))
w, h = a.size
W, H = int(w * sc), int(h * sc)
a = a.resize((W, H), Image.LANCZOS)
b = b.resize((W, H), Image.LANCZOS)

gap = 12
canvas = Image.new('RGB', (W * 2 + gap, H + 18), (0, 80, 160))
canvas.paste(a, (0, 18))
canvas.paste(b, (W + gap, 18))
d = ImageDraw.Draw(canvas)
d.text((4, 4), "MOCK", fill=(255, 255, 255))
d.text((W + gap + 4, 4), "RENDER", fill=(255, 255, 255))
canvas.save(out)
print(out, canvas.size)
