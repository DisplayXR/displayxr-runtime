# Conversion: reference view synthesis (ADR-053 stage S5)

The runtime's own depth-image-based renderer: one colour image + one depth image → N views at
explicit viewpoints. A pipeline **stage**, never a provider and never selectable
([ADR-053](../adr/ADR-053-conversion-providers.md) decision 3); `conv_pipeline` runs it for
`VIEWS_FROM_DEPTH` and after any provider that outputs depth only (the `COMPOSED` bit). It is a
port of the browser's `live-dibr.js` (displayxr-web-lift), hole-fill included, so the browser can
delete its copy (roadmap P4). Code: `src/xrt/auxiliary/util/u_conv_viewsyn.{h,c}` (contract +
pure math, C11) and `src/xrt/compositor/d3d11_service/d3d11_conv_viewsyn.{h,cpp}` +
`d3d11_conv_viewsyn_shaders.h` (GPU). Tests: `tests_conv_viewsyn_math` (the JS math test, case
for case), `tests_conv_viewsyn_d3d11` (WARP, step edge into 2 and 4 views).

## Contract (`struct u_conv_viewsyn_params`)

| input | meaning |
|---|---|
| colour SRV, depth SRV | row 0 = image top; depth `.r` is the raw sample; colour a UNORM (not sRGB) view |
| `depth` | `kind`, `value_scale/shift`, `invalid_sample` — mirror of `xrt_conv_depth_info` (draft) |
| `range_near`, `range_far` | decoded values mapped to n = 1 / n = 0 — the **policy's** smoothed range |
| `convergence` | decoded value placed on the glass (zero parallax) |
| `budget` | near–far disparity of the nominal pair, fraction of width, strength folded in |
| `rear_cap_m` | ADR-040 rear cap, metres behind the glass; < 0 unlimited |
| `viewpoints_m[N]`, `rect_*_m` | ADR-048 D1: rect-relative metres, +z toward the viewer |
| target + layout | `ROW` (SBS strip, width / N per view) or `ARRAY` (slice i) |

Inside, everything is in rect heights (origin rect centre, +y up): eye = viewpoint / rect height,
source camera C = the eyes' centroid (or an explicit one from the look-around policy),
`q = qScale·(n − n_conv)·taper`, `qScale = budget·A / (κ·C.z)`, κ = 0.063 / 0.6. Passes: prep
(decode → nearness → normalised u) → optional dilation → snap cores → guide → joint-bilateral
upsample + edge snap (long side ≤ 1280) → optional `dilate_hi` → one draw per view (32-step march,
6 bisections, disocclusion gather). Passes 1–6 re-run only when content or a parameter they read
changes. The stage runs on the caller's context: no thread, no `render_mutex`, no per-frame log.

## Deliberate deviations from `live-dibr.js`

1. **No normalisation, no auto-convergence, no EMA, no scene-cut reset, no look-around anchor.**
   `percentileRange`, `DepthNormalizer`, `centreMean/Median`, `deadBand` and the anchor EMA are
   policy stages (ADR-053 decision 4). The stage takes their results: a decoded range, a decoded
   convergence, an optional explicit source camera. Their JS tests are not ported.
2. **Depth arrives in declared units.** METRIC → 1/z (as `toDisparity`); AFFINE depth (larger =
   farther, absent in JS) → −value; every inverse / disparity kind and UNKNOWN pass through. An
   `invalid_sample` reads as the far end (JS has no invalid value).
3. **Normalisation is baked into the prep pass**, not two main-pass uniforms: the depth rasters hold
   the unclamped normalised u, an increasing affine map of the JS raw disparity, so bilinear, JBU,
   min / max filters and `snapTau·(hi−lo)` are unchanged. Cost: a range change re-runs passes 1–6
   (JS reuses the upsample and only moves uniforms); the upsample therefore always uses the
   current range, where JS used the range at depth time.
4. **Dilation and the snap cores run on the GPU** (separable filters, radius ≤ 16) instead of on
   the CPU at depth resolution; same clamped-window result (CPU references pinned in the test).
5. **Viewpoints, not projection matrices.** Eyes come from rect-relative metres; `viewEye` is kept
   as a helper and pinned against it. All views share the rect's aspect (one qScale per frame).
6. **ADR-040 rear cap** (absent in JS): negative q is scaled by `rear_k` so the deepest content
   lands exactly on the cap — compression, not a clip, so depth order behind the glass survives.
7. **Defaults are `lift()`'s, not `DEFAULT_PARAMS`**: `dilate` 0 (the snap removes the halo).
8. **Orientation**: D3D textures are row 0 = top, so the GLSL's colour `1 − v` flips live only in
   the main pass; shader math is otherwise line for line.
9. `maxLayers` (JS throws above 1 too), `getStats` and the `lookAround` time constant are not ported.

Parity with `live-dibr.js` on the real-media panel corpus is checked by the supervisor
(roadmap §2.6); this port pins the math host-side and the GPU on WARP.
