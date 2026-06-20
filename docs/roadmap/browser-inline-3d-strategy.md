# Browser Inline 3D Strategy — Present-Ownership, `weave()`, and Browser-Owner Adoption

**Status:** Strategy / direction (no committed implementation yet)
**Relationship to other docs:**
- Builds on [WebXR Bridge v2](webxr-bridge-v2-plan.md) — the *metadata sideband*. This doc is about the **frame path** that bridge v2 deliberately leaves untouched.
- App-class mechanics: [App Classes](../getting-started/app-classes.md) (Texture / present-ownership).
- Layout model: [display zones](display-zones.md), [local 3D zones](local-3d-zones.md), [surround→zones deprecation](surround-zones-deprecation.md).
- Reuses the overlay machinery from [avatar overlay (native)](avatar-overlay-native.md).

---

## 1. Thesis: the web's natural model for a 3D display is *inline*, not *immersive*

WebXR on a headset is **immersive** — you enter an exclusive session that takes over present, because the headset *is* the display surface. A glasses-free 3D display is different: it sits on the desk all day, surrounded by ordinary 2D content. Its natural content model is **inline** — a 3D element living *inside* a normal web page, surrounded by HTML, scrolling with the rest of the content:

- a product page whose model is 3D, among the reviews and the buy button;
- a 3D photo in a social feed;
- a 3D chart in a dashboard.

Today WebXR-on-DisplayXR runs **hosted/immersive** (see [bridge v2](webxr-bridge-v2-plan.md)): the runtime owns present and takes the whole screen. That is correct for "enter XR" experiences but structurally cannot place weaved 3D in *one element* among the DOM. Inline 3D is a **present-ownership problem**: Chrome's compositor owns the page surface, and we want a weaved region inside it.

This is the single highest-reach, lowest-friction content model for a desktop 3D display, and it is the reason the **Texture / present-ownership** app class exists (see [App Classes §"Who passes the window handle"](../getting-started/app-classes.md)). The browser is where present-ownership finally pays off.

## 2. The hard constraint: weave is per-subpixel and must reach the panel losslessly

Weaving interleaves the L/R views at the **subpixel** level, and the weave **phase is locked to absolute screen position**. Two consequences govern every option below:

1. **You cannot compress or transport weaved pixels.** Any block/DCT codec averages across the L/R subpixel lattice and destroys the parallax. So "render in the cloud / another machine and stream the weave" is a non-starter. (This is also why a *weaved* screen-share is pointless — if you ever want to share a 3D session, ship the **pre-weave stereo pair**: SBS or MV-HEVC, re-weave on playback.)
2. **Apps/elements can never weave independently and then be re-composited.** Re-compositing already-weaved tiles breaks the lattice. The present-owner must receive **pre-weave** content and weave **once**, at final screen coordinates.

Present-ownership in the browser is one of the few cases that satisfies (1): the weave happens **locally, same GPU → same panel, native-res, no transport**. That is exactly why it works where streaming fails.

## 3. The target API: `session.displayXR.weave()`

Extend the `session.displayXR` namespace (introduced by [bridge v2](webxr-bridge-v2-plan.md)) with a frame-path call:

```
weavedTexture = session.displayXR.weave(srcStereoTexture, screenRect, eyes)
```

- **`srcStereoTexture`** — the page's pre-weave stereo content (e.g. SBS in a `<canvas>` / WebGL FBO).
- **`screenRect`** — the element's absolute device-pixel rect on the physical panel (so phase is correct).
- **`eyes`** — current eye positions (head-tracked weave; comes from the existing bridge eye-pose feed — `weave()` is **not** standalone, it composes with bridge v2 metadata).
- Browser composites the returned weaved texture into the page at `screenRect` and presents locally.

### What falls out for free
- **Mixed 2D/3D comes from the DOM, not app-side zone machinery.** The page's HTML *is* the 2D surround; the weaved `<canvas>` *is* the 3D zone. Browser layout already composites "flat content around one special element," so the [display-zones](display-zones.md) result is expressed as ordinary page layout. Layout rect → 3D zone; everything else → 2D.
- **Reach.** Any three.js / WebGL / WebGPU developer adds 3D with one call — no native build, no OpenXR, no installer, no CMake. This is how content *volume* happens.

### Why it's hard (and gated on real browser support)
1. **Phase/position alignment under async compositing.** Scroll, window drag, and zoom move the element's physical-panel position every frame. If the browser composites the weave even one device-pixel off from the `screenRect` the weave assumed, the lattice misaligns and the 3D collapses. Chromium compositing (Viz) is async/threaded, so "weave-assumed position == final composited position" is the core engineering problem.
2. **DPI / fractional zoom.** CSS px ≠ canvas backing store ≠ device px. Weave needs device-pixel-exact alignment.
3. **Needs the browser to cooperate at the GPU level** — zero-copy access to the page's texture (see §5).

## 4. Prototyping routes available today

Two routes, trading fidelity against cost.

### Route 1 — Overlay compositing (stock Chrome, no fork) — buildable now
Treat the browser purely as the 2D surround; render the 3D zone in a **separate transparent overlay window** the runtime owns. This is the "pass the window handle + zone rects, *not* textures" approach.

- The page reserves a placeholder element for the 3D zone.
- The **extension/bridge reports that element's absolute screen rect + scroll/resize/zoom events** to the runtime (extends the bridge v2 sideband).
- The runtime drives a transparent always-on-top overlay over that rect and weaves into it — **literally the [avatar overlay](avatar-overlay-native.md) `WS_POPUP` + position-tracking machinery already shipped**.
- The 3D is rendered by a companion DXR process; **only geometry crosses the wire, never pixels** → no texture-IPC bottleneck.

> **Note on "pass the browser HWND and let the runtime weave into it":** this does *not* work directly. Chrome's compositor (Viz) repaints its own window every frame and overwrites anything the runtime draws. A *separate* overlay window is required — hence Route 1's transparent top-most window rather than co-owning Chrome's surface.

**Limitations (honest):**
- **Z-order is wrong** — the overlay floats above all page content; a dropdown / sticky header / scrollbar cannot occlude the 3D. It's "3D punched through a hole," fine for a hero element or product viewer, wrong for a 3D thumbnail in a scrolling feed.
- **Scroll-chase latency** — the overlay must chase the element; lag shows the 3D sliding behind the page.
- It's a **companion renderer**, not the page's own WebGL scene (unless a scene description is piped across).

**Value:** de-risks the *hard* part — phase/position tracking under scroll/zoom — with parts we already ship, and demonstrates the experience.

### Route 2 — CEF offscreen rendering (Chromium-faithful, no source fork) — recommended for faithful prototype
Use **CEF (Chromium Embedded Framework)** in **offscreen-render (OSR)** mode. Its `OnAcceleratedPaint` callback hands you a **shared D3D11 texture of the fully-composited page** — zero-copy, no Chromium source changes:

1. Page draws its stereo SBS pair into the zone canvas.
2. CEF hands you the whole-page texture via `OnAcceleratedPaint`.
3. Crop the zone sub-rect, weave it (phase = its known screen rect), composite back, present.

Because *you* own present in OSR, this is the present-ownership model working as intended, and z-order is correct (the weave replaces the flat canvas in the composited page). It's **Chromium**, so it's representative of the eventual real-Chrome target, and `OnAcceleratedPaint` *is* the "browser cooperation" hook — pre-built as public API. **This is the harness to validate the `weave()` contract zero-copy.**

## 5. Why texture-over-IPC "was too slow," and what cooperation means

Earlier texture attempts were slow because they were **CPU readback / cross-process copy** (`readPixels` → serialize → weave → send back) — hopeless per frame. The only fast version is **zero-copy GPU handle sharing** (Windows: DXGI/NT shared handle + keyed mutex). Stock Chrome does not expose its canvas/WebGL backing texture to an outside process — *that* is the "needs browser cooperation" part. CEF's `OnAcceleratedPaint` is exactly that cooperation as public API; a full Chromium fork would expose it per-`<canvas>`.

## 6. Open-source browsers on Windows, ranked

| Option | Cost | Notes |
|--------|------|-------|
| **CEF (Chromium Embedded Framework)** | Low–Med | Best. `OnAcceleratedPaint` → zero-copy shared D3D11 texture, no source fork. Chromium-faithful. **Start here.** |
| **Full Chromium fork** | High (permanent) | Only if per-`<canvas>` texture sharing (true element-level inline with correct intra-page occlusion) is required. Multi-hour builds, Viz/SharedImage surgery. It is the eventual production target. |
| **Servo** (Rust, Mozilla) | Med–High | Open, embeddable, very hackable, runs on Windows; more experimental, more effort than CEF for the GPU hook. |
| **Electron** | Med | Chromium-based but app-oriented; does not expose the accelerated-paint texture hook as cleanly as CEF. Worse for *this* specifically. |

## 7. Does a full Chromium fork accelerate browser-owner adoption?

**Not by itself — and a permanent product fork can slow it.** Separate two goals:

- **Ship inline 3D to our own users now** → a fork (or CEF app) delivers this directly.
- **Get Google/Chrome (and others) to adopt it natively** → a fork is *necessary input* (a reference implementation) but **not the lever**. Browser owners do not adopt features because a fork exists; Brave/Edge/Vivaldi/Arc/Opera have shipped custom features for years that never landed upstream. A permanent fork can even reduce upstream urgency ("they already have a browser").

### What actually moves browser owners (in weight order)
1. **A standards-track spec.** WebXR is governed by the **W3C Immersive Web CG/WG (WICG)**. Chrome implements specs, not vendor namespaces. `session.displayXR.*` must become a standardized WebXR module — e.g. a *non-immersive / auto-stereo display* extension. Path: explainer → WICG → intent-to-prototype → origin trial → flag.
2. **A hardware install base.** WebXR got Chrome priority on headset momentum; glasses-free 3D needs a credible "N units in users' hands" story.
3. **Demonstrated developer demand + content.**
4. **A clean, mergeable, flagged reference implementation** — the *least*-weighted of the four, and the only one a fork addresses.

### The right artifact is a minimal patch, not a product fork
For (4), keep a **minimal, rebasable Chromium patch series** (SharedImage/canvas-handle exposure + a flagged `displayXR.weave`) used to produce demos and an origin-trial-style proof — **without** the company-scale burden of running a browser (rebasing Chrome's ~4-week cadence, the security-patch treadmill, signing, auto-update — the Brave/Vivaldi reality, a dedicated team). Pair it with **CEF** for the embedding/distribution story to our own users.

### Near-term realistic adopters
The other **Chromium-derived browsers** (Edge, Brave, Arc, Opera, Vivaldi) are the realistic early adopters, not Google first — they can cherry-pick a clean Chromium patch far more readily than Google will originate a spec. A well-structured patch genuinely helps there.

## 8. Recommended sequencing

1. **De-risk now — Route 1 overlay.** Reuse the avatar overlay + bridge sideband; extension reports zone rects; prove phase/position tracking under scroll/zoom. Cheapest path, no browser changes.
2. **Prove the real model — Route 2 CEF OSR.** Validate the `session.displayXR.weave(canvas)` contract zero-copy on a Chromium-faithful engine.
3. **Standards push in parallel (non-engineering).** A **WICG explainer/spec for a non-immersive 3D-display WebXR module**, backed by the demos from steps 1–2 and a hardware-install-base narrative. This — not the fork — is what makes browser owners adopt.
4. **Minimal rebasable Chromium patch** (handle exposure + flagged `weave`) as the reference implementation for the spec and for Chromium-derivative cherry-picks. Avoid a maintained product fork unless owning a browser becomes strategic.

**One-line takeaway:** the fork makes the *demo* possible; the **spec + hardware story** is what drives adoption. Spend the engineering on a small patch + CEF harness, and put the weight behind standards.
