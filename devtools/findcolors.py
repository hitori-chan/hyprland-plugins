#!/usr/bin/env python3
# findcolors — where each color is DRAWN in a capture.
# usage: findcolors.py <png> <name=r,g,b>...  ->  "<name> <cx> <cy> <pixels>"
# per color (tolerance 10 per channel; -1 -1 0 when absent)
import sys

from PIL import Image

im = Image.open(sys.argv[1]).convert("RGB")
W, H = im.size
px = im.load()
want = {}
for arg in sys.argv[2:]:
    name, rgb = arg.split("=")
    want[name] = tuple(int(v) for v in rgb.split(","))
acc = {n: [0, 0, 0] for n in want}
for y in range(H):
    for x in range(W):
        p = px[x, y]
        for n, c in want.items():
            if abs(p[0] - c[0]) <= 10 and abs(p[1] - c[1]) <= 10 and abs(p[2] - c[2]) <= 10:
                a = acc[n]
                a[0] += x
                a[1] += y
                a[2] += 1
for n, (sx, sy, k) in acc.items():
    print(n, sx // k if k else -1, sy // k if k else -1, k)
