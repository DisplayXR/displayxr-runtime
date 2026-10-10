#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
"""Sample colour_probe_metal_macos's regions out of its atlas capture.

Usage: read_atlas.py [png] [COLSxROWS]
       (defaults: $TMPDIR/displayxr_atlas.colour_probe_metal_macos.png, 2x1)

Prints one line per region with the median RGBA of a 7x7 patch at the region
centre, in EVERY tile (the probe submits the same image to each view with zero
disparity, so the tiles must agree).
"""
import os
import statistics
import sys

from PIL import Image

png = (sys.argv[1] if len(sys.argv) > 1 else
       os.path.join(os.environ.get("TMPDIR", "/tmp"), "displayxr_atlas.colour_probe_metal_macos.png"))
cols, rows = (int(v) for v in (sys.argv[2] if len(sys.argv) > 2 else "2x1").split("x"))

# (name, centre x, centre y) as fractions of one tile — see main.mm.
REGIONS = [
    ("proj_38", 0.25, 0.95),
    ("proj_200", 0.75, 0.95),
    ("L1_srgb_opaque_38", 0.125, 0.225),
    ("L2_unorm_opaque_38", 0.325, 0.225),
    ("L3_srgb_black_a128_over200", 0.75, 0.225),
    ("L4_srgb_white_a128_over38", 0.25, 0.675),
]

im = Image.open(png).convert("RGBA")
W, H = im.size
tw = W // cols
th = H // rows
px = im.load()
print(f"atlas {W}x{H}, {cols}x{rows} tiles of {tw}x{th}")
for name, fx, fy in REGIONS:
    per_tile = []
    for t in range(cols * rows):
        cx = (t % cols) * tw + int(fx * tw)
        cy = (t // cols) * th + int(fy * th)
        patch = [px[x, y] for x in range(cx - 3, cx + 4) for y in range(cy - 3, cy + 4)]
        per_tile.append(tuple(int(statistics.median(p[c] for p in patch)) for c in range(4)))
    print(f"{name:30s} " + "  ".join(f"t{t}=RGBA{v}" for t, v in enumerate(per_tile)))
