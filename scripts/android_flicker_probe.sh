#!/usr/bin/env bash
#
# android_flicker_probe.sh — host-side flicker detector for a DisplayXR app on Android.
#
# Answers ONE question without a human in front of the panel:
#
#     Does the app's composited on-screen content alternate between two (or more)
#     frame classes, rather than evolving smoothly?
#
# It bursts full-resolution SurfaceFlinger screenshots (`screencap`) on-device,
# ships only the rows of interest back over adb, and scores the resulting stack.
#
#   FLICKER=yes|no SCORE=<n> METHOD=<name>
#
# SCOPE — READ THIS BEFORE TRUSTING A `no`.
# `screencap` reads the *composited surface*, which on the NP02J is upstream of
# the panel's final 3D interlace: a burst captured while the Leia weave is
# demonstrably running (`HW_GEO: ... tiles=2x1`) contains no interlace pattern at
# all (2-D FFT high-frequency energy 0.3%, column/row parity < 0.2/255). So this
# probe sees COMPOSITED-CONTENT flicker and is structurally blind to
# weave-phase / scanout-level flicker. `FLICKER=no` means "the surface handed to
# SurfaceFlinger is stable", NOT "the panel is stable". See
# docs/reference/android-flicker-probe.md for the calibration data.
#
# Usage:
#   scripts/android_flicker_probe.sh <package> [options]
#
#   --band l,t,r,b   Screen rect to analyse. Default: auto-detected from
#                    `dumpsys window` (the package's largest non-activity window,
#                    i.e. the overlay, falling back to the activity window).
#   --seconds N      Sample for ~N seconds (default 5, which is the N=24 calibration point; ~4.8 captures/s).
#   --frames N       Exact frame count (overrides --seconds).
#   --method M       static | bimodal | hf | all (default: static -- the only
#                    metric with a calibrated threshold; the others are printed
#                    as diagnostics under -v but are NOT part of the verdict,
#                    because both fire on known-clean configurations).
#   --json           Emit a JSON blob after the summary line.
#   --keep DIR       Keep the pulled frames in DIR for offline analysis.
#   -v, --verbose    Print every metric, not just the verdict.
#
# Exit status: 0 = no flicker, 1 = flicker, 2 = probe error.
#
set -uo pipefail

PKG=""; BAND=""; SECONDS_ARG=5; FRAMES=""; METHOD="static"; JSON=0; KEEP=""; VERBOSE=0

die() { echo "android_flicker_probe: $*" >&2; exit 2; }

while [ $# -gt 0 ]; do
  case "$1" in
    --band)    BAND="$2"; shift 2;;
    --seconds) SECONDS_ARG="$2"; shift 2;;
    --frames)  FRAMES="$2"; shift 2;;
    --method)  METHOD="$2"; shift 2;;
    --json)    JSON=1; shift;;
    --keep)    KEEP="$2"; shift 2;;
    -v|--verbose) VERBOSE=1; shift;;
    -h|--help) sed -n '2,44p' "$0" | sed 's/^# \{0,1\}//'; exit 0;;
    -*) die "unknown option $1";;
    *)  [ -z "$PKG" ] || die "unexpected argument $1"; PKG="$1"; shift;;
  esac
done
[ -n "$PKG" ] || die "usage: $0 <package> [--band l,t,r,b] [--seconds N]"

command -v adb >/dev/null || die "adb not on PATH"
command -v python3 >/dev/null || die "python3 not on PATH"
python3 -c 'import numpy' 2>/dev/null || die "python3 numpy is required (pip install numpy)"
adb get-state >/dev/null 2>&1 || die "no adb device"

# ---------------------------------------------------------------- display size
read -r DW DH < <(adb shell dumpsys window displays 2>/dev/null \
  | grep -m1 -oE 'cur=[0-9]+x[0-9]+' | tr -d 'cur=' | tr 'x' ' ')
[ -n "${DW:-}" ] || read -r DW DH < <(adb shell wm size 2>/dev/null \
  | grep -m1 -oE '[0-9]+x[0-9]+' | tr 'x' ' ')
[ -n "${DW:-}" ] && [ -n "${DH:-}" ] || die "could not determine display size"

# ------------------------------------------------------------- band resolution
if [ -z "$BAND" ]; then
  # Prefer the app's own (overlay) window -- `Window{<id> u0 <pkg>}` -- over its
  # activity window -- `Window{<id> u0 <pkg>/<pkg>.MainActivity}`. For the
  # avatar-style topology the overlay is what the runtime actually weaves into,
  # and it is a far tighter band than the (mostly transparent) fullscreen
  # activity, which matters: every metric here is diluted by dead area.
  BAND=$(adb shell dumpsys window windows 2>/dev/null \
    | awk -v pkg="$PKG" '
        $0 ~ ("Window\\{[^}]* " pkg "}")  { want=1; overlay=1; next }
        $0 ~ ("Window\\{[^}]* " pkg "/")  { want=1; overlay=0; next }
        want && /frame=\[/ {
          if (match($0, /frame=\[[0-9-]+,[0-9-]+\]\[[0-9-]+,[0-9-]+\]/)) {
            s = substr($0, RSTART+7, RLENGTH-7); gsub(/[\[\]]/, ",", s);
            split(s, a, ","); l=a[1]; t=a[2]; r=a[4]; b=a[5];
            if ((r-l) > 0 && (b-t) > 0) {
              if (overlay) { ov = l "," t "," r "," b }
              else if (act == "") { act = l "," t "," r "," b }
            }
          }
          want=0
        }
        END { print (ov != "" ? ov : act) }')
  [ -n "$BAND" ] || die "could not auto-detect a window for $PKG (is it running?)"
fi
IFS=, read -r BL BT BR BB <<<"$BAND"
[ "$BR" -gt "$BL" ] && [ "$BB" -gt "$BT" ] || die "bad --band $BAND"
# Clamp to the display.
[ "$BL" -lt 0 ] && BL=0; [ "$BT" -lt 0 ] && BT=0
[ "$BR" -gt "$DW" ] && BR=$DW; [ "$BB" -gt "$DH" ] && BB=$DH

# `screencap`'s raw stream is row-major over the whole display, so we can only
# crop ROWS on-device (dd). Cap the row count so the adb transfer stays cheap;
# a centred slab is representative and keeps ~4.8 captures/s achievable.
MAXROWS=512
ROWS=$((BB - BT)); TOP=$BT
if [ "$ROWS" -gt "$MAXROWS" ]; then
  TOP=$((BT + (ROWS - MAXROWS) / 2)); ROWS=$MAXROWS
fi
STRIDE=$((DW * 4))

# 24 frames (~5 s) is the calibration point; other counts are normalised back
# to it in the scorer, so --seconds stays meaningful without moving the verdict.
if [ -n "$FRAMES" ]; then N="$FRAMES"; else
  N=$(python3 -c "print(max(10,int(round($SECONDS_ARG*4.8))))"); fi

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT

# ------------------------------------------------------------------- the burst
# One adb shell round-trip; `screencap | dd` avoids writing 16 MB per frame.
adb shell "rm -f /data/local/tmp/dxrflick_*.bin; for i in \$(seq 1 $N); do \
  screencap | dd bs=$STRIDE skip=$TOP count=$((ROWS + 1)) \
  of=/data/local/tmp/dxrflick_\$i.bin 2>/dev/null; done" >/dev/null 2>&1 \
  || die "on-device burst failed"
adb pull /data/local/tmp/ "$WORK/" >/dev/null 2>&1
adb shell "rm -f /data/local/tmp/dxrflick_*.bin" >/dev/null 2>&1
ls "$WORK"/tmp/dxrflick_*.bin >/dev/null 2>&1 || die "no frames came back"

[ -n "$KEEP" ] && { mkdir -p "$KEEP"; cp "$WORK"/tmp/dxrflick_*.bin "$KEEP/"; }

# ------------------------------------------------------------------- the score
python3 - "$WORK/tmp" "$BL" "$BR" "$ROWS" "$DW" "$METHOD" "$JSON" "$VERBOSE" <<'PY'
import sys, glob, re, json, numpy as np

d, L, R, ROWS, W, method, emit_json, verbose = (
    sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]),
    int(sys.argv[5]), sys.argv[6], int(sys.argv[7]), int(sys.argv[8]))

files = sorted(glob.glob(d + "/dxrflick_*.bin"),
               key=lambda p: int(re.search(r"_(\d+)\.bin$", p).group(1)))
need = ROWS * W * 4
frames = []
for f in files:
    # dd started at a whole-row offset of the FILE, and screencap's raw stream
    # carries a 16-byte header, so the first 16 bytes are the tail of the
    # preceding row: drop them and the rest is exactly our slab.
    raw = np.fromfile(f, dtype=np.uint8)[16:16 + need]
    if raw.size < need:
        continue
    frames.append(raw.reshape(ROWS, W, 4)[:, L:R, :3])
if len(frames) < 6:
    print("FLICKER=error SCORE=0 METHOD=none  (only %d usable frames)" % len(frames))
    sys.exit(2)

A = np.stack(frames).astype(np.int16)              # (N, H, Wb, 3)
N = A.shape[0]
Y = (A.astype(np.float32) @ np.array([.299, .587, .114], np.float32))
res = {"frames": N, "band_px": int(Y[0].size)}

# -- static -------------------------------------------------------------------
# A digital readback has no sensor noise: a pixel showing static content is
# BIT-EXACT across the whole burst (measured: 1.0000 over 30 frames on a clean
# T2 backdrop). So take the pixels that barely move -- the backdrop, the dead
# margin, anything the animation does not touch -- and ask what fraction of them
# are nevertheless not bit-exact. That is a pure-artifact number with, in the
# clean case, an exactly-zero noise floor.
rng = (A.max(axis=0) - A.min(axis=0)).max(axis=2)
pool = rng <= 8                                     # "should be static"
if pool.any():
    raw_score = float(np.mean(rng[pool] >= 2))
    not_exact = float(np.mean(rng[pool] != 0))
else:
    raw_score, not_exact = 0.0, 0.0
# NOTE ON FRAME COUNT. `rng` is a saturating statistic -- it counts a pixel once
# if it EVER moved -- which is exactly why it out-detects a per-consecutive-pair
# rate (a pairwise variant was tried and lost the positive control entirely).
# The price is that it grows with N, so the threshold below is calibrated at
# N=CAL_N and the scorer says so rather than pretending to rescale: an
# independence-model rescale was tried and amplified run-to-run noise ~4x.
CAL_N = 24
static_score = raw_score
res["static"] = {"pool_frac": float(pool.mean()), "not_bitexact": not_exact,
                 "cal_n": CAL_N, "score": static_score}

# -- bimodal ------------------------------------------------------------------
# Two alternating frame classes make the pairwise-distance matrix cluster into
# two groups whose membership is INDEPENDENT of time; smooth animation instead
# makes distance grow with the frame gap. Score = cluster separation, discounted
# by however much of the structure the time-gap trend already explains.
F = Y.reshape(N, -1)
D = np.stack([np.abs(F - F[i]).mean(axis=1) for i in range(N)])
iu = np.triu_indices(N, 1)
gap = np.abs(np.subtract.outer(np.arange(N), np.arange(N)))[iu].astype(float)
dv = D[iu]
gap_corr = float(np.corrcoef(gap, dv)[0, 1]) if dv.std() > 0 else 0.0
# 2-means on the frame-mean-distance profile, then separation in sigma.
prof = D.mean(axis=1)
s = np.sort(prof); best = 0.0
for k in range(2, N - 1):
    a, b = s[:k], s[k:]
    w = (a.var() * len(a) + b.var() * len(b)) / N
    if w > 0:
        best = max(best, float((b.mean() - a.mean()) / np.sqrt(w)))
bimodal_score = best * max(0.0, 1.0 - abs(gap_corr))
res["bimodal"] = {"separation": best, "gap_corr": gap_corr, "score": bimodal_score}

# -- hf -----------------------------------------------------------------------
# Per-frame high-spatial-frequency energy. A weave-phase or resample alternation
# swaps between a sharper and a softer frame; animation does not. Score = the
# coefficient of variation of that energy, amplified if it is two-valued.
lap = (Y[:, 1:-1, 1:-1] * 4 - Y[:, :-2, 1:-1] - Y[:, 2:, 1:-1]
       - Y[:, 1:-1, :-2] - Y[:, 1:-1, 2:])
hfe = np.sqrt((lap ** 2).mean(axis=(1, 2)))
cv = float(hfe.std() / hfe.mean()) if hfe.mean() > 0 else 0.0
sh = np.sort(hfe); hbest = 0.0
for k in range(2, N - 1):
    a, b = sh[:k], sh[k:]
    w = (a.var() * len(a) + b.var() * len(b)) / N
    if w > 0:
        hbest = max(hbest, float((b.mean() - a.mean()) / np.sqrt(w)))
hf_score = cv * hbest
res["hf"] = {"cv": cv, "separation": hbest, "score": hf_score}

# -- verdict ------------------------------------------------------------------
# Thresholds are calibrated in docs/reference/android-flicker-probe.md against a
# known-flickering config (bg2d teardown race) and two known-clean ones.
TH = {"static": 0.0080, "bimodal": 2.20, "hf": 0.060}
# Only `static` is calibrated. `bimodal` and `hf` are always computed (they are
# free, and a future flicker class might land in them) but are excluded from the
# verdict unless explicitly asked for, because both produce false positives on
# known-clean configurations -- see the calibration table in the docs.
cands = ["static", "bimodal", "hf"] if method == "all" else [method]
diag = [c for c in ("static", "bimodal", "hf") if c not in cands]
if any(c not in TH for c in cands):
    print("FLICKER=error SCORE=0 METHOD=none  (unknown --method %s)" % method)
    sys.exit(2)
ratios = {c: res[c]["score"] / TH[c] for c in cands}
winner = max(ratios, key=ratios.get)
score = res[winner]["score"]
flick = ratios[winner] >= 1.0

warn = "" if (winner != "static" or N == CAL_N) else \
    "  WARNING: static is calibrated at N=%d, this burst was N=%d" % (CAL_N, N)
print("FLICKER=%s SCORE=%.4f METHOD=%s%s"
      % ("yes" if flick else "no", score, winner, warn))
if verbose:
    for c in cands + diag:
        print("  %-8s score=%.4f threshold=%.4f  ratio=%.2f%s  %s"
              % (c, res[c]["score"], TH[c], res[c]["score"] / TH[c],
                 "" if c in cands else " (diagnostic only)",
                 " ".join("%s=%.4f" % (k, v) for k, v in res[c].items()
                          if k != "score")))
    print("  note: screencap reads the composited surface, upstream of the panel's"
          " 3D interlace -- a `no` does not clear weave-phase/scanout flicker.")
if emit_json:
    res["verdict"] = {"flicker": bool(flick), "score": score, "method": winner,
                      "thresholds": TH}
    print(json.dumps(res))
sys.exit(1 if flick else 0)
PY
