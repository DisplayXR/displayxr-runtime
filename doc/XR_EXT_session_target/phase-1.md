# XR_EXT_session_target - Phase 1 Implementation Summary

## Overview

Phase 1 implements the **XR_EXT_session_target** OpenXR extension, enabling applications to provide their own window handle (HWND on Windows) to the runtime. This allows SR Runtime to operate in **windowed mode** instead of fullscreen, solving input routing issues and enabling multi-app support.

## Key Achievement

**Before:** `CreateSRWeaver(..., NULL, ...)` = fullscreen mode (runtime owns display)
**After:** `CreateSRWeaver(..., hwnd, ...)` = windowed mode (app owns window)

## Benefits

| Feature | Before | After |
|---------|--------|-------|
| Input routing | Runtime captures input | App receives keyboard/mouse/gamepad directly |
| Multi-app | One XR app at a time | Multiple simultaneous XR apps |
| Desktop integration | Exclusive fullscreen | Windowed, works with other apps |
| Window lifecycle | Runtime manages | App controls |

## Architecture

```
App creates HWND
       │
       ▼
xrCreateSession + XR_EXT_session_target(HWND)
       │
       ├──► comp_target (VkSwapchain from HWND)
       │
       └──► SR Weaver (windowed mode, bound to HWND)
                  │
                  ▼
         Interlaced 3D output to app's window
                  │
App receives input ◄─┘
```

## Files Modified

### New Files
| File | Purpose |
|------|---------|
| `src/external/openxr_includes/openxr/XR_EXT_session_target.h` | Extension definition header |

### Modified Files

| File | Change |
|------|--------|
| `src/xrt/include/xrt/xrt_compositor.h` | Added `external_window_handle` to `xrt_session_info` |
| `src/xrt/include/xrt/xrt_openxr_includes.h` | Include new extension header |
| `src/xrt/drivers/leiasr/leiasr.h` | Added `windowHandle` parameter to `leiasr_create()` |
| `src/xrt/drivers/leiasr/leiasr.cpp` | Pass `windowHandle` to `CreateSRWeaver()` |
| `src/xrt/compositor/main/comp_window.h` | Declared `comp_window_mswin_create_from_external()` |
| `src/xrt/compositor/main/comp_window_mswin.c` | Added `owns_window` flag, external window creation |
| `src/xrt/compositor/main/comp_compositor.h` | Added `external_window_handle` field |
| `src/xrt/compositor/main/comp_compositor.c` | Handle external HWND in `compositor_begin_session()` |
| `src/xrt/compositor/main/comp_renderer.c` | Pass window handle to `leiasr_create()` |
| `src/xrt/compositor/multi/comp_multi_private.h` | Added `external_window_handle` to `multi_system_compositor` |
| `src/xrt/compositor/multi/comp_multi_compositor.c` | Bridge HWND from session to native compositor |
| `src/xrt/state_trackers/oxr/oxr_session.c` | Parse `XR_EXT_session_target` extension |

## Extension API

```c
#define XR_EXT_session_target 1
#define XR_TYPE_SESSION_TARGET_CREATE_INFO_EXT ((XrStructureType)1000999001)

typedef struct XrSessionTargetCreateInfoEXT {
    XrStructureType             type;           // XR_TYPE_SESSION_TARGET_CREATE_INFO_EXT
    const void* XR_MAY_ALIAS    next;
    void*                       windowHandle;   // HWND on Windows
} XrSessionTargetCreateInfoEXT;
```

## Usage Example

```c
// App creates its own window
HWND myWindow = CreateWindowEx(...);

// Pass window to OpenXR via extension
XrSessionTargetCreateInfoEXT targetInfo = {
    .type = XR_TYPE_SESSION_TARGET_CREATE_INFO_EXT,
    .windowHandle = myWindow,
};

XrSessionCreateInfo sessionInfo = {
    .type = XR_TYPE_SESSION_CREATE_INFO,
    .next = &targetInfo,
    .systemId = systemId,
};

xrCreateSession(instance, &sessionInfo, &session);

// App's message pump receives input directly
while (running) {
    while (PeekMessage(&msg, myWindow, 0, 0, PM_REMOVE)) {
        // Handle keyboard/mouse/gamepad input
    }
    // Standard OpenXR frame loop...
}
```

## Data Flow

```
┌─────────────────────────────────────────────────────────────────────┐
│  oxr_session.c:oxr_session_create()                                 │
│    - Parses XrSessionTargetCreateInfoEXT                            │
│    - Stores HWND in xrt_session_info.external_window_handle         │
└─────────────────────────────────────┬───────────────────────────────┘
                                      │
                                      ▼
┌─────────────────────────────────────────────────────────────────────┐
│  comp_multi_compositor.c:multi_compositor_create()                  │
│    - Stores mc->xsi = *xsi (including external_window_handle)       │
└─────────────────────────────────────┬───────────────────────────────┘
                                      │
                                      ▼
┌─────────────────────────────────────────────────────────────────────┐
│  comp_multi_compositor.c:multi_compositor_begin_session()           │
│    - Detects first session with external HWND                       │
│    - Passes to comp_compositor.external_window_handle               │
└─────────────────────────────────────┬───────────────────────────────┘
                                      │
                                      ▼
┌─────────────────────────────────────────────────────────────────────┐
│  comp_compositor.c:compositor_begin_session() (deferred surface)    │
│    - compositor_init_window_from_external(HWND)                     │
│    - comp_window_mswin_create_from_external() → comp_target         │
│    - compositor_init_renderer() → leiasr_create(HWND)               │
└─────────────────────────────────────────────────────────────────────┘
```

## Testing

To test this implementation:

1. Create a native Win32 app that:
   - Creates its own HWND
   - Initializes OpenXR with `XR_EXT_session_target` extension
   - Passes HWND in `XrSessionTargetCreateInfoEXT`
   - Runs frame loop submitting projection layers

2. Verify:
   - App window receives keyboard/mouse input
   - 3D interlaced output appears in app window
   - No separate Monado window is created
   - SR Runtime logs confirm windowed mode active

## Next Steps (Phase 2)

Phase 2 will implement **per-session SR Weaver** for true multi-app support:
- Move SR weaver from native compositor to per-session (in `multi_compositor` struct)
- Each session gets its own weaver bound to its own window
- Multiple XR apps can run simultaneously with independent 3D output

## References

- [app-control.md](app-control.md) - Full architectural design document
- SR Runtime documentation for windowed mode (runtime >= 1.34)
