#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
"""Weave QA analysis: crosstalk and eye order of a 3D panel, through a stereo camera.

The panel shows the A/B patterns (pattern.py) full screen. A side-by-side stereo camera
(Acer SpatialLabs Eyes: 3840x2160, left lens = left half) stands where a viewer's eyes
would be, and SR's tracker aims the weave at its lenses (tracking_target = stereo_camera).

  python analyse.py DIR [--levels dim|normal]   analyse cam_*.png frames already in DIR
  python analyse.py --selftest                  a fake camera with known leaks must read back

Per lens and frame: find the ArUco markers (they say pattern A or B), map the pattern into
the camera picture (homography), read every segment of the four rows (own / black / wrong /
reference). The camera's response is unknown and its exposure drifts, so each frame
calibrates itself: the reference row's grey steps (known display levels, seen by the lens
from both views) plus the black row give camera value -> light, per colour channel. Then,
in light:
    crosstalk = (wrong - black) / (own - black)
per segment, skipping any segment with clipped pixels; averaged over A and B (which swap
the rows). Self-check: (own - black) + (wrong - black) should equal (reference - black).
0 % = perfect separation, 100 % = both lenses see the same (no 3D), > 100 % = eyes swapped.
"""
import argparse
import glob
import json
import os
import sys

import cv2
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pattern as gp  # noqa: E402

DETECTOR = cv2.aruco.ArucoDetector(gp.DICT, cv2.aruco.DetectorParameters())
CLIP = 250            # camera value treated as clipped
CLIP_FRAC = 0.01      # a segment with more clipped pixels than this is not used
MIN_SIGNAL = 6.0      # own - black, in camera values, below which a segment is noise
LUMA = np.array([0.0722, 0.7152, 0.2126])   # B, G, R weights of linear light


def display_linear(code):
    """Light the panel emits for an 8-bit value (sRGB-like 2.2), relative to full white."""
    return (np.asarray(code, np.float64) / 255.0) ** 2.2


def marker_corners_display(pattern_ids):
    """Display-space (one eye, 3840x2160) corners of each marker, keyed by id."""
    out = {}
    for (x, y), mid in zip(gp.marker_origins(), pattern_ids):
        x0, y0 = x + gp.QUIET, y + gp.QUIET
        s = gp.MARK
        out[mid] = np.array([[x0, y0], [x0 + s, y0], [x0 + s, y0 + s], [x0, y0 + s]], np.float32)
    return out


def find_screen(gray):
    """-> (pattern, homography display->lens) or None. Needs 2 of the 4 markers."""
    corners, ids, _ = DETECTOR.detectMarkers(gray)
    if ids is None:
        return None
    ids = ids.flatten().tolist()
    for pattern, want in (("A", [0, 1, 2, 3]), ("B", [4, 5, 6, 7])):
        seen = [i for i in want if i in ids]
        if len(seen) >= 2:
            disp = marker_corners_display(want)
            src = np.vstack([disp[i] for i in seen])
            dst = np.vstack([corners[ids.index(i)].reshape(4, 2) for i in seen])
            Hm, _ = cv2.findHomography(src, dst, cv2.RANSAC, 5.0)
            return (pattern, Hm) if Hm is not None else None
    return None


def read_rows(bgr, Hm, scale=0.25):
    """Mean camera value [row][segment][B,G,R] and clipped fraction [row][segment]."""
    S = np.diag([1 / scale, 1 / scale, 1.0])
    warped = cv2.warpPerspective(bgr, Hm @ S, (int(gp.W * scale), int(gp.H * scale)),
                                 flags=cv2.WARP_INVERSE_MAP | cv2.INTER_AREA)
    # The lens may not see the whole screen (foam, frame edge): mark what it does see.
    seen = cv2.warpPerspective(np.full(bgr.shape[:2], 255, np.uint8), Hm @ S,
                               (int(gp.W * scale), int(gp.H * scale)), flags=cv2.WARP_INVERSE_MAP) > 0
    rr, _ = gp.rows()
    lev = np.full((gp.ROWS, len(gp.SEGMENTS), 3), np.nan)
    clip = np.ones((gp.ROWS, len(gp.SEGMENTS)))
    for r, (y0, y1) in enumerate(rr):
        h = y1 - y0
        ys0, ys1 = int((y0 + 0.25 * h) * scale), int((y1 - 0.25 * h) * scale)
        for s, (x0, x1, _, _) in enumerate(gp.segments()):
            w = x1 - x0
            xs0, xs1 = int((x0 + 0.15 * w) * scale), int((x1 - 0.15 * w) * scale)
            if not seen[ys0:ys1, xs0:xs1].all():
                continue
            px = warped[ys0:ys1, xs0:xs1].reshape(-1, 3).astype(np.float64)
            lev[r, s] = px.mean(0)
            clip[r, s] = float((px >= CLIP).any(1).mean())
    return lev, clip, warped


def response(lev, clip):
    """Per channel: camera value -> light (display-linear), from the reference row's greys + black."""
    curves = []
    blk = np.nanmean(lev[1], axis=0)
    for c in range(3):
        pts = [(blk[c], 0.0)]
        for s, (name, col) in enumerate(gp.SEGMENTS):
            if name.startswith("grey") and clip[3, s] <= CLIP_FRAC and not np.isnan(lev[3, s, c]):
                pts.append((lev[3, s, c], float(display_linear(col[c]))))
        pts.sort()
        xs = [p[0] for p in pts]
        if len(pts) < 3 or np.any(np.diff(xs) <= 0.5):   # need a rising curve with 2+ steps
            curves.append(None)
            continue
        curves.append((np.array(xs), np.array([p[1] for p in pts])))
    return curves


def to_light(v, curve):
    xs, ys = curve
    if v > xs[-1] + 1.0:          # brighter than the brightest unclipped reference step
        return np.nan
    return float(np.interp(v, xs, ys))


def lens_frame(bgr):
    """One lens, one frame -> (pattern, {segment: {...}}) or None."""
    gray = cv2.cvtColor(bgr, cv2.COLOR_BGR2GRAY)
    fs = find_screen(gray)
    if fs is None:
        return None
    pattern, Hm = fs
    lev, clip, warped = read_rows(bgr, Hm)
    curves = response(lev, clip)
    return pattern, lev, clip, curves, warped


def segment_crosstalk(pattern, lens, lev, clip, curves):
    """{segment name: {"xt": % or nan, "check": ratio or nan}} for one lens and frame."""
    own_r = 0 if (pattern == "A") == (lens == "L") else 2
    wrong_r = 2 - own_r
    out = {}
    for s, (name, col) in enumerate(gp.SEGMENTS):
        chans = [0, 1, 2] if name.startswith("grey") else [int(np.argmax(col))]
        if max(clip[own_r, s], clip[wrong_r, s], clip[3, s]) > CLIP_FRAC or any(curves[c] is None for c in chans):
            out[name] = {"xt": np.nan, "check": np.nan}
            continue
        L = {}
        for key, r in (("own", own_r), ("wrong", wrong_r), ("ref", 3), ("black", 1)):
            L[key] = np.array([to_light(lev[r, s, c], curves[c]) for c in chans])
        if np.nanmax(lev[own_r, s, chans] - lev[1, s, chans]) < MIN_SIGNAL or np.isnan(L["own"]).any():
            out[name] = {"xt": np.nan, "check": np.nan}
            continue
        w = LUMA[chans] / LUMA[chans].sum()
        own = float(w @ (L["own"] - L["black"]))
        wrong = float(w @ (L["wrong"] - L["black"]))
        ref = float(w @ (L["ref"] - L["black"]))
        out[name] = {"xt": 100.0 * wrong / own if own > 1e-6 else np.nan,
                     "check": (own + wrong) / ref if ref > 1e-6 else np.nan}
    return out


def frame_crosstalk(img):
    """Both lenses of one camera frame -> [L %, R %] (grey, light-weighted), or None."""
    half = img.shape[1] // 2
    res = []
    for lens, part in (("L", img[:, :half]), ("R", img[:, half:])):
        r = lens_frame(part)
        if r is None:
            return None
        pattern, lev, clip, curves, _ = r
        seg = segment_crosstalk(pattern, lens, lev, clip, curves)
        g = [v["xt"] for k, v in seg.items() if k.startswith("grey") and not np.isnan(v["xt"])]
        if not g:
            return None
        res.append(float(np.median(g)))
    return res


def analyse(dirpath):
    frames = sorted(glob.glob(os.path.join(dirpath, "cam_*.png")) + glob.glob(os.path.join(dirpath, "cam_*.jpg")))
    if not frames:
        sys.exit(f"no cam_*.png in {dirpath}")
    acc = {(lens, p): [] for lens in "LR" for p in "AB"}
    found = 0
    overlay_saved = False
    for fpath in frames:
        img = cv2.imread(fpath)
        half = img.shape[1] // 2
        for lens, part in (("L", img[:, :half]), ("R", img[:, half:])):
            r = lens_frame(part)
            if r is None:
                continue
            found += 1
            pattern, lev, clip, curves, warped = r
            acc[(lens, pattern)].append(segment_crosstalk(pattern, lens, lev, clip, curves))
            if not overlay_saved and lens == "L":
                cv2.imwrite(os.path.join(dirpath, "lensL_in_display_space.png"), warped)
                overlay_saved = True
    report = {"frames": len(frames), "lens_frames_with_markers": found}
    missing = [k for k, v in acc.items() if not v]
    if missing:
        report["error"] = "markers not found for " + ", ".join(f"lens {l} pattern {p}" for l, p in missing)
        return report
    result = {}
    for lens in "LR":
        per_seg = {}
        for name, _ in gp.SEGMENTS:
            # median over frames per pattern, then mean of A and B (cancels row unevenness)
            ab = [np.nanmedian([f[name]["xt"] for f in acc[(lens, p)]]) if any(
                not np.isnan(f[name]["xt"]) for f in acc[(lens, p)]) else np.nan for p in "AB"]
            ck = [np.nanmedian([f[name]["check"] for f in acc[(lens, p)]]) if any(
                not np.isnan(f[name]["check"]) for f in acc[(lens, p)]) else np.nan for p in "AB"]
            per_seg[name] = {"crosstalk_pct": None if np.isnan(ab).any() else round(float(np.mean(ab)), 1),
                             "self_check": None if np.isnan(ck).any() else round(float(np.mean(ck)), 3)}
        greys = [v["crosstalk_pct"] for k, v in per_seg.items() if k.startswith("grey") and v["crosstalk_pct"] is not None]
        checks = [v["self_check"] for v in per_seg.values() if v["self_check"] is not None]
        result[lens] = {"crosstalk_pct": round(float(np.median(greys)), 1) if greys else None,
                        "by_colour": {c: per_seg[c]["crosstalk_pct"] for c in ("red", "green", "blue")},
                        "self_check": round(float(np.median(checks)), 3) if checks else None,
                        "segments": per_seg}
    report["lenses"] = result
    means = [result[l]["crosstalk_pct"] for l in "LR"]
    if None in means:
        report["error"] = "no usable grey segment for a lens (all clipped, too dark, or hidden)"
        return report
    worst, avg = max(means), sum(means) / 2
    chk = [result[l]["self_check"] for l in "LR" if result[l]["self_check"] is not None]
    report["self_check_ok"] = bool(chk) and all(abs(c - 1) < 0.15 for c in chk)
    report["eye_order"] = "SWAPPED" if avg > 200 else "ok" if avg < 60 else "n/a (no separation)"
    report["verdict"] = ("eyes swapped" if avg > 200 else "no separation (2D, or weave aimed elsewhere)"
                         if avg > 60 else "poor" if worst > 15 else "ok" if worst > 5 else "good")
    return report


# ---------------------------------------------------------------- self-test: a fake camera
def fake_camera(out_dir, leak_bgr, frames=8, gain=1.6, ambient=0.02, noise=1.5, swap=False, seed=1):
    """Render what each lens of a camera would see: own view + leak x other view, + room light,
    times an exposure that clips the bright steps, through a camera curve that is NOT 2.2."""
    rng = np.random.default_rng(seed)
    os.makedirs(out_dir, exist_ok=True)
    for f in glob.glob(os.path.join(out_dir, "cam_*")):
        os.remove(f)
    leak = np.array(leak_bgr, np.float64)
    # lens picture: screen fills the middle, a little perspective, different per lens
    hw, hh = 1920, 2160
    quads = {"L": [[260, 520], [1700, 470], [1690, 1500], [250, 1460]],
             "R": [[200, 470], [1650, 520], [1660, 1460], [210, 1500]]}
    disp = np.float32([[0, 0], [gp.W, 0], [gp.W, gp.H], [0, gp.H]])
    for i in range(frames):
        pattern = "AB"[i % 2]
        views = {e: display_linear(gp.eye_image(pattern, e)) for e in "LR"}
        halves = []
        for lens in "LR":
            own, other = (views["L"], views["R"]) if lens == "L" else (views["R"], views["L"])
            if swap:
                own, other = other, own
            light = own + leak * other + ambient
            Hm = cv2.getPerspectiveTransform(disp, np.float32(quads[lens]))
            cam = cv2.warpPerspective(light.astype(np.float32), Hm, (hw, hh), flags=cv2.INTER_AREA,
                                      borderValue=(ambient, ambient, ambient))
            v = np.clip(cam * gain, 0, None)
            code = 255 * (v / (1 + 0.35 * v)) ** (1 / 2.6) / (1 / 1.35) ** (1 / 2.6)   # soft knee + odd gamma
            code = code + rng.normal(0, noise, code.shape)
            halves.append(np.clip(code, 0, 255).astype(np.uint8))
        cv2.imwrite(os.path.join(out_dir, f"cam_{i + 1:04d}.png"), np.hstack(halves))


def selftest_passes():
    """The fake-camera cases, as pass/fail (used by tests and --selftest)."""
    import tempfile
    base = os.path.join(tempfile.gettempdir(), "weave_qa_selftest")
    ok = True
    for name, leak, swap, want in [("leak", (0.30, 0.10, 0.05), False, "ok"),
                                   ("clean", (0.0, 0.0, 0.0), False, "good"),
                                   ("2D", (1.0, 1.0, 1.0), False, "no separation"),
                                   ("swapped", (0.10, 0.10, 0.10), True, "eyes swapped")]:
        d = os.path.join(base, name)
        # The dim levels are for a camera whose exposure rises on a dark screen.
        fake_camera(d, leak, swap=swap, gain=12.0 if gp.SEGMENTS is gp.LEVELS["dim"] else 1.6)
        rep = analyse(d)
        got = rep.get("verdict", rep.get("error", ""))
        good = got.startswith(want)
        if name == "leak":
            L = rep["lenses"]["L"]
            expect = 100 * (leak[1] * LUMA[1] + leak[0] * LUMA[0] + leak[2] * LUMA[2]) / LUMA.sum()
            good = good and abs(L["crosstalk_pct"] - expect) < 1.5 and abs(L["by_colour"]["blue"] - 30) < 3
        print(f"{'PASS' if good else 'FAIL'} {name:8s} verdict={got!r}")
        ok = ok and good
    return ok


def write_report(dirpath, rep):
    with open(os.path.join(dirpath, "report.json"), "w") as f:
        json.dump(rep, f, indent=2)


def main():
    ap = argparse.ArgumentParser(description="Weave QA analysis")
    ap.add_argument("dir", nargs="?")
    ap.add_argument("--levels", choices=sorted(gp.LEVELS), default="dim")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    gp.set_levels(a.levels)
    if a.selftest:
        sys.exit(0 if selftest_passes() else 1)
    if not a.dir:
        ap.error("DIR is required (or --selftest)")
    rep = analyse(a.dir)
    write_report(a.dir, rep)
    print(json.dumps(rep, indent=2))


if __name__ == "__main__":
    main()
