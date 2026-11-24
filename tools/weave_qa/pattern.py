#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
"""Weave QA test patterns: two side-by-side stereo images, A and B.

Each eye, in display space (W x H per eye):
  - four ArUco markers in the corners, IDENTICAL in both eyes (so the camera finds the
    screen in each lens whatever the weave does); their IDs say which pattern is up:
    pattern A = ids 0..3, pattern B = ids 4..7 (TL, TR, BR, BL);
  - four horizontal rows between them:
        row 0  A: lit for the LEFT eye only      B: lit for the RIGHT eye only
        row 1  black in both eyes                 (room light reflected off the glass)
        row 2  A: lit for the RIGHT eye only     B: lit for the LEFT eye only
        row 3  lit in BOTH eyes                   (reference)
    A lit row is split left to right into SEGMENTS: grey steps of rising brightness (so
    whatever the camera's exposure, some are neither clipped nor lost in noise), then
    solid red, green and blue (the leak per colour).
A perfect weave shows each lens only its own lit row; light in the other row is crosstalk.
The reference row is what a lens sees from BOTH views at once, so in linear light
    (own - black) + (wrong - black) == (reference - black)      (the self-check)
and its grey steps give the camera's response curve inside the same frame (no assumption
about the camera's gamma or exposure).

The images are side by side: left half = left eye, right half = right eye, each half a
full W x H eye (so a 2*W x H PNG). cube_handle_d3d11_win shows them with
DISPLAYXR_WEAVE_PATTERN=A.png;B.png, one half per view, alternating.

    python pattern.py OUT_DIR [--levels dim|normal]

Levels: 'dim' (default) suits a camera whose auto-exposure clips everything above about
grey 60 and cannot be locked (Acer SpatialLabs Eyes). The analysis must use the same levels.
"""
import argparse
import os

import cv2
import numpy as np

W, H = 3840, 2160          # one eye, display space
LIT = 180                  # marker quiet-zone level
LEVELS = {
    # (name, (B, G, R)) left to right. Greys are the response-curve points; colours sit
    # mid-range so the greys around them calibrate their channel.
    "normal": ([(f"grey{v}", (v, v, v)) for v in (16, 32, 48, 64, 96, 128, 176, 224)] +
               [("red", (0, 0, 80)), ("green", (0, 80, 0)), ("blue", (80, 0, 0))]),
    "dim": ([(f"grey{v}", (v, v, v)) for v in (8, 12, 16, 20, 26, 32, 40, 48)] +
            [("red", (0, 0, 32)), ("green", (0, 32, 0)), ("blue", (32, 0, 0))]),
}
SEGMENTS = LEVELS["dim"]
MARK = 300                 # marker side (display px)
QUIET = 50                 # white quiet zone around each marker
MARGIN = 60
ROWS = 4

DICT = cv2.aruco.getPredefinedDictionary(cv2.aruco.DICT_4X4_50)


def set_levels(name):
    """Select the segment levels ('dim' or 'normal') for generation and analysis."""
    global SEGMENTS
    SEGMENTS = LEVELS[name]


def marker_origins():
    s = MARK + 2 * QUIET
    return [(MARGIN, MARGIN), (W - MARGIN - s, MARGIN), (W - MARGIN - s, H - MARGIN - s), (MARGIN, H - MARGIN - s)]


def rows():
    """[(y0, y1)] of the four rows and the (x0, x1) span they share, display px."""
    s = MARK + 2 * QUIET
    y0 = MARGIN + s + 40
    y1 = H - MARGIN - s - 40
    h = (y1 - y0) // ROWS
    return [(y0 + i * h, y0 + (i + 1) * h) for i in range(ROWS)], (MARGIN, W - MARGIN)


def segments():
    """[(x0, x1, name, bgr)] of the lit-row segments, display px."""
    _, (x0, x1) = rows()
    w = (x1 - x0) / len(SEGMENTS)
    return [(int(x0 + i * w), int(x0 + (i + 1) * w), n, c) for i, (n, c) in enumerate(SEGMENTS)]


def lit_rows(pattern, eye):
    """Indices of the rows lit for this eye."""
    first = (pattern == "A") == (eye == "L")     # A: left eye gets row 0
    return [0 if first else 2, 3]


def eye_image(pattern, eye):
    img = np.zeros((H, W, 3), np.uint8)
    ids = range(0, 4) if pattern == "A" else range(4, 8)
    for (x, y), mid in zip(marker_origins(), ids):
        s = MARK + 2 * QUIET
        img[y:y + s, x:x + s] = LIT
        m = cv2.aruco.generateImageMarker(DICT, mid, MARK)
        img[y + QUIET:y + QUIET + MARK, x + QUIET:x + QUIET + MARK] = np.where(m[..., None] > 127, LIT, 0)
    rr, _ = rows()
    for r in lit_rows(pattern, eye):
        y0, y1 = rr[r]
        for sx0, sx1, _, c in segments():
            img[y0:y1, sx0:sx1] = c
    return img


def sbs_image(pattern):
    """Side by side, full resolution per eye: 2*W x H."""
    return np.hstack([eye_image(pattern, e) for e in "LR"])


def write_patterns(out_dir, levels="dim"):
    """Writes pattern_A.png and pattern_B.png; returns their paths."""
    set_levels(levels)
    os.makedirs(out_dir, exist_ok=True)
    paths = []
    for p in "AB":
        path = os.path.join(out_dir, f"pattern_{p}.png")
        cv2.imwrite(path, sbs_image(p))
        paths.append(path)
    return paths


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("out_dir")
    ap.add_argument("--levels", choices=sorted(LEVELS), default="dim")
    a = ap.parse_args()
    for p in write_patterns(a.out_dir, a.levels):
        print("wrote", p)


if __name__ == "__main__":
    main()
