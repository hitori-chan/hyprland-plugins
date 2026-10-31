#!/usr/bin/env python3
# Where each color is DRAWN in a capture.
# usage: findcolors.py <png> <name=r,g,b>...  ->  "<name> <cx> <cy> <pixels>"
# per color (tolerance 10 per channel; -1 -1 0 when absent)
import sys

import numpy as np
from PIL import Image

px = np.asarray(Image.open(sys.argv[1]).convert("RGB"), dtype=np.int16)
for arg in sys.argv[2:]:
    name, rgb = arg.split("=")
    hit = (np.abs(px - [int(v) for v in rgb.split(",")]) <= 10).all(axis=2)
    ys, xs = np.nonzero(hit)
    k = xs.size
    print(name, int(xs.sum()) // k if k else -1, int(ys.sum()) // k if k else -1, k)
