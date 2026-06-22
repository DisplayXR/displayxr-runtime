# Deprecating the 2D-surround / output-rect path in favour of display-zones

**Status:** DONE — the output-rect/surround entry points + bespoke surround code are removed;
display-zones is the sole region paradigm. Runtime spec bumps: `XR_EXT_win32_window_binding` v8,
`XR_EXT_cocoa_window_binding` v7. Decision recorded as **ADR-031**.
**Owner:** runtime.
**Refs:** ADR-031 (removal decision), ADR-027 (display zones), `XR_EXT_display_zones`,
`XR_EXT_win32_window_binding`, `XR_EXT_cocoa_window_binding`.

> This doc now serves as **ADR-031's background** — the decision to remove the surround /
> output-rect mechanism in favour of display-zones is recorded there. ADR-027 was left intact
> (it added zones; it didn't decide to remove surround).

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
   - **Gate:** `u_capability_enabled("DISPLAYXR_SURROUND_SHIM", "SurroundShim", true)`,
     **default ON** (since Step 3) → legacy surround routes through the unified composite
     (one-time `Surround→zones shim ACTIVE` log). Precedence: env var (dev override) >
     `HKLM\Software\DisplayXR\Capabilities\SurroundShim\Enabled` REG_DWORD (admin
     force-OFF kill-switch) > default ON. Set either to `0` to fall back to the bespoke strips.
   - **Status:** Metal done + verified (`cube_texture_metal_macos`, flag-on routes through
     the unified path, flag-off unchanged). **D3D11 + D3D12 validated on real Leia SR
     hardware** (2026-06-21): flag-off renders the bespoke strips, flag-on logs the one-time
     `Surround→zones shim ACTIVE` WARN and routes through the unified mask composite, opaque
     BGRA texture-app content correct in both, feathered canvas edge as expected. All three
     backends with a working bespoke surround path are now shim-validated.
   - **Product gate follow-up (done, Step 3):** the env override was promoted to the shared
     `u_capability_enabled()` helper (`auxiliary/util/u_capability.{c,h}`) reading the
     `Capabilities\SurroundShim\Enabled` marker, per the registry-over-env-var convention.
3. **Flip default ON** (code-complete): all three gates default ON via `u_capability_enabled`;
   the canonical path now serves legacy apps unconditionally unless force-disabled. Precedence
   matrix validated on real Leia SR hardware 2026-06-21 (D3D11 + D3D12: default→ACTIVE,
   env=0→bespoke, registry=0→bespoke, registry=1→ACTIVE). No installer change needed — the
   marker is read-if-present (default is in code). *Pending before merge:* the standard
   hardware eyeball; Metal default-flip still needs a macOS eyeball (the shim path itself was
   already verified on Metal).
4. **Migrate first-party apps** off the surround calls to native zones submission (done): the
   canonical texture-class parity apps are now `cube_zones_texture_{d3d11_win,d3d12_win,metal_macos}`;
   the legacy `cube_texture_*` surround apps were retired.
5. **Delete** (done — Step 5, branch `feat/634-step5-delete-surround`): the bespoke surround code
   (strip blit, surround shader, keyed-mutex/fence surround handling, the `DISPLAYXR_SURROUND_SHIM`
   translation shim) and the three entry points (`xrSetSharedTextureOutputRectEXT`,
   `xrSetSharedTextureSurround2DEXT`, `xrSetSharedTextureSurround2DFenceEXT`) are removed; the
   extension spec versions are bumped (`XR_EXT_win32_window_binding` v8, `XR_EXT_cocoa_window_binding`
   v7). The decision is recorded as **ADR-031**, with a forward pointer added from ADR-027.

## 4. Backend coverage — which backends the shim touches (and why)

The shim only matters where a **working bespoke surround path** exists that real apps depend
on. That is exactly **D3D11, D3D12, Metal** — all three have `*_blit_surround_strips` (a real
per-frame surround fill) and shipping `cube_texture_*` apps that use it. Those are the three
backends the shim covers (Metal verified; D3D11/D3D12 pending Windows validation).

- **GL and VK_native do NOT need the shim.** Their `comp_*_set_surround_2d` only *stores* the
  handle and logs `"(open + blit pending)"` — there is no import, no strip blit, no composite
  of the surround. They register it and ignore it, and there are no GL/VK **texture apps**
  (only `cube_handle_gl_*` / `cube_handle_vk_*`). There is no working behaviour to preserve, so
  there is nothing to shim. (Both already have the unified composite — `gl_composite_local_2d`
  / `vk_composite_local_2d` + `*_update_zone_wish_mask` — they just never wired surround to it.)
- **Android needs nothing.** The Android OOP runtime uses the `vk_native` compositor (the
  service path), and there is no Android texture app or Android surround code (only
  `cube_handle_vk_android`). Covered by "VK = stub".
- **Future GL/VK surround → canonical, not a shim.** If a GL or VK texture app ever needs a 2D
  region, implement it **directly through `XR_EXT_display_zones` / the existing unified
  composite from day one** — skip the bespoke-strip-blit-then-shim cycle entirely. New backends
  never grow a bespoke surround path to deprecate; that is the point of converging on zones.

## 5. Risks / notes

- **Texture + zones on a real weaver — opaque zone went black (ROOT-CAUSED + FIXED, Leia
  plugin).** Surfaced by `cube_zones_texture_d3d11_win` on the Leia SR machine (PR #610):
  with a real lenticular weave the opaque Zone A rendered **black** while transparent Zone B
  + the Local2D strip composited fine. **Root cause is entirely in the Leia plugin, not the
  runtime:** the D3D11 alpha-gate (`alpha_gate_run_post_weave`) copies the woven back-buffer
  into `ck_strip_tex` for the opaque "keep" path, but `ck_ensure_strip_source` hardcoded
  `DXGI_FORMAT_R8G8B8A8_UNORM`. `_handle` apps render to an RGBA back buffer (match → works);
  `_texture` apps' shared texture is **BGRA** (`B8G8R8A8_UNORM`), so the `CopyResource` failed
  silently → `ck_strip_tex` stayed black → the gate resampled black for all opaque woven
  content. Transparent pixels still punched to desktop, so it looked like the opaque zone
  vanished. Fixed in **DisplayXR/displayxr-leia-plugin#66** (use the back-buffer's actual
  `DXGI_FORMAT`; no ABI change). The runtime composite + super-atlas + DP target sizing are
  all correct — **the earlier "weave into a canvas-sized target" hypothesis was wrong**
  (`DISPLAYXR_TRANSPARENT_BG=0` and the surround app both weave fine into the same
  worst-case-sized target). `sim_display` SBS has no gate, which is why Mac/CI didn't surface
  it. **Resolved:** the **D3D12** variant has the same fix (Leia-plugin commit `5b4f74e`,
  mirror of the D3D11 #66 change — uses the back-buffer's actual `DXGI_FORMAT` for both the
  strip texture desc and the strip PSO RTV format; on `main`). The **GL** variant is **not
  affected** — its `ck_ensure_strip_source` populates the strip via `glBlitFramebuffer` (a
  format-converting blit), not a format-identical `CopyResource`, so the byte-order mismatch
  cannot occur. Opaque BGRA texture-app content confirmed correct on D3D11 + D3D12 during the
  2026-06-21 Leia shim validation (no black zone).
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
- **External apps.** The entry points are now removed (Step 5); the spec-version bump
  (`XR_EXT_win32_window_binding` v8, `XR_EXT_cocoa_window_binding` v7) signals it. Any
  unmigrated external app must move to display-zones.
