---
status: Active
owner: David Fattal
updated: 2026-03-15
issues: [79]
code-paths: [src/xrt/state_trackers/oxr/oxr_system.c]
---

# Legacy App Support: Adaptive View Sizing and Compositor Tile Processing

## Background: Legacy vs Display-Info-Aware Apps

OpenXR apps that target DisplayXR fall into two categories based on whether they enable the `XR_EXT_display_info` extension:

### Display-Info-Aware Apps (`XR_EXT_display_info` enabled)

These apps can:
- Enumerate rendering modes via `xrEnumerateDisplayRenderingModesEXT()` to discover available modes with their `viewCount`, `tileColumns`, `tileRows`, `viewScaleX`, `viewScaleY`
- Request mode changes via `xrRequestDisplayRenderingModeEXT()`
- Receive `XrEventDataRenderingModeChangedEXT` events when the mode changes (e.g., via qwerty device keyboard shortcut)
- Adapt rendering parameters (viewport layout, tile dimensions, view count) dynamically per mode

These apps can render optimally for every mode because they know the exact tile layout and per-view scale at all times.

### Legacy Apps (no `XR_EXT_display_info`)

These are standard OpenXR apps (including WebXR apps on Windows) that:
- Don't know about display rendering modes
- Set all rendering parameters at init based on `xrEnumerateViewConfigurationViews()` recommended dimensions
- Create swapchains once with fixed dimensions
- Cannot adapt when the runtime switches modes (e.g., 3D <-> 2D via qwerty device)

The problem: `recommendedImageRectWidth/Height` is computed once at `xrGetSystem` time using `recommended_view_scale_x/y`. Currently this is set to the **minimum scale across all modes** (0.5x0.5 for both Leia and sim_display), which means:
- In 3D mode (SBS, 0.5x0.5): tiles fit correctly
- In 2D mode (1.0x1.0): the app only rendered at half resolution in both dimensions -- significant quality loss

## Solution

### 1. Detect legacy apps

The runtime already tracks enabled extensions via `inst->extensions.EXT_display_info`. This boolean distinguishes legacy from aware apps.

### 2. Restrict legacy apps to 2 modes

Legacy apps only support switching between:
- **Mode 0**: 2D (1 view, 1.0x1.0)
- **Mode 1**: Default 3D mode

### 3. Adaptive view scale for legacy apps

Instead of using `min(all modes)` for `recommended_view_scale`, use a compromise based on the default 3D mode's properties:

| Default 3D mode | Communicated scaleXY | Rationale |
|---|---|---|
| 2 views, scaleX <= 0.5, scaleY <= 0.5 | **0.5 x 1.0** | Compromise: full height preserves 2D quality, half width is correct for SBS |
| All other cases (>2 views, or scaleX/Y > 0.5) | **3D mode's actual scaleXY** | Already optimal or close to optimal |

For the common case (Leia SR and sim_display SBS, both 0.5x0.5), this means the legacy app renders at 0.5x1.0 -- each view is half-width but full-height.

### 4. Compositor tile processing

The compositor adds a transform step between the app's submitted atlas and the display processor input, to handle the mismatch between what the legacy app rendered and what the current mode expects:

| Active mode | Case A (app at 0.5x1.0, 3D expects 0.5x0.5) | Case B (app at 3D's actual scale) |
|---|---|---|
| **3D** | Downscale Y per tile (1.0 -> 0.5) before DP | Passthrough -- tiles match |
| **2D** | Stretch X on left view (0.5 -> 1.0) before DP | Stretch to 1.0x1.0 before DP |

### Implementation locations

- **View scale logic**: `oxr_system_fill_in()` in `src/xrt/state_trackers/oxr/oxr_system.c` -- check `inst->extensions.EXT_display_info` to decide which scale to communicate
- **Mode restriction**: When qwerty device triggers mode change for a legacy app session, clamp to mode 0/1 only
- **Tile processing**: In each native compositor's `process_atlas` path (or in the display processor), compare submitted tile dimensions to expected tile dimensions and blit/scale as needed

## Examples

### Leia SR (3840x2160 display, default 3D mode: 0.5x0.5)

**Legacy app sees**: recommended 1920x2160 per view -> swapchain 3840x2160
- 3D mode: compositor downscales each 1920x2160 tile to 1920x1080 for SR SDK
- 2D mode: compositor stretches left 1920x2160 view to 3840x2160

### Sim display (1920x1080, default 3D SBS: 0.5x0.5)

**Legacy app sees**: recommended 960x1080 per view -> swapchain 1920x1080
- 3D mode: compositor downscales each 960x1080 tile to 960x540
- 2D mode: compositor stretches left 960x1080 view to 1920x1080

## Compositor Contract

When `legacy_app_tile_scaling` is true on `xrt_system_compositor_info`:

1. **View dimensions are fixed for the session lifetime.** The compositor MUST use `display_pixel_width * legacy_view_scale_x` and `display_pixel_height * legacy_view_scale_y` for per-view content dimensions, regardless of which rendering mode is active. The compositor MUST NOT recompute view dimensions from the active mode's native scale.

2. **Only tile layout changes on mode switch.** When the user toggles modes (V key), the compositor updates `tile_columns` and `tile_rows` from the active mode but keeps view_width and view_height fixed.

3. **Crop-blit delivers compromise-sized content.** The content region passed to the display processor is always `tile_columns * compromise_vw` × `tile_rows * compromise_vh`:
   - 2D mode (1×1): content = 1 × compromise_vw × 1 × compromise_vh (left eye only, DP stretches to fill)
   - 3D mode (2×1): content = 2 × compromise_vw × 1 × compromise_vh (full SBS atlas, DP interlaces)

4. **Scale values are stored in `xrt_system_compositor_info`.** The fields `legacy_view_scale_x` and `legacy_view_scale_y` hold the compromise scale computed in `oxr_system_fill_in()`. These are propagated to each compositor via `set_sys_info` (Metal, GL, VK) or `set_legacy_app_tile_scaling` (D3D11, D3D12).
