# Phase 3 — 2D as a first-class composition layer (`XrCompositionLayerLocal2DEXT`)

**Spec:** [`unified-2d-3d-compositing.md`](unified-2d-3d-compositing.md) §6 (Phase 3), §4 (composite + output-alpha), §5.1 (coordinate contract). Epic #439.
**Builds on:** D3D11 Phases 0–2 + D3D12 consumer + cross-API base (all on `main`); the `XRT_LAYER_WINDOW_SPACE` machinery as the structural sibling.

> **STATUS: macOS hub leg LANDED + validated 2026-06-07 (header v3, xrt/oxr plumbing, view-size event, all vtables wired, Metal Tier-1/2 backend + consumer; §8 cases 1–4 green on the metal test apps). Windows legs (§7) next — D3D11 consumer is the pathfinder.**

---

## 1. What Phase 3 is

Replace the shared-texture **surround side-channel** as the 2D source: the app submits a **post-weave 2D screen-space layer** through the normal `xrEndFrame` layer list, beside its projection layers. The compositor's layer accumulator gains a **third routing bucket** — *post-weave masked 2D* — beside pre-weave projection and pre-weave window-space (which stays as-is: different mechanism, zero-disparity into the atlas).

This is the unlock for: **`handle + mask`** (every demo + engine plugin finally has a 2D source), the **VK consumer** (parked on exactly this), and **crisp window-spanning 3D** (resolution renegotiation, §5).

## 2. Resolved design decisions (Q1–Q4, 2026-06-07)

| # | Decision | Consequence |
|---|---|---|
| Q1 | **API home: `XR_EXT_local_3d_zone` v3** — `XrCompositionLayerLocal2DEXT` | One extension = one feature story; header v3 auto-syncs to `displayxr-extensions` |
| Q2 | **Sub-rect quad** — layer places its swapchain at a client-window pixel rect; outside = transparent 2D contribution | Where M=0 and no 2D coverage, `final.a → 0` → the compose-under-bg transparency path shows desktop. Consistent with §4.2 by construction; full-window is the degenerate rect |
| Q3 | **Implicit mask from coverage** — no active mask ⇒ the union of Local2D layer rects implies M=0 there, M=1 elsewhere | The common case (one 2D panel over 3D) needs zero mask-API calls. An explicit mask, when submitted, takes **total** authority. The implicit mask behaves exactly like an explicit Tier-2 mask built from the layer rects — including **superseding the canvas** (uniform rule: *any* active mask supersedes; no third state) |
| Q4 | **Resolution renegotiation in scope** — `XrEventDataLocal3DZoneViewSizeChangedEXT` | When mask activation / deactivation / window resize changes the runtime's view dims, the app gets an event with the new recommended view size and recreates its projection swapchains. Closes the Phase-2 upscale caveat |

Decisions made by precedent (no open question):
- **Layer ↔ surround precedence:** Local2D layers **supersede** a registered surround texture for that frame (the established "newer authority wins totally" rule, as mask-supersedes-canvas). Surround entry points stay working for non-layer apps; deprecate after migration (spec §6 migration note).
- **Stacking:** multiple Local2D layers allowed (bounded by `XRT_MAX_LAYERS`), flattened in layer-list order (later = on top) with premultiplied-alpha *over* into the 2D scratch; `XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT` honored per layer (existing `convert_layer_flags()` path).
- **Coordinates:** integer client-window pixels (`XrRect2Di`-shaped), matching the mask tiers — NOT window-space's fractional [0..1]. One §5.1 coordinate contract for mask + 2D layers.
- **IPC:** `xrt_layer_data` serializes the new union member for free (`ipc_layer_entry` carries it); the **service/multi-compositor consumer is out of scope** for v1 — in-process native compositors first, shell-era follow-up.

## 3. API (header v3)

```c
#define XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_EXT            ((XrStructureType)1000999135)
#define XR_TYPE_EVENT_DATA_LOCAL_3D_ZONE_VIEW_SIZE_CHANGED_EXT ((XrStructureType)1000999136)

// Post-weave 2D content at a client-window pixel rect, mask-gated:
// final = M·weave + (1−M)·flatten(local2D layers). Premultiplied alpha unless
// XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT.
typedef struct XrCompositionLayerLocal2DEXT {
    XrStructureType             type;       // XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_EXT
    const void* XR_MAY_ALIAS    next;
    XrCompositionLayerFlags     layerFlags; // alpha bits honored
    XrSwapchainSubImage         subImage;   // source texture + sub-rect
    XrRect2Di                   rect;       // client-window pixels, dest
} XrCompositionLayerLocal2DEXT;

// Queued when the runtime's recommended view size changes (mask activation /
// deactivation / window resize): recreate projection swapchains at the new size.
typedef struct XrEventDataLocal3DZoneViewSizeChangedEXT {
    XrStructureType          type;
    const void* XR_MAY_ALIAS next;
    uint32_t                 recommendedImageRectWidth;
    uint32_t                 recommendedImageRectHeight;
} XrEventDataLocal3DZoneViewSizeChangedEXT;
```

`XR_EXT_local_3d_zone_SPEC_VERSION` → 3. It is valid to submit Local2D layers **without** ever creating a mask object (Q3); the extension still must be enabled.

## 4. Runtime architecture — the elegant part

The shipped zone-mask composite already does `final = M·weave + (1−M)·twod` with `twod` = the surround scratch. **Phase 3 only changes where `twod` comes from**: flatten the frame's Local2D layers into that scratch instead of copying the surround texture. The masked composite pass itself is untouched on every API that has it (D3D11, D3D12).

Per-frame (D3D11 reference; each consumer leg mirrors):
1. Accumulate `XRT_LAYER_LOCAL_2D` entries (new `comp_layer_accum` member, sibling of window-space).
2. Resolve the frame's mask: explicit staged mask if active, else rasterize the implicit Tier-2-style mask from the layer rects (reuses the Tier-2 clear-rects path on a runtime-owned mask). Either way: **mask active ⇒ effective canvas = window** (Phase-2 rule, now uniform).
3. DP weaves (full window, per 2).
4. Flatten Local2D layers (premultiplied over, list order) into the 2D scratch. If no Local2D layers this frame: legacy surround copy (unchanged path).
5. Existing masked composite lerps. HUD. Present.

## 5. View-size renegotiation (Q4)

The runtime already resizes its **internal** view dims per-frame (Phase 2's mode-sync). New: when the recommended app-facing view size changes, queue `XrEventDataLocal3DZoneViewSizeChangedEXT` (oxr event queue, model: `XrEventDataEyeTrackingStateChangedEXT` from #441). The app recreates projection swapchains; the projection pass already scales arbitrary submitted sizes, so a laggy app stays correct (just soft) — **no hard protocol step**, purely advisory. Debounce: only fire when dims actually change (resize storms → coalesce by emitting on change, the queue naturally drops dupes per the #441 pattern).

## 6. The xrt/oxr surface (anchors on `main` @ `fe7eb2ea1`)

| Piece | Where | Model |
|---|---|---|
| `XRT_LAYER_LOCAL_2D` enum + `xrt_layer_local_2d_data` (sub, dest rect) | `xrt_compositor.h:76-87`, union @ :481-491 | `xrt_layer_window_space_data` |
| `xrt_comp_layer_local_2d()` helper + vtable fn | `xrt_compositor.h` (57th method) | `xrt_comp_layer_window_space` |
| `comp_layer_accum_local_2d()` | `compositor/util/comp_layer_accum.{h,c}` | window-space sibling |
| oxr verify + submit | `oxr_session_frame_end.c` (`verify_window_space_layer` :1189, `submit_window_space_layer` :1716) | same pair |
| Event queue plumbing | `oxr_event.c` + push helper in `oxr_objects.h` | `oxr_event_push_XrEventDataEyeTrackingStateChangedEXT` (#441) |
| Vtable wiring in all native compositors + multi/client | accumulate (or ignore+WARN once for not-yet-consumer APIs) | window-space entries |
| Header v3 | `XR_EXT_local_3d_zone.h` | v2 pattern |

IPC: nothing — the union rides `ipc_layer_entry` as-is (out-of-process *consumer* deferred per §2).

## 7. Leg split + rollout order

- **macOS leg (hub, this repo’s established pattern):** header v3, xrt enum/union/vtable, `comp_layer_accum`, oxr verify/submit + event, vtable wiring everywhere, **and the Metal consumer** — fully buildable *and testable* here (`cube_texture_metal_macos` has surround #468 to A/B against; `cube_handle_metal_macos` becomes the first-ever `handle + mask + 2D layer` app). MinGW-check the portable bits.
- **Windows leg 1 — D3D11 consumer** (pathfinder): §4 steps 2/4, implicit-mask rasterization, surround-precedence, view-size event firing, full validation on Leia (matrix in §8).
- **Windows leg 2 — VK consumer**: un-parks the crossapi §4 decision; closes the epic "VK — all demos" box. Mechanism portable (developable on macOS if preferred), Leia validation on Windows.
- **Then:** D3D12 (small delta — its masked composite already shipped; only steps 2/4), GL last. Per-leg caps/feature gating: a session whose compositor lacks the consumer drops Local2D layers with a one-time WARN (no error — layers are advisory compositing).

## 8. Validation (per consumer leg)

1. **Surround A/B (zero regression + equivalence):** `cube_texture_*` submitting its surround content as a Local2D full-window layer must match the surround-path output (capture diff ≈ 0 in the window; the legacy surround path itself stays byte-identical when no layer is present).
2. **`handle + mask + layer` (the headline):** `cube_handle_*` + Tier-2 islands + a Local2D panel — 3D islands weave correctly, 2D panel crisp, desktop visible where neither covers (output-alpha rule).
3. **Implicit mask:** layer-only app (no mask calls) → 2D where the rect is, 3D elsewhere, canvas superseded.
4. **Stacking + alpha:** two overlapping layers, one unpremultiplied — order + fringing check (§4 premultiplied rationale).
5. **Resize/renegotiation:** drag-resize → event fires once per change; app recreates swapchains → 3D sharp at new size (no upscale).
6. `displayxr-cli selftest` + no-layer apps byte-identical throughout.

## 9. Done-when

- [x] Header v3 + xrt/oxr layer plumbing + event, macOS-green, Metal consumer validated locally (§8 cases on metal test apps). *(2026-06-07: case 1 surround-region byte-identical, canvas resampling epsilon mean<2 with app-side world-scale constancy; cases 2/3/4 pixel-checked incl. §4.2 alpha; view-size event fires exactly once at mask activation, silent for handle apps. Notes: Metal Tier-3 = `XR_ERROR_FEATURE_UNSUPPORTED` (no Metal binding type in v3); the event struct carries the conventional `XrSession session` field the §3 sketch omitted; hosted (framebufferOnly) drawables skip the composite with a one-time WARN.)*
- [ ] D3D11 consumer Leia-validated (§8 full matrix); surround path zero-regression.
- [ ] VK consumer landed → epic "VK — all demos" ticked; demos can adopt.
- [ ] D3D12 delta landed; GL tracked or ticked.
- [ ] Epic #439 Phase-3 box ticked; surround deprecation note added to `XR_EXT_win32_window_binding` spec (§3.6/3.7) pointing at Local2D.
