# Android weave satellite (Architecture C) — P0 design

Tracking: [#1277](https://github.com/DisplayXR/displayxr-runtime/issues/1277).
Status: **design** (P0 not started). Driver: large-format Android panels where
apps run in freeform windows as the primary mode — app-side weaving cannot
survive a container scale (measured: SurfaceFlinger `tr=[0.67,0][0,0.67]` on
NP02J's mini-window; browser#173/#128/#186 are the evidence trail).

## The P0-shaping finding (2026-08-28)

**The wire already carries everything the satellite needs.** On every
`xrWeaveSubmitDXR`, the service-side compositor
(`comp_multi_weave_android.c`) receives:

- the **unwoven input** — an AHardwareBuffer holding every rect's squeezed-SBS
  content at its own window position (spec v7 batch layout);
- the **rect list** (window-relative device px);
- the **window geometry** — now truthful, per browser patch 0123: the
  compositor SurfaceView's on-screen origin plus the container-scaled flag.

Architecture C's P0 is therefore **not** a new IPC design. It is a
present-mode flip inside the service: weave into a surface the *service* owns
and presents, instead of into an output buffer returned to the client.

## Second P0 reduction (recon, same night): the overlay half already ships

`Java_org_freedesktop_monado_ipc_MonadoImpl_nativeCreateServiceOverlay`
(`service_target.cpp`) is the #558 avatar-over-launcher machinery: the SERVICE
self-creates a `TYPE_APPLICATION_OVERLAY` surface with **no Activity** via
`android_custom_surface` (works from the service Context; `debug.dxr.transparent`
makes it TRANSLUCENT; `android_custom_surface_can_draw_overlays` gates it;
the #558 stale-overlay heal covers client restarts), publishes it through
`android_globals`, and the compositor presents into it. So the satellite's
present surface, permission handling, translucency and lifecycle are shipped,
field-tested code.

**P0's entire remaining delta is the weave divert** in
`comp_multi_weave_android.c`: on `debug.dxr.weave_satellite=1`, acquire the
overlay window (same `android_custom_surface` path), build a
`VK_KHR_android_surface` swapchain on it (`comp_window_android` shows the
recipe), and per submit blit the woven output into it at the window's physical
rect instead of returning `weavedTexture`. One blit + present per frame, all
inside machinery this file already owns (it has the vk bundle, the queue-lock
discipline, and the geometry via `set_window_geometry`).

## P0 scope: one browser window, satellite-presented weave

```
browser ──xrWeaveSubmitDXR──▶ service ──weave──▶ service-owned overlay Surface
   │ (unchanged wire)            │                    (full-panel, UNSCALED,
   └─ page-owned 2D shows        └─ CNSDK interlacer   above apps)
      under the overlay             viewport at the
                                    window's PHYSICAL rect
```

1. **Overlay surface.** The runtime APK already holds `SYSTEM_ALERT_WINDOW`
   (granted by every install script; the #558 overlay mode exercises it).
   `MonadoService` adds a full-panel `TYPE_APPLICATION_OVERLAY` SurfaceView —
   `FLAG_NOT_FOCUSABLE | FLAG_NOT_TOUCHABLE`, translucent — and hands its
   `ANativeWindow` to the service compositor, which already knows how to
   present to an Android surface (the hosted path). The overlay is never
   scaled by the window manager: it IS the physical-pixel canvas.
2. **Satellite weave mode** in `comp_multi_weave_android.c`: gated by
   `debug.dxr.weave_satellite=1`. When on, the weave renders into the overlay
   swapchain; `XrWeaveOutputDXR::weavedTexture` is never returned (spec v7
   already allows steady-state frames to return none, and the browser's
   over-plane skips drawing when it holds no mailbox — so the page's own 2D
   pixels show *under* the overlay with **zero browser changes**). Overlay
   unavailable ⇒ fall back to exactly today's return-the-output path.
3. **Phase becomes trivial.** The satellite weaves the full panel at (0,0):
   the interlace phase needs no per-window anchor at all. Per-window
   *placement* is the CNSDK per-window interlacer viewport
   (`set_viewport` + `set_viewport_screen_position` — the 1-core/N-interlacer
   model from the concurrent-multi-app report), fed by the geometry the wire
   already carries.
4. **Scale-aware placement.** Under the OEM mini-window the reported origin is
   physical but the size is logical; the satellite must place at the physical
   footprint (logical × scale). P0 escape hatch: `debug.dxr.satellite_scale`
   (measured 0.67 on NP02J); the durable answer — the platform exposing the
   task scale — goes on the OEM asks list
   (`docs/specs/vendor/oem-android-platform-requirements.md`).

## Deliberately OUT of P0

- **Occlusion** (another window overlapping the woven one must clip the weave)
  — P1; needs a visible-region feed or whole-panel composition.
- **Input** beyond `FLAG_NOT_TOUCHABLE` passthrough — P1.
- **N concurrent weaving windows** (policy: ADR-025, #967 conflicts) — P2.
- OEM z-order guarantees (overlay vs system chrome/caption) — recorded as an
  OEM ask, accepted as best-effort in P0.

## P0 STATUS (2026-08-29, overnight session)

**Core parity: PASSED, human-verified** — the satellite-presented weave was judged
"clean" against the in-app golden reference (NP02J, landscape fullscreen; see the
golden-standard memory/fingerprint). The bring-up found and fixed, in order:
the overlay's own origin inset (60-row phase beat), whole-output blits copying the
DP's compose-under backdrop (dark film → per-rect blits, which is also
occlusion-lite), SUBOPTIMAL-as-recreate thrash (→ tolerate; IN_USE → bounded retry),
and the decisive one — **Android's anti-tapjacking clamp composited the overlay at
alpha 0.8**, blending 20% of the under-content through the weave (per-eye
crosstalk). Dev unlock: `settings put global maximum_obscuring_opacity_for_touch
1.0`; the ship requirement (trusted overlay / per-package exemption) is now in
`oem-android-platform-requirements.md`. Diagnostic lesson that generalizes:
screencap cross-correlation proved content+placement identical, isolating the fault
to scanout composition — **screenshots cannot see HWC-level blending; dump the HWC
layer list.**

**Physical-rect weave: ACCEPTED, human-verified (2026-08-29 morning)** — with
`debug.dxr.satellite_scale=0.67` and the browser dragged to the OEM mini-window,
the cube was judged **"crisp 3D"** on NP02J: the P0 acceptance case (test 2 below),
the one no app-side weave can do. Expected artifact while the knob is set globally:
a *fullscreen* window then shows a 0.67-size woven rect over the browser's own mono
draw — the prop is a stand-in for the real per-window scale, which is the P1
auto-derivation item (fullscreen→1.0, each freeform window→its true OS scale).

**Rotation: fixed post-soak** — the overnight rotation check was a false green (it
asserted only no-failures and could not see rendering). Portrait→landscape left the
stale portrait swapchain on screen (squished duplicate over the fresh frame) because
tolerating SUBOPTIMAL had removed the only rotation signal. Fix: detect the
out-vs-overlay orientation mismatch and **rebuild only the swapchain with
`oldSwapchain` chaining** — the overlay view survives rotation via
surfaceChanged-in-place (measured), and a full view release/re-ensure collides with
the #558 single-window globals (stale published window → IN_USE latch, 121 retry
frames measured). Round-trip rebuild ~55 ms/direction, clean both ways.

**#1278 weave-idle lens release: shipped and OS-verified** on the same branch
(lens vote released 2.0 s after the last submit — the OEM backlight service logs
`Disable` — and re-asserted on the next weave). Two structural findings recorded in
the commit: the multi main loop is parked for pure present-owner clients (the pass
is now also driven from the IPC 20 Hz loop), and the release must hit the weave's
own DP directly (no dp_visibility edge exists for a present-owner).

**Overnight soak: 14/14 PASS** — overlay lifecycle ×4 (appear/teardown, no leaks),
prop toggle, idle-release ×3, rotation ×2.

## P0 acceptance test

On NP02J, `debug.dxr.weave_satellite=1`, browser at hello-cube:

1. ✅ Fullscreen: crisp 3D via the overlay (parity with today) — "yes clean !!" / golden.
2. ✅ **OEM mini-window: crisp 3D** — "cube is crisp 3D !" (scale prop 0.67) — the case no app-side weave can ever do,
   and the entire point (browser#186's wedge and #173's phase class are both
   structurally impossible here: the satellite target is never scaled and
   never resizes with the window).
3. ✅ Freeform→fullscreen→freeform cycling + rotation: stable (swapchain-only
   rebuild on orientation change; no per-cycle state in the client-facing path).
4. `debug.dxr.weave_satellite=0` restores today's behaviour bit-for-bit.

## P1 STATUS (2026-08-29)

**Per-window container-scale auto-derivation: SHIPPED, human-verified.** The
global `debug.dxr.satellite_scale` knob is demoted to a diagnostic override;
the satellite now derives the scale per window at submit:

- **The tell** (all field-measured on NP02J): an OEM mini window is a fixed
  phone-profile task (sw540dp → 1080×1685 logical) whose WM bounds are a
  hybrid — physical origin + logical size — while a SurfaceFlinger leash
  (`tr=[0.67,0][0,0.67]`) scales presentation. So the caller-reported rect
  EXCEEDS the panel while its origin lies inside; that is app-visibly unique
  to a container-scaled window. The factor itself is app-invisible (leash-only;
  a11y bounds logical-clipped; no config/settings/prop — all probed), so the
  tell selects a per-device constant: `debug.dxr.satellite_miniwindow_scale`,
  default 0.67.
- **Verified end-to-end with zero props set**: fullscreen weaves at derived
  1.0 (golden), the dragged mini window logs `PHYSICAL-RECT weave, scale
  0.670 (window 1757,236 1080x1685 vs panel 2560x1600)` and weaves crisp —
  both correct simultaneously, which the global knob could never do.
- **Ship ask filed**: `oem-android-platform-requirements.md` § *Container-scale
  visibility* — the platform must expose the presentation scale (the constant
  is correct only while the OEM mini-window scale stays fixed).
- Caveat (direction of failure): an unscaled freeform window dragged off-edge
  would trip the tell and be wrongly scale-woven; this OEM clamps mini windows
  in-panel and has no unscaled-freeform UX, so not reachable today.

**P1 occlusion + input + full-panel overlay: SHIPPED (same day).**

- **Input passthrough: verified, zero code** — FLAG_NOT_TOUCHABLE passes
  touches through the overlay to the client, and the OS unscales them into the
  window's logical space itself (verified with injected scroll: page scrolled,
  weave tracked).
- **Full-panel overlay** (`span_system_bars` on `android_custom_surface`):
  the overlay now lays out edge-to-edge (measured 2560x1600, origin 0,0)
  instead of inset below the status bar. Fixes the immersive-toggle bug ("tap
  the fullscreen browser -> broken weave"): the tap flips Chrome edge-to-edge
  (window 0,0 2560x1600), and the inset overlay's present clamped dst_y=-60 to
  0 without shifting the source — the whole weave landed 60 rows low. The blit
  path now also clips in DST space with source compensation, so partial
  off-panel rects map correctly in general.
- **Occlusion: a11y-fed window subtraction.** `WindowWatcherService`
  (AccessibilityService in the runtime APK, adb/user-enabled, OFF by default)
  serializes the interactive window list {type, layer, bounds} to
  `files/dxr_occlusion.bin` (atomic rename; the file transport crosses slot
  processes with zero IPC). The satellite subtracts occluders above the client
  from each blit rect (band decomposition, <=64 pieces): IME always; app
  windows by a11y layer vs the origin-matched client (fullscreen clients use
  the non-fullscreen-window rule); scaled occluders corrected by the same
  hybrid-bounds tell as the client scale. Verified on device: with the OEM
  split keyboard summoned over the cube, the weave clipped exactly at the
  IME's top edge (before the feed: cube drew over the keys). Watcher disabled
  or dead -> no occluders -> exactly the pre-P1 behavior.
- Dev trap: `am force-stop` on the runtime package kills the a11y watcher and
  Android only rebinds it on a settings retoggle — after a force-stop, retoggle
  `enabled_accessibility_services`.

Remaining for P2: N-window policy (ADR-025), the OEM platform asks (filed in
`oem-android-platform-requirements.md` + the KBXR OEM brief), a11y-independent
occlusion if the platform ever exposes window geometry directly.


## Risks / open questions

- Does the OEM allow a `TYPE_APPLICATION_OVERLAY` surface to cover a freeform
  window's caption region? (Best-effort in P0; OEM ask otherwise.)
- Overlay present jitter vs the in-window path (the overlay present is a
  second SurfaceFlinger client; measure with the #663 pipelined-weave lens).
- Transparency: the overlay must punch through where no window weaves — the
  woven region is opaque, everything else transparent black; verify no
  full-surface GPU composition cost on the SoC (Adreno).
- Lens lifecycle rides #1278 (weave-idle release) — same residual as in-app.
