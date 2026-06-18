# Deprecating the 2D-surround / output-rect path in favour of display-zones

**Status:** In progress (deprecation + translation shim landed; removal pending migration).
**Owner:** runtime.
**Refs:** ADR-027 (display zones), `XR_EXT_display_zones`, `XR_EXT_win32_window_binding` §3.5–3.7.

> This is the working design note. Once the migration is complete and confirmed, the
> *decision* is recorded as a new ADR (supersession of the surround mechanism); this doc
> becomes that ADR's background. Do **not** rewrite ADR-027 to remove the surround — it
> added zones, it didn't decide to remove surround.

## 1. Why

There are two mechanisms in the runtime for expressing 2D vs 3D regions in a window:

| Mechanism | Expressiveness |
|---|---|
| `xrSetSharedTextureOutputRectEXT` + `xrSetSharedTextureSurround2DEXT`/`…FenceEXT` (the "2D surround") | **Exactly one** 3D rect + a monolithic full-window 2D fill, blitted 1:1, with per-API keyed-mutex / fence sync. |
| `XR_EXT_display_zones` (ADR-027) | **N** 3D zones (each rect + rig + swapchain) + **M** 2D zones (`XrCompositionLayerLocal2DEXT`) + a per-pixel wish mask fed to the DP/hardware. |

The surround is a strict, less-capable special case of display-zones:

- an **output rect** ≡ **one 3D zone** covering that rect;
- a **2D surround** ≡ **one Local2D zone** covering the complement.

The surround cannot express the product goal (arbitrarily many 2D and 3D zones + a separate
alpha mask telling the display which areas are physically 2D vs 3D) — that goal *is*
display-zones, which is already implemented (ADR-027 Phases 1–5). Keeping both is redundant
and doubles the compositor's 2D-fill code (the bespoke strip blit / surround shader per API,
plus the keyed-mutex/fence plumbing).

Parity is proven: a **texture-class** app gets the full multi-zone + Local2D composite in
its shared-texture read-back, identical to a handle app — locked in by
`cube_zones_texture_metal_macos` (built + captured on Metal) and `cube_zones_texture_d3d11_win`
(D3D11 sibling).

## 2. End state

`XR_EXT_display_zones` is the single canonical model for all 2D/3D region expression. The
output-rect + surround entry points are removed; the bespoke surround compositor code
(strip blit, surround shader, keyed-mutex/fence surround handling) is deleted. The
handle / texture / hosted **app-class** distinction is orthogonal and unaffected — it is
about window/texture ownership, not region expression.

## 3. Transition plan

1. **Deprecation markers** (done): banner on `XR_EXT_win32_window_binding` §3.5–3.7; this
   doc; header annotations on the three entry points; CLAUDE.md texture-app-layout note.
2. **Translation shim** (in progress): the runtime rewrites a legacy
   surround + output-rect frame into the synthetic *one 3D zone (the canvas) + surround as
   the 2D source* form and routes it through the **canonical unified mask composite** —
   the same path zones/Local2D use — instead of the bespoke strip blit. The DP still weaves
   into the canvas **sub-rect** (the app's atlas is canvas-sized); only the post-weave
   composite is rerouted, spanning the window with the mask M=1 inside the canvas, so the
   result is `M·weave + (1−M)·surround` — the strip-blit result via the canonical path.
   - **Gate:** `DISPLAYXR_SURROUND_SHIM`, **default OFF** → zero behavioural change; legacy
     apps keep the bespoke strips. **Opt-in ON** → shim engages (one-time
     `Surround→zones shim ACTIVE` log). This is the validation switch.
   - **Status:** Metal done + verified (`cube_texture_metal_macos`, flag-on routes through
     the unified path, flag-off unchanged). D3D11 / D3D12 mirror the same pattern.
   - **Product gate follow-up:** promote the env override to a
     `HKLM\Software\DisplayXR\Capabilities\…` registry gate (per the registry-over-env-var
     convention) before flipping the default.
3. **Flip default ON** once the shim is validated on all three backends + real hardware
   (Leia SR) — the canonical path now serves legacy apps unconditionally.
4. **Migrate first-party apps** off the surround calls to native zones submission (the
   `cube_texture_*` apps, any editor/host integrations).
5. **Delete** the bespoke surround code (strip blit, surround shader, keyed-mutex/fence
   surround handling) and the three entry points; bump the extension spec version. Record
   the decision as a new ADR; add a one-line forward pointer to it from ADR-027.

## 4. Risks / notes

- **Feathered vs hard canvas edge.** The shim's synthetic mask uses the zone wish-mask
  raster (feathered edge), so the canvas/surround boundary is a soft blend rather than the
  hard strip edge. This matches zones aesthetics and is acceptable; flagged here so it is
  not mistaken for a bug during validation.
- **Extra weave.** The shim keeps the DP weaving the canvas sub-rect only (no extra weave) —
  unlike a naive "treat as full-window zone" approach, which would stretch the canvas-sized
  atlas. The composite (not the weave) is what spans the window.
- **framebufferOnly outputs.** Runtime-owned-window drawables (hosted) are not readable by
  the composite; the shim harmlessly falls back to the bespoke strips for those. Surround is
  a texture-app feature in practice, so this is not a regression.
- **External apps.** The entry points stay ABI-present until the removal step; the spec
  banner + header annotations warn new apps off. Removal is gated on the spec-version bump.
