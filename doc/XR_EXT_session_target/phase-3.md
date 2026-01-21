# XR_EXT_session_target - Phase 3 Implementation Summary

## Overview

Phase 3 implements the **Target Provider Service Pattern** to break the circular library dependency between `comp_main` and `comp_multi`, enabling actual **per-session comp_target and SR weaver creation**.

## Status: Complete

Per-session rendering infrastructure is now fully functional. Each session with an external HWND gets its own:
- `comp_target` (VkSwapchain bound to the window)
- SR weaver (for interlaced 3D output)
- Command pool (for weaver operations)

## Key Achievement

**Problem Solved:** Circular library dependency prevented `comp_multi` from calling target creation functions in `comp_main`.

**Solution:** Service interface pattern where `comp_main` provides a service object to `comp_multi` at initialization time.

```
Before (Blocked):
  comp_main ──links──► comp_multi
  comp_multi ──CANNOT link──► comp_main (circular!)

After (Working):
  comp_main provides service ──► comp_multi uses service callbacks
```

## Architecture

### Target Provider Service Pattern

```
┌─────────────────────────────────────────────────────────────────┐
│  comp_util (shared layer)                                       │
│    └── comp_target_service.h  (interface definition)            │
└─────────────────────────────────────────────────────────────────┘
                    ▲                           ▲
                    │                           │
         implements │                           │ uses
                    │                           │
┌───────────────────┴───────┐     ┌────────────┴──────────────────┐
│  comp_main                 │     │  comp_multi                   │
│    - Creates service       │────►│    - Receives service at init │
│    - Implements callbacks  │     │    - Calls service.create()   │
└────────────────────────────┘     └───────────────────────────────┘
```

### Per-Session Resource Lifecycle

```
Session Created (with external HWND)
         │
         ▼
multi_compositor stores HWND in session_render.external_window_handle
         │
         ▼
First frame render triggers multi_compositor_init_session_render()
         │
         ├──► comp_target_service_create(HWND) ──► comp_target (VkSwapchain)
         │
         └──► leiasr_create(HWND) ──► SR weaver + command pool
         │
         ▼
Per-session resources stored:
  - session_render.target
  - session_render.weaver
  - session_render.weaver_cmd_pool
  - session_render.initialized = true
         │
         ▼
Session Ends (xrDestroySession)
         │
         ├──► leiasr_destroy(weaver)
         ├──► vkDestroyCommandPool(cmd_pool)
         └──► comp_target_service_destroy(target)
```

### Multi-App Architecture

```
┌────────────────────────────────────────────────────────────────────────────┐
│ App creates HWND A                           App creates HWND B            │
│        │                                            │                      │
│        ▼                                            ▼                      │
│  xrCreateSession(HWND A)                     xrCreateSession(HWND B)       │
└────────┬───────────────────────────────────────────┬───────────────────────┘
         │                                           │
         ▼                                           ▼
┌────────────────────────────────────────────────────────────────────────────┐
│  multi_compositor (Session 1)          multi_compositor (Session 2)        │
│    session_render.target ────┐           session_render.target ────┐       │
│    session_render.weaver ─┐  │           session_render.weaver ─┐  │       │
└───────────────────────────│──│─────────────────────────────────│──│────────┘
                            │  │                                 │  │
                            ▼  ▼                                 ▼  ▼
┌────────────────────────────────────────────────────────────────────────────┐
│  comp_target_service (provided by comp_main)                               │
│    create_from_window() ──► comp_window_mswin_create_from_external()       │
│    destroy_target()     ──► comp_target_destroy()                          │
│    get_vk()             ──► vk_bundle for Vulkan operations                │
└────────────────────────────────────────────────────────────────────────────┘
                            │                                 │
                            ▼                                 ▼
┌────────────────────────────────┐     ┌────────────────────────────────────┐
│  Per-Session Pipeline A        │     │  Per-Session Pipeline B            │
│    comp_target (HWND A)        │     │    comp_target (HWND B)            │
│    leiasr weaver (HWND A)      │     │    leiasr weaver (HWND B)          │
│    Interlaced output ──► App A │     │    Interlaced output ──► App B     │
└────────────────────────────────┘     └────────────────────────────────────┘
```

## Files Modified

### New Files

| File | Purpose |
|------|---------|
| `src/xrt/compositor/util/comp_target_service.h` | Service interface for per-session target creation |

### Modified Files

| File | Change |
|------|--------|
| `src/xrt/compositor/CMakeLists.txt` | Added `comp_target_service.h` to comp_util; linked comp_multi to `aux_vk` and `drv_leiasr` |
| `src/xrt/compositor/main/comp_compositor.h` | Added `#include "util/comp_target_service.h"`; added `target_service` field |
| `src/xrt/compositor/main/comp_compositor.c` | Implemented service callbacks; initialized service during compositor creation |
| `src/xrt/compositor/multi/comp_multi_private.h` | Added `target_service` pointer to `multi_system_compositor`; added `weaver_cmd_pool` field |
| `src/xrt/compositor/multi/comp_multi_compositor.c` | Full implementation of `multi_compositor_init_session_render()`; cleanup in end_session/destroy |
| `src/xrt/compositor/multi/comp_multi_system.c` | Set `msc->target_service` from native compositor during creation |

## Service Interface

```c
// src/xrt/compositor/util/comp_target_service.h

struct comp_target_service
{
    // Create a render target from an external window handle
    xrt_result_t (*create_from_window)(struct comp_target_service *service,
                                       void *external_window_handle,
                                       struct comp_target **out_target);

    // Destroy a target created by this service
    void (*destroy_target)(struct comp_target_service *service,
                           struct comp_target **target);

    // Get the Vulkan bundle for GPU operations
    struct vk_bundle *(*get_vk)(struct comp_target_service *service);

    // Opaque context (comp_compositor*)
    void *context;
};

// Convenience wrappers
static inline xrt_result_t
comp_target_service_create(struct comp_target_service *service,
                           void *external_window_handle,
                           struct comp_target **out_target);

static inline void
comp_target_service_destroy(struct comp_target_service *service,
                            struct comp_target **target);

static inline struct vk_bundle *
comp_target_service_get_vk(struct comp_target_service *service);
```

## Per-Session Resource Creation

```c
// src/xrt/compositor/multi/comp_multi_compositor.c

bool
multi_compositor_init_session_render(struct multi_compositor *mc)
{
    // Check prerequisites
    if (mc->session_render.initialized) return true;
    if (mc->session_render.external_window_handle == NULL) return false;
    if (mc->msc->target_service == NULL) return false;

    // Create per-session comp_target using the service
    xrt_result_t ret = comp_target_service_create(
        mc->msc->target_service,
        mc->session_render.external_window_handle,
        &mc->session_render.target);

    if (ret != XRT_SUCCESS) return false;

#ifdef XRT_HAVE_LEIA_SR
    // Create per-session SR weaver
    struct vk_bundle *vk = comp_target_service_get_vk(mc->msc->target_service);

    // Create command pool for weaver
    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = vk->main_queue->family_index,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
    };

    VkCommandPool cmd_pool;
    vk->vkCreateCommandPool(vk->device, &pool_info, NULL, &cmd_pool);

    ret = leiasr_create(1000.0, vk->device, vk->physical_device,
                        vk->main_queue->queue, cmd_pool,
                        mc->session_render.external_window_handle,
                        &mc->session_render.weaver);

    mc->session_render.weaver_cmd_pool = cmd_pool;
#endif

    mc->session_render.initialized = true;
    return true;
}
```

## Cleanup Implementation

```c
// In multi_compositor_end_session() and multi_compositor_destroy()

if (mc->session_render.initialized) {
#ifdef XRT_HAVE_LEIA_SR
    // Destroy per-session SR weaver
    if (mc->session_render.weaver != NULL) {
        leiasr_destroy(mc->session_render.weaver);
        mc->session_render.weaver = NULL;
    }

    // Destroy the command pool
    if (mc->session_render.weaver_cmd_pool != VK_NULL_HANDLE) {
        struct vk_bundle *vk = comp_target_service_get_vk(mc->msc->target_service);
        if (vk != NULL) {
            vk->vkDestroyCommandPool(vk->device, mc->session_render.weaver_cmd_pool, NULL);
        }
        mc->session_render.weaver_cmd_pool = VK_NULL_HANDLE;
    }
#endif

    // Destroy per-session target using the service
    if (mc->session_render.target != NULL && mc->msc->target_service != NULL) {
        comp_target_service_destroy(mc->msc->target_service, &mc->session_render.target);
    }

    mc->session_render.initialized = false;
}
```

## Build Configuration

```cmake
# src/xrt/compositor/CMakeLists.txt

# comp_util now includes the service header
add_library(comp_util STATIC
    ...
    util/comp_target_service.h
    ...
)

# comp_multi links to Vulkan helpers and SR driver
target_link_libraries(comp_multi
    PUBLIC xrt-interfaces
    PRIVATE aux_util aux_os aux_vk
)

# Conditional SR weaver support
if(XRT_HAVE_LEIA_SR)
    target_compile_definitions(comp_multi PRIVATE XRT_HAVE_LEIA_SR)
    target_link_libraries(comp_multi PRIVATE drv_leiasr)
endif()
```

## Testing

### Verification Steps

1. **Build verification:**
   ```bash
   cmake --build build --config Release
   ```

2. **Log verification:**
   - Look for "Created per-session comp_target for HWND"
   - Look for "Created per-session SR weaver for HWND"
   - Look for "Initialized per-session render resources for HWND"

3. **Resource verification:**
   - Session start creates target + weaver
   - Session end destroys them cleanly
   - No resource leaks on repeated session create/destroy

### Current Test Results

- ✅ Single app with external HWND: Creates per-session resources
- ✅ Session registration: Per-session target and weaver created
- ✅ Session cleanup: Resources properly destroyed
- ⏳ Multiple apps simultaneously: Infrastructure ready, render pipeline pending (Phase 4)

## Summary

Phase 3 completes the infrastructure for per-session rendering:

| Feature | Status |
|---------|--------|
| Service interface in comp_util | ✅ Complete |
| Service implementation in comp_main | ✅ Complete |
| Service passed to comp_multi | ✅ Complete |
| Per-session comp_target creation | ✅ Complete |
| Per-session SR weaver creation | ✅ Complete |
| Per-session resource cleanup | ✅ Complete |
| CMake build configuration | ✅ Complete |
| Build verification (Windows CI) | ✅ Passed |

## Next Steps (Phase 4)

Phase 4 will implement the **per-session render pipeline**:

1. **Expose stereo views from comp_renderer** - Add accessor to get rendered left/right views
2. **Add per-session weave pass** - After shared render, weave to each session's window
3. **Handle frame timing** - Each session may have different present timing
4. **Test multi-app** - Run multiple XR apps with independent 3D output

## References

- [phase-1.md](phase-1.md) - Single app external HWND implementation
- [phase-2.md](phase-2.md) - Per-session infrastructure and data structures
- [app-control.md](../app-control.md) - Full architectural design document
