# WebXR Support — Status & Roadmap

Single source of truth for how DisplayXR supports the browser. Two chapters:

1. **Shipped today** — the *WebXR Bridge v2* metadata/control sideband (Chrome extension + headless companion process). Leaves the frame path untouched.
2. **Roadmap** — *inline 3D in ordinary web pages* via a `session.displayXR.weave()` frame-path API, delivered through Chromium patches.

**Related docs:**
- Developer/usage docs for the shipped bridge: [`webxr-bridge/README.md`](../../webxr-bridge/README.md) (setup), [`webxr-bridge/DEVELOPER.md`](../../webxr-bridge/DEVELOPER.md) (integration), [`webxr-bridge/PROTOCOL.md`](../../webxr-bridge/PROTOCOL.md) (wire schema).
- App-class mechanics (Texture / present-ownership): [App Classes](../getting-started/app-classes.md).
- Why a WebXR session is "legacy" at the OpenXR level: [legacy-app-support](../specs/runtime/legacy-app-support.md), [extension-vs-legacy](../architecture/extension-vs-legacy.md).
- Layout model the browser path piggybacks on: [display zones](display-zones.md), [local 3D zones](local-3d-zones.md).

> History note: this consolidates and supersedes the temporary `webxr-bridge-v2-plan.md` agent-guidance doc (Bridge v2 is shipped — its phase plan lived only to coordinate the build).

---

## Chapter 1 — Shipped today: the WebXR Bridge v2 (metadata sideband)

WebXR already runs on DisplayXR with **no bridge at all**: Chromium's built-in WebXR speaks standard OpenXR, goes through the loader into `displayxr-service`, and renders via the D3D11 service compositor. Frames are standard OpenXR swapchain images on shared DXGI handles — zero-copy, no WebSocket. That bare path has one structural limitation: Chrome does not enable `XR_EXT_display_info`, so every WebXR session is a **legacy app** at the OpenXR level and gets a compromise-scaled framebuffer (`oxr_system.c` legacy branch), with no display geometry, rendering-mode catalogue, mode-change events, or tracked eye poses surfaced to JS.

**The Bridge v2 fixes the *metadata* gap without touching frames.** A Chrome MV3 extension plus a small headless companion (`displayxr-webxr-bridge.exe`) act as a sideband. A DisplayXR-aware page then behaves like a handle app — display-info aware, mode-event aware, dynamically sized render targets, tracked eyes — while still rendering through Chrome's normal WebXR frame path.

```
Chrome MV3 extension ──── WebSocket 127.0.0.1:9014 ──── displayxr-webxr-bridge.exe
  MAIN world:                JSON metadata + input         (headless OpenXR client:
    navigator.xr Proxy       (display-info, mode-changed,   XR_EXT_display_info +
    session.displayXR         eye-poses, window-info, …)    XR_MND_headless; WS server)
  ISOLATED world:                                                   │ xrLocateViews / xrPollEvent
    WebSocket client                                                ▼
Chrome WebXR session ──── OpenXR loader ──── IPC ──── displayxr-service.exe
(frames, UNCHANGED, on shared DXGI handles)            (D3D11 service compositor, runs DP)
```

Two **separate OpenXR sessions** hit the same service: Chrome's (renders frames, never sees `session.displayXR` at the OpenXR level — the extension wraps it client-side) and the bridge's (headless, no swapchain; queries display info, polls events + eye poses, relays over WebSocket). The service is multi-client; it spawns the bridge **on demand** the first time a page touches the `session.displayXR` getter, so legacy pages never trigger a spawn.

**What the page gets** (`session.displayXR`, after `await session.displayXR.ready`): physical display geometry in meters, the rendering-mode catalogue + current mode + per-view tile dims, `renderingmodechange` events, tracked eye poses (for Kooima off-axis projection), window metadata, a compositor HUD, and forwarded keyboard/mouse input. Pages without the extension see `session.displayXR === undefined` and fall back to standard WebXR. Full surface + integration in [`DEVELOPER.md`](../../webxr-bridge/DEVELOPER.md).

**What is deliberately *not* touched:** the WebXR frame pipeline (Chrome WebXR → loader → IPC → service compositor → DP, frames on shared DXGI handles); the `oxr_system.c` legacy compromise branch (still the fallback); existing IPC apps. Vendor-agnostic — the bridge talks to the service compositor, not to any DP.

**Status:** shipped on Windows / D3D11 service compositor. macOS + shell-hosting deferred.

---

## Chapter 2 — Roadmap: inline 3D in ordinary web pages

### 2.1 Thesis — the web's natural model for a *display* is inline, not immersive

WebXR on a headset is **immersive**: you enter an exclusive session that owns present, because the headset *is* the display. A glasses-free 3D display sits on the desk all day, surrounded by ordinary 2D content. Its natural model is **inline** — a 3D element living *inside* a normal page, surrounded by HTML, scrolling with the rest of the content: a product page whose model is 3D among the reviews and the buy button; a 3D photo in a feed; a 3D chart in a dashboard.

Chapter 1's bridge runs WebXR **hosted/immersive** — the runtime owns present and takes the whole screen. That is right for "enter XR" but structurally cannot place weaved 3D in *one element* among the DOM. Inline 3D is a **present-ownership problem**: Chrome's compositor owns the page surface, and we want a weaved region inside it. This is exactly the reason the **Texture / present-ownership** app class exists (see [App Classes](../getting-started/app-classes.md)); the browser is where present-ownership finally pays off.

### 2.2 The hard constraint — weave is per-subpixel and must reach the panel losslessly

Weaving interleaves L/R views at the **subpixel** level, and **weave phase is locked to absolute screen position**. Two consequences govern everything below:

1. **You cannot compress or transport weaved pixels.** Any block/DCT codec averages across the L/R subpixel lattice and destroys the parallax — so "render elsewhere and stream the weave" is dead. (To *share* a 3D session, ship the **pre-weave stereo pair** — SBS or MV-HEVC — and re-weave on playback.)
2. **Elements can never weave independently and then be re-composited** — re-compositing already-weaved tiles breaks the lattice. The present-owner must receive **pre-weave** content and weave **once**, at final screen coordinates.

Browser present-ownership is one of the few cases satisfying (1): the weave happens **locally, same GPU → same panel, native-res, no transport**. That is precisely why it works where streaming fails.

### 2.3 The target API — `session.displayXR.weave()`

Extend the `session.displayXR` namespace (Chapter 1) with a frame-path call:

```
weavedTexture = session.displayXR.weave(srcStereoTexture, screenRect, eyes)
```

- **`srcStereoTexture`** — the page's pre-weave stereo content (e.g. SBS in a `<canvas>` / WebGL FBO).
- **`screenRect`** — the element's absolute **device-pixel** rect on the physical panel (so phase is correct).
- **`eyes`** — current eye positions; head-tracked weave, sourced from the existing bridge eye-pose feed. `weave()` **composes with**, does not replace, the Chapter 1 metadata.
- The browser composites the returned weaved texture into the page at `screenRect` and presents locally.

**What falls out for free:**
- **Mixed 2D/3D comes from the DOM**, not app-side zone machinery. The page's HTML *is* the 2D surround; the weaved `<canvas>` *is* the 3D zone. Browser layout already composites "flat content around one special element," so the [display-zones](display-zones.md) result is expressed as ordinary page layout. Layout rect → 3D zone; everything else → 2D.
- **Reach.** Any three.js / WebGL / WebGPU developer adds 3D with one call — no native build, no OpenXR, no installer. This is how content *volume* happens.

**Why it's hard (and needs real browser cooperation):**
1. **Phase/position under async compositing.** Scroll, window drag, and zoom move the element's physical-panel position every frame. If the browser composites the weave even one device-pixel off from the `screenRect` the weave assumed, the lattice misaligns and the 3D collapses. Chromium's compositor (Viz) is async/threaded, so "weave-assumed position == final composited position" is the core engineering problem.
2. **DPI / fractional zoom** — CSS px ≠ canvas backing store ≠ device px; weave needs device-pixel-exact alignment.
3. **Zero-copy GPU texture access** — the page's canvas/WebGL backing texture must be shared with the weaver without a CPU copy. Earlier texture attempts were slow precisely because they were CPU readback / cross-process copies; the only viable version is a shared GPU handle (Windows: DXGI/NT shared handle + keyed mutex). Stock Chrome does not expose that handle to an outside process — **this is the cooperation that requires a Chromium change.**

### 2.4 Implementation path — straight to Chromium patches

We go **directly to GPU-texture-level Chromium work** — no OS-overlay intermediate.

> **Rejected: OS-overlay compositing.** A transparent always-on-top window weaving over a page-reserved rect (the avatar-overlay technique) needs no browser changes, but it gives **wrong z-order** (the overlay floats above all page content — no dropdown/header/scrollbar can occlude it), **scroll-chase latency**, and only a **companion renderer** rather than the page's own WebGL. It is a dead end for true inline 3D, so we skip it.

**Step A — CEF prototype harness (Chromium-faithful, no source fork).** Use **CEF (Chromium Embedded Framework)** in **offscreen-render (OSR)** mode. Its `OnAcceleratedPaint` callback hands a **shared D3D11 texture of the fully-composited page** — zero-copy, no source changes:
1. Page draws its stereo SBS pair into the zone canvas.
2. CEF hands the whole-page texture via `OnAcceleratedPaint`.
3. Crop the zone sub-rect, weave it (phase = its known screen rect), composite back, present.

Because *you* own present in OSR, this is present-ownership working as intended, and z-order is correct (the weave replaces the flat canvas in the composited page). It's Chromium, so it represents the eventual real-Chrome target, and `OnAcceleratedPaint` *is* the cooperation hook as public API. **This validates the `weave()` contract zero-copy** before touching upstream Chromium.

**Step B — minimal, rebasable Chromium patch.** A small patch series — expose the page's canvas/WebGL SharedImage handle to the weaver + a flagged `session.displayXR.weave` — kept rebasable against Chrome's release cadence. This is **both** the way we ship inline 3D to our own users (a patched Chromium/CEF distribution) **and** the reference implementation a standards proposal needs. Deliberately *not* a maintained product fork (see §2.5).

### 2.5 Adoption — does a Chromium fork accelerate browser-owner uptake?

**Not by itself — a permanent product fork can even slow it.** Separate two goals:
- **Ship inline 3D to our own users now** → a patched Chromium / CEF distribution delivers this directly.
- **Get Google/Chrome (and other engines) to adopt it natively** → a fork is *necessary input* (a reference implementation) but **not the lever**. Brave/Edge/Vivaldi/Arc/Opera have shipped custom features for years that never landed upstream; an existing fork can reduce upstream urgency ("they already have a browser").

**What actually moves browser owners** (weight order):
1. **A standards-track spec.** WebXR is governed by the **W3C Immersive Web CG/WG (WICG)**; Chrome implements specs, not vendor namespaces. `session.displayXR.*` must become a standardized WebXR module — e.g. a *non-immersive / auto-stereo display* extension. Path: explainer → WICG → intent-to-prototype → origin trial → flag.
2. **A hardware install base.** WebXR got Chrome priority on headset momentum; glasses-free 3D needs a credible "N units in users' hands" story.
3. **Demonstrated developer demand + content.**
4. **A clean, flagged, mergeable reference implementation** — the *least*-weighted, and the only one a patch/fork addresses.

So the right artifact for (4) is the **minimal rebasable patch of §2.4 Step B**, not a product fork (running a browser = rebasing Chrome's ~4-week cadence + security treadmill + signing + auto-update — a dedicated team). The realistic **early adopters are the other Chromium-derived browsers** (Edge, Brave, Arc, Opera, Vivaldi) — they can cherry-pick a clean patch far more readily than Google will originate a spec.

### 2.6 Sequencing

1. **CEF OSR prototype** (§2.4 Step A) — validate `session.displayXR.weave(canvas)` zero-copy on a Chromium-faithful engine; solve phase/position under scroll/zoom.
2. **Minimal rebasable Chromium patch** (§2.4 Step B) — SharedImage handle exposure + flagged `weave`; ship to our users via a patched distribution; serve as the reference impl.
3. **Standards push, in parallel** (non-engineering) — a WICG explainer/spec for a non-immersive 3D-display WebXR module, backed by the demos from steps 1–2 and a hardware-install-base narrative.

**One-line takeaway:** the patch makes the *demo* and the *product* possible; the **spec + hardware story** is what makes browser owners adopt. Spend engineering on a small rebasable patch + CEF harness, and put the weight behind standards.
