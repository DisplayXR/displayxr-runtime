# In-Process vs Service Compositor Architecture

This document explains the architectural differences between the **in-process (native app)** and **service/IPC (WebXR/shell)** compositor pipelines in the DisplayXR runtime.

> For the high-level view of what ships, what runs, and when each path activates, see [Production Components](production-components.md). This document covers the D3D11 implementation details.

## Overview

| Aspect | In-Process (Native) | Service/IPC (WebXR or Shell) |
|--------|---------------------|------------------------------|
| **Compositor** | `comp_d3d11_compositor` | `d3d11_service_compositor` |
| **Process Model** | Single process | N+1 processes (one or more IPC clients — Chrome tab, shell-launched apps — plus `displayxr-service`) |
| **D3D11 Device** | App's device (shared) | Service's own device |
| **Swapchain Textures** | Local textures | Cross-process shared (NT handles + KeyedMutex) |
| **View Poses** | Direct from compositor | Via IPC with SR-aware poses from server |
| **Eye Tracking** | Compositor queries SR weaver | IPC server queries SR weaver |
| **Session Events** | Direct callbacks | IPC message queue |

---

## Process Architecture

### In-Process (Native Apps)

```
┌──────────────────────────────────────────────────────────────┐
│                     Native OpenXR App                        │
│  ┌─────────────┐    ┌──────────────────────────────────────┐ │
│  │ App Code    │───▶│ OpenXR State Tracker (oxr_session.c) │ │
│  │             │    │                                      │ │
│  │ Uses app's  │    │ Uses app's D3D11 device              │ │
│  │ D3D11 device│    └────────────────┬─────────────────────┘ │
│  └─────────────┘                     │                       │
│                                      ▼                       │
│              ┌───────────────────────────────────────────┐   │
│              │     comp_d3d11_compositor                 │   │
│              │  - Uses app's D3D11 device (AddRef)       │   │
│              │  - Creates local swapchains               │   │
│              │  - Owns SR weaver                         │   │
│              │  - Renders to output window               │   │
│              └───────────────────────────────────────────┘   │
└──────────────────────────────────────────────────────────────┘
```

**Key Points:**
- Everything runs in the same process
- App provides D3D11 device via `XrGraphicsBindingD3D11KHR`
- Compositor adds a reference to app's device
- Swapchain textures are local (no cross-process sharing needed)
- Direct access to SR weaver for eye tracking

### Service/IPC (WebXR or Shell)

The IPC plumbing is identical whether the client is a Chrome tab or an app launched by the shell — the same `ipc_client_compositor`, the same shared-texture protocol, the same service compositor. The differences are who connects, how many, and under what security context. The Chrome case is shown first; shell-mode differences are called out below.

```
┌─────────────────────────────────────┐     ┌─────────────────────────────────────┐
│          Chrome Process             │     │        displayxr-service Process       │
│  ┌──────────────────────────────┐   │     │   ┌─────────────────────────────┐   │
│  │   WebXR JavaScript API       │   │     │   │   IPC Server Handler        │   │
│  └──────────────┬───────────────┘   │     │   └──────────────┬──────────────┘   │
│                 ▼                   │     │                  ▼                  │
│  ┌──────────────────────────────┐   │     │   ┌─────────────────────────────┐   │
│  │ Chrome's OpenXR Backend      │   │     │   │   d3d11_service_system      │   │
│  │ (ipc_client_compositor)      │◀──┼─IPC─┼──▶│   (xrt_system_compositor)   │   │
│  │                              │   │     │   │                             │   │
│  │ - Has Chrome's D3D11 device  │   │     │   │ - Owns service D3D11 device │   │
│  │ - Imports swapchain textures │   │     │   │ - Creates shared swapchains │   │
│  │ - Submits layers via IPC     │   │     │   │ - Owns SR weaver            │   │
│  └──────────────────────────────┘   │     │   │ - Renders to output window  │   │
│                                     │     │   └─────────────────────────────┘   │
│  Swapchain textures imported via    │     │   Swapchain textures created with   │
│  OpenSharedResource1 (NT handle)    │     │   SHARED_KEYEDMUTEX + SHARED_NTHANDLE│
└─────────────────────────────────────┘     └─────────────────────────────────────┘
```

**Key Points:**
- Two separate processes with different D3D11 devices
- Service creates its own D3D11 device (not app's)
- Swapchains must be shared via NT handles
- KeyedMutex synchronizes cross-process access
- IPC protocol handles all communication

#### Differences in shell mode

```
┌─────────────────────────┐     ┌────────────────────────────────────────────┐
│   displayxr-shell       │─IPC─│           displayxr-service                │
│  (privileged client)    │     │                                            │
└─────────────────────────┘     │   d3d11_service_system                     │
┌─────────────────────────┐     │   - Multi-compositor (N clients → 1 out)   │
│  3D App #1 (IPC client) │─IPC─│   - Owns SR weaver                         │
└─────────────────────────┘     │   - Renders combined output                │
┌─────────────────────────┐     │                                            │
│  3D App #2 (IPC client) │─IPC─│                                            │
└─────────────────────────┘     └────────────────────────────────────────────┘
```

- **N clients, not one.** The service runs in multi-compositor mode and composites every connected client (shell plus one or more 3D apps) into a single output.
- **Not sandboxed.** Shell-launched apps run with the user's normal token — no AppContainer. `ipc_server_mainloop_windows.cpp` still sets a named-pipe DACL that grants access to both the normal user SID and AppContainer, so the same pipe works for both clients. The shared-handle `SECURITY_ATTRIBUTES` for Chrome adds a `ALL APPLICATION PACKAGES` ACE; shell-launched apps don't need that ACE but don't reject it either.
- **Shell is a privileged IPC client.** Beyond the standard frame path, the shell sends window pose, focus, layout, and 2D-capture commands over the shell↔service control channel (see [shell-runtime-contract.md](../roadmap/shell-runtime-contract.md)).
- **Mode gate.** `DISPLAYXR_SHELL_SESSION=1` is the flag the shell sets in the environment of every app it launches; the runtime DLL sees it in `u_sandbox_should_use_ipc()` and routes to IPC even though the process isn't sandboxed.

---

## Swapchain Creation and Sharing

### In-Process (Native)

```cpp
// comp_d3d11_swapchain.cpp
struct comp_d3d11_swapchain {
    struct xrt_swapchain_native base;
    ID3D11Texture2D *images[MAX_SWAPCHAIN_IMAGES];  // Local textures
    ID3D11ShaderResourceView *srvs[...];            // Local SRVs
    ID3D11RenderTargetView *rtvs[...];              // Local RTVs
};

// Creation: Simple local texture
D3D11_TEXTURE2D_DESC desc = {};
desc.MiscFlags = 0;  // No sharing needed
app_device->CreateTexture2D(&desc, nullptr, &texture);
```

- Textures are local to the process
- No cross-process synchronization needed
- App renders directly to compositor's textures

### Service/IPC (WebXR or Shell)

```cpp
// comp_d3d11_service.cpp
struct d3d11_service_swapchain {
    struct xrt_swapchain_native base;  // Contains shared handles for IPC
    struct d3d11_service_image images[...];
    // Each image has:
    //   - texture (ID3D11Texture2D)
    //   - srv (ID3D11ShaderResourceView)
    //   - keyed_mutex (IDXGIKeyedMutex) - for sync
    bool service_created;  // true = created by service
};

// Creation: Shared texture with NT handle
D3D11_TEXTURE2D_DESC desc = {};
desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX |  // Cross-process sync
                 D3D11_RESOURCE_MISC_SHARED_NTHANDLE;      // Real kernel handle

service_device->CreateTexture2D(&desc, nullptr, &texture);

// Get NT handle for IPC transfer
IDXGIResource1* resource;
resource->CreateSharedHandle(
    &security_attrs,  // DACL includes ALL APPLICATION PACKAGES so Chrome's
                      // AppContainer can open the handle; shell-launched apps
                      // inherit access via the user SID in the same DACL.
    DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
    nullptr,
    &shared_handle    // Sent to the IPC client (Chrome tab or shell app) via IPC
);
```

**IPC Flow:**
1. Chrome calls `xrCreateSwapchain()`
2. IPC client sends request to service
3. Service creates shared texture with NT handle
4. Service returns handle via `DuplicateHandle()` to Chrome's process
5. Chrome imports via `OpenSharedResource1(handle)`
6. KeyedMutex coordinates access:
   - Chrome acquires mutex, renders, releases
   - Service acquires mutex, composites, releases

---

## View Pose Pipeline

### In-Process (Native)

```
App calls xrLocateViews()
        │
        ▼
oxr_session_locate_views()
        │
        ▼
comp_d3d11_compositor → SR weaver → leiasr_d3d11_get_predicted_eye_positions()
        │                                      │
        │                                      ▼
        │                          Eye positions with depth (z=0.6m)
        │                          {-0.032, 0, 0.6} / {0.032, 0, 0.6}
        ▼
View poses returned directly to app
```

**Native compositor** (`comp_d3d11_compositor.cpp`, eye position query in `comp_d3d11_get_eye_positions()`):
```cpp
// Get predicted eye positions from SR weaver
struct xrt_vec3 left_eye = {-0.032f, 0.0f, 0.6f};   // Default fallback
struct xrt_vec3 right_eye = {0.032f, 0.0f, 0.6f};

#ifdef XRT_HAVE_LEIA_SR_D3D11
if (c->weaver != nullptr) {
    float left[3], right[3];
    if (leiasr_d3d11_get_predicted_eye_positions(c->weaver, left, right)) {
        left_eye = {left[0], left[1], left[2]};   // Live eye tracking!
        right_eye = {right[0], right[1], right[2]};
    }
}
#endif
```

### Service/IPC (WebXR or Shell) — With SR Support

```
Chrome calls xrLocateViews()
        │
        ▼ (IPC)
ipc_client_hmd_get_view_poses()
        │
        ▼ (to service)
ipc_handle_device_get_view_poses_2()
        │
        ▼
ipc_try_get_sr_view_poses() ─────────────────────┐
        │                                         │
        ▼                                         ▼
comp_d3d11_service_get_predicted_eye_positions() + ipc_compute_kooima_fov()
        │                                         │
        ▼                                         ▼
SR weaver eye tracking data            Kooima asymmetric FOV from eye positions
        │                                         │
        └────────────────┬────────────────────────┘
                         ▼
              + qwerty device pose (player transform)
                         │
                         ▼
              SR-aware view poses with proper depth
                         │
        ▼ (IPC response)
Chrome receives SR-aware poses + Kooima FOV
        │
        ▼
App renders with correct 3D perspective
```

**IPC Server SR Integration** (`ipc_server_handler.c`):
```cpp
// ipc_handle_device_get_view_poses_2() now tries SR-aware poses first
#if defined(XRT_HAVE_LEIA_SR_D3D11) && defined(XRT_HAVE_D3D11_SERVICE_COMPOSITOR)
if (ipc_try_get_sr_view_poses(ics->server, xdev, default_eye_relation, at_timestamp_ns,
                               view_count, &out_info->head_relation, out_info->fovs, out_info->poses)) {
    return XRT_SUCCESS;  // Used SR-aware poses
}
#endif
// Fall back to qwerty device if SR not available
```

The `ipc_try_get_sr_view_poses()` function:
1. Gets eye positions from SR weaver via `comp_d3d11_service_get_predicted_eye_positions()`
2. Gets display dimensions via `comp_d3d11_service_get_display_dimensions()`
3. Computes Kooima asymmetric FOV from eye positions
4. Gets qwerty device pose as "player transform" (WASD movement)
5. Combines everything into SR-aware view poses

---

## Eye Position / Convergence

### The Z-Depth Problem

For proper 3D display convergence, eye positions need **depth (z value)**:

| Component | Eye Position Format | Issue |
|-----------|---------------------|-------|
| SR Weaver | `{±0.032, 0, 0.6}` | Has depth (60cm from screen) |
| Native compositor | Uses SR weaver values | Correct |
| Service compositor (before fix) | `{±0.032, 0, 0}` | No depth! |
| Qwerty device | Identity pose | No eye offset at all |

**Result:** WebXR cameras point at infinity instead of converging at the screen plane.

### Correct Eye Position Flow (Implemented)

Eye tracking now flows to Chrome via IPC:

```
SR Weaver (eye tracking camera)
        │
        ▼
leiasr_d3d11_get_predicted_eye_positions()
        │
        ▼
Returns: left={-0.032, y, 0.6}, right={0.032, y, 0.6}
        │
        ├──────────────────────────────────────┐
        ▼                                      ▼
Service Compositor                       IPC Server Handler
(for UI layer rendering)                 (ipc_try_get_sr_view_poses)
                                               │
                                               ▼
                                         Chrome via IPC
                                         (xrLocateViews result)
```

Chrome now receives SR-aware poses with proper depth and Kooima FOV.

---

## Session State Events

### In-Process (Native)

Session state changes are delivered directly:
```cpp
// oxr_session.c
xrt_session_event_sink *xses = sess->event_sink;
// Events pushed directly to session's event queue
```

### Service/IPC (WebXR or Shell)

Session state must be communicated via IPC:
```cpp
// ipc_client_compositor.c
struct ipc_client_compositor {
    bool initial_visible;   // State from server at creation
    bool initial_focused;   // Avoids race condition
};

// Server pushes events via IPC message queue
// Client polls for events in xrPollEvent()
```

**Race Condition Fix:** The `initial_visible` and `initial_focused` fields were added to avoid the race where the IPC client (Chrome tab or shell-launched app) might miss the initial `XR_SESSION_STATE_VISIBLE/FOCUSED` events if they're sent before its event loop starts polling.

---

## D3D11 Device Ownership

### In-Process (Native)

```cpp
struct comp_d3d11_compositor {
    ID3D11Device *device;    // App's device (AddRef'd)
    ID3D11DeviceContext *context;
    // We share the app's device
};

// At creation:
app_device->AddRef();
c->device = app_device;
```

### Service/IPC (WebXR or Shell)

```cpp
struct d3d11_service_system {
    wil::com_ptr<ID3D11Device5> device;  // Service's OWN device
    wil::com_ptr<ID3D11DeviceContext4> context;
    // Completely separate from every IPC client's device
};

// At creation:
D3D11CreateDevice(..., &device);  // New device
```

**Why separate devices?**
- Cross-process GPU work requires separate devices either way (the client's and the service's are in different processes)
- The service needs a device in *its own* process to own the SR weaver and render the final composite
- With multiple IPC clients (shell mode), a shared "app device" wouldn't make sense — there is no single app
- Chrome adds an extra constraint: its device lives in an AppContainer with potentially different feature levels, so the service device must be independent

---

## SR Weaver Integration

### In-Process (Native)

```cpp
struct comp_d3d11_compositor {
    struct leiasr_d3d11 *weaver;  // Owned by compositor
};

// Used for:
// 1. Eye tracking (leiasr_d3d11_get_predicted_eye_positions)
// 2. Display dimensions (leiasr_d3d11_get_display_dimensions)
// 3. Final weaving (leiasr_d3d11_weave)
// 4. Swap chain resize handling
```

### Service/IPC (WebXR or Shell)

```cpp
struct d3d11_service_system {
    struct leiasr_d3d11 *weaver;  // Owned by system compositor
};

// Used for:
// 1. Display dimensions (at initialization)
// 2. Eye tracking for UI layers (after fix)
// 3. Final weaving
// 4. Window management
```

---

## Summary of Key Differences

| Feature | In-Process | Service/IPC |
|---------|------------|-------------|
| Swapchain textures | Local | Shared (NT handles + KeyedMutex) |
| D3D11 device | App's (shared) | Service's own |
| View poses source | SR weaver via compositor | SR weaver via IPC server |
| Eye tracking | Direct compositor query | IPC server queries, sends to client |
| FOV computation | `oxr_session_locate_views()` | `ipc_try_get_sr_view_poses()` |
| Player transform | Qwerty device in-process | Qwerty device on server side (WebXR); shell-owned and forwarded to focused app (shell mode) |
| Session events | Direct callback | IPC message queue |
| Window ownership | App or compositor | Service's window |
| Process count | 1 | 2 (WebXR: Chrome + service) or N+1 (shell: shell + N apps + service) |
| GPU sync | Local barriers | KeyedMutex (cross-process) |

---

## Current Limitations (IPC Paths)

1. **Double compositing:** The IPC client renders to shared textures, then the service compositor re-renders to output. This adds latency vs native in-process apps. Affects both WebXR and shell-launched apps.

2. **Security constraints (WebXR only):** Chrome's AppContainer sandbox requires shared-handle `SECURITY_ATTRIBUTES` with a DACL that grants `ALL APPLICATION PACKAGES`. Shell-launched apps run under the normal user token and don't need this, but the service applies the same permissive DACL uniformly.

3. **QWERTY keyboard control:**
   - **WebXR path:** the qwerty device lives in the service and drives the player transform; it requires the service window to have focus.
   - **Shell path:** the qwerty device is owned by the shell, not by individual apps. The shell forwards input to the focused app window; apps receive window-relative eye poses but no qwerty device of their own.

---

## Implementation Files

### In-Process Path
- `src/xrt/state_trackers/oxr/oxr_session.c` - `oxr_session_locate_views()` with SR eye tracking
- `src/xrt/compositor/d3d11/comp_d3d11_compositor.cpp` - Native D3D11 compositor

### Service/IPC Path
- `src/xrt/ipc/server/ipc_server_handler.c` - `ipc_try_get_sr_view_poses()`, `ipc_compute_kooima_fov()`
- `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` - Service compositor with SR helper functions
- `src/xrt/ipc/client/ipc_client_hmd.c` - Client-side view pose IPC
