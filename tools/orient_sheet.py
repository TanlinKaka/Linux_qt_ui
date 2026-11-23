#!/usr/bin/env python3
# 把四种朝向候选拼成一张 2x2 对照图，方便一眼挑出「正」的那张。
import sys, os
from PIL import Image, ImageDraw, ImageFont

src = sys.argv[1] if len(sys.argv) > 1 else '/tmp/or'
out = sys.argv[2] if len(sys.argv) > 2 else '/tmp/orient_sheet.png'

names = [
    ('rot0',    'A  rot=0   (no rotation)'),
    ('rot180',  'B  rot=180 (upside down)'),
    ('mirrorH', 'C  mirrorH (left-right)'),
    ('mirrorV', 'D  mirrorV (top-bottom)'),
]

cell_w, cell_h, pad, bar = 660, 495, 16, 30
W = pad + (cell_w + pad) * 2
H = pad + (cell_h + bar + pad) * 2
sheet = Image.new('RGB', (W, H), (30, 30, 30))
d = ImageDraw.Draw(sheet)
try:
    font = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', 20)
except Exception:
    font = ImageFont.load_default()

for i, (key, label) in enumerate(names):
    p = os.path.join(src, 'orient_%s.png' % key)
    if not os.path.exists(p):
        continue
    im = Image.open(p).convert('RGB')
    im = im.resize((cell_w, cell_h), Image.LANCZOS)
    cx = pad + (i % 2) * (cell_w + pad)
    cy = pad + (i // 2) * (cell_h + bar + pad)
    d.text((cx + 6, cy + 4), label, fill=(240, 240, 240), font=font)
    sheet.paste(im, (cx, cy + bar))
    d.rectangle([cx - 1, cy + bar - 1, cx + cell_w, cy + bar + cell_h], outline=(90, 90, 90))

sheet.save(out)
print('sheet', out, sheet.size)
