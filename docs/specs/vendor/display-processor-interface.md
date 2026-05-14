---
status: Active
owner: David Fattal
updated: 2026-03-15
issues: [45]
code-paths: [src/xrt/include/xrt/xrt_display_processor.h, src/xrt/drivers/]
---

# Standardize Display Processor Interface Across Graphics APIs

> **Implementation note:** The unified `process_atlas()` method with tile layout
> parameters has been implemented across all five DP variants. The historical
> discussion below documents `process_views`/`process_stereo` naming that has
> since been resolved — all variants now use `process_atlas()`.

## Tracking

Platform-specific work:
- #62 (Windows) -- D3D11/D3D12 display processor standardization
- #63 (macOS) -- Metal display processor standardization

Note: Vulkan display processor standardization is cross-platform and remains in this parent issue.

Parent: #23

## Problem

We have 4 separate `xrt_display_processor` interface variants -- one per graphics API -- with inconsistent naming, input conventions, and struct types. This makes it harder for new display vendors to integrate and increases maintenance burden.

## Current State

| | Vulkan | D3D11 | D3D12 | Metal |
|---|---|---|---|---|
| **Struct** | `xrt_display_processor` | `xrt_display_processor_d3d11` | `xrt_display_processor_d3d12` | `xrt_display_processor_metal` |
| **Weave method** | `process_views` | `process_stereo` | `process_stereo` | `process_stereo` |
| **Input format** | Overloaded: SBS `(sbs, NULL)` or separate `(L, R)` via `prefers_sbs_input` flag | SBS only | SBS only | SBS only |
| **Header** | `xrt_display_processor.h` | `xrt_display_processor_d3d11.h` | `xrt_display_processor_d3d12.h` | `xrt_display_processor_metal.h` |
| **Factory** | `xrt_dp_factory_vk_fn_t` | `xrt_dp_factory_d3d11_fn_t` | `xrt_dp_factory_d3d12_fn_t` | `xrt_dp_factory_metal_fn_t` |

**Auxiliary methods are identical across all 4:**
- `get_predicted_eye_positions`
- `get_window_metrics`
- `request_display_mode`
- `get_display_dimensions`
- `get_display_pixel_info`
- `destroy`

## Specific Issues

1. **Method naming**: D3D11/D3D12/Metal use `process_stereo`, Vulkan uses `process_views`. Should standardize on **`process_views`** -- this is forward-looking: today it's stereo (2 views), but with multiview support (#5) it could be N views tiled into a single texture.

2. **Vulkan input overloading**: The Vulkan interface is called two different ways depending on `prefers_sbs_input`:
   - Leia SR: `process_views(cmd, sbs_view, VK_NULL_HANDLE, sbs_width*2, ...)` -- SBS packed into left_view, right=NULL
   - sim_display: `process_views(cmd, left_view, right_view, eye_width, ...)` -- genuine separate views

   This should be standardized to SBS input everywhere. sim_display's Vulkan processor needs updating to accept SBS (trivial shader change -- sample left/right halves instead of two separate textures).

3. **No shared base**: The 6 auxiliary methods are copy-pasted across all 4 structs. A vendor implementing a new display processor must implement 4 separate interfaces with identical boilerplate.

4. **Separate struct types**: Each API has its own struct, so the compositor must know at compile time which API variant it's dealing with. This complicates the multi compositor (#43) which needs to hold display processors of any API type.

## Naming Convention: `process_views`

All APIs should use **`process_views`**, not `process_stereo`:
- "Views" is the correct abstraction -- today 2 views (stereo), tomorrow N views (#5 multiview)
- Input is a single tiled texture containing all views (today: side-by-side L|R; future: NxM tile grid)
- The display processor doesn't care how many views -- it processes whatever tile layout it receives
- Aligns with OpenXR terminology (`XrViewConfigurationType`, `xrLocateViews`)

## Standardized Conventions

1. **Method name**: `process_views` everywhere (rename D3D11/D3D12/Metal from `process_stereo`)
2. **Input**: Single tiled texture (SBS today, multi-view tiles in future per #5). No separate L/R path.
3. **Remove `prefers_sbs_input` flag**: Always tiled input. Update sim_display Vulkan processor accordingly.
4. **View count parameter**: Add `uint32_t view_count` so the display processor knows how many views are tiled. Today always 2, future: N.

## Proposed Standardization

### Phase 1: Naming and input consistency (low risk)
- Rename D3D11/D3D12/Metal `process_stereo` -> `process_views`
- Update sim_display Vulkan processor to accept SBS/tiled input (shader samples left/right halves of single texture)
- Remove `prefers_sbs_input` flag -- always tiled
- Add `view_count` parameter (always 2 for now, future-proofs for #5)

### Phase 2: Shared base struct (medium risk)
- Extract common auxiliary methods into `xrt_display_processor_base`
- API-specific structs embed the base:
  ```c
  struct xrt_display_processor_d3d11 {
      struct xrt_display_processor_base base;
      void (*process_views)(...);  // API-specific types
  };
  ```
- Vendors implement auxiliary methods once, `process_views` per-API

### Phase 3: Vendor SDK interface documentation
- Document the standardized interface as the "display processor SDK"
- A new vendor (e.g., Looking Glass, RED Hydrogen) implements:
  1. One set of auxiliary methods (eye tracking, display info, etc.)
  2. One `process_views` per graphics API they support
  3. One factory function per API
- Provide sim_display as a reference implementation template

## Implementations to Update

| Vendor | Vulkan | D3D11 | D3D12 | Metal | GL |
|---|---|---|---|---|---|
| **Leia SR** | `leia_display_processor.cpp` | `leia_display_processor_d3d11.cpp` | `leia_display_processor_d3d12.cpp` | -- | `leia_display_processor_gl.cpp` |
| **sim_display** | `sim_display_processor.c` (needs SBS input) | `sim_display_processor_d3d11.cpp` | `sim_display_processor_d3d12.cpp` | `sim_display_processor_metal.m` | -- |

## Related

- #5 -- Multiview support (N views tiled into single texture)
- #43 -- Multi compositor (needs to hold display processors of any API type)

## Acceptance Criteria

- All display processor variants use `process_views` with tiled view input
- `view_count` parameter added for future multiview (#5)
- Common auxiliary methods extracted into shared base
- GL display processor interface added
- sim_display Vulkan processor updated to accept SBS input
- All existing implementations updated
- CI passes on Windows and macOS
