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

## P0 acceptance test

On NP02J, `debug.dxr.weave_satellite=1`, browser at hello-cube:

1. Fullscreen: crisp 3D via the overlay (parity with today).
2. **OEM mini-window: crisp 3D** — the case no app-side weave can ever do,
   and the entire point (browser#186's wedge and #173's phase class are both
   structurally impossible here: the satellite target is never scaled and
   never resizes with the window).
3. Freeform→fullscreen→freeform cycling: stable (no per-cycle state in the
   client-facing path).
4. `debug.dxr.weave_satellite=0` restores today's behaviour bit-for-bit.

## Risks / open questions

- Does the OEM allow a `TYPE_APPLICATION_OVERLAY` surface to cover a freeform
  window's caption region? (Best-effort in P0; OEM ask otherwise.)
- Overlay present jitter vs the in-window path (the overlay present is a
  second SurfaceFlinger client; measure with the #663 pipelined-weave lens).
- Transparency: the overlay must punch through where no window weaves — the
  woven region is opaque, everything else transparent black; verify no
  full-surface GPU composition cost on the SoC (Adreno).
- Lens lifecycle rides #1278 (weave-idle release) — same residual as in-app.
