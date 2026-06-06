# Unified 2D/3D Compositing — One Mechanism, Two Axes, One Mask

**Status:** design draft (June 2026). Pre-implementation.
**Scope:** the in-process native compositors (D3D11/D3D12/GL/VK on Windows first; Metal/macOS tracks behind). Collapses the `handle` / `texture` app classes into one pipeline with orthogonal options, generalizes the rectangular 2D-surround into an arbitrary alpha mask, and unifies that mask with the hardware-zone mask of [#224 local-3d-zones](local-3d-zones.md).
**Audience:** DisplayXR runtime contributors; extension authors; engine-plugin and demo owners (the D3D12 + VK consumers — see §8).
**Supersedes (conceptually):** the app-class taxonomy in [app-classes.md](../getting-started/app-classes.md) and the rect-only model of [surround-2d-rollout.md](surround-2d-rollout.md), without breaking either.

---

## 1. Thesis

Today DisplayXR exposes three in-process "app classes" — `handle`, `texture`, `hosted` — described as if they were three mechanisms. They are not. There is **one** in-process pipeline:

```
3D layers ──(per-tile atlas)──> WEAVE ──> interlaced output over a rect
                                            │
2D content ─────────────────────────────── COMPOSITE (post-weave, masked) ──> SURFACE ──> present
```

and the "classes" are two **orthogonal options** bolted onto it:

| | **surface = app window** (runtime presents) | **surface = app shared-texture** (app presents) | **surface = runtime window** |
|---|---|---|---|
| **2D region = ∅ (all 3D)** | `handle` (today) | — | `hosted` (today) |
| **2D region = rect complement** | — | `texture` + surround (today) | — |
| **2D region = arbitrary alpha mask** | *the goal* | *the goal* | *the goal* |

The unification: collapse `class` into **{who presents} × {2D/3D region}**, and generalize the region from "nothing / a rectangle" to "an arbitrary premultiplied-alpha mask." The existing rectangular 2D-surround (`d3d11_blit_surround_strips`, [surround-2d-rollout.md](surround-2d-rollout.md)) is **literally the rectangular special case** of the general operation.

### 1.1 The two axes are independent — keep them that way

- **Axis 1 — who presents (surface ownership).** Runtime-presented (today's `handle`): the runtime owns the swapchain on the app's HWND and calls `Present`. App-presented (today's `texture`): the runtime weaves into an app-owned shared texture and stays out of the present path; the app composites/presents.
- **Axis 2 — region (the mask).** All-3D, or an arbitrary alpha mask selecting 2D-vs-3D per pixel. Available to **both** present modes.

Once `handle + mask` exists, the *layout* reason most apps reach for `texture` disappears; `texture` survives only for its orthogonal property — **the app owns the present** (engine/browser with its own swapchain, pure offscreen capture/streaming, feeding weaved pixels into a larger scene). `texture + mask` is a real, supported combination. See §7.

**Design rule (non-negotiable):** the weave's destination is a **parameter**, never a forced offscreen intermediate. Runtime-presented + all-3D must remain byte-for-byte today's zero-copy fast path (weave straight into the swapchain back buffer → present). The mask cost and the surface cost must never compound into "always render offscreen then copy."

---

## 2. The single mask, and its (up to three) consumers

The app authors **one alpha mask** — "these regions are 3D" — and it can drive up to three consumers that **must agree** or the display is wrong:

1. **Compositor (software).** Weave vs alpha-composite-2D. Active only when the runtime owns the composite (`handle + mask`, or `texture + mask` where the runtime composites). ← *this spec*
2. **Hardware DP (firmware).** Threshold the alpha → switchable-lens cell state, so the lens over a 3D region is in 3D mode and the lens over a 2D region is flat. Active on switchable-zone hardware. ← *[local-3d-zones.md](local-3d-zones.md)*
3. **The app itself.** If the app owns its own 2D composition (the original local-3d-zones model: app pre-weaves and composites 2D in its own window), consumer #1 happens in app-space rather than runtime-space.

**The agreement invariant:** weaved pixels need a 3D lens cell over them; flat-2D pixels need a flat cell. Driving consumers #1 and #2 from the *same* authored mask is what guarantees this for free. A weaved pixel under a flat lens reads as interlace garbage; a flat pixel under a 3D lens reads as soft/ghosted. **Do not let the two masks diverge** — they are one artifact with two readers.

This is the larger unification: this spec contributes the **compositor-consumer leg**; local-3d-zones contributes the **hardware-consumer leg**; the **mask-authoring API is shared** (§5).

---

## 3. Correctness — why "weave first, then mask" is the only phase-safe order

Interlace phase is tied to **absolute screen pixel position** (the weaver writes L/R columns aligned to physical lens/barrier geometry). Therefore:

> **Weave the full bounding rect first (phase is globally correct everywhere in it), then overwrite with 2D wherever the mask says 2D.**

The mask never touches the weave, so it can never break phase. A disconnected / non-rectangular 3D region is no harder than one rectangle: weave the whole bounding box, and the 2D composite paints over the gaps. The only cost is weaving some pixels that 2D later covers — bounded, and optimizable later by weaving the mask's bounding box instead of the full window (§6, Phase 2).

Two preserved properties:

- **2D is full-resolution.** Because the 2D composite is *post-weave* at native resolution, 2D regions are not halved by interlacing. This is exactly why today's surround is post-weave; it carries over unchanged.
- **Phase is canvas-parameterized already.** `canvas_offset/size` already feeds DP phase + Kooima projection (`xrt_display_processor_*_process_atlas`). Feeding it the weave bounding box instead of a hand-set rect composes with no new math.

---

## 4. The composite — alpha is region + blend + content-transparency, all at once

The 2D layer is **RGBA, premultiplied, and its alpha *is* the mask.** There is no separate binary-mask concept. The composite is one in-place blended fullscreen pass on the weave target:

```
final.rgb = lerp(weaved.rgb, layer2D.rgb, layer2D.a)   // premultiplied: final.rgb = layer2D.rgb + weaved.rgb*(1 - layer2D.a)
```

- `a = 0` → pure weave (3D). `a = 1` → pure 2D. Binary mask = hard 0/1 alpha — nothing lost.
- One resource, not two. The app renders its 2D UI with a transparent background; transparency *is* "show 3D here." AA text edges and PNG-style cutouts just work, giving **free anti-aliased 2D/3D boundaries**.
- Partial alpha unifies "region select" and "translucent 2D panel over 3D" into one op.

### 4.1 In-place blend, not a copy

The composite binds the weave target (swapchain back buffer / shared texture / runtime window) as RTV and draws one fullscreen quad sampling the 2D layer, letting the blend unit read the weaved pixels as `dst`. No read-back surface, no extra allocation. Mask cost = **one blended fullscreen quad**, bandwidth-bound on the 2D-covered pixels only. When the mask is absent (all-3D), the stage is **skipped** — runtime-presented apps pay nothing.

### 4.2 Output-alpha rule — protect the compose-under-bg path (issue #225)

The weaved target's **alpha channel is load-bearing.** Per `comp_d3d11_renderer.cpp:466–473`, the layered composite uses Porter-Duff "over" specifically so `dst.a` survives, because the Leia DP **compose-under-bg** pass lerps the captured desktop under `atlas.a` (transparency / WGC background path, [#225](https://github.com/DisplayXR/displayxr-runtime/issues/225)). The 2D composite therefore must **define what it writes to alpha**, not just RGB:

```
final.a = layer2D.a + weaved.a * (1 - layer2D.a)   // same "over" rule; never clobber dst.a with src.a
```

Getting RGB right but alpha wrong does **not** corrupt the image — it silently regresses **window transparency**, which is far harder to catch in review. This rule must be pinned in Phase 0 and asserted by a capture diff against a known-transparent scene.

### 4.3 Premultiplied, and forward-compatible with OpenXR layers

Use **premultiplied** alpha for the 2D layer to avoid edge fringing along the 2D/3D boundary. When the 2D side graduates from a side-channel texture to a real composition layer (§6, Phase 3), OpenXR already models this via `XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT` — the layer carries its own alpha-mode flag, so we inherit a specified contract instead of inventing one.

---

## 5. Mask-authoring API — adopt `XR_EXT_local_3d_zone`, add a second consumer

**Do not invent a second mask API.** The tiered authoring surface designed in [local-3d-zones.md](local-3d-zones.md) (`XR_EXT_local_3d_zone`) is exactly what the compositor consumer needs:

- **Tier 1 — whole-window.** `xrSetLocal3DZoneWholeWindowEXT(mask, enable)`. The degenerate all-3D / all-2D case; one call at startup, zero per-frame work.
- **Tier 2 — rect list.** `xrSetLocal3DZoneFromRectsEXT(mask, count, rects)`. Runtime rasterizes rects into the mask texture. **This is the strict generalization of today's canvas sub-rect + surround** — a single rect reproduces current `texture` behavior.
- **Tier 3 — freeform render target.** `xrAcquireLocal3DZoneRenderTargetEXT(mask, &binding)`. App draws arbitrary alpha into the mask each frame via the API-typed sibling binding.

The wire primitive in all tiers is one shared GPU alpha texture in **client-window pixel space**. The only delta this spec adds: that same mask now also routes to the **compositor composite** (§4), not only to the DP hardware publish (local-3d-zones §"DP vtable contract"). The mask object grows from "one consumer (DP)" to "fan-out to up-to-two runtime-side consumers (composite + DP) from one authored source."

> Migration note for `texture` apps: today's `xrSetSharedTextureOutputRectEXT` + `xrSetSharedTextureSurround2DEXT[Fence]` becomes the **Tier-2 single-rect** path. Both surfaces are kept working in parallel during Phases 0–2 (§6); the surround entry points are not removed until the layer path (Phase 3) lands and apps migrate.

### 5.1 Coordinate-space contract (pin in Phase 0)

The mask, the 2D layer, the weave target, and the physical screen rect must align 1:1. local-3d-zones already defines "client-window pixels → physical screen rect (post-DPI)." This spec must confirm the **compositor** composite lands in that same space, especially after the windowed D3D11 **DP-crop / DPI** work in [#431](https://github.com/DisplayXR/displayxr-runtime/issues/431) (`903cfffa5`). Concretely: the space in which `canvas_offset/size` is expressed to `process_atlas` is the space the mask and 2D layer must be authored/sampled in. Write this contract down once; every API implementation references it.

---

## 6. Implementation phasing — additive, non-regressing

**Non-regression guarantee:** current apps never opt into the mask, so they ride the unchanged paths byte-for-byte. Each phase is independently shippable.

### Phase 0 — Refactor rect-surround into a general masked composite (no API change). *D3D11.*
Re-express `d3d11_blit_surround_strips` internals as the in-place premultiplied-alpha composite of §4, but keep **deriving the mask from the canvas rect** (inside → keep weave, outside → 2D surround texture). Output must be **pixel-identical** to today's strip blit — validate by diffing atlas/post-compose captures (`MCP_CAPTURE_MODE_POST_COMPOSE`). Pin the §4.2 output-alpha rule and the §5.1 coordinate contract here. Pure refactor, zero behavioral change, zero regression surface.

### Phase 1 — Add the opt-in alpha mask behind `XR_EXT_local_3d_zone` (Tiers 1–3). *D3D11.*
Wire the mask object's compositor-consumer leg: when an app supplies a mask, the composite uses it; when it doesn't, fall back to the Phase-0 rect-derived mask. Regression surface = zero (new path only reachable on explicit opt-in). This already unlocks arbitrary 2D/3D regions on `handle` apps (`handle + mask`).

### Phase 2 — Generalize the weave region. *D3D11.*
Today weave region = canvas rect. With a mask present, weave the **bounding box of the mask's 3D pixels** (start simply: full window when a mask is present — correct, mildly wasteful; tighten to the mask bbox under profiling). Feed that bbox as `canvas_offset/size` to `process_atlas` (§3). Disconnected 3D islands fall out for free.

### Phase 3 — 2D as a first-class composition layer (the real "one mechanism with options"). *D3D11, then cross-API.*
Replace the shared-texture surround side-channel with the app submitting a **post-weave 2D screen-space layer (+ its alpha)** through the normal `xrEndFrame` layer list, alongside its stereo/projection layers. The compositor's `layer_accum` gains a third routing bucket — *post-weave masked 2D* — beside the existing projection-pass and window-space-pass buckets. This is the cross-cutting lift: it touches the oxr state tracker (layer validation/handling), the IPC proto (new layer type serialization — see [proto codegen](../../) cascade), and every API's compositor. Do it last, behind the proven Phases 0–2.

> Note: today's **window-space layers** (composited *pre*-weave, into the atlas, at zero disparity → flat at the screen plane) are a *different* mechanism and stay as-is. The new bucket is *post*-weave, full-resolution, mask-gated. Three buckets total: pre-weave projection, pre-weave window-space (zero-disparity flat), post-weave masked 2D.

---

## 7. What survives of `texture` once `handle + mask` ships

`handle + mask` subsumes texture's **layout** role (2D chrome around 3D), at lower cost (zero-copy, no shared-resource sync). `texture` survives only for its orthogonal **present-ownership** role:

1. **App owns its own compositor/engine/swapchain** (browser, game engine, Electron/Qt) — wants the weaved 3D as one input texture it blends and presents itself.
2. **Pure offscreen / headless** — recording, streaming, encoding, feeding weaved pixels into a larger 3D scene or a WebXR/browser texture; no normal window present at all.
3. **App needs the final pixels in hand** — its own color grade/overlay as real code, or present decoupled from the runtime's cadence.

`texture + mask` is supported (app owns present **and** arbitrary regions — the browser/engine case). The honest end-state taxonomy renames the axis from class to capability: **runtime-presented vs app-presented**, with the mask orthogonal and available to both. Each path then justified by exactly one thing, no overlap.

---

## 8. Cross-API rollout priority

D3D11 is Phases 0–2's pathfinder (most complete; cheapest to iterate). **D3D12, VK, and GL on Windows must follow shortly after — they are not afterthoughts:**

- **D3D12 — all engine plugins.** Unity/Unreal integrations render through the D3D12 native compositor. The mask composite must reach D3D12 (fence-synced, mirroring the surround D3D12 fence path already shipping per [surround-2d-rollout.md](surround-2d-rollout.md)) for any engine-plugin app to use local 2D/3D.
- **VK — all demos.** Every DisplayXR demo (`displayxr-demo-*`, the Gaussian-splat / media-player line) is Vulkan. The mask composite on `vk_native` gates the demos adopting the feature.
- **GL — Windows parity.** Round out the Windows matrix.

Recommended ordering once D3D11 Phases 0–2 are proven: **D3D12 and VK in parallel** (they unblock engine plugins and demos respectively), **GL behind**. Metal/macOS tracks last. Each API needs: the in-place premultiplied-alpha composite pass (§4), the §4.2 output-alpha rule, and the §5.1 coordinate contract — all referencing the single written-down contract, not re-derived per API.

Per-API sync primitive for the shared 2D source (when app-presented): D3D11 keyed-mutex, D3D12 fence, VK external-memory + timeline semaphore, GL via the chosen interop. Same shape as the existing surround paths.

---

## 9. Open questions

1. **Mask resolution defaults** — inherit local-3d-zones' suggestion (`window/8` clamped to `[16² .. 1024²]`)? The *compositor* consumer may want higher resolution than the *hardware* consumer (soft AA edges vs coarse lens cells). Allow the composite to sample the mask at full layer resolution while the DP publish downsamples? (Leaning yes — they are different readers of the same authored source.)
2. **Large translucent 2D over 3D** — weaved pixels showing through a big semi-transparent region carry visible L/R interlace structure. Fine for AA edges and opaque panels; a real artifact for large translucent overlays. Document as a known limitation, or offer an app hint to force-flatten (re-weave at zero disparity) under translucent regions?
3. **Mask ↔ layer coherence** — when the 2D layer and the mask are authored separately (Tier 3 freeform), must they be submitted atomically per frame to avoid a one-frame mismatch? Likely yes; define the submission barrier.
4. **Capture pipeline** — `POST_COMPOSE` capture now includes masked 2D; `PROJECTION_ONLY` stays pre-2D. Confirm both capture intents still mean what their consumers (logos, debugging, atlas tooling) expect.
5. **Phase-2 weave-bbox vs full-window** — is the wasted weave under large 2D regions ever measurable enough to justify the bbox optimization, or is full-window weave permanently fine on target GPUs?

---

## 10. Cross-references

- [local-3d-zones.md](local-3d-zones.md) — the hardware-consumer leg of the shared mask; `XR_EXT_local_3d_zone` authoring API (#224).
- [surround-2d-rollout.md](surround-2d-rollout.md) — the rectangular special case this generalizes; per-API sync-primitive precedent (#225-adjacent).
- [app-classes.md](../getting-started/app-classes.md) — the taxonomy this reframes as present-ownership × region.
- `comp_d3d11_compositor.cpp` — weave-target-as-parameter: offscreen shared-texture path (`:1521`), windowed path (`:1581`), rect surround (`d3d11_blit_surround_strips`), DP-crop (#431, `903cfffa5`).
- `comp_d3d11_renderer.cpp:466–473` — the `dst.a` / compose-under-bg "over" rule the §4.2 output-alpha rule must honor (#225).
- [compositor-pipeline.md](../architecture/compositor-pipeline.md) — projection-pass / window-space-pass buckets the Phase-3 post-weave-2D bucket joins.
- [ADR-007](../adr/ADR-007-compositor-never-weaves.md) — compositor never weaves (DP does); the masked composite is post-DP and does not violate this.
