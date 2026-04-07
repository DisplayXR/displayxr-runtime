---
status: Proposal
owner: David Fattal
updated: 2026-03-15
issues: [46]
code-paths: [src/xrt/auxiliary/math/, src/xrt/state_trackers/oxr/]
---

> **Status: Proposal** — not yet implemented. Tracking issue: [#46](https://github.com/DisplayXR/displayxr-runtime-pvt/issues/46)

# Display Spatial Model: Physical Displays as Spatial Objects

## Summary

Physical displays are **spatial entities** -- they have position, orientation, and extent in the real world. Today the runtime treats them as flat metadata on a singleton struct. This design promotes displays into first-class spatial objects that live in Monado's XR space graph, enabling multi-display topology, inter-display transforms, and registration fidelity -- all prerequisites for the Spatial OS (#43) and 3D Shell (#44).

**Prerequisite for:** #43, #44 | **Related:** #34, #45

## Problem Statement

| Capability | Today | Needed for multi-display |
|---|---|---|
| Display identity | None -- anonymous singleton | Named, enumerable, persistent ID |
| Physical geometry | Flat fields on `xrt_system_compositor_info` (`display_width_m`, `display_height_m`) | Per-display struct with full extent + pixel dims |
| Pose in world | Implicit -- display is "in front of user" | Explicit `xrt_pose` per display, updatable |
| Inter-display transform | N/A | `locate_space(display_A, display_B)` via space DAG |
| Display to compositor binding | 1:1, hardcoded | M:N -- multiple displays, each with own compositor pipeline |
| Registration fidelity | N/A | Enum: static-config to co-observation to external-tracking |
| Enumeration API | None | `XR_EXT_display_info` or similar extension |

### Codebase Anchors

- **`xrt_system_compositor_info`** -- `xrt_compositor.h` -- the singleton struct that currently bundles per-display properties (physical dims, pixel dims, screen coords, viewer position, DP factories) alongside compositor-level properties (blend modes, GPU UUIDs, max layers). This needs to be factored apart.
- **`xrt_space_overseer`** -- `xrt_space.h` -- the DAG infrastructure that displays will eventually plug into as spatial nodes.
- **`xrt_tracking_origin`** -- `xrt_tracking.h` -- pattern to follow: named identity + type enum + initial pose, set once by the builder.
- **`create_offset_space`** -- `xrt_space.h` -- the function that will create per-display offset spaces relative to LOCAL.
- **`locate_space`** -- `xrt_space.h` -- enables `locate(display_A, display_B)` queries once displays are spatial nodes.
- **Target builders** -- `target_builder_leia.c`, `target_builder_sim_display.c` -- where `xrt_display` objects get populated at init time.

## Design Principles

1. **Display as spatial entity** -- a display has identity, geometry, pose, capabilities, and factory pointers. It is not a bag of fields on a compositor config struct.
2. **Vendor boundary** -- the `xrt_display` struct is an opaque, driver-populated object. The compositor and state tracker consume it without knowing which SDK or driver produced it.
3. **Anchor in existing Monado abstractions** -- follow the `xrt_tracking_origin` pattern (named, typed, posed, builder-set). Plug into `xrt_space_overseer` rather than inventing a parallel spatial system.
4. **Incremental** -- each stage is independently shippable and testable. Stage 1 is a pure refactor with no behavioral change.

## Staged Plan

### Stage 0 -- Current State (baseline)

Display properties live as flat fields on `xrt_system_compositor_info`:
- Physical dims: `display_width_m`, `display_height_m`
- Pixel dims: `display_pixel_width`, `display_pixel_height`
- Screen coords: `display_screen_left`, `display_screen_top`
- Nominal viewer position: `nominal_viewer_{x,y,z}_m`
- DP factories: `dp_factory_vk`, `dp_factory_d3d11`, `dp_factory_metal`
- Eye tracking modes: `supported_eye_tracking_modes`, `default_eye_tracking_mode`
- Mode switching: `supports_display_mode_switch`

Single display, single compositor, works today.

### Stage 1 -- `xrt_display` Object (pure refactor)

Extract display-specific fields from `xrt_system_compositor_info` into a new `struct xrt_display`:

```c
struct xrt_display
{
    char name[XRT_TRACKING_NAME_LEN];
    xrt_uuid_t id;

    struct {
        float width_m;
        float height_m;
        uint32_t width_pixels;
        uint32_t height_pixels;
    } geometry;

    struct {
        int32_t left;
        int32_t top;
    } screen;

    struct xrt_vec3 nominal_viewer_pos;

    struct {
        float x;
        float y;
    } recommended_view_scale;

    bool supports_display_mode_switch;
    uint32_t supported_eye_tracking_modes;
    uint32_t default_eye_tracking_mode;

    void *dp_factory_vk;
    void *dp_factory_d3d11;
    void *dp_factory_metal;
};
```

`xrt_system_compositor_info` retains a pointer (or embedded instance) of `xrt_display` and drops the duplicated flat fields. All existing consumers updated mechanically.

**Deliverables:**
- New struct definition
- Refactor `xrt_system_compositor_info` to use it
- Update target builders, compositor init, state tracker reads
- Zero behavioral change -- pure data reorganization

### Stage 2 -- Displays as Spaces in the DAG

Each `xrt_display` gets a **pose** and an **`xrt_space`** node:

```c
struct xrt_display
{
    // ... fields from Stage 1 ...

    struct xrt_pose pose;
    struct xrt_space *space;
};
```

- Target builders call `xrt_space_overseer_create_offset_space(xso, local_space, &display->pose, &display->space)` during system init.
- Multi-display: `xrt_system_compositor_info` holds an array: `struct xrt_display displays[XRT_MAX_DISPLAYS]; uint32_t display_count;`
- `locate_space(display_A->space, display_B->space)` returns the inter-display transform.
- Static config loaded from JSON/TOML (display positions, orientations).
- New OpenXR extension `XR_EXT_display_info` to enumerate displays and query their spatial properties.

### Stage 3 -- Registration Fidelity Levels

Not all multi-display setups have the same spatial accuracy. Add a fidelity enum:

```c
enum xrt_display_registration_fidelity
{
    XRT_DISPLAY_REGISTRATION_NONE        = 0, // Unknown / default
    XRT_DISPLAY_REGISTRATION_STATIC      = 1, // User-configured (JSON), centimeter-level
    XRT_DISPLAY_REGISTRATION_COOBSERVED  = 2, // Shared camera/marker, millimeter-level
    XRT_DISPLAY_REGISTRATION_TRACKED     = 3, // External tracking (OptiTrack, etc.), sub-mm
};
```

- Field on `xrt_display`: `enum xrt_display_registration_fidelity registration;`
- The compositor / shell can use this to decide rendering strategy (e.g., blend vs. hard-cut at display edges).
- Static config is the initial target; higher fidelity levels come with #43 infrastructure.

### Stage 4 -- Shared XR Spatial Graph (future, depends on #43)

When the Spatial OS multi-compositor (#43) lands:
- Each compositor instance binds to one (or more) `xrt_display` objects.
- The shared spatial graph holds display spaces alongside device spaces.
- Cross-process `locate_space` queries work through IPC.
- The 3D Shell (#44) uses display spatial data to manage window placement.

## Dependency Graph

```
#23  Lightweight runtime
 |
 +-- #45  Standardize display processor interface
 |
 +-- #46  Display spatial model  <-- THIS DESIGN
 |    |
 |    +-- Stage 1: xrt_display struct (refactor)
 |    +-- Stage 2: displays as spaces in DAG
 |    +-- Stage 3: registration fidelity
 |
 +-- #43  Spatial OS (multi-compositor)
 |    |    depends on #46 Stages 1-2
 |    |
 |    +-- #44  3D Shell
 |              depends on #43 + #46 Stage 2-3
 |
 +-- #34  Remove DISPLAY ref space (DONE)
```

## Milestone Checklist

- [ ] **S1-1** Define `struct xrt_display` (new header or inline in `xrt_compositor.h`)
- [ ] **S1-2** Refactor `xrt_system_compositor_info` to embed/reference `xrt_display`
- [ ] **S1-3** Update target builders (`target_builder_leia.c`, `target_builder_sim_display.c`) to populate `xrt_display`
- [ ] **S1-4** Update compositor init paths to read from `xrt_display`
- [ ] **S1-5** Update state tracker (`oxr_system.c`, `oxr_session.c`) to read from `xrt_display`
- [ ] **S1-6** Verify zero behavioral change -- all existing tests pass
- [ ] **S2-1** Add `pose` and `space` fields to `xrt_display`
- [ ] **S2-2** Create per-display offset spaces in target builders via `create_offset_space`
- [ ] **S2-3** Multi-display array on `xrt_system_compositor_info` (`displays[]`, `display_count`)
- [ ] **S2-4** Static multi-display config file format (JSON or TOML)
- [ ] **S2-5** `XR_EXT_display_info` extension for app-side enumeration
- [ ] **S3-1** Define `xrt_display_registration_fidelity` enum
- [ ] **S3-2** Plumb fidelity into compositor rendering decisions

## Open Questions

1. **New header vs. inline?** -- Should `xrt_display` live in a new `xrt_display.h` or be defined inline in `xrt_compositor.h`? A separate header is cleaner but adds an include dependency.

2. **Display handle type** -- Should displays be reference-counted (`xrt_display_reference`)? Or plain embedded structs like `xrt_tracking_origin`? Embedded is simpler for Stage 1; refcounting may be needed for Stage 4 (dynamic display hotplug).

3. **Session-display binding** -- How does an `XrSession` declare which display(s) it targets? Options include extension on `XrSessionCreateInfo`, automatic assignment by the shell, or both.

4. **Config format** -- JSON (consistent with `openxr_monado-dev.json`) or TOML (more human-friendly for multi-display layout)?

5. **Interaction with #45** -- The display processor factories (`dp_factory_*`) are moving to a standardized interface in #45. The `xrt_display` struct should reference the new interface rather than raw `void*` pointers.
