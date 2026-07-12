# Inline 3D for the Web — an auto-stereo (glasses-free) WebXR display module

**A WICG-style explainer.** Status: **Draft / pre-incubation.** This is a strawman
for discussion with the W3C Immersive Web Community Group, backed by a working
vendor-agnostic reference stack (DXR weave RPC + a Chromium-faithful CEF host;
see [`webxr-support.md`](webxr-support.md)). It is **not** a shipped standard.

Authors: DisplayXR. Feedback / issues: this repository.

---

## TL;DR

Glasses-free 3D displays (lenticular / parallax-barrier "light-field" panels) are
shipping in laptops and monitors. They show real depth to the naked eye by
**interleaving (weaving) a stereo pair under the panel's optics** and steering the
two views to the viewer's eyes via face tracking. Today only **native** apps can
drive them. The web cannot — there is no way for a page to say "render *this
element* in 3D" and have the browser weave it.

This explainer proposes a small **non-immersive WebXR module** that lets a page
make **one or more individual DOM elements** auto-stereo, while the rest of the page
stays ordinary 2D. It deliberately **reuses the existing WebXR rendering loop**
(views, viewer pose, per-eye projection) so any three.js / WebGL / WebGPU developer
already knows the API. The single genuinely new browser capability is **handing the
element's rendered stereo pair to the OS display runtime to weave**, and compositing
the woven result back at the element's committed on-screen rect — **the browser
never contains any weave or vendor code.**

```js
// Feature-detect a glasses-free 3D display.
if (await navigator.xr.isSessionSupported('inline-3d')) {
  const session = await navigator.xr.requestSession('inline-3d');

  // Bind the session to a DOM element — the browser weaves into its rect.
  const canvas = document.querySelector('#hero');
  const layer = new XRDisplayLayer(session, canvas, { /* gl/gpu binding */ });
  session.updateRenderState({ layers: [layer] });

  // Standard WebXR render loop — two views (eyes), off-axis projection.
  session.requestAnimationFrame(function render(t, frame) {
    const pose = frame.getViewerPose(refSpace);
    for (const view of pose.views) {          // left + right
      const vp = layer.getViewport(view);
      gl.viewport(vp.x, vp.y, vp.width, vp.height);
      drawScene(view.projectionMatrix, view.transform.inverse.matrix);
    }
    session.requestAnimationFrame(render);
  });
}
```

---

## The problem

1. **The hardware is real and growing.** Lenticular / parallax-barrier auto-stereo
   panels (e.g. Leia, and others) ship in consumer laptops and monitors. They are
   glasses-free: the viewer just looks at the screen and sees depth.

2. **Only native apps can use them.** Driving such a panel means producing a stereo
   pair, weaving it (a vendor-specific, per-display-calibrated shader), and steering
   it with eye tracking. That lives behind a native runtime (OpenXR / vendor SDK).
   The web platform has **no API** to reach it.

3. **The naive workarounds are wrong.** A page *could* try to fake it with a
   side-by-side image, but it cannot weave (no access to the panel's optics or eye
   tracker), cannot get glasses-free depth, and cannot follow the viewer's head for
   correct parallax. Browser-side "just composite an overlay window" approaches give
   wrong z-order, scroll-chase lag, and no access to the page's own WebGL content.

4. **Mixed 2D/3D is the point.** The web is overwhelmingly 2D. The valuable case is
   a *mostly-2D page with a few 3D elements* — a product viewer, a map, a model, a
   game canvas — not a whole-screen immersive takeover. The unit of 3D should be a
   **DOM element**, and which elements are 3D should come from the **DOM**, not from
   app-side display bookkeeping.

## Goals

- Let a page render **one or more individual elements** in glasses-free 3D while the
  rest of the page stays normal 2D, with **correct DOM z-order, scroll, and zoom**.
- **Reuse the existing WebXR rendering model** (`XRSession` render loop, `XRView`
  per eye, off-axis `projectionMatrix`, `XRViewerPose`) so the learning curve is ~0
  for anyone who has used WebXR or three.js's `WebXRManager`.
- Drive correct **glasses-free depth + look-around** from the device's real eye/face
  tracker, exposed only as **eye positions / per-eye projection** — never raw camera
  frames.
- Keep the browser **100% vendor-agnostic**: the browser hands the OS display runtime
  a rendered stereo pair and gets back a woven texture. **No weave shader, no vendor
  SDK, no per-display calibration ever enters the browser** (this is the architectural
  red line — see "Non-goals" and "Why a browser change is required").
- Be a **clean, flagged, rebasable** capability suitable as a standards reference
  implementation and cherry-pickable by Chromium-derived browsers.

## Non-goals

- **No weaving in the browser.** Weaving is vendor code (each display's calibrated
  shader). Putting it in the browser would force every display vendor to ship and
  maintain a weaver inside every browser — a non-starter for vendor isolation. The
  browser only does **handle-in → woven-handle-out** through a generic OS API.
- **No immersive / headset takeover.** This is a *non-immersive*, on-desktop,
  per-element capability. It is complementary to `immersive-vr` / `immersive-ar`,
  not a replacement.
- **No raw eye-tracking data to the page.** The page receives only what it needs to
  render: eye positions / per-eye projection matrices. No gaze stream, no camera.
- **No new graphics API.** Rendering is plain WebGL / WebGPU into the layer, exactly
  as immersive WebXR does today.

## Why this needs a browser change (and only a small one)

The whole capability hinges on **zero-copy GPU handle exchange between the page's
content and the OS display runtime**:

- The element's rendered stereo pair must be shared to the runtime as a **GPU handle**
  (Windows: a DXGI/NT shared texture + keyed-mutex; macOS: an `IOSurface`), never a
  CPU readback — earlier CPU-copy attempts were too slow to be usable.
- The runtime weaves it (in the **vendor** display processor) and hands back a woven
  GPU handle, which the browser composites at the element's **committed** on-screen
  rect.

Stock Chromium does **not** expose a content layer's `SharedImage` as an
out-of-process shared handle, and does not let an external runtime drive compositing
of a sub-rect. That is the one piece of browser cooperation required. Everything else
— the weave, the eye tracking, the panel calibration — already exists behind the OS
runtime and stays there. The proposed Chromium patch is therefore small and
vendor-free: **handle export/import + one RPC call + the JS surface.**

## Proposed API

The design is a thin extension of existing WebXR. The novel surface is intentionally
tiny.

### 1. A new non-immersive session mode: `'inline-3d'`

```webidl
// Feature detection + session request reuse existing WebXR entry points.
partial interface XRSystem {
  // 'inline-3d' joins 'inline' | 'immersive-vr' | 'immersive-ar'.
};
```

`navigator.xr.isSessionSupported('inline-3d')` resolves true only on a connected
glasses-free 3D display. `requestSession('inline-3d')` is **non-immersive**: it does
not take over the screen, requires no `XRSessionInit` consent prompt beyond what
`inline` needs, and many sessions/elements may be active at once.

### 2. Bind the session to a DOM element: `XRDisplayLayer`

```webidl
[Exposed=Window]
interface XRDisplayLayer : XRCompositionLayer {
  constructor(XRSession session, Element target, XRDisplayLayerInit init);
  readonly attribute Element target;     // the DOM element shown in 3D
  XRViewport? getViewport(XRView view);  // per-eye viewport into the layer
};

dictionary XRDisplayLayerInit {
  // Exactly one binding, mirroring XRWebGLLayer / XRGPUProjectionLayer.
  XRWebGLRenderingContext? gl;
  GPUCanvasContext? gpu;
  double pixelScale = 1.0;               // backing-store px per CSS px (DPI/zoom)
};
```

The layer's `target` Element is what the browser **weaves into and composites at**.
The browser tracks that element's committed device-pixel rect every frame (through
scroll, zoom, window drag, reflow) and weaves at the corresponding panel phase — so
the lattice stays aligned and the 3D never collapses. The page does not manage rects;
it just renders.

### 3. Rendering: unchanged WebXR loop, two views, off-axis projection

Per frame the page gets an `XRViewerPose` with **two `XRView`s** (left/right eye).
Each `XRView.projectionMatrix` is an **asymmetric-frustum (Kooima) off-axis**
projection derived from the **tracked eye position** relative to the element's
on-screen rectangle — this is what produces glasses-free depth *and* look-around as
the viewer moves. The page renders each view into the layer exactly as it renders
each eye in immersive WebXR today. three.js's existing `WebXRManager` render path
works unmodified.

### 4. Multiple elements (mixed 2D/3D from the DOM)

Any number of `XRDisplayLayer`s — one per 3D element — may be active concurrently,
each bound to its own DOM element and rendered in the same or separate render loops.
The set of 3D regions is therefore **driven by the DOM**: add an element + a layer →
it's 3D; remove it → it's 2D. The browser weaves each element's pair into its rect
and composites them with correct DOM z-order against the surrounding 2D page. No
app-side region/zone machinery is required — the DOM *is* the zone description.

### Convenience layer (non-normative)

A one-call helper (a tiny JS library, not a platform API) can wrap the above for the
common "make this canvas 3D" case, which is how content *volume* happens:

```js
import { makeAutoStereo } from 'auto-stereo';
makeAutoStereo(canvas, drawSceneForView);   // detects, sessions, renders, done
```

## Key scenarios

- **E-commerce product viewer.** A 2D product page; the hero `<canvas>` shows the
  product with real glasses-free depth and look-around. Everything else stays 2D.
- **Maps / data viz.** A 2D dashboard with a 3D terrain or point-cloud panel.
- **Game / model embed.** A WebGL game canvas pops into 3D inside an otherwise 2D
  site; the HUD around it stays crisp 2D.
- **A gallery of 3D thumbnails.** Many small `XRDisplayLayer`s in a scrolling grid —
  the multi-element case — each a depth thumbnail, composited in DOM order.

## How it maps to the implementation (vendor-agnostic)

Under the hood this rides the **DXR weave RPC** (`XR_DXR_weave`): `bindWindow(handle)`
once, then per element per frame `weave(stereoPairHandle, elementRect) → {wovenHandle,
trackedEyes}`. The browser:

1. exports each `XRDisplayLayer`'s rendered stereo pair as a shared GPU handle,
2. calls the weave RPC with the element's committed rect,
3. composites the returned woven handle at that rect (replacing the flat element),
4. feeds the returned eye positions back as the next frame's per-view projection.

The **display processor (vendor plug-in) does all weaving and reads the vendor's own
eye tracker** — the browser passes it nothing vendor-specific and contains no weave
code (ADR-007 / ADR-019). Window moves reach the runtime through the bound handle so
it re-snaps the interlace phase (`xrWeaveSnapWindowRectDXR`). This contract is already
implemented and hardware-validated end-to-end via a CEF offscreen-render host that is
byte-for-byte a Chromium compositor; Step B is the minimal Chromium patch that swaps
the CEF host for `content_shell` / `chrome` calling the *same* RPC.

## Alternatives considered

- **Weave inside the browser** (a Viz compositor pass). Rejected: requires every
  vendor's calibrated weaver in every browser; breaks vendor isolation; an
  unmaintainable security/calibration surface.
- **OS overlay window** (transparent always-on-top window over a reserved rect).
  Rejected: wrong z-order (no DOM element can occlude it), scroll-chase latency, and
  it renders a *companion* surface rather than the page's own WebGL.
- **A bespoke non-WebXR API** (`element.requestAutoStereo()` with its own render
  model). Rejected as the *normative* surface: it would reinvent views, poses, and
  off-axis projection that WebXR already standardizes. (It survives as the
  *non-normative convenience wrapper* above.)
- **CPU side-by-side** handed to the OS. Rejected: too slow (readback), and still
  cannot weave or eye-track.

## Privacy & security

- **No raw sensor data.** The page sees only eye **positions** / per-eye projection,
  the minimum to render correct parallax — never camera frames or a gaze stream. The
  same exposure as the per-eye transforms immersive WebXR already provides.
- **Consent / activation.** `'inline-3d'` is non-immersive and exposes no more than
  `inline` WebXR; UA policy decides whether a transient activation or permission is
  required to begin head-tracked rendering. Eye-position exposure should follow the
  same gating WebXR uses for `viewer` pose.
- **Fingerprinting.** Display presence and capabilities (e.g. resolution, that it is
  a 3D panel) are a fingerprinting surface; expose coarse capability flags and gate
  detailed metrics behind permission, consistent with existing display-capability
  guidance.
- **No cross-origin leakage.** A layer only composites its own element's rendered
  content; the woven handle is the page's own pixels.

## What standardization actually requires (and what this explainer is *not*)

A reference implementation is the **least**-weighted lever for browser adoption. What
moves browser owners, in order: (1) a **standards-track spec** (WICG → intent →
origin trial → flag), (2) a credible **hardware install base**, (3) **developer
demand + content**, and only then (4) a clean mergeable patch. This explainer + the
working RPC/host stack address (4) and seed (3); the weight belongs on (1) and (2).
The realistic early adopters are **Chromium-derived browsers** (Edge, Brave, Arc,
Opera, Vivaldi), which can cherry-pick a clean, flagged patch far sooner than a new
capability would originate upstream.

## Open questions

- **Session mode vs. layer-only.** Should `'inline-3d'` be a full session mode, or
  should `XRDisplayLayer` attach to an existing `inline` session? (This explainer
  picks a dedicated mode for clean feature detection + lifetime.)
- **DPI / fractional zoom.** Exact device-pixel alignment of the woven rect under
  CSS-px ↔ backing-store ↔ device-px scaling (`pixelScale`) needs precise definition;
  one device-pixel of drift collapses the lattice.
- **Multi-viewer / no-viewer.** Behavior when the tracker has no lock (fall back to a
  default IPD / centered eyes) and when multiple faces are present.
- **Power / scheduling.** Per-element render loops vs. a single shared loop; how the
  UA throttles offscreen / occluded 3D elements.
- **Relationship to existing layer types.** Whether `XRDisplayLayer` should be a
  specialization of `XRProjectionLayer` rather than a new sibling.

---

*This document is a discussion strawman, not a committed API. The IDL is illustrative.
The architectural invariant that is **not** negotiable: the browser stays
vendor-agnostic and never contains weave or calibration code — it exchanges GPU
handles with the OS display runtime, which owns all vendor specifics.*
