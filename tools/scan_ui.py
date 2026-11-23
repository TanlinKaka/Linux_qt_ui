#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Scan a UI mockup PNG and print ink bounding boxes.

usage: scan_ui.py <img> <x0> <y0> <x1> <y1> [thr] [gap]
  thr = per-channel tolerance vs background (default 12)
  gap = max gap (px) merged into one column block (default 8)
"""
import sys
from PIL import Image
from collections import Counter


def main():
    path = sys.argv[1]
    x0, y0, x1, y1 = [int(v) for v in sys.argv[2:6]]
    thr = int(sys.argv[6]) if len(sys.argv) > 6 else 12
    gap = int(sys.argv[7]) if len(sys.argv) > 7 else 8

    im = Image.open(path).convert('RGB')
    r = im.crop((x0, y0, x1, y1))
    w, h = r.size

    c = Counter(r.getdata())
    bg = c.most_common(1)[0][0]
    print("BOX %d,%d %dx%d  BG %d,%d,%d" % (x0, y0, w, h, bg[0], bg[1], bg[2]))

    d = r.tobytes()
    br, bgg, bb = bg
    cols = [0] * w
    rows = [0] * h
    for y in range(h):
        base = y * w * 3
        for x in range(w):
            j = base + x * 3
            if (abs(d[j] - br) > thr or abs(d[j + 1] - bgg) > thr
                    or abs(d[j + 2] - bb) > thr):
                cols[x] += 1
                rows[y] += 1

    # group columns, merging gaps <= gap
    segs = []
    start = -1
    last = -1
    for x in range(w):
        if cols[x] > 0:
            if start < 0:
                start = x
            last = x
        else:
            if start >= 0 and x - last > gap:
                segs.append((start, last))
                start = -1
    if start >= 0:
        segs.append((start, last))

    for (a, b) in segs:
        # rows inside this column block
        rs = [0] * h
        for y in range(h):
            base = y * w * 3
            s = 0
            for x in range(a, b + 1):
                j = base + x * 3
                if (abs(d[j] - br) > thr or abs(d[j + 1] - bgg) > thr
                        or abs(d[j + 2] - bb) > thr):
                    s += 1
            rs[y] = s
        rsegs = []
        st = -1
        lt = -1
        for y in range(h):
            if rs[y] > 0:
                if st < 0:
                    st = y
                lt = y
            else:
                if st >= 0 and y - lt > 3:
                    rsegs.append((st, lt))
                    st = -1
        if st >= 0:
            rsegs.append((st, lt))

        print("COL x=%d..%d (abs %d..%d) w=%d" % (a, b, x0 + a, x0 + b, b - a + 1))
        for (p, q) in rsegs:
            print("   ROW y=%d..%d (abs %d..%d) h=%d" % (p, q, y0 + p, y0 + q, q - p + 1))


main()
