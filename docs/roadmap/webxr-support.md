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

### 2.3 The target API — a window-bound weave session + per-element `weave()`

**The browser never weaves** (see §2.4). It hands DXR a texture handle and gets a weaved handle back; the **DP weaves inside DXR**, exactly as for a native app. Because weave phase is locked to the element's absolute screen position, the DP must also track the **browser window's** moves/resizes (the DP *phase-snaps* the interlace to the window's panel position). So the page establishes a **weave session bound to the browser's OS window handle**, then weaves per element:

```
// once: bind Chrome's window so the DP can phase-snap on move/resize
session.displayXR.bindWindow(browserWindowHandle)   // reuses XR_EXT_{win32,cocoa}_window_binding

// per frame, per weaved element — a GPU handle round-trip, both ways zero-copy.
// Returns the weaved handle AND the current tracked eyes (see below):
{ weavedHandle, eyes } = session.displayXR.weave(preWeaveHandle, windowRelativeRect)
```

- **`preWeaveHandle`** — shared GPU handle to the element's pre-weave stereo content (SBS `<canvas>` / WebGL FBO).
- **`windowRelativeRect`** — the element's device-pixel rect *within the window*. DXR combines it with the HWND-tracked window position to get the absolute-screen phase, so dragging/resizing the Chrome window re-snaps phase automatically.
- **`eyes` flow *out*, not in.** The interlace itself is the **DP's** job and reads the **vendor's own eye tracker** internally — the page feeds it nothing for the weave. What the page needs eyes *for* is its **own off-axis (asymmetric-frustum / Kooima) projection**: as the viewer's head moves, the page must re-render its pre-weave stereo pair with frusta skewed to the new eye positions (virtual-camera motion / look-around). So `weave()` **returns** the runtime's current tracked eyes, and the page renders the **next** frame's pair from them. (Equivalently, an OpenXR-session present-owner gets the same eyes via `xrLocateViews`; the return value is what makes the *session-less* caller work.)
- DXR routes `{handle, rect, window-position}` to the DP, which weaves at the correct phase, returns the weaved shared handle the browser composites at that rect, and hands back the tracked eyes for the next frame.

```
browser (present-owner)                 DXR (runtime/service)           DP (vendor plug-in)
  bindWindow(handle)        ──────────▶  track window move/resize  ──▶  phase-snap source
  export pre-weave canvas   ──handle──▶  weave(handle, rect)        ──▶  weave at window-
    as shared GPU handle                                                  position + rect phase
  composite weaved quad     ◀─handle──   return weaved handle       ◀──  weaved texture
  off-axis render next      ◀──eyes───   + current tracked eyes     ◀──  vendor eye tracker
    frame from eyes
    at rect, present
```

**Relationship to a texture-class app — it *is* one.** Architecturally this is a [texture-class app](../getting-started/app-classes.md): a present-owner that owns its OS window (passes the real window handle for DP position tracking / phase-snap), hands the runtime pre-weave textures for canvas sub-rects, lets the DP weave them, and composites/presents itself. A texture app **already** submits any number of 2D + 3D zones with z-order and transparency via [display-zones](display-zones.md) — so the browser adds **no new capability**. It differs only in:
1. **Who authors the zone layout, and how often.** The zones are driven **live by the DOM compositor** — they scroll/move/zoom every frame at positions chosen by web content, not by the app author. Occlusion and the 2D surround are the DOM itself (layout rect → 3D zone; everything else → 2D).
2. **The process boundary** — the weave is a **cross-process per-frame RPC** to the service, not an in-process texture compositor.

So we reuse the texture + window-binding + display-zones machinery wholesale rather than inventing a new path. The genuinely new engineering is the per-frame weave RPC and the DOM-driven dynamic-zone bookkeeping.

**What falls out for free:**
- **Mixed 2D/3D comes from the DOM**, not app-side zone machinery (point 1 above).
- **Reach.** Any three.js / WebGL / WebGPU developer adds 3D with one call — no native build, no OpenXR, no installer. This is how content *volume* happens.

**Why it's hard (and needs real browser cooperation):**
1. **Phase/position under async compositing.** Scroll, window drag, and zoom move the element every frame. If Viz composites the weaved quad even one device-pixel off from the `windowRelativeRect` DXR weaved for, the lattice misaligns and the 3D collapses — so the rect must be the *committed* compositor position, and window moves must reach the DP (via the bound handle) to re-snap phase.
2. **DPI / fractional zoom** — CSS px ≠ canvas backing store ≠ device px; weave needs device-pixel-exact alignment.
3. **Zero-copy GPU handle access** — the page's canvas/WebGL backing must be shared as a GPU handle, never a CPU copy (earlier attempts were slow precisely because they were CPU readback). The round-trip is shared-handle both ways (Windows: DXGI/NT shared handle + keyed mutex). Stock Chrome does not expose that handle to an outside process — **this is the cooperation that requires a Chromium change.**

### 2.4 Implementation path — DXR weave RPC first; the browser never weaves

**The browser must never contain weave code.** Weaving is **vendor** code — each DP's calibrated, per-display shader. Embedding it in the browser would mean every display vendor ships and maintains a weave shader *inside every browser*, which breaks vendor isolation (ADR-019: all vendor code stays behind the plug-in; the runtime DLL itself carries zero vendor identifiers) and ADR-007 (only the DP weaves). So the browser only ever **hands DXR a texture handle and gets a weaved handle back** — the DP weaves inside DXR, behind DXR's generic API, exactly as for native apps. The browser stays 100% vendor-agnostic.

> **Rejected: weaving inside the browser** (e.g. a Viz compositor pass) — would require every vendor's weaver in every browser. Non-starter.
> **Rejected: OS-overlay compositing** — a transparent always-on-top window over a page-reserved rect (avatar-overlay technique) gives **wrong z-order** (no DOM element can occlude it), **scroll-chase latency**, and only a **companion renderer** rather than the page's own WebGL.

Order of work, smallest testable first:

**Step 0 — DXR weave RPC (in-tree, no browser).** Add a window-bound one-shot weave service to `displayxr-service`:
`bindWindow(handle)` + `weave(shared_handle, window_relative_rect) → {weaved_shared_handle, tracked_eyes}`. It imports the handle, runs the **DP** weave at the phase derived from window-position + rect, exports the weaved handle, and returns the DP's current tracked eyes for the caller's off-axis (look-around) rendering (eyes flow out, not in — the interlace reads the vendor tracker internally; see §2.3). A synchronous weave *service* distinct from the steady swapchain frame loop, reusing the existing texture + window-binding + DP `process_atlas`/weave paths. **Testable with zero browser:** a tiny native client hands it a known SBS texture + rect, presents the weaved output, and renders look-around from the returned eyes on **real 3D-display hardware (Windows)**. First milestone. **Shipped:** `XR_EXT_weave` (`xrWeaveBindWindowEXT` + `xrWeaveSubmitEXT`) + the `weave_rpc_probe_d3d11_win` harness; hardware-validated on Leia. Phase-0 audit + gap analysis: [`webxr-weave-rpc-step0-gap-analysis.md`](webxr-weave-rpc-step0-gap-analysis.md) (issue #625).

**Step A — CEF host as browser stand-in (no host-side weave).** Fork `cefsimple` in **offscreen-render (OSR)** mode; `OnAcceleratedPaint` hands a **shared D3D11 texture of the composited page** — zero-copy, public CEF API, no source changes. The host exports the zone sub-rect handle, **drives the Step-0 RPC** (the real DP through the real contract — *not* a host-side weave shader), composites the returned handle, and presents. Because CEF owns present in OSR, z-order is correct (the weave replaces the flat canvas in the composited page). Validates the full round-trip + phase/position under scroll/zoom on a Chromium-faithful engine. **Shipped:** the [`displayxr-cef-host`](https://github.com/DisplayXR/displayxr-cef-host) repo (CEF OSR + `XR_EXT_weave`, hardware-validated on Leia) — round-trip ~0.6 ms, scroll-tracking, look-around, and live page animation during window drag all confirmed. **Known limitation (carried to Step B):** the interlace *phase* does not re-snap while the window is **dragged** — the vendor weaver's phase-snap (`WeaverBaseImpl::weaverWndProc` → `SnapToPhase`, which rewrites `WM_WINDOWPOSCHANGING` to a phase-aligned position) is an **in-process WndProc subclass** that cannot attach to a cross-process (IPC) present-owner window. The fix is an **opaque `xrWeaveSnapWindowRectEXT(origin, target) → snapped` RPC**: the host intercepts `WM_WINDOWPOSCHANGING` and applies the DP-returned snapped rect; the snap math + lens params (slant/pitch) never leave the DP, keeping the host vendor-neutral. Implementable today in the DP via a service-side probe window that invokes the SDK's own `SnapToPhase`; cleanest is a one-line public SR `snapWindowRect` wrapper. (`getDotPitch` + `GetSlant` are already public.)

**Step B — minimal, rebasable Chromium patch (vendor-free).** Bind Chrome's window handle to the weave session (reuse `XR_EXT_{win32,cocoa}_window_binding`), export each weaved canvas's `SharedImage` as a shared handle, call the **same Step-0 RPC**, composite the returned quad in Viz at its committed rect. Test in **`content_shell`** (the minimal Chromium embedder — builds far faster than full `chrome`), then `chrome`, on real 3D-display hardware. The patch contains only **handle export/import + the RPC call + the JS surface (`bindWindow`/`weave`)** — zero vendor code, zero weave shader in Chromium. Kept rebasable against Chrome's release cadence; it is **both** how we ship inline 3D to our own users (a patched Chromium/CEF distribution) **and** the reference implementation a standards proposal needs. Deliberately *not* a maintained product fork (see §2.5).

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

1. **DXR weave RPC** (§2.4 Step 0) — window-bound `bindWindow` + `weave(handle, rect) → {weaved, eyes}`; validate the DP round-trip, look-around from the returned eyes, **and its per-frame latency** headless on real 3D-display hardware, no browser. ✅ shipped (`XR_EXT_weave`, hw-validated).
2. **CEF OSR host** (§2.4 Step A) — drive that RPC from `OnAcceleratedPaint`; solve phase/position under scroll/zoom on a Chromium-faithful engine. ✅ shipped ([`displayxr-cef-host`](https://github.com/DisplayXR/displayxr-cef-host)); scroll/zoom phase solved, window-drag phase-snap deferred to the `xrWeaveSnapWindowRectEXT` follow-up (shared with Step B).
3. **Minimal rebasable Chromium patch** (§2.4 Step B) — window binding + canvas `SharedImage` handle export + the RPC call + the `bindWindow`/`weave` JS surface; test in `content_shell`; ship to our users via a patched distribution; serve as the reference impl.
4. **Standards push, in parallel** (non-engineering) — a WICG explainer/spec for a non-immersive 3D-display WebXR module, backed by the demos from steps 1–3 and a hardware-install-base narrative.

**One-line takeaway:** the DXR weave RPC + patch make the *demo* and the *product* possible; the **spec + hardware story** is what makes browser owners adopt. Spend engineering on the weave RPC + a small rebasable patch + CEF harness, and put the weight behind standards.
