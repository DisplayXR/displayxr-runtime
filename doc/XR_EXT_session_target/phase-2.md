# XR_EXT_session_target - Phase 2 Implementation Summary

## Overview

Phase 2 adds the **infrastructure** for per-session rendering, enabling tracking of multiple OpenXR applications that can run simultaneously with their own windows.

## Status: Infrastructure Complete, Resource Creation Deferred

The data structures for per-session rendering are in place. Actual per-session comp_target and SR weaver creation is deferred to Phase 3 due to architectural constraints (circular library dependencies between comp_multi and comp_main).

## What Was Implemented

### 1. Per-Session Render Data Structure in multi_compositor

Added new fields to `struct multi_compositor` for per-session rendering tracking:

```c
struct {
    void *external_window_handle;    // HWND from XR_EXT_session_target
    struct comp_target *target;       // Per-session VkSwapchain (Phase 3)
    struct leiasr *weaver;           // Per-session SR weaver (Phase 3)
    bool initialized;                 // Registration flag
} session_render;
```

### 2. Session Registration for Per-Session Rendering

Added `multi_compositor_init_session_render()` function that:
- Checks if session has external HWND
- Marks session as registered for per-session rendering
- Logs registration for debugging
- Called lazily on first frame render

### 3. Per-Session Cleanup Hooks

Modified `multi_compositor_end_session()` with cleanup hooks:
- Resets initialization flag
- Clears external window handle
- Ready for Phase 3 resource cleanup

### 4. Render Loop Integration

Modified `transfer_layers_locked()` to:
- Check for sessions with external window handles
- Call registration function on first frame
- Maintain backward compatibility with shared rendering (Phase 1)

## Files Modified

| File | Change |
|------|--------|
| `src/xrt/compositor/multi/comp_multi_private.h` | Added session_render struct, helper functions |
| `src/xrt/compositor/multi/comp_multi_compositor.c` | Added registration/cleanup for per-session |
| `src/xrt/compositor/multi/comp_multi_system.c` | Lazy registration in render loop |

## Architecture

### Current Flow (Phase 1 + Phase 2 Infrastructure)

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  Session 1 (with external HWND)                                             │
│    - Layers submitted to multi_compositor                                   │
│    - session_render.external_window_handle stored                           │
│    - session_render.initialized = true (registered)                         │
└─────────────────────────────────────┬───────────────────────────────────────┘
                                      │
                                      ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│  Multi-System Compositor (render loop)                                      │
│    - Collects layers from all sessions                                      │
│    - Currently: all layers go to shared native compositor                   │
│    - Sessions tracked for future per-session pipeline (Phase 3)             │
└─────────────────────────────────────┬───────────────────────────────────────┘
                                      │
                                      ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│  Native Compositor (comp_compositor)                                        │
│    - Renders all layers                                                     │
│    - Uses shared weaver (from Phase 1)                                      │
│    - Outputs to first session's window                                      │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Target Flow (Phase 3)

```
┌──────────────────────────┐    ┌──────────────────────────┐
│  Session 1               │    │  Session 2               │
│  (external HWND A)       │    │  (external HWND B)       │
│  - session_render.target │    │  - session_render.target │
│  - session_render.weaver │    │  - session_render.weaver │
└────────────┬─────────────┘    └────────────┬─────────────┘
             │                               │
             ▼                               ▼
┌──────────────────────────┐    ┌──────────────────────────┐
│  Per-Session Pipeline    │    │  Per-Session Pipeline    │
│  - Render session layers │    │  - Render session layers │
│  - Weave to HWND A      │    │  - Weave to HWND B      │
└──────────────────────────┘    └──────────────────────────┘
```

## Architectural Constraint Discovered

### Circular Library Dependency

During Phase 2 implementation, we discovered that `comp_multi` cannot directly call functions from `comp_main`:

- `comp_main` links to `comp_multi` (for multi-client support)
- `comp_multi` cannot link to `comp_main` (would create circular dependency)
- Functions like `comp_window_mswin_create_from_external()` and `comp_target_destroy()` are in `comp_main`

### Resolution for Phase 3

Options to resolve this for Phase 3:

1. **Move target creation to comp_main** - Have the native compositor handle per-session target creation through a callback interface
2. **Factor out shared code** - Create a `comp_target_factory` library that both can use
3. **Interface abstraction** - Add target creation to xrt_compositor interface

## Remaining Work for Full Multi-App Support (Phase 3)

### 1. Per-Session Target Creation

Implement one of the architectural solutions above to enable:
- Per-session comp_target from HWND
- Per-session SR weaver bound to window

### 2. Per-Session Render Pipeline

Refactor `comp_renderer` to support:
- Stereo view extraction
- Per-session weave pass
- Per-session present

### 3. Frame Timing

Handle per-session timing:
- Each session may have different present timing
- Synchronize with app's window refresh

## Testing

### Current State (Phase 2 Infrastructure)
- Single app with external HWND: Works (Phase 1 behavior)
- Per-session registration: Yes (logged)
- Multiple apps simultaneously: Not yet (renders to first app only)

### To Verify Phase 2 Infrastructure
1. Run app with external HWND
2. Check logs for "Session registered for per-session rendering with HWND"
3. On session end, check logs for "Cleaned up per-session render resources"

## Backward Compatibility

- Sessions WITHOUT external HWND: Use shared native compositor (unchanged)
- Sessions WITH external HWND: Registered for per-session rendering, but currently still rendered through shared pipeline (Phase 1 behavior preserved)

## Summary

Phase 2 establishes the data structure infrastructure for per-session rendering:
- ✅ Per-session data structure added to multi_compositor
- ✅ Session registration on first frame
- ✅ Cleanup hooks on session end
- ✅ Helper functions for checking per-session capability
- ⏳ Per-session comp_target creation (Phase 3 - requires architecture work)
- ⏳ Per-session SR weaver creation (Phase 3 - requires architecture work)
- ⏳ Per-session render pipeline (Phase 3 - requires comp_renderer refactoring)

The tracking infrastructure is in place. Phase 3 will address the library architecture to enable actual per-session resource creation.
