# DisplayXR — Project Orientation

*A top-down, plain-language read of the whole project. Start here, read front-to-back, and you'll know what DisplayXR is, why it's built the way it is, and where every piece of documentation lives. Aimed at anyone new to the project — PM, engineering lead, vendor, or executive — regardless of graphics background.*

*For a role-based jump table ("I'm an app developer / core contributor / vendor, take me to my docs"), see [`README.md`](README.md). This document is the linear story that sits above it.*

---

## 1. What DisplayXR is, in one paragraph

DisplayXR is a standalone OpenXR **runtime for glasses-free 3D displays** — the software layer that lets an ordinary application render correct 3D for a spatial monitor (Leia SR, Acer SpatialLabs, Samsung Odyssey 3D, and others) through the industry-standard OpenXR API instead of a vendor-specific SDK. An app written once against DisplayXR runs on any supported 3D display, exactly the way an OpenXR app written once runs on any VR headset. It ships on Windows, macOS, and Android, with Linux hardware-validated in Preview (Vulkan-only, not yet GA), and covers every major graphics API (Direct3D 11/12, Vulkan, Metal, OpenGL). It began as a fork of [Monado](https://monado.freedesktop.org/), the reference open-source OpenXR runtime, and was re-pointed from head-mounted displays to fixed 3D displays.

**The one sentence to remember:** DisplayXR is the neutral *interface and host*; the hardware-specific magic (weaving, calibration, eye tracking) lives behind each display vendor's own SDK, reached through a small plug-in. DisplayXR itself contains none of it.

---

## 2. The problem it solves

Glasses-free 3D displays are reaching mass production, but the software is fragmenting: every vendor ships its own runtime, SDK, and content pipeline, so a developer who wants an app to run in 3D must integrate separately against each one — and most never do. This is the trap early VR fell into: capable hardware, almost no content, because there was no common target. VR escaped it with OpenXR. The 3D-display category needs the same neutral interface, and DisplayXR is that interface: one thing developers build against, with each display lighting up through its own plug-in underneath.

---

## 3. The mental model — four layers, one rule

```
   Application  (any graphics API — renders the 3D pixels itself)
        │   OpenXR standard API  +  a few 3D-display extensions
   ┌────┴─────────────────────────────────────────────┐
   │  DisplayXR runtime                                │
   │   • OpenXR state tracker (inherited from Monado)  │
   │   • Native compositor, one per graphics API       │
   └────┬─────────────────────────────────────────────┘
        │   xrt_plugin ABI  (the vendor boundary)
   Display Processor  (vendor plug-in DLL — does the "weave")
        │
   The 3D display
```

Read the layers top to bottom:

1. **The application** renders the scene. Crucially, **there is no view synthesis anywhere in DisplayXR** — the app itself renders each of the display's views (typically two, sometimes more). This single fact explains most of the design below: because the app produces the pixels, the runtime must hand it exactly what it needs to do that (where the viewer's eyes are, the screen geometry) and nothing more.
2. **The runtime** speaks standard OpenXR to the app, computes the off-axis projection, and tiles the app's rendered views into a single texture ("the atlas"). It does **not** weave.
3. **The display processor (DP)** is a vendor plug-in that takes the atlas and performs the "weave" — the vendor-proprietary step that interlaces the views for that specific panel's optics. This is where, and the only where, vendor technology lives.
4. **The display** shows the result.

**The load-bearing rule** ([ADR-007](adr/ADR-007-compositor-never-weaves.md)): *the compositor never weaves; the display processor always does.* The boundary between "neutral runtime" and "vendor secret sauce" is the `xrt_plugin` ABI, and it is enforced by architecture, not by policy — the runtime DLL is built with zero vendor identifiers in it ([ADR-019](adr/ADR-019-vendor-plugin-aux-boundary.md)).

---

## 4. How one frame flows (the 60-second version)

1. The app asks the runtime where to render from. The runtime returns standard OpenXR views whose projection is computed from the **physical screen geometry and the viewer's tracked eye position** — an off-axis "Kooima" frustum, expressed in stock OpenXR's own `XrFovf` asymmetric-angle field ([kooima-projection](architecture/kooima-projection.md), [ADR-012](adr/ADR-012-window-relative-kooima-projection.md)).
2. The app renders each view into a shared swapchain sized for the worst case across all modes ([ADR-010](adr/ADR-010-shared-app-iosurface-worst-case-sized.md), [swapchain-model](specs/runtime/swapchain-model.md)).
3. The native compositor for that graphics API crops each submission to the active mode and tiles the views into the atlas ([ADR-005](adr/ADR-005-multiview-atlas-layout.md), [multiview-tiling](specs/runtime/multiview-tiling.md)). It crops *before* handing off; zero-copy is a narrow special case, not the norm ([ADR-030](adr/ADR-030-crop-before-dp-zero-copy-only-when-swapchain-equals-atlas.md)).
4. The compositor calls the vendor DP's `process_atlas`. The DP weaves and presents to the panel.

That's the whole hot path. Everything else — the shell, IPC, capture, WebXR, MCP — is optional machinery layered around this core, and a plain app never touches it.

---

## 5. Why it's built this way — the decisions that matter

The project's design rationale is captured in **31 Architecture Decision Records** ([`adr/`](adr/)). You don't need all of them; these are the load-bearing ones, grouped by the question they answer.

**"Why fork Monado instead of building on stock?"**
- [ADR-001](adr/ADR-001-native-compositors-per-graphics-api.md) — a **native compositor per graphics API** (D3D11/12, Vulkan, Metal, GL) instead of one Vulkan compositor with translation. Avoids a per-frame interop copy, unlocks native present semantics and latency wins, and the Vulkan leg does double duty for Android/Linux.
- [ADR-004](adr/ADR-004-d3d11-native-over-vulkan-multi-compositor.md) — why D3D11 native is the primary Windows path (late latching the interop path can't match).
- [ADR-009](adr/ADR-009-upstream-cherry-pick-strategy.md) — how divergence from upstream Monado is bounded: always take fixes, skip large refactors. Keeps the fork maintainable.

**"Who owns the display surface?"** (the deepest departure from stock OpenXR, which assumes a private headset)
- [ADR-008](adr/ADR-008-display-as-spatial-entity.md) — the display is a spatial entity in the world, not a headset.
- [XR_DXR_win32_window_binding](specs/extensions/XR_DXR_win32_window_binding.md) / [XR_DXR_cocoa_window_binding](specs/extensions/XR_DXR_cocoa_window_binding.md) — the app hands the runtime its own window; the runtime tracks that window's rectangle on the panel so perspective stays correct even windowed.
- [app-classes](getting-started/app-classes.md) — the four ways an app can connect (handle / texture / hosted / IPC) and when to use each.

**"Why is eye tracking visible to the app at all?"**
- [ADR-024](adr/ADR-024-raw-vs-render-ready-views.md) — because the app renders the pixels, it needs the eye-derived projection. The runtime keeps the hard math (the app gets render-ready views via a "rig" descriptor); raw eye positions are exposed only to advanced consumers.
- [eye-tracking-modes](specs/vendor/eye-tracking-modes.md), [ADR-022](adr/ADR-022-per-mode-capability-flags-frozen-enum-structs.md) — the MANAGED vs MANUAL fall-back-to-2D contract and the ABI-stable per-mode capability flags.

**"What's the vendor boundary?"**
- [ADR-003](adr/ADR-003-vendor-abstraction-via-display-processor-vtable.md) — vendor code is isolated behind a vtable.
- [ADR-019](adr/ADR-019-vendor-plugin-aux-boundary.md) — the runtime DLL carries zero vendor identifiers (CI-enforced).
- [ADR-020](adr/ADR-020-plugin-abi-compatibility-policy.md) — how the plug-in ABI stays compatible across versions.
- [ADR-025](adr/ADR-025-android-vendor-dp-out-of-process.md) — on Android the DP runs out-of-process.

**"How does mixed 2D/3D content and multi-display work?"**
- [ADR-027](adr/ADR-027-display-zones.md) + [XR_DXR_display_zones](specs/extensions/XR_DXR_display_zones.md) — the current model for laying out N 3D zones + 2D zones on one panel; [ADR-031](adr/ADR-031-remove-surround-output-rect-zones-sole-region-model.md) made zones the sole region paradigm.
- [ADR-028](adr/ADR-028-display-mode-recipe-vs-hardware-state.md) — 2D⇄3D is two independent signals (panel hardware state vs. content mode).
- [ADR-015](adr/ADR-015-displayxr-owns-multi-display-vendor-routing.md) — how multiple display vendors coexist on one machine.
- [ADR-021](adr/ADR-021-color-management-encoding-state-invariant.md) — the color/encoding-state invariant that keeps pixels faithful end-to-end.

The remaining ADRs (002, 006, 010–014, 016–018, 023, 026, 029, 030) cover IPC, legacy-app scaling, the shell/workspace contracts, capture, and orientation handling — all indexed in [`README.md`](README.md#architecture-decision-records).

---

## 6. The repository map — what lives where

DisplayXR is not one repo; it's an ecosystem with a deliberately thin, neutral core and separately-owned satellites.

| Repo | Contents | Why separate |
|---|---|---|
| **displayxr-runtime** *(this repo)* | The runtime, all native compositors, the OpenXR state tracker, the plug-in ABI, `sim_display` (a hardware-free reference vendor), test apps, CI | The neutral core |
| **displayxr-extensions** | The OpenXR extension headers, auto-synced from this repo | So third parties can build against the extensions without cloning the runtime |
| **displayxr-leia-plugin** | Leia's display processor (the weaver) — built entirely on Leia's *public* SR SDK | Vendor code stays out of the neutral core ([ADR-019](adr/ADR-019-vendor-plugin-aux-boundary.md)) |
| **displayxr-mcp** | The MCP (agent-control) framework | Reusable beyond the runtime |
| **displayxr-shell-pvt / -releases** | The spatial desktop shell (a *workspace controller*, not part of the runtime) | The runtime owns no specific desktop; controllers register against a contract |
| **displayxr-installer** | The meta-installer bundle | Packaging concern |
| **displayxr-demo-\*** | Standalone demo apps (splat viewer, model viewer, media player, avatar, earth view) | Independent evolution |
| **displayxr-website** | displayxr.org landing/docs front-end | Public identity |

The key structural point, visible in this table: **the runtime holds the neutral contract; each vendor's proprietary technology lives in its own plug-in repo, behind that vendor's own public SDK.** `sim_display` in this repo proves the runtime works with no proprietary code at all.

---

## 7. What is *not* in DisplayXR (the proprietary boundary)

Because this comes up constantly, stated plainly: the things that differentiate a Leia display — **view weaving / anti-crosstalk, calibration systems, predictive eye/head tracking** — are **not in DisplayXR**. They live in the LeiaSR engine, reached only through Leia's public SR SDK, and are loaded from whatever LeiaSR runtime is already installed on the device. DisplayXR ships no vendor runtime; when none is present it falls back to `sim_display`'s simulated preview. The boundary is structural: the neutral contract is in the runtime, vendor code is in the plug-in, proprietary internals stay behind the vendor SDK. See [separation-of-concerns](architecture/separation-of-concerns.md) for the authoritative layer rules.

---

## 8. Maturity — what's solid, what's genuinely not done

**Solid and shipping:** native compositors for all five graphics APIs across Windows/macOS/Android; the extension set and vendor plug-in model; the app-authoring system ([displayxr-app-rules](guides/displayxr-app-rules.md) with enforceable invariants + a linter + a scaffolder); CI on every platform with a headless self-test gate that exercises plug-in discovery, the display-processor path, and ABI on every PR; versioned release automation with an ABI gate.

**Genuinely not done yet** (the honest gaps, in priority order):
1. **Khronos CTS conformance** — today there's a headless smoke test, not full OpenXR conformance. This is the one that matters most for standard credibility. Tracked in [`roadmap/cts-windows-handoff.md`](roadmap/cts-windows-handoff.md).
2. **Independent spec-to-code verification** — the ADRs and specs capture design intent authoritatively; a systematic audit that the implementation matches them is worth doing.
3. **Broader maintainership / formal QA** — test plans and shared ownership so continuity doesn't rest on any single contributor.

Note what is *not* on that list: design docs and specs. They exist and are extensive (this document is the map to them). The historical gap was discoverability — no single front-to-back read — which is exactly what this file closes.

---

## 9. Where to go next

- **Build and run it:** [getting-started/building.md](getting-started/building.md) → [getting-started/first-handle-app.md](getting-started/first-handle-app.md)
- **Understand the layers in depth:** [architecture/separation-of-concerns.md](architecture/separation-of-concerns.md) (authoritative), then [architecture/compositor-pipeline.md](architecture/compositor-pipeline.md)
- **Write an app:** [guides/displayxr-app-rules.md](guides/displayxr-app-rules.md) + `scripts/check_displayxr_app.py`
- **Integrate a display:** [guides/vendor-plugin-onboarding.md](guides/vendor-plugin-onboarding.md) + [reference/xrt_plugin_iface.md](reference/xrt_plugin_iface.md)
- **Everything else, by role:** [README.md](README.md)
