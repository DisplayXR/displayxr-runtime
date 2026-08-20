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

### (c) Privileged capture — and exactly which API, at exactly which tier

This is the mechanism that is *actually correct*, and the sub-questions matter,
because the obvious phrasing of the ask ("`captureDisplay` with
`setExcludeLayers`") **does not name a real API combination**. Verified against
AOSP `android13-release`, which is what the NP02J runs:

| | `captureDisplay(DisplayCaptureArgs)` | `captureLayers(LayerCaptureArgs)` |
|---|---|---|
| covers the whole display | **yes**, from a display token | only the subtree under a root `SurfaceControl` |
| `setExcludeLayers(SurfaceControl[])` | **no** — the setter is on `LayerCaptureArgs` only | **yes** |
| `CaptureArgs.setUid(long)` | yes — *"skip any surfaces that don't belong to the specified uid"* | yes |
| `setSize(w, h)` downscale in SurfaceFlinger | yes | via `setFrameScale` |

So the only display-wide filter available anywhere is **uid *inclusion***. There
is no exclude-uid, and the real per-layer exclusion needs a display-root
`SurfaceControl` that only the window manager owns. **No privilege tier fixes
this** — it is an API-shape limit, not a permission one.

The permission side is worse than "needs a system app", and worse than the
earlier draft of this note implied:

| permission | protectionLevel (AOSP 13) |
|---|---|
| `READ_FRAME_BUFFER` | `signature\|recents` |
| `CAPTURE_VIDEO_OUTPUT` | `signature` |
| `ACCESS_SURFACE_FLINGER` | `signature` |

**None of the three is `signature|privileged`.** That kills the usual escape
hatch: moving an APK into `/system/priv-app` and allowlisting it in
`privapp-permissions` grants *nothing* here. The only holders are the platform
signature and the `recents` role. `com.leialoft.display.config` is signed with a
**Leia** key (CN=Sean Liu) and its firmware baseline lives in `/system/app` —
SYSTEM, not privileged — so it cannot hold capture permission on any device,
however it is installed. Vendor code cannot close this gap; only the OEM can,
by platform-signing the capture service or hosting it inside a component that
already is.

What that leaves as the correct vendor ask is therefore *narrower and cheaper*
than "give us READ_FRAME_BUFFER": either (i) platform-sign a small capture
service, or (ii) add an **exclude-uid** filter beside the existing inclusion
filter in SurfaceFlinger's layer traversal, which is where the correct
primitive actually belongs. Filed as L10/L12 on #1038.

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
`screenrecord` are shell-only, and — contrary to an earlier claim in this note —
they are **not** blind to `TYPE_APPLICATION_OVERLAY`: a `screencap` taken while
the avatar overlay was up on the NP02J contains the avatar in full. So an
unfiltered display capture really does feed back, which is what makes T2's
`once` mode the correct dev tier rather than merely the convenient one.

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
| **T0** | Runtime-supplied backdrop through the existing `set_background_2d` slot; DP composes under, gate flattens to opaque in the band | runtime (colour / still / under-Local2D stack) | static or near-static backgrounds; **the whole workspace-overlay case** | **SHIPPED** — runtime #1073, plug-in `feat/t0-android-compose-under` |
| **T1** | MediaProjection producer behind the same seam, dev-gated | `MediaProjection` + `VirtualDisplay` (+ hidden-API layer exclusion where the ROM allows) | lab validation, A/B of the compose pass against a real background | ~1 wk, **not shippable** |
| **T2** | Out-of-process capture producer feeding the same seam. Consumer shipped (`comp_multi_bg2d_capture.c`, `debug.dxr.bg2d=capture`); producer shipped in the vendor display-config APK with two entry points — a bound service for the day the platform permits it, and an `app_process` daemon at shell/root uid for now | vendor service (product) / permitted-uid daemon (dev) | `once`: complete and feedback-free, correct for a static background — the avatar-over-launcher case. `uid`: single-app backgrounds. Everything else needs the OEM | our side done; vendor side is the schedule |

### T2 and the canvas rect (leia-plugin#174)

Slot 16's contract is that the backdrop arrives **in the canvas rect** — the
same client-window pixel space `process_atlas` works in — which is why the DP
maps the whole image onto each atlas tile with `bg_uv = (0,0)-(1,1)` and needs
no notion of producer tier. T0 satisfies that by construction: the runtime draws
the backdrop, so it *is* canvas-space.

**A T2 producer does not.** A screen capture is the whole screen, so the frame
is whole-**panel** pixels. Passing it straight through put the entire launcher
inside the avatar's zone-3D canvas — the bottom 75 % of the panel — shifted down
25 % and scaled to 0.75. On a full-width window the horizontal half of that
error is the identity, so what remained visible was a pure vertical
displacement; against T0's x-constant gradient it was invisible entirely, which
is why it surfaced only once a real background arrived.

`comp_multi_bg2d_ensure` therefore **crops the captured frame to the canvas rect
before upload**. The rect is `window_screen_{x,y}` + the zone-3D placement rect,
in panel pixels; the frame is a uniform downscale of the panel
(`DisplayCaptureArgs.setSize`), so the mapping is one ratio per axis. Cropping
in the receiver rather than in the DP keeps one coordinate space on the seam,
puts no producer-tier flag on the wire, and matches Windows/Linux, where the
capture module hands the DP a window sub-rect instead of the DP
reverse-engineering one.

Verified on an NP02J by pushing a deterministic asymmetric pattern through
`@displayxr.bg2d` with the plug-in's `debug.dxr.leia.bgdebug=1` (compose the
backdrop only, skip the alpha gate — both tiles then carry identical
zero-disparity content, so a panel `screencap` reads back as the backdrop
itself): the quadrant boundary moved from panel y 0.625 to 0.500, and a real
launcher capture composes 1:1 with the live screen behind it.

*(#174 reported this as the background being sampled mirrored in X. It is not —
the same pattern test shows every corner and edge marker landing where it was
sent. The defect was only ever vertical.)*

### T0 as built (#1073)

- **Producer** — `src/xrt/compositor/multi/comp_multi_bg2d.c`: a 4x256 RGBA8
  solid/gradient the runtime rasterises and uploads once per session, selected
  by `debug.dxr.bg2d` (`1` | `solid:RRGGBB` | `grad:RRGGBB,RRGGBB`; unset =
  off, which is the shipping default). Uploaded on its OWN one-shot command
  buffer, not the frame one — the Android vendor DP is self-submitting, so
  anything recorded into `cmd` is still unsubmitted when the DP samples it.
- **Transport** — `comp_multi_system.c` calls
  `xrt_display_processor_set_background_2d` before every `process_atlas` on
  transparent Android sessions, and clears it explicitly otherwise. This is
  the first `comp_multi_*` call site for slot 16 anywhere.
- **Consumer** — `drv_leia_android`'s `compose_pre_weave`, the Linux DP's
  `compose_*` pipeline over a byte-identical copy of
  `drv_leia/shaders/compose_under_bg.frag`, plus alpha-gate **mode 2
  (flattened)**, selected automatically whenever the compose pass actually ran
  and never by `debug.dxr.alphagate`.
- **Formal flag** — `android_transparent_requested()` now reads
  `xsi.transparent_background_enabled` first, so
  `XrAndroidSurfaceBindingCreateInfoDXR::transparentBackgroundEnabled` stops
  being parsed and dropped on Android.
- **Measured on the NP02J** (avatar, 3840x866 atlas): the compose pass costs
  **~2.6-2.8 ms/frame**, dominated by the fence wait, because CNSDK exposes no
  wait-semaphore hook — the pass must be *finished*, not merely queued, before
  the weave is submitted. That is the single number to beat before T0 could
  ever default on; a semaphore handshake with the vendor weave is the fix, and
  it is a #1038-class ask, not something the runtime can do alone.
- **Known T0 limitation, by construction:** the band stops being *chromatic*
  and becomes *the wrong colour* wherever the static backdrop disagrees with
  the real screen behind it. That is the whole of what T1/T2 buy.

#### The app-supplied backdrop already has a channel — and it is not new API

Worth recording, because it removes a question rather than opening one: an
**app-supplied** backdrop needs **no new extension**. The channel is
`XR_DXR_display_zones` Local2D layers plus `xrEndFrame` **list order** —
Local2D layers submitted *before* the projection layer are "under" layers, and
`comp_vk_native_compositor.c`'s `vk_flatten_backdrop_2d` (`:6309`) already
flattens exactly those into a premultiplied scratch and hands it to
`set_background_2d`. Slot 16's own doc comment describes this as its purpose.

So the remaining T0 increment is a **port, not a design**: give `comp_multi`
the same under/over split and pre-weave flatten, and prefer an app's under-layers
over the runtime's synthetic colour. Two things it must solve that the
in-process path did not:

1. `comp_multi`'s Local2D path is a plain `vkCmdBlitImage` of *all* Local2D
   layers post-weave — there is no under/over split and no
   `vk_local2d_composite` flatten pipeline on this path yet.
2. The flatten records into the frame command buffer, which is still
   **unsubmitted** when a self-submitting Android DP samples the backdrop —
   the same hazard `comp_multi_bg2d.c` sidesteps with its own one-shot submit.

That increment is also what makes #1073's "reachable without a sysprop"
literally true: an app that submits an under-layer gets compose-under with no
`debug.dxr.bg2d` at all.

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
- The capture helper of §4(c), in the display-config APK
  (`leia/device/android/service/.../capture/`, branch
  `dxr/background-capture-service`): `ILeiaBackgroundCapture` (product shape,
  `AHardwareBuffer` + fence + seq), `BackgroundCaptureService` (honest
  `STATUS_NO_PERMISSION` on every build today), and `CaptureDaemonMain` (the
  tier that runs now). Feature-flagged off by default
  (`-PleiaBackgroundCapture=true`), so a device that does not want it does not
  have the component.
- The remaining OEM ask — platform signature, or an exclude-uid filter — is
  L10/L12 on #1038, not a separate CNSDK issue.

### T2 as built

The consumer is a **listener**, not a client: the runtime opens an abstract
unix socket (`@displayxr.bg2d`) and a producer connects to it. A producer that
never appears, dies, or restarts costs nothing — no frame simply means no
background, which is byte-for-byte the pre-#1073 path, so probe and fallback
need no handshake and no timeout.

Delivery is a plain RGBA byte stream rather than an `AHardwareBuffer`, and
deliberately: the background exists to fill the thin de-occlusion band,
SurfaceFlinger downscales for free via `setSize`, and 512x320 at <= 10 Hz reuses
T0's staging upload verbatim — no JNI in the vendor APK, no cross-process image
import, no fence protocol. The zero-copy shape is specified in the AIDL for when
the producer is in-platform and the frame rate starts to matter.

Modes, and what each is honestly correct for:

| `--mode` | filter | correct when |
|---|---|---|
| `uids` | N × `CaptureArgs.setUid`, composited bottom-up | the background is the home screen **and** this build's uid filter leaves skipped layers transparent (see below — the NP02J's does not). Feedback-free *and* continuous, so where it works it survives a rotation. **Requested by default**; the daemon probes and degrades |
| `once` | none, captured **before** the overlay exists | the background is static. Complete and feedback-free at every privilege tier, and re-captured between consumer sessions, so it follows a rotation that goes through an app restart. **The working default on the NP02J** |
| `uid` | one `CaptureArgs.setUid` inclusion | the background is one app's window. Drops anything another uid drew, wallpaper included. Superseded by `uids` |
| `all` | none, continuous | diagnostic only — includes our own layer, so it feeds back |

### Device rotation, and why `once` cannot survive one

A rotation invalidates a held capture **twice over**: the frame's aspect no
longer matches the panel, and the launcher behind it has *re-laid out*, so even
a correctly rotated copy of the old pixels would be wrong. Only a genuinely new
capture in the new orientation fixes it — which raises the question of when a
new capture could possibly be clean.

The answer, measured on an NP02J, is *never*, for a whole-display capture:

- **There is no gap.** Looping `screencap` across a `user_rotation` flip, the
  **first** landscape frame already contains the consumer's woven content.
  SurfaceFlinger keeps the layer's last buffer latched straight through the
  rotation; the swapchain going `OUT_OF_DATE` and being recreated (≈ 4 ms here)
  never blanks the layer. So "re-capture in the window where our surface is
  destroyed" — the obvious plan — has no window to aim at.
- **`all` demonstrably feeds back.** With the backdrop-debug view on
  (`debug.dxr.leia.bgdebug=1`, leia-plugin#175), `--mode=all` shows the band
  visibly recursing.
- **Hiding our own layer for a frame** would work, and a rotation is the one
  moment where the flicker would be masked by the system's own animation — but
  it buys a handshake, a present stall and a timing race to solve a problem a
  filter solves outright.

`uids` was meant to solve it outright, and does — *on a platform that allows
it*. The idea: a uid-filtered `captureDisplay` leaves every layer it skipped
**transparent**, which makes a union of several well defined (`SRC_OVER` of
capture[i+1] over capture[i] reproduces SurfaceFlinger's own stacking for the
listed uids). The wallpaper host and the home launcher are different uids
(`com.android.systemui` and the launcher package on this device) — which is
precisely why a single `--uid` drops the wallpaper and why the plural exists.
Their union is the whole home screen, and the consumer's uid is absent by
construction. Being feedback-free it can run **continuously**, so the next tick
is already in the new orientation with the launcher re-laid out.

#### The premise is false on the NP02J (runtime#1101 follow-up, CNSDK#718)

SurfaceFlinger composites a display screenshot over a **fill layer** whose alpha
comes from `RenderArea::CaptureFill`, and `DisplayRenderArea` uses **`OPAQUE`**.
Every per-uid capture therefore comes back fully opaque — *black* wherever that
uid drew nothing — and the union collapses to the **last** uid.

Measured: with `--uid=<systemui>,<launcher>` the composed backdrop carried the
launcher's icons and dock **in exactly the right place** over pure black, the
wallpaper gone. The systemui uid captured *alone* returns the wallpaper
complete and correctly registered, so neither uid resolution nor ordering was at
fault. The decisive isolation was `--uid=<systemui>,19998` — a uid that owns no
layer at all — which wiped the wallpaper to pure black. **An empty capture
cannot erase anything unless the fill is opaque.** (RGB alone could never have
settled this: "nothing drawn" and "drew black" are the same colour. Only alpha
answers it, which is what the probe reads.)

Note the failure mode — it *looks* correct. Right icons, right positions, right
registration; only the missing wallpaper gives it away. That is why the daemon
now **probes rather than assumes** (`ScreenCapture.uidFilterLeavesRestTransparent`),
and falls back to `once` with a line on stdout naming the cause. `uids` is kept,
not deleted: it is right on a build whose fill is `CLEAR`, and the probe decides
per device. **Run it on every new panel.**

The fallback would have cost the rotation-follow `uids` exists for, so `once`
was narrowed at the same time: it **re-captures in the window between one
consumer session ending and the next beginning**. That window is exactly the
feedback-free moment `once` needs — the overlay is off screen — and an
orientation-locked overlay app must be relaunched to change orientation anyway,
so it is where a rotation lands in practice. Verified by the held frame's clock
advancing across an app restart (12:05 → 12:13).

What survives unchanged from the original argument: there is **no gap** during a
rotation for a *whole-display* capture of a *running* consumer, so the
"re-capture mid-rotation" plan still has no window to aim at. The session
boundary is a different window, and it is the one that exists.

The consumer's half is one line of correctness: the re-crop key is the canvas
rect **and the panel extent**, because `bg2d_canvas_crop_rect` maps canvas→frame
*through* the panel dims, so the same canvas rect on a rotated panel is a
different crop. A capture whose aspect disagrees with the panel's now also logs
one line naming the cause, so "PASS at launch, FAIL after rotating" reads out of
a bug report instead of needing to be reproduced.

The honest cost of `uids`: content drawn by a uid that is not on the list is
missing (transparent, forced to black). That is correct for the home-screen case
the capability ships against and not for an arbitrary app stack — which is the
same boundary the *product* tier crosses, with
`captureLayers(excludeLayers=[our layer])` in a platform-signed helper.

### Both compositors, one producer

The backdrop producer is `src/xrt/compositor/util/comp_bg2d.{c,h}` (plus the
socket receiver `comp_bg2d_capture.{c,h}`), consumed by **both** compositor
paths through a caller-owned `struct comp_bg2d_state`:

- **out of process** — `comp_multi_system.c`, per session, canvas rect from the
  frame's zone-3D layer plus the `android_globals` window rect (#1033);
- **in process** — `comp_vk_native_compositor.c`, same geometry from the app's
  own `xrSetAndroidWindowGeometryDXR` publish (#1037).

In-process is the one that matters for the end state. Out of process the weave
lands on the service's `TYPE_APPLICATION_OVERLAY` and therefore carries the
≤ 0.80 anti-tapjacking alpha clamp — a 20 % launcher ghost over *every* pixel
that no backdrop can remove (§2). In process (ADR-036 Architecture A) the app
owns a plain translucent `TYPE_APPLICATION` window: no clamp, no overlay at all.
Compose-under there is the shipping combination rather than a demonstration of
one.

Precedence where both could supply the slot: an **app-supplied Local2D backdrop
wins**. `vk_flatten_backdrop_2d` (#491) claims slot 16 when the frame has
2D-under Local2D layers — the app explicitly said what is behind its 3D content,
and a screen capture is a *guess* at the same question. The capture only fills
the slot on frames where the flatten declined it; the two are never blended.

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
