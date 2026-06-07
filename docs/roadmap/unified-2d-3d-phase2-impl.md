# Phase 2 — generalize the weave region: an active mask supersedes the canvas rect

**Branch suggestion:** `feature/unified-2d-3d-phase2`, off `main` post-Phase-1.
**Spec:** [`unified-2d-3d-compositing.md`](unified-2d-3d-compositing.md) §6 (Phase 2), §9 Q5 (full-window first, bbox under profiling). Epic #439.
**Builds on:** Phase 1 (`XR_EXT_local_3d_zone` oxr layer + D3D11 consumer, merged in PR #469; window-clamped per #464).

> **STATUS: implemented + hardware-validated (2026-06-06).** `d3d11_effective_canvas()` applied at all 8 sites; §5 matrix green on Leia hardware — no-mask leg statistically identical to a Phase-1 `main` build (DP weave geometry byte-identical, pixel diff within the same-runtime noise envelope, beyond-window region diff 0), mask-active weave flips to `canvas=(0,0,win)` with window-derived view dims, Tier-2 islands beyond the old canvas show real weave on-glass (Phase 1 showed stale 2D there), destroy snaps geometry back, drag-resize tracks the client rect per frame, `selftest` passes. Note: the window-spanning 3D is upscaled from the app's canvas-sized swapchain (app didn't re-render; expected — view dims are the runtime side only). Windows-only — D3D11 compositor + Leia validation; no oxr, no DP-interface, no macOS-verifiable code.

---

## 1. The problem Phase 2 solves

Phase 1 lerps the authored mask over whatever is in the target — but the DP only **weaves the canvas rect**. An M=1 (3D) pixel *outside* the canvas lerps toward pixels that were never weaved (stale/2D content), so disconnected 3D islands beyond the canvas don't actually show 3D. Phase 2 makes the weave cover every pixel the mask can select: **when a mask is active, weave the full client window** (spec §9 Q5: full window first; mask-bbox is a profiling-gated later optimization — disconnected islands tend to produce a near-full-window bbox anyway).

## 2. The design decision: mask ⇒ canvas superseded

Everywhere in the D3D11 frame path, `canvas.valid == false` already means "full window" — the fallbacks all exist and are correct. So Phase 2 is **not** new geometry plumbing; it is one rule applied uniformly:

> **While `c->active_zone_mask != nullptr`, the effective canvas is the client-window rect `{0, 0, win_w, win_h}` (top-left anchored, per the #464 window clamp), regardless of any `xrSetSharedTextureOutputRectEXT` call.**

Consequences (all intended, per spec §6/§7):

- **The 3D layer becomes window-spanning** while a mask is active: view dims re-sync to window-derived dims through the existing per-frame mode-sync, and the Kooima/FOV metrics follow. The mask is a window-space *region selector* over a window-spanning 3D scene — region selection moves entirely to the mask, exactly the §7 migration story ("today's output-rect + surround becomes the Tier-2 single-rect path").
- **Output rect is ignored, not an error**, while a mask is active; it snaps back when the mask is destroyed (`active_zone_mask` cleared → next frame's sync restores canvas-derived dims). Document this in the extension header comment for `xrSubmitLocal3DZoneEXT`.
- **Visual re-baseline for `texture` apps that set both** (e.g. `cube_texture_d3d11_win`'s 'Z' cycle): the Phase-1 "Tier-2 single rect == canvas rect == Phase-0 output" equivalence **changes meaning** — the rect now crops a window-spanning weave instead of framing a canvas-fit scene. That is the correct end-state semantics; the §6 validation matrix below re-baselines it. The **no-mask** path must remain byte-identical (zero regression — same bar as Phases 0/1).

Why supersede rather than union/honor both: the spec feeds the weave bbox *as* `canvas_offset/size` (§6), and a mask user has no second region mechanism to coordinate with — two live region authorities (rect + mask) would need a precedence rule anyway; "mask wins totally" is the only one that doesn't create hybrid states impossible to validate.

## 3. Implementation — one effective-canvas helper, applied at every canvas read site

Add a small helper in `comp_d3d11_compositor.cpp`:

```c
// #439 Phase 2: an active zone mask supersedes the canvas output rect —
// the weave region, view dims, Kooima metrics, and composite region all
// become the client-window rect (top-left anchored per #464).
static struct u_canvas_rect
d3d11_effective_canvas(struct comp_d3d11_compositor *c)
{
	if (c->active_zone_mask == nullptr) {
		return c->canvas;
	}
	struct u_canvas_rect win = {};
	HWND wnd = c->hwnd != nullptr ? c->hwnd : c->app_hwnd;
	RECT r;
	if (wnd != nullptr && GetClientRect(wnd, &r) && r.right > 0 && r.bottom > 0) {
		win.valid = true;
		win.x = 0;
		win.y = 0;
		win.w = (uint32_t)r.right;
		win.h = (uint32_t)r.bottom;
		return win;
	}
	return win; // invalid → existing full-target fallbacks
}
```

Returning a *valid window rect* (not just "invalid") matters for the texture path: the shared texture is display-sized worst-case, so "no canvas" there would fall back to `settings.preferred` (display dims) — the window rect keeps the #464 clamp. For handle/DXGI apps, target == window, so the two are equivalent.

Replace `c->canvas` with the effective canvas at **every frame-path read site** (anchors on `main` @ `d3cb3a14d`, all in `comp_d3d11_compositor.cpp` unless noted):

| # | Site | Lines | What it controls |
|---|---|---|---|
| 1 | Target-dims fallback (shared-texture mode) | :1347–1349 | `tgt_width/height` for mono viewport sizing |
| 2 | Mode-aware view-dim sync (`u_tiling_compute_canvas_view`) | :1364–1366 | **view dims** — this is what makes the 3D layer window-spanning |
| 3 | `process_atlas` canvas params, shared-texture path | :1607–1610 | **the weave region** (the headline change) |
| 4 | Surround-composite region args, shared-texture path | :1625–1633 | Phase-0/1 surround path region (only reached when no mask — still convert for consistency) |
| 5 | `process_atlas` canvas params, DXGI path | :1692–1695 | weave region, windowed path |
| 6 | Surround-composite region args, DXGI path | :1710–1718 | as #4 |
| 7 | Zone-composite region inside `d3d11_composite_zone_mask` | :2881–2884 | the masked-composite viewport → full window |
| 8 | `u_canvas_apply_to_metrics` in `get_window_metrics` | :3262 | **Kooima/adaptive-FOV metrics** — must follow the weave region or projection and weave diverge |

Notes:
- Compute the effective canvas **once per frame** under `c->mutex` (same lock `zone_mask_submit` takes — :3087) and thread it; `active_zone_mask` must not flip between the `process_atlas` call and the composite (extends the Phase-1 §9-Q3 atomicity to the weave region).
- Sites 4/6 are the no-mask surround path — when a mask is active they're not reached (`d3d11_composite_zone_mask` returns true first, :1622/:1707), so converting them is a no-op; do it anyway so there is exactly one canvas authority.
- Site 8: simplest is to make `comp_d3d11_compositor_get_window_metrics` apply `u_canvas_apply_to_metrics` with the effective canvas; when the mask is active the canvas equals the window so the helper no-ops by construction (`u_canvas.h:90` early-out also fires if you pass the invalid rect for handle apps).
- **No DP change** (`process_atlas` already takes the canvas params; Leia DP honors them per #85, sim-display ignores them per ADR-025 TODO). **No oxr change. No IPC change** (zone masks are in-process D3D11 only in Phase 1–2).
- Check the stale-sounding comment at :144 ("hidden weaver window is positioned to match this sub-rect") — the hidden-window approach was removed with #85; fix the comment while there.

## 4. Edge cases to handle

1. **Mask created but never submitted** — `active_zone_mask` is set by *submit* (sticky last-submit-wins). Until first submit, canvas behavior is unchanged. Keep it that way (authoring a mask must not change geometry until it's staged).
2. **Mask destroyed mid-session** — destroy hook clears `active_zone_mask` (:151–153 comment); next frame's sync restores canvas-derived view dims. Verify the resize path handles shrink-back cleanly (same machinery as a 2D/3D mode switch).
3. **Window resize while mask active** — effective canvas is recomputed from `GetClientRect` each frame; view-dim sync follows. Same as today's handle-app behavior.
4. **Tier-2 rects beyond the window** — already clamped by #464; the widened weave doesn't change that (mask texture is window-sized).
5. **Legacy apps** — `legacy_app_tile_scaling` skips the mode sync (:1356); legacy apps can't enable the extension anyway (extension apps only). No interaction.

## 5. Validation (Windows / Leia, capture probe + on-glass)

Reuse the Phase-1 harness (`cube_texture_d3d11_win` 'Z' cycle + `DISPLAYXR_SURROUND_CAPTURE` probe):

1. **No mask → byte-identical to Phase 1** (zero regression; canvas rect honored, diff 0). The non-negotiable.
2. **Mask active, Tier-2 island *outside* the old canvas rect** → island shows correctly weaved 3D (the Phase-2 win; in Phase 1 this lerped toward unweaved pixels). On-glass eyeball for interlace correctness at the island.
3. **Tier-1 whole-window 3D** → weave spans the full window with window-derived view dims; compare against the same app with *no output rect and no mask* (should match — both are full-window weave).
4. **Re-baseline: Tier-2 single rect == canvas rect** → now a window-spanning weave cropped to the rect (document the new golden; explicitly *not* the Phase-0/1 canvas-fit output).
5. **Transitions** — submit→destroy→submit cycling ('Z' wrap-around): view dims resize cleanly both directions, no flicker/crash, canvas restored after destroy.
6. **Window resize during mask-active** — drag-resize; weave + mask + 2D stay aligned (coordinate contract, spec §5.1).
7. `displayxr-cli selftest` still passes.

## 6. Done-when

- [x] Active mask ⇒ weave region, view dims, Kooima metrics, and composite region are all the client-window rect (one effective-canvas authority, all 8 sites). DP log: `canvas=(320,180 640x360)` → `canvas=(0,0 1280x720)`, `view=320x180` → `640x360` on submit; reverse on destroy.
- [x] No-mask path byte-identical to Phase 1 (the helper returns `c->canvas` verbatim with no mask; capture A/B vs a Phase-1 `main` build: weave geometry identical, beyond-window diff 0, window-region diff within the same-runtime capture noise — strict diff-0 is unattainable across runs because the app's surround + cube animate).
- [x] Tier-2 island beyond the old canvas shows real weave (capture: island region 100% changed vs noise-level on Phase 1; interlace eyeballed good on-glass). Re-baselined golden: Tier-2 single rect now crops a window-spanning weave (82% changed inside the rect vs the Phase-0/1 canvas-fit output) — intended end-state semantics per §2.
- [x] Mask destroy restores output-rect behavior (capture matches pre-mask frame within noise; geometry snap-back logged).
- [x] Comment fixes (:144) + extension-header note (output rect superseded while a mask is active).
- [ ] Epic #439 Phase-2 checkbox ticked; this doc's STATUS updated.
