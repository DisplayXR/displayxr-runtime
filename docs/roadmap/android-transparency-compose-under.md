# Android transparency: compose-under-background

**Status:** design note / roadmap. Refs #1031 (Android per-window compositors,
ADR-036), #568 (Android avatar transparency, shipped), #1038 (vendor service
contract), leia-plugin `docs/android-weaving-and-transparency.md` §4.

Windows and Linux resolve overlay transparency by **compositing a captured
background under the app content before the weave**. Android does not. This
note establishes *why* (the reason is a missing **producer**, not a missing
compose pass), what today's Android path actually costs visually, and a tiered
plan to close the gap.

---

## 1. The invariant, restated

The weave is **per-subpixel**: R, G and B of one output pixel are drawn from
*different* views (leia-plugin `docs/weaver.md`; `docs/android-weaving-and-transparency.md` §1). An RGBA
buffer carries **one alpha per pixel**, and every OS compositor blends
`out = src + bg·(1−a)` with that single `a` shared across the three channels.

Therefore:

| Pixel class | Correct alpha exists? |
|---|---|
| all views transparent | yes — `a = 0` |
| all views opaque | yes — `a = 1` |
| **mixed** (silhouette / parallax de-occlusion band) | **no** — `a_r = 1`, `a_g = 0`, … |

This is a **representational** limit, not an algorithmic one, and no amount of
gate tuning removes it. The two real fixes are (1) a per-channel-alpha format
and compositor (does not exist anywhere), or (2) resolve transparency **while
the views are still independent**, i.e. composite a background into each view
*before* the interlace — "compose-under". That is the durable principle behind the Leia transparency model, and it is why
"alpha-weave" (weaving the alpha channel as a second pass) was rejected on
Windows.

Compose-under needs exactly two things: a **compose pass** (`mix(bg, view.rgb,
view.a)`, opaque out) and a **background producer**. Android has the first and
lacks the second.

---

## 2. What Android does today

The current path is **live + unshaped** in the vocabulary of
`docs/architecture/transparency-modes.md` — SurfaceFlinger blends our layer
over the live screen every frame, and the DP reconstructs a per-pixel alpha
*after* the weave.

Chain, as built (#568, verified against the tree at `v2.8.0`):

1. **Runtime — translucent surface.** The OOP service overlay is a
   `TYPE_APPLICATION_OVERLAY` `SurfaceView` with `PixelFormat.TRANSLUCENT`
   (`android_custom_surface.cpp:177-182`, `MonadoView.java:126-130`) and the VK
   swapchain picks `VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR`
   (`comp_target_swapchain.c`). The per-session composite already clears to
   `(0,0,0,0)` and blends alpha-preserving Porter-Duff
   (`comp_multi_system.c:801-816`, `:1463`), so the atlas reaches the DP with
   real per-view alpha.
2. **Runtime — enable.** `comp_multi_compositor.c:1995-2000` calls
   `xrt_display_processor_vk_set_transparent_background(dp, true, /*client_presents=*/true)`
   whenever `android_transparent_requested()` passes — which is
   `android_globals_get_overlay_mode()` **or** the `debug.dxr.transparent`
   sysprop (`:74-88`). *The formal extension flag is parsed and dropped:*
   `XrAndroidSurfaceBindingCreateInfoDXR::transparentBackgroundEnabled` reaches
   `xsi.transparent_background_enabled` (`oxr_session.c:4023`) but the only
   compositor that reads that field is `comp_d3d11_service.cpp:19010`. Closing
   that seam is a small, separate cleanup.
3. **Plug-in — weave, then gate.** `leia_display_processor_cnsdk.cpp:1396-1404`
   runs `alpha_gate_run()` after `leia_cnsdk_weave` when
   `transparent_bg_enabled && tiles == 2×1`. `shaders/alpha_gate.frag` mode 1
   (default) matches the woven RGB against each atlas tile, and emits **that
   view's** alpha — recovering the interlacer's own per-pixel view selection.
   Mode 0 (all-views-transparent mask) is kept for A/B via
   `debug.dxr.alphagate`. The `canvas` push constant punches everything outside
   the display-zone band fully transparent.
   There is **no compose pass and no background sampler** in the Android DP.

### Visual limitations of the gate, enumerated

- **Chromatic fringe at the silhouette and in the de-occlusion band.** The gate
  picks *one* view per pixel from a single RGB distance, so on a mixed pixel two
  of three channels get the wrong alpha. The visible artifact is a thin coloured
  rim, widening with pop-out (the band is ~1 px near the display plane and grows
  with disparity). Per-channel matching does not fix it — it just relocates the
  error, because the output still has one `a`.
- **Ambiguity is resolved toward opaque.** Two views with equal RGB and
  different alpha take the first match, deliberately (never over-punch). Where
  that guesses wrong the live screen is occluded by a pixel that should have
  been see-through.
- **Partial alpha is *not* broken.** Where all subpixels come from the same
  view, premultiplied `woven.rgb` plus that view's `a` blends exactly right in
  SurfaceFlinger. Soft edges and antialiasing survive; only mixed pixels fail.
  (This is a genuine advantage over the Windows *live* path and over the old
  chroma-key hard mask.)
- **Background depth is already correct.** The live screen is physically at the
  panel plane and both eyes see it there, with zero inter-view disparity. This
  is the one property compose-under exists to guarantee on Windows, and Android
  gets it for free — a captured background would be composited identically into
  every view for the same reason.
- **A 20 % ghost over the whole overlay, unrelated to the gate.** The OOP
  overlay carries `FLAG_NOT_TOUCHABLE` (it must, or it eats every tap — the
  per-region touchable API is `@hide` and the ROM blocks the reflection bypass),
  and Android's anti-tapjacking rule clamps such a window to **≤ 0.80 alpha**
  (`android_custom_surface.cpp:135-145`). So even fully opaque avatar pixels let
  20 % of the launcher through. **Compose-under does not fix this** — it is a
  window-policy artifact, and the escape is the in-process `TYPE_APPLICATION`
  window of #1031's Architecture A, not a background capture.

---

## 3. The compose pass already exists — twice

The reason Android has no compose-under is **not** that the pass is expensive or
architecturally awkward. It is written, in Vulkan, in this ecosystem, already:

| Path | Compose pass | Background producer |
|---|---|---|
| Windows D3D11/D3D12/VK/GL | `compose_run_pre_weave` (`leia_display_processor_d3d11.cpp:956`, shader `:169`) | WGC — `leia_bg_capture_win.{h,cpp}` |
| **Linux VK** | `drv_leia_linux/leia_display_processor_linux.c:500+`, reusing `drv_leia/shaders/compose_under_bg.frag` | **`bg2d_view` seam**, fed by `set_background_2d`; `leia_bg_capture_linux` (portal/PipeWire) plugs into the same seam (WS1) |
| Android VK (CNSDK) | *(absent)* | *(absent)* |

The Linux DP is the template Android should copy almost literally: same Vulkan,
same shader, background sampled directly from a view in the compositor's own
`VkDevice` (no cross-API import), and a **producer-agnostic seam** —
`bg2d_view` — that either the runtime or a capture module can fill.

And the transport already exists in the runtime ABI. **`set_background_2d` is
base DP vtable slot 16** (`xrt_display_processor.h:452`): *"the runtime flattens
[the 2D-under layers] into a single premultiplied-RGBA texture in the same
client-window pixel space / canvas rect as `process_atlas` and passes its view
here. The DP composites it … and uses the result as the under-3D background for
the NEXT `process_atlas`."* It is implemented on D3D11, D3D12, GL, Metal and the
Linux VK DP — and **never called from `comp_multi_*`, i.e. never on Android or
macOS**, and not implemented in `drv_leia_android`.

So the plumbing question has a clean answer: **an Android background image needs
no new DP slot and no ABI bump.** It goes through slot 16 in exactly the pixel
space `process_atlas` already uses.

*(A second, even shorter route exists: `comp_multi` owns the atlas on Android,
so it could draw the background under each tile itself in
`composite_layers_to_intermediate` and hand the DP an already-opaque atlas — no
plug-in change at all. Prefer slot 16 anyway: it keeps the vendor DP the single
owner of "what the weaver eats", matches Windows/Linux, and survives the case
where the producer turns out to live vendor-side and the runtime never sees the
pixels.)*

---

## 4. Why there is no producer: the Android capture landscape

The honest answer to "why doesn't Android do compose-under" is: **there is no
public Android API that captures the screen while excluding your own layer.**
Everything below was evaluated against five criteria — correctness (does it
yield the true background, at the panel plane, with no feedback loop), latency,
power, consent UX, and API level.

### (a) MediaProjection + VirtualDisplay

The only public full-screen capture. Fatal on its own: it captures the
**composited** display *including our overlay*, so the image converges to black
within a few frames. Leia's own on-device PoC
(`com.leia.mediaprojectionpoc`) hits the same wall.

Self-exclusion attempts, all dead ends:

- **`FLAG_SECURE` on our window** — does *not* omit the layer; SurfaceFlinger
  **blacks it out** on a non-secure output (`VIRTUAL_DISPLAY_FLAG_SECURE` unset ⇒
  "content of secure windows will be blanked"). We would capture an opaque black
  rectangle exactly where we need background. Strictly worse than the feedback
  loop.
- **`SurfaceControl.Transaction#setSkipScreenshot` / `setPrivacySensitive`** —
  `@hide`, reachable only by reflection through the non-SDK blocklist, and their
  coverage of the *display-mirroring* path (as opposed to the `captureDisplay`
  screenshot path) is unverified. Even if it works, hidden-API access means a
  system/vendor app — at which point tier (c) is strictly better. Worth one
  cheap experiment on the rooted NP02J only to close the question.
- **Android 14+ single-app capture** (`MediaProjectionConfig`, user picks one
  app) — the one *public* configuration with no feedback, because the captured
  task is not ours. But the user must pick the app behind us, it does not follow
  z-order or task switches, it cannot capture the launcher, and Android 14
  requires **fresh consent per `createVirtualDisplay`** plus a
  `foregroundServiceType="mediaProjection"` service. Unusable as a background
  service for an always-on avatar.

Cost, even where it works: one extra full-panel SurfaceFlinger composition
(1600×2560 on NP02J) plus a GPU copy per delivered frame, and ≥ 1 frame of
staleness. Windows contains the same cost with a delivery throttle
(`LEIA_DP_CAPTURE_MIN_INTERVAL_MS`, default 66 ms); the Android producer would
need the same knob.

**Verdict:** dev/lab mode only.

### (b) `SurfaceControlViewHost` / re-parenting the background app

Not possible. `SurfaceControlViewHost` lets a *host* embed a surface that the
*guest* explicitly hands over. There is no mechanism for an unrelated
foreground app to adopt the launcher's or another app's layer.

### (c) Privileged capture — `SurfaceControl.captureDisplay` + `setExcludeLayers`

The mechanism that is *actually correct*: exclude our own layer by
`SurfaceControl`, capture everything else, no feedback, no consent dialog, one
frame of latency, and the result is genuinely "what is behind us". It needs
`READ_FRAME_BUFFER` / `CAPTURE_VIDEO_OUTPUT` / `ACCESS_SURFACE_FLINGER` — all
`signature`-level.

**"System app" is not sufficient, and this is the crux.** On the NP02J those
permissions are held only by *platform-signed* ZTE/Qualcomm services
(`cn.nubia.gamelab`, `com.qualcomm.wfd.service`). Every Leia/CNSDK service
(`com.leialoft.display.config`, `com.leia.headtrackingservice`,
`com.leiainc.media.service`) ships pre-installed as SYSTEM but signed with a
**Leia** key and holds **zero** capture permissions. So the existing vendor
services cannot be leveraged as-is — the gate is OEM signing (ZTE), not code.

This is nonetheless the **product** answer on a vendor 3D device, and it is
squarely a #1038-class ask: a small vendor capture service doing
`captureDisplay(excludeLayers=[our layer])` and shipping frames over the
`AHardwareBuffer`-over-IPC primitive the runtime already uses. The plug-in
receives the AHB, imports it, and fills `bg2d_view` — the Linux `bg_capture`
shape, unchanged.

### (d) Wallpaper-only compose-under

Attractive on paper (the avatar's actual use case is "floating over the
launcher", where the background *is* the wallpaper) and **dead as a public
path**: `WallpaperManager.getDrawable()` returns the *default* wallpaper on
Android 13 and **throws `SecurityException` unconditionally from Android 14**
unless the caller holds `MANAGE_EXTERNAL_STORAGE`. Live wallpapers cannot be
read at all. Keep it only as an opportunistic enrichment on a device where the
runtime is privileged anyway — by which point (c) is available and strictly
better.

### (e) Runtime-drawn backdrop

Have the runtime supply the background itself: a solid colour, an
app-supplied still, a controller-supplied backdrop, or the flattened *under*
Local2D layers that slot 16 was designed for. Always available, zero permissions,
zero latency, zero power. It is **wrong** wherever the real background is
dynamic — but it is exactly right for the case we most need it (see §6), and it
is the only tier that requires no platform concession at all.

### (f) Evaluated and rejected

`AccessibilityService.takeScreenshot()` is public and consent-once — but it is
rate-limited to one shot per ~333 ms (≈3 Hz), still captures our own overlay,
and repurposing accessibility is a Play-policy violation. `screencap` /
`screenrecord` are shell-only (and blind to `TYPE_APPLICATION_OVERLAY` anyway).

---

## 5. What the shell / workspace overlay mode changes

A great deal — for the apps it covers, and nothing for the case that hurts.

In the opt-in **workspace overlay mode** (#1006 / #967 d, re-scoped by ADR-036),
`comp_multi` composites **N clients into one atlas behind one DP**. Every
DisplayXR client is then just another layer in a runtime-owned composite, so
"compose a DXR app under another DXR app" is not a capture problem at all — the
runtime already holds both sets of pixels, pre-weave, with real alpha. Ordering
is a compositor policy decision, and slot 16 is the natural channel for the
flattened under-stack.

Note the corollary for #1031's shipped path: **Architectures A and C do *not*
get this for free.** N per-window compositor instances weave independently, so a
DXR window overlapping another DXR window has exactly the §1 mixed-alpha
problem. Shared-atlas overlay mode is the only topology in which inter-app
transparency is correct by construction — a real argument for it beyond
resource sharing.

What none of this touches: **a lone transparent app over the OS launcher or a
third-party app.** That is the avatar's shipping configuration, and it remains a
capture problem.

---

## 6. Recommendation

The Windows conclusion was that *live* composition is the shippable default and
the DP should bake **only the de-occlusion band** (hybrid; see
`transparency-modes.md` §"The default"). On Android that conclusion is *stronger*,
for three reasons:

1. SurfaceFlinger's per-pixel alpha blend is cheap and native — Android's live
   path costs nothing like DWM's, so there is no perf argument for baking.
2. Android has no `SetWindowRgn`; shaping (visual coverage) is not available at
   all, so a fully baked opaque overlay would occlude the whole screen instead
   of punching through. Live is not merely preferred, it is the only unshaped
   option that works.
3. The failure is *confined* to the mixed band, which is thin.

So the target is **not** "port the Windows model wholesale". It is: keep live
composition, and give the DP a background good enough for **the band only**.
That reframing is what makes tier 0 worth shipping.

| Tier | What | Producer | Correct for | Effort |
|---|---|---|---|---|
| **T0** | Runtime-supplied backdrop through the existing `set_background_2d` slot; DP composes under, gate flattens to opaque in the band | runtime (colour / still / under-Local2D stack) | static or near-static backgrounds; **the whole workspace-overlay case** | runtime ~2 d, plug-in ~2 d |
| **T1** | MediaProjection producer behind the same seam, dev-gated | `MediaProjection` + `VirtualDisplay` (+ hidden-API layer exclusion where the ROM allows) | lab validation, A/B of the compose pass against a real background | ~1 wk, **not shippable** |
| **T2** | Vendor-privileged `captureDisplay(excludeLayers=…)` helper, frames over `AHardwareBuffer`, into the same seam | OEM/vendor system service | everything, incl. avatar over an arbitrary app | our side ~1 wk; vendor side is the schedule |

### Split of work

**Runtime**
- Call `xrt_display_processor_set_background_2d` from the `comp_multi`
  per-session render path (before `process_atlas`, same canvas rect) — today it
  is never called off Windows. This one change unlocks T0, T1 and T2 alike,
  since all three fill the same seam.
- Close the formal-flag seam: read `xsi.transparent_background_enabled` in
  `comp_multi_compositor.c` instead of gating on `debug.dxr.transparent`
  (`XR_DXR_android_surface_binding` already carries the flag).
- Workspace overlay mode (#1006): flatten the under-stack into the backdrop.
- **Do not** put screen capture in the runtime. A capture producer is
  vendor/platform-specific; it reaches the runtime, if ever, only as an opaque
  image handle (`AHardwareBuffer`), exactly like `comp_multi_weave_android.c`'s
  overlay atlas already does.

**Plug-in (`drv_leia_android`)**
- Port the Linux DP's compose-under seam: `bg2d_view` + `compose_*` pipeline +
  `drv_leia/shaders/compose_under_bg.frag`, run pre-weave on transparent
  multi-view frames that have a background bound.
- Add gate **mode 2 = flattened**: where a background was composed, emit `a = 1`
  (the pixel is genuinely opaque); elsewhere keep mode 1. Same shape as the
  Windows `#116` flatten.
- Later: `leia_bg_capture_android.{h,cpp}` with the six-function shape of
  `leia_bg_capture_win.h` (`create` / `get_size` / `poll(uv_origin, uv_extent,
  fence)` / `get_view` / `destroy`), plus a delivery throttle.

**Vendor (CNSDK / OEM)**
- The privileged capture helper of §4(c). File as a new limitation under #1038
  rather than a separate CNSDK issue, alongside L1/L2/L7.

### Sequencing

T0 first — it is the only tier with no external dependency, it exercises the
compose pass end to end, and it makes the workspace-overlay case correct. T1
exists to validate T0's pass against a real, moving background. T2 is gated on a
platform concession we do not control, so it should be *asked for now* and
*planned as if it may not arrive*.

### Known non-goals

- Fixing the ≤ 0.80 overlay alpha clamp (§2) — that is #1031 Architecture A.
- Per-channel alpha, per-channel gate matching, or any other post-weave scheme.
  §1 closes those permanently.
- Bake-everywhere ("baked + unshaped") as an Android default. Windows measured
  its cost as *background smear during ordinary interaction*
  (`transparency-modes.md` rule 5), and Android has no shaping escape hatch.

---

## References

- `docs/architecture/transparency-modes.md` — live/baked × shaped/unshaped vocabulary
- `docs/adr/ADR-036-android-per-window-compositor-instances.md`, `docs/roadmap/android-concurrent-multi-app.md`
- `docs/adr/ADR-027-display-zones.md` (the `canvas` sub-rect the gate honours)
- leia-plugin `docs/android-weaving-and-transparency.md` §4, `docs/transparency.md`, `docs/weaver.md`
- Issues: #568, #1006, #1031, #1038, #967; leia-plugin #150, #151
