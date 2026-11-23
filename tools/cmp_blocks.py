#!/usr/bin/env python3
"""按 32x32 分块看差异热点。

整屏缩放后做镜像，亚像素采样位置变了，文字边缘必然有 1~2px 的抗锯齿差，
所以"整体差异率"不能当判据。真正的判据是：**有没有整块区域全错**——
如果某个控件（比如弹层）没跟着变换，它所在的分块差异会接近 100%，
而单纯的锯齿噪点半块差异只有百分之几。
"""
import sys
from PIL import Image, ImageChops

BLOCK = 32
TOL = 32


def main():
    base = Image.open(sys.argv[1]).convert('RGB')
    variants = [
        ('rot180',  sys.argv[2], Image.ROTATE_180),
        ('mirrorH', sys.argv[3], Image.FLIP_LEFT_RIGHT),
        ('mirrorV', sys.argv[4], Image.FLIP_TOP_BOTTOM),
    ]
    W, H = base.size
    for name, path, op in variants:
        act = Image.open(path).convert('RGB')
        exp = base.transpose(op)
        d = ImageChops.difference(exp, act).convert('L')
        px = d.load()
        worst = []
        for by in range(0, H, BLOCK):
            for bx in range(0, W, BLOCK):
                bad = 0
                n = 0
                for y in range(by, min(by + BLOCK, H)):
                    for x in range(bx, min(bx + BLOCK, W)):
                        n += 1
                        if px[x, y] > TOL:
                            bad += 1
                if n:
                    worst.append((bad / float(n), bx, by))
        worst.sort(reverse=True)
        total_bad = sum(1 for y in range(H) for x in range(W) if px[x, y] > TOL)
        print('%-8s 整屏差异 %.3f%%  最差分块:' % (name, 100.0 * total_bad / (W * H)))
        for r, bx, by in worst[:3]:
            print('           %5.1f%%  @ (%d,%d)' % (r * 100, bx, by))


main()
