# Phase 1 — opt-in authored mask via `XR_EXT_local_3d_zone` (compositor-consumer leg)

**Branch:** `feature/unified-2d-3d-phase1` (worktree `.claude/worktrees/unified-2d3d-p1`), off `main` post-Phase-0.
**Spec:** [`unified-2d-3d-compositing.md`](unified-2d-3d-compositing.md) §5, §6 (Phase 1); the authoring API is from [`local-3d-zones.md`](local-3d-zones.md). Epic #439.
**Builds on:** Phase 0 (the masked-composite shader, validated max-diff-0 on Leia hardware). The shader **already carries the Phase 1 path** (`use_rect_mask = 0` → sample `mask_tex` t1, lerp against `weave_tex` t2). Phase 1 lights it up.

> **STATUS: drafting.** This doc + the extension header are the first deliverables; oxr handlers + compositor wiring follow once the contract is reviewed.

---

## 1. Scope — and the one thing it is NOT

Phase 1 = **generalize the post-weave 2D region from a rectangle to an arbitrary authored mask**, opt-in, with the existing 2D source (the surround texture). Concretely:

- New extension **`XR_EXT_local_3d_zone`** (the tiered mask-authoring API, shared with the hardware leg — §5 of the spec). oxr mask object + handlers.
- The authored scalar mask flows to the **D3D11 compositor consumer** (the Phase-0 shader's `use_rect_mask = 0` path).
- **Fallback:** no mask → Phase-0 rect-derived behavior, byte-for-byte. Zero regression (the env/opt-in gate stays).

**NOT in Phase 1** (each deferred deliberately):
- **The hardware DP-publish leg** — local-3d-zones' DP vtable / firmware zone switching. Phase 1 is the *compositor* consumer only.
- **Occlusion / Z-order / multi-app** — that's the hardware multi-client path in local-3d-zones.
- **D3D12 / VK / GL** — follow per spec §8 (D3D12 = engine plugins, VK = demos).
- **Full `handle + mask`** — see §2: a handle app has no 2D content source yet; that needs the Phase-3 composition layer. Phase 1's reachable win is **`texture + arbitrary mask`**.

## 2. The scoping reality: whose 2D content?

`handle + mask` is the eventual prize, but a **handle app supplies no 2D content** (no surround texture — that's a texture-app feature). The mask only *selects* regions; the 2D *pixels* still come from somewhere. In Phase 1 that somewhere is the **surround texture**, which only texture apps provide. So:

- **Phase 1 delivers `texture + arbitrary mask`:** the surround supplies the 2D pixels; the authored mask replaces the rect-derived region selection. This is the honest generalization of Phase 0 (rect → arbitrary).
- **`handle + mask` waits for Phase 3's 2D composition layer** (the app submits a 2D RGBA layer through `xrEndFrame`), which becomes the `twod` source for handle apps.

Stating this in the spec avoids over-promising Phase 1.

## 3. D3D11 consumer — three SRV inputs, and the weave goes readable

Phase 0's hard mask used `discard` (no weave read). An **authored mask can be soft** (anti-aliased edges, partial translucency), so Phase 1 must run the real **mask-lerp**, which reads the weave:

```
final.rgb = M·weave.rgb + (1−M)·twod.rgb        // (and .a — §4.2 output-alpha rule)
```

So the composite pass binds **three** SRVs (the shader already declares them):

| Slot | Phase 1 source |
|---|---|
| `twod_tex` (t0) | surround scratch (Phase-0 `surround_scratch`, unchanged) |
| `mask_tex` (t1) | **new** — the authored mask, copied into an SRV-capable scratch (scalar R8) |
| `weave_tex` (t2) | **new** — the weave, copied into an SRV-capable scratch (the DP wrote it into the target; RT≠SRV, so copy target→`weave_scratch` first) |

Flow per frame (extends `d3d11_composite_surround_shader`):
1. DP weaves into the target (unchanged).
2. `CopyResource` target → `weave_scratch` (SRV-capable) — the lerp's `weave` source.
3. `CopyResource` surround → `surround_scratch` (Phase 0, unchanged) — `twod`.
4. Mask already resides in `mask_scratch` (authored via the extension; §4).
5. Set `use_rect_mask = 0`; bind t0/t1/t2; `Draw(3)`; the PS lerps → writes the target.
6. No-mask path: `use_rect_mask = 1`, skip t1/t2 — identical to Phase 0.

Cost: +2 copies/frame vs Phase 0's +1 (weave + mask). Both evaporate in Phase 3 (runtime-owned layers, weave-into-SRV-RT directly). Acceptable for the mechanism; optimize under profiling (mirrors the Phase-0 / spec §6 stance).

> **sRGB note (carried from Phase 0 §4):** all views UNORM, point sampler. The lerp is in the view's numeric space — keep `weave_scratch` / `surround_scratch` UNORM so the math matches the bytes; the mask is R8_UNORM scalar.

## 4. The mask object + authoring (oxr) — `XR_EXT_local_3d_zone`

The extension (header drafted alongside this doc) defines a mask handle and three authoring tiers (from local-3d-zones §"runtime piece"):

- **Tier 1** `xrSetLocal3DZoneWholeWindowEXT(mask, enable)` — all-3D / all-2D; the degenerate case, one call.
- **Tier 2** `xrSetLocal3DZoneFromRectsEXT(mask, count, rects)` — runtime rasterizes rects into the mask texture (a single rect reproduces Phase 0).
- **Tier 3** `xrAcquireLocal3DZoneRenderTargetEXT(mask, &binding)` — app draws arbitrary alpha into the mask (API-typed binding; D3D11 first).

oxr work:
1. **Extension registration** — `XR_EXT_local_3d_zone.h`; `oxr_extension_support.h` (`OXR_HAVE_EXT_local_3d_zone` + `OXR_EXTENSION_SUPPORT_EXT_local_3d_zone` + master-list entry ~`:1089`); `oxr_api_negotiate.c` `ENTRY_IF_EXT(...)` for each function.
2. **Mask handle object** — `XrLocal3DZoneMaskEXT` backed by an oxr struct owning a runtime-allocated scalar R8 GPU texture (client-window px) + an SRV. New `oxr_local_3d_zone.c` (handlers) beside `oxr_session.c`; model the handle lifecycle on an existing oxr object.
3. **Tier rasterization** — Tiers 1/2 rasterize into the mask texture via a tiny runtime shader (or `ClearView`/rect fills); Tier 3 hands back the D3D11 RTV on the mask texture.
4. **Caps** — `xrGetLocal3DZoneCapabilitiesEXT` reports `supported = true` for the **compositor consumer** even with no hardware zones (the hardware grid fields come with the DP-publish leg later). The compositor samples the mask at full resolution (spec §9 Q1 decision).

## 5. Mask → compositor delivery + atomicity (Q3)

The mask object's SRV must reach the D3D11 compositor at composite time. Path: the session holds the active mask; `xrSetSharedTextureOutputRectEXT`-style plumbing carries the mask SRV (or its shared handle, in-process direct) to `comp_d3d11_compositor`, mirroring how `canvas` / `surround_2d` already flow. **Atomic per frame (spec §9 Q3):** the mask, the 2D layer, and the 3D layers are consumed as one `xrEndFrame` set — `xrSubmitLocal3DZoneEXT` stages the mask for the *next* frame submission, and the compositor reads the staged mask coherently with that frame's weave. Define the staging so a Tier-3 mask update and the frame can't tear.

## 6. Validation (Windows / Leia)

Reuse the Phase-0 `DISPLAYXR_SURROUND_CAPTURE` probe (POST_COMPOSE reads the atlas, **not** the surround composite — do not use it here). Cases on `cube_texture_d3d11_win`:
1. **No mask** → identical to Phase 0 (regression check; diff vs a Phase-0 capture = 0).
2. **Tier 1 whole-window 3D** → full weave, no 2D (mask M=1 everywhere).
3. **Tier 2 single rect** = the canvas rect → must match the Phase-0 rect output (cross-checks the authored-mask path against the analytic one).
4. **Tier 2 multi-rect / Tier 3 freeform** → arbitrary 2D/3D regions; eyeball on the Leia display (disconnected 3D islands, soft edges).
Confirm soft-edge AA (a Tier-3 gradient mask) shows a clean 2D↔3D blend, no interlace bleed in the 2D region (validates the separate-mask design, spec §4.0).

## 7. Done-when

- [ ] `XR_EXT_local_3d_zone` registered; `displayxr-cli selftest` still passes.
- [ ] Tiers 1–3 author a mask; the D3D11 consumer lerps it (soft edges work).
- [ ] No-mask path byte-identical to Phase 0 (zero regression).
- [ ] Tier-2-single-rect == Phase-0 rect output (authored path validated against analytic).
- [ ] `git clang-format` clean; extension header syncs to `displayxr-extensions` on merge.

## 8. Hand-off split

- **macOS (me):** extension header + spec, oxr registration + handlers + mask object + Tier 1/2 rasterization (portable C; MinGW-cross-check the portable bits).
- **Windows (agent):** the D3D11 consumer wiring (weave/mask scratch SRVs, flip `use_rect_mask`, Tier-3 RTV), and all §6 validation on Leia hardware.
