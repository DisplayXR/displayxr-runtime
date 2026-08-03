# Transparency modes — canonical vocabulary

How a transparent DisplayXR overlay window gets its see-through pixels is a
**2×2 of two independent axes**, not a single switch. Established 2026-08-03
with the #833/#837 decoupled-presentation work; the numbers below are the
gated iGPU A/B from that work (dxr-perf-study `BENCH-FINDINGS.md`,
2026-08-03 addendum).

## Axis 1 — composition: who completes the transparency

- **Live** (transparent present): DComp premultiplied swapchain; **DWM**
  alpha-blends the window over the live desktop every frame. Zero lag,
  expensive (per-pixel blend, never flip-promotes).
- **Baked** (opaque present, `DXR_PRESENT_OPAQUE=1`): opaque HWND flip-model
  swapchain; DWM blends nothing. The display processor **bakes the
  WGC-captured desktop** (~1 frame old) into every non-opaque pixel
  (compose-under-bg + flattened alpha-gate, leia-plugin#116).

## Axis 2 — shaping: what the window physically covers

- **Unshaped**: the window owns its full rect.
- **Shaped**: `SetWindowRgn` from per-frame content coverage carves the
  all-views-transparent pixels **out of the window** — the desktop there is
  real desktop, not our pixels (`dxr::ClickThroughRegion` in
  displayxr-common ≥ v2.6.0; the Unity provider pioneered the technique).

## The four combinations (measured on Intel UHD, 811×1421 window)

| | Unshaped | Shaped |
|---|---|---|
| **Live** | Classic transparency. Live punch-through everywhere. ~56 pts system GPU, dwm ~32 %, Composed: Flip. | Live everywhere (holes are real desktop; in-region blends DWM-live). dwm ∝ region area. Composed. |
| **Baked** | Bake everywhere the window covers. **Cheapest**: 12–15 pts, dwm ≈ 0, **Hardware Composed: Independent Flip**. | **Live punch-through in the carved holes**; bake only in the in-region blend band (silhouette edges, de-occluded pixels, kept chrome). Cheap region compose. Composed. |

## Rules (each learned the hard way)

1. **Shaping only punches through what you carve.** Kept surface (OS frame,
   chrome bands/rects) shows the bake wherever it is not opaque content.
   Chrome in a baked+shaped app therefore has exactly three options: hide it
   while transparent (product default for the demos), keep it and accept
   bake in its transparent padding, or make it tight/opaque.
2. **Frame and shaping are exclusive per-moment.** A region suppresses the
   DWM themed frame, and a `WS_EX_NOREDIRECTIONBITMAP` window cannot paint
   the classic fallback — a shaped framed window shows *no* frame. Toggle
   between framed-unshaped (positioning) and borderless-shaped (product).
   Never change window styles while shaped (the flip-chain window vanishes).
3. **Independent Flip belongs to unshaped-baked only.** Any punch-through
   (shaped) window is Composed by construction: a flip surface scans out its
   full opaque rect, and per-pixel-alpha hardware planes are not reachable
   through HWND flip chains (#833, measured dead ends).
4. **Baked composition requires session adapter == the panel's scanout
   adapter** (`DXR_VK_FORCE_GPU=igpu` on Optimus boxes): cross-adapter WGC
   delivers black/no frames → holes flatten to black (the "opaque squared
   Local2D" failure; leia-plugin#119).
5. **Bake lag is only perceptible under motion.** Over a static desktop the
   ~1-frame-old full-monitor capture is pixel-identical at any window
   position (given capture self-exclusion), so drags over static desktops
   are clean; animated content behind the blend band is the residual case.
   True dynamic live↔baked switching on drag brackets would need runtime
   swapchain recreation — not implemented, by choice.
6. **Window-space-stamped chrome is additionally broken under baked
   composition** (leia-plugin#121, open): present in the DP's input atlas,
   missing from its gated output. Local2D chrome (post-weave composite) is
   the proven path if chrome must show while transparent.

## What each app uses

| App | Transparent-mode combo | Notes |
|---|---|---|
| cube test apps | baked + unshaped | Full-cover content ⇒ no holes to punch; gets Independent Flip. `DISPLAYXR_TRANSPARENT_BG=1` + env. |
| avatar (`displayxr-demo-avatar`) | baked + shaped (borderless) / baked + unshaped (framed `B` positioning mode) | Silhouette+bubble region; `B`-mode client bake is documented semantics. |
| modelviewer (`displayxr-demo-modelviewer`) | opaque scene default; `Ctrl+T` = baked + shaped, chrome hidden, RMB-drag to move | Region from view-0 alpha via `dxr::ClickThroughRegion`; toast band kept in-region. |
| gaussiansplat (`displayxr-demo-gaussiansplat`) | baked + unshaped *(shaping wiring pending)* | Kit-cached HUD landed; punch-through follows the modelviewer recipe. |
| Unity provider overlay | shaped (region from rendered alpha); present path = whatever the runtime session uses | The origin of the shaping technique; gets baked composition with the same env, no plugin changes. |

Present-path env: `DXR_PRESENT_OPAQUE` (default off). App-side building
blocks: displayxr-common ≥ v2.6.0 (`vk_overlay_kit.h`,
`vk_clickthrough_region.h`, `win_window_drag.h`).
