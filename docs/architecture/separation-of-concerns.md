---
status: Active
owner: David Fattal
updated: 2026-05-31
---
# Separation of Concerns: App → OXR → Compositor → Driver/DP

This document defines what each architectural layer owns and what must not cross boundaries. Reference this during development whenever there's a question about where code belongs.

## Layer 1: Application

- Provides window handle (`_handle`, `_texture`) or lets runtime create one (`_hosted`)
- Creates swapchains at recommended dimensions
- Renders views into swapchain tiles
- Optionally enables `XR_EXT_display_info` for mode awareness
- Computes own camera model (Kooima projection) in RAW mode

## Layer 2: OXR State Tracker (`src/xrt/state_trackers/oxr/`)

- OpenXR API validation and handle management
- Session state machine (IDLE → READY → FOCUSED)
- Extension dispatch (mode enumeration, eye tracking queries)
- Swapchain sizing computation (recommended dimensions from driver metadata)
- Legacy app compromise scaling logic (`oxr_system_fill_in()`)
- Mode switch routing (V-toggle → compositor → DP)
- Event queuing to app (`XrEventDataRenderingModeChangedEXT`, `XrEventDataHardwareDisplayStateChangedEXT`)
- **Must NOT contain**: vendor SDK headers, graphics API types, interlacing logic

## Layer 3: Native Compositors (`src/xrt/compositor/`)

- Graphics API-specific swapchain creation (both app swapchain and target swapchain) and image management
- Layer accumulation and atlas rendering (tile all views into one texture)
- Display processor instantiation via factory from `xrt_system_compositor_info`
- Crops atlas to content dimensions before calling DP (`tile_columns * view_width × tile_rows * view_height`)
- Calls `xdp->process_atlas()` to transform atlas → display output
- Eye position pass-through from display processor to OXR
- Window management (uses app-provided handle or creates own)
- **Must NOT contain**: OpenXR extension logic, vendor-specific interlacing, mode enumeration

## Layer 4: Device Drivers (`src/xrt/drivers/`)

- Display dimensions (physical size, pixel resolution, refresh rate)
- Rendering mode array (`rendering_modes[]` with view_count, view_scale, tile layout)
- Active mode index management
- Pose tracking (or delegation to qwerty HMD)
- Display processor factory registration on `xrt_system_compositor_info`
- **Must NOT contain**: compositor rendering code, OXR state management

## Layer 5: Display Processors

Implementations live in `src/xrt/drivers/` (vendor-specific) or `src/xrt/compositor/` (generic).

- `process_atlas()` — transform tiled atlas to display-specific output for all advertised modes (interlacing, SBS, anaglyph, and 2D passthrough). Receives atlas texture with dimensions exactly matching the mode's tile layout — no need to handle oversized or mismatched textures
- `get_predicted_eye_positions()` — N-view eye positions from vendor SDK
- `request_display_mode()` — hardware 2D/3D switching
- `get_display_dimensions()` — physical size for Kooima FOV
- `get_render_pass()` — Vulkan render pass for framebuffer compatibility (Vulkan DP only)
- `set_output_format()` — deferred format configuration (D3D12 DP only)
- Pure vtable interface per API: `xrt_display_processor` (Vulkan), `xrt_display_processor_d3d11`, `xrt_display_processor_d3d12`, `xrt_display_processor_metal`, `xrt_display_processor_gl`
- **Must NOT contain**: OXR types, session state, swapchain management

## Responsibility Matrix

| Task | App | OXR | Compositor | Driver/DP |
|---|---|---|---|---|
| Provide window | creates | — | uses | — |
| Enumerate modes | queries | dispatches | — | provides |
| Switch mode | requests | routes | calls DP | DP implements |
| Render views | renders | — | — | — |
| Allocate swapchain | requests | validates | creates images | — |
| Composite layers | — | — | owns | — |
| Atlas → display | — | — | calls DP | DP implements |
| Eye positions | — | queries | extracts from DP | DP provides |
| Display dimensions | — | queries | — | DP provides |
| Window metrics | — | — | queries from DP | DP provides |

## Vendor Isolation Rule

> A new vendor integrates by adding files **only** under `src/xrt/drivers/<vendor>/` and `src/xrt/targets/common/`. Zero changes to compositor or state tracker code.

## Workspace Controller / Runtime Boundary

The spatial-workspace surface (`XR_EXT_spatial_workspace`, spec_version 7) splits responsibility between the runtime and a separate **workspace controller** process (the DisplayXR Shell is the reference implementation):

- **Runtime owns mechanism**: cross-process texture sharing, atlas composition at controller-specified poses, depth pipeline, cursor-sprite compositing (at the per-eye depth — and over-window dim alpha — the controller pushes via `xrSetWorkspaceCursorDepthEXT`), input-event drain (POINTER / KEY / SCROLL / MOTION + FRAME_TICK carrying the OS cursor position), and lifecycle dispatch (close / fullscreen RPCs).
- **Controller owns policy + appearance**: hit-testing (the eye→cursor raycast against window planes — which window / region / depth the cursor is over), every pixel of chrome (pill background, grip dots, buttons, icons, glyphs, focus glow), every region's hit-region geometry, every animation curve (hover-fade, slot-anim transitions, carousel rotation), every layout preset, the drag / resize / rotation state machines (driven via `xrSetWorkspaceClientWindowPoseEXT`), and every keyboard / window-behavior shortcut (close, launch, focus-cycle, maximize, depth-step) decided off the KEY event stream.

After Phase 2.C the runtime ships with **zero default chrome**. Workspace clients render as bare content quads unless a controller submits a chrome swapchain via `xrCreateWorkspaceClientChromeSwapchainEXT`. This means controllers (third-party shells, accessibility overlays, OEM skins) can change chrome appearance, geometry, and behavior without forking the runtime.

**Hit-test ownership (spec_version 22, issue #370):** the runtime no longer raycasts. `workspace_raycast_hit_test` and the public `xrWorkspaceHitTestEXT` were deleted; the runtime stopped enriching POINTER / POINTER_MOTION events and stopped emitting POINTER_HOVER. The controller runs the hit-test itself (it already owns the window poses it pushed, gets eye positions via its session, and receives the OS cursor position each frame on FRAME_TICK), generates its own hover transitions, and feeds the runtime only the resulting cursor **depth** so the sprite composites at the right per-eye disparity. The drag / resize / rotation state machines that ADR-018 flagged as the real layering violation have likewise moved to the controller. This reverses ADR-018's "hit-test stays in the runtime as plumbing" decision — see that ADR's superseded note.

**Residual input + look-and-feel policy retired (spec_version 23, issue #376):** the last runtime-side workspace policies moved to the controller. The runtime's `DELETE` (close-focused-client) and `Ctrl+O` (browse + launch an arbitrary exe) key intercepts were deleted — both are now controller policy driven off the drained KEY events (DELETE → `xrRequestWorkspaceClientExitEXT`; Ctrl+O → the controller's own file picker + launch path), joining the TAB / F11 / `[` / `]` shortcuts the controller already owned (#305–#307). The over-window cursor body alpha — previously a hardcoded `0.30` in the compositor — is now controller-pushed as `dimFactor` on `xrSetWorkspaceCursorDepthEXT`. (The dead `mc->drag` field, a vestige of the migrated drag state machine, was also removed.) The runtime retains the close / launch / cursor *mechanisms*; the controller decides when and how they fire.

**Reference**: `feedback_controllers_own_motion` for the architectural North Star; ADR-018 for the hit-test plumbing vs interactive policy split; `docs/specs/extensions/XR_EXT_spatial_workspace.md` for the surface; `src/xrt/targets/shell/shell_chrome.cpp` for the reference chrome implementation.

## Data Flow Examples

### Mode Switch Flow
```
User presses V-key
  → OXR state tracker receives mode switch request
    → Routes to compositor via xrt_compositor::request_display_rendering_mode()
      → Compositor calls xdp->request_display_mode(enable_3d)
        → Display processor activates/deactivates hardware 3D
          → Compositor updates active mode index
            → OXR queues XrEventDataRenderingModeChangedEXT to app
```

### Eye Position Flow
```
App calls xrLocateViews()
  → OXR state tracker calls compositor->get_eye_positions()
    → Compositor calls xdp->get_predicted_eye_positions()
      → Display processor queries vendor SDK (e.g., a vendor's eye-tracking API)
        → Returns xrt_eye_positions (N eye positions)
    → OXR applies Kooima FOV math (RENDER_READY) or passes raw (RAW mode)
      → App receives XrView[] with poses and FOVs
```

### Swapchain Sizing Flow
```
Driver populates rendering_modes[] on xrt_system_compositor_info at init
  → OXR reads modes in oxr_system_fill_in()
    → Computes max(tileColumns[i] * scaleX[i] * displayW) across all modes
      → Reports recommendedImageRectWidth/Height to app
        → App creates swapchain at recommended dimensions
```
