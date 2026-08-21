# Android flicker probe — what it measures, and what it provably cannot

`scripts/android_flicker_probe.sh <package>` answers one question without a human
in front of the panel:

> Does the app's **composited on-screen content** alternate between frame classes,
> rather than evolving smoothly?

It prints one line — `FLICKER=yes|no SCORE=<n> METHOD=<name>` — and exits `0` for
clean, `1` for flicker, `2` for a probe error, so it works as the loop metric for
an automated bisection.

```bash
scripts/android_flicker_probe.sh com.displayxr.avatar_vk_android -v
scripts/android_flicker_probe.sh com.displayxr.avatar_vk_android \
    --band 1031,288,1529,1600 --json
```

## How it captures

`adb exec-out screencap -p` costs ~1.8 s per frame (on-device PNG encode
dominates) — far too slow. The probe instead runs the whole burst **on the
device** in one `adb shell` round-trip, piping raw `screencap` straight into `dd`
so nothing large is written to flash, and cropping to the rows of interest before
the data crosses USB:

```
screencap | dd bs=<display_width*4> skip=<top_row> count=<rows+1> of=/data/local/tmp/...
```

Measured on an NP02J (2560×1600): **~4.8 captures/s**, versus 0.55/s for
`screencap -p` and 0.63/s for a full raw pull. Rows are capped at 512 (a centred
slab). `screencap`'s raw stream carries a 16-byte header, so the slab's first 16
bytes are the tail of the preceding row and are dropped host-side.

At 4.8 Hz against a 60 Hz alternation each capture is effectively an independent
draw from the frame classes — which is all the statistics below need. The probe
does **not** try to sample fast enough to see the alternation directly.

The band is auto-detected from `dumpsys window windows`, preferring the app's
overlay window (`Window{<id> u0 <pkg>}`) over its activity window
(`Window{<id> u0 <pkg>/<pkg>.MainActivity}`). This matters more than it sounds:
the avatar's activity window is a transparent 2560×1600 fullscreen sheet, and
scoring that instead of the 498×1312 overlay diluted every metric below its
threshold and turned a true positive into a false negative.

## The metric that works: `static`

A digital readback has no sensor noise, so a pixel showing static content is
**bit-exact** across an entire burst. Measured on a clean T2 backdrop region:
`bitexact = 1.0000` over 30 frames, per-pixel range `max = 0`. The noise floor is
*exactly zero*.

So: take the pixels that barely move over the burst (total range ≤ 8 — backdrop,
dead margin, anything the animation misses), and score the fraction of them that
are nevertheless **not** bit-exact by ≥ 2 LSB. That is pure artifact.

`bimodal` (2-cluster separation of the pairwise frame-distance matrix, discounted
by the distance-vs-frame-gap correlation) and `hf` (per-frame Laplacian energy
variation) are computed from the same burst because they are free, and are shown
under `-v`. **Neither is part of the verdict**: `bimodal` fired on a known-clean
configuration 3/3, and `hf` never fired on anything. Use `--method all` only for
exploration.

### Frame count is part of the calibration

`rng` is a *saturating* statistic — it counts a pixel once if it ever moved —
which is exactly why it out-detects a per-consecutive-pair rate. Two alternatives
were tried and are recorded here so they are not retried:

- A **per-pair rate** (mean over consecutive pairs of the fraction of pool pixels
  differing by ≥ 2) is frame-count independent, but it collapsed the separation:
  the positive control scored 0.0008–0.0012 against a clean control's
  0.0006–0.0012 — complete overlap, 0/10 detection.
- **Rescaling** a burst-wide range to an N=24 equivalent under an
  independent-per-step model amplified run-to-run noise about 4× (the same
  configuration scored 0.0529 at N=12 and 0.0045 at N=30).

So the threshold is calibrated at **N = 24 frames (~5 s)**, which is the default,
and the probe prints an explicit `WARNING` in its summary line if you ask for a
different count. Do not tune `--seconds` and then trust the verdict.

## Calibration

Four configurations of the converted avatar demo on an NP02J, landscape,
`overlay.slab=82`, 10 runs each at N=24. Ground truth is human-eyeball, recorded
in the flicker repro table (repros #4, #5, #6, #8).

| Config | knobs | truth | `static` (min–max, n=10) | verdict |
|---|---|---|---|---|
| #4 positive control | `bg2d=capture aspect=38 jiggle=10 sync=0` | **flicker** | 0.0091 – 0.0107 | **10/10 yes** |
| #6 open repro | `bg2d=capture aspect=38 jiggle=0 sync=1` | **flicker** | 0.0100 – 0.0108 | **10/10 yes** |
| #5 wide box | `bg2d=capture aspect=75 jiggle=0 sync=1` | clean | 0.0038 – 0.0045 | 10/10 no |
| #8 T2 off | `bg2d=off aspect=38 jiggle=0 sync=1` | clean | 0.0059 – 0.0087 | 9/10 no |

Threshold: **`static` = 0.0080** — the geometric midpoint of the two bulk
distributions (#8 median 0.0062, #6 median 0.0106). The single #8 false positive
is one outlier run at 0.0087; the other nine sit at ≤ 0.0071. No run in the whole
40-run matrix falls between 0.0072 and 0.0080, so the exact threshold inside that
window does not change any verdict.

Repro #4 earns its place as the **positive control**: a probe that only ever says
`no` is worthless, and #4 is an independently-known-bad configuration (the bg2d
teardown race, `jiggle=10 sync=0`) that the probe must catch for a `no` elsewhere
to mean anything.

The `#5` control is what makes this a flicker detector rather than a "is T2 armed"
detector: #5 has the T2 backdrop **on** and scores *lower* than #8, which has it
off. The metric tracks the artifact, not the feature.

## THE LIMITATION — read this before trusting a `no`

`screencap` reads the **composited surface**, which on the NP02J is upstream of
the panel's final 3D interlace. Measured, not assumed:

- The Leia weave is demonstrably running in the captured configuration —
  `HW_GEO: view=1920x1200 (aspect 1.600) tiles=2x1 atlas=3840x1200 target=498x1312`
  — so the surface *should* contain a two-view interlace.
- It does not. A 2-D FFT of the captured band puts **0.31 %** of the energy above
  half-Nyquist in x and **0.35 %** above quarter-Nyquist in y. Column parity
  (mean odd-column minus even-column luminance) is **0.19/255**; row parity is
  **0.04/255**. Both axes were checked, because SurfaceFlinger composites this
  layer with `bufferTransform=ROT_90`, which turns panel columns into screenshot
  rows. Visually the band is a clean, single, non-combed view.

So the interlace happens somewhere `screencap` cannot see — consistent with the
`leiadisp-hal-1-0` vendor display HAL being in the path. The consequence:

> **`FLICKER=no` means "the surface handed to SurfaceFlinger is stable". It does
> NOT mean "the panel is stable".** The probe is structurally blind to
> weave-phase and scanout-level artifacts.

What survives is that composited-content flicker leaves a *statistical* trace
even when it leaves no visible interlace, and that trace is what `static` scores.

Two other host-side signals were evaluated and rejected:

- `dumpsys SurfaceFlinger --latency <layer>` returns **all zeros** for a
  `BufferStateLayer` — the modern SurfaceControl path does not populate the
  legacy latency FIFO. Dead end.
- HWC composition state *does* live in the `Composition layers` section of
  `dumpsys SurfaceFlinger` (per-layer `forceClientComposition`,
  `clearClientTarget`, `displayFrame`, `sourceCrop`, `bufferTransform`) — this is
  the section an earlier grep failed to find, recorded so nobody hunts for it
  again. But `dumpsys` samples at ~1 Hz, far too slow to see 60 Hz flapping.

## Adding a new app

Nothing in the probe is avatar-specific. It needs a package that has a window
`dumpsys window` can find, and a band with *some* near-static content — a fully
animated full-bleed scene leaves an empty `pool` and the score degenerates
(`pool_frac` in `-v` tells you; below ~0.1 treat the verdict as unreliable). The
threshold is calibrated on the avatar's overlay topology; re-run the four-way
matrix above before trusting it on a very different app.

## First application: the narrow-box T2 flicker (repro #6)

The probe's first job was to bisect the open repro it was calibrated on — T2
armed + narrow overlay + landscape — with no human in the loop. Sweeping
`debug.dxr.avatar.overlay.aspect`, 5 runs each, `bg2d=capture`, `jiggle=0`,
`sync=1`, reading the woven target width straight out of the `HW_GEO:` log line:

| `aspect` | woven target | `static` median | verdict |
|---|---|---|---|
| 75 | 984×1312 | 0.0040 | clean 0/5 |
| 60 | 787×1312 | 0.0050 | clean 0/5 |
| 50 | 656×1312 | 0.0057 | clean 0/5 |
| 45 | 590×1312 | 0.0082 | **flicker 5/5** |
| 40 | 524×1312 | 0.0125 | **flicker 5/5** |
| 38 | 498×1312 | 0.0104 | **flicker 5/5** |

The score is **monotone in target width** and the onset sits between **656 px
(clean) and 590 px (flicker)**. Two things this rules out:

- It is not a band-size artifact of the probe itself. Repro #8 has the *same*
  498 px band with T2 **off** and scores 0.0059–0.0087 (clean), so the narrow band
  alone does not raise the score — the artifact needs T2 armed *and* a narrow
  target.
- **It is not orientation.** Re-run in portrait at the same 498 px target
  (`user_rotation 0`, display 1600×2560, `HW_GEO ... target=498x1312`), the probe
  reports **flicker 5/5, median 0.0096** — statistically identical to landscape.
  The repro table's entry #9 ("narrow + portrait = no flicker") therefore does not
  generalise: the controlling variable is the **woven target width**, and the
  landscape/portrait split in that table is a confound.

Where to look next, in rank order:

1. **The T2 backdrop crop quantisation.** `comp_bg2d.c` crops the capture in
   *downscaled frame* pixels — the producer sends 512×320 for a 2560×1600 panel,
   so one source pixel covers five display pixels. For the 498 px canvas the log
   reads `cropped the 512x320 panel capture to the canvas 1031,288 498x1312 ... ->
   100x262 at (206,58)`: a ±1 source-pixel movement of that origin is a ±5 display
   pixel jump of the whole backdrop. The narrower the canvas, the fewer source
   pixels carry it and the coarser that lattice gets relative to the content.
2. **Compose-under sampling at the canvas edge**, where the 5× upscaled backdrop
   meets the alpha-gated character silhouette — that is where the non-bit-exact
   pool pixels actually live (the pure-backdrop region well above the character is
   bit-exact 1.0000 over 30 frames, so the interior is stable and the boundary is
   not).
3. Weave phase versus odd window x (`pos=(1031,288)`) — plausible but unranked,
   and note the probe cannot see weave phase at all (see the limitation above).

Raising the T2 capture resolution above 512×320 would test hypothesis 1 directly
and is the cheapest next experiment.
