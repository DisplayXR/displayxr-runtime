---
status: Accepted
date: 2026-06-10
---
# ADR-026: Orientation-Independent Rendering Modes, Config-Derived View Scale, Rotation-Aware Worst-Case Atlas

## Context

A 3D display renders each eye at a per-view resolution that is a fraction of the panel —
the **view scale**. On Windows the Leia DP advertises this from the SR SDK
(`drv_leia/leia_device.c` mode 1 = 0.5×0.5; `leia_plugin.c` computes it from the SDK's
recommended view dims). On **Android** the Leia DP originally hardcoded
`recommended_view_scale = 1.0×1.0` and advertised **no** rendering modes, so each eye
over-rendered at display×(0.5 from the 2×1 SBS tile split)×1.0 — e.g. 1280×1600 on a
2560×1600 panel, with the wrong aspect for the lenticular weave (#518).

Two things make Android harder than Windows:

1. **The tile geometry is device-config-driven and asynchronous.** CNSDK exposes the
   per-view tile via `LEIA_DEVICE_CONFIG_PROPERTY_VIEW_RESOLUTION_PX` (e.g. 1200×1920 on
   the nubia NP02J), in the device **natural** orientation (portrait on that unit). The
   config is only readable after the async core init, but rendering modes + display info
   are queried at `xrCreateInstance`.

2. **The device rotates, and the app's render swapchain is never recreated on rotation.**
   The cube (and OpenXR apps generally) create their swapchain once at session start; on
   rotation the runtime swaps the Kooima FOV meters (`oxr_session.c`) and the CNSDK weave
   rotates the physical interlacing, but the app keeps the same swapchain. So the per-eye
   geometry differs by orientation (1920×1200 landscape vs 1200×1920 portrait) on a
   **fixed** swapchain.

## Decision

1. **Rendering modes are content modes, orientation-independent.** A device advertises 2D
   (scale 1×1, mono) and Leia 3D (2×1 SBS, config-derived scale). **Orientation is NOT a
   rendering mode** — modes are baked at `xrCreateInstance` and re-negotiating them on
   every rotation fights that. Orientation is handled orthogonally (below).

2. **3D `view_scale` = tile ÷ panel**, both in the device natural orientation, mapped to
   the orientation the display dims are reported in (axes swapped when they differ; a
   symmetric tile like the nubia's 0.75×0.75 is invariant). The plug-in reads
   `VIEW_RESOLUTION_PX` on the worker and logs it; because the panel + tile are
   device-intrinsic, the modes/`display_info` use a fixed-device **baseline** at create
   time (the async value confirms it on-device and serves late re-queries). The runtime
   prefers the plug-in's `recommended_view_scale` (`target_instance.c`).

3. **Per-mode `XRT_RENDERING_MODE_FLAG_CAN_ROTATE`** — a new `mode_flags` bit
   (`xrt_device.h`), NOT a new struct field, per [ADR-022](ADR-022-per-mode-capability-flags-frozen-enum-structs.md)
   (the `xrt_rendering_mode` struct stride is a plug-in ABI contract). Set when a mode is
   usable in either orientation; clear = orientation-locked. Default 0 keeps existing
   modes (sim_display's 5, the Windows Leia modes) at single-orientation sizing — pure
   back-compat; only opt-in modes change.

4. **Worst-case atlas spans both orientations for CAN_ROTATE modes.** Because the app
   swapchain is never recreated on rotation, it is sized to the worst case across modes
   **and** orientations (`u_tiling_compute_system_atlas_oriented`, called from
   `target_instance.c`): each mode contributes its native atlas and, if CAN_ROTATE, also
   its 90°-swapped atlas. The app renders a **per-orientation sub-rect** of that fixed
   swapchain (per-eye = current-orientation display × view_scale), so each held
   orientation is rendered at its correct native tile resolution. Validated on the nubia:
   live landscape↔portrait swaps the per-eye 1920×1200 ↔ 1200×1920 on a fixed 3840×2560
   atlas, weave correct in both.

## Alternatives considered

- **Separate rendering modes per orientation** (landscape-3D / portrait-3D): rejected —
  conflates content mode with orientation, requires auto-switching + re-negotiation on
  every rotation, and the baked-at-startup mode table doesn't support it cleanly.
- **Recreate the app swapchain on rotation**: rejected — OpenXR apps don't expect a
  mid-session view-config resize; the fixed-worst-case + per-orientation sub-rect is the
  idiomatic approach (swapchain = max, render to recommended rect).
- **A `supported_orientations` bitmask** (mirroring CNSDK `leia_legal_orientations`):
  deferred — for *dimension* sizing only the 90° wide-vs-tall distinction matters
  (reverse-landscape has the same extent as landscape), so a single CAN_ROTATE bit
  suffices. A bitmask is a future enhancement if per-orientation *policy* is needed.

## Consequences

- Android Leia 3D now renders each eye at the panel's true per-view tile resolution and
  aspect, in both orientations, without over-rendering.
- Asymmetric panels (view_scale_x ≠ view_scale_y) need the runtime to swap the scale axes
  on the rotation event that already swaps the display dims — not yet implemented; the
  current symmetric nubia path needs none. Tracked as follow-up.
- Files: `xrt_device.h` (flag), `util/u_tiling.h` (oriented worst-case helper),
  `targets/common/target_instance.c` (call site); plug-in
  `drv_leia_android/{leia_cnsdk.*,leia_plugin_android.c}`; reference app
  `test_apps/cube_handle_vk_android`.
