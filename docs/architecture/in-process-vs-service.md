# In-Process vs Service Compositor Architecture

> **Partially superseded (2026-08-16).** The service-side internals described
> here predate the vendor plug-in extraction ([ADR-019](../adr/ADR-019-vendor-plugin-aux-boundary.md))
> and the #925 stabilisation. The authoritative map of the service — processes,
> threads, locks, the two compositor modes, client classes, and failure domains
> — is [service-architecture.md](service-architecture.md). Vendor symbols named
> below (`leiasr_d3d11_*`, `XRT_HAVE_LEIA_SR_D3D11`, `struct leiasr_d3d11 *weaver`,
> `ipc_compute_kooima_fov`) no longer exist: the display processor is reached
> only through the vendor-neutral `xrt_display_processor_*` vtables, and the
> Kooima math lives in `displayxr-common`.

This document explains the architectural differences between the **in-process (native app)** and **service/IPC (WebXR/shell)** compositor pipelines in the DisplayXR runtime.

> For the high-level view of what ships, what runs, and when each path activates, see [Production Components](production-components.md). This document covers the D3D11 implementation details.

## Overview

| Aspect | In-Process (Native) | Service/IPC (WebXR or Shell) |
|--------|---------------------|------------------------------|
| **Compositor** | `comp_d3d11_compositor` | `d3d11_service_compositor` |
| **Process Model** | Single process | N+1 processes (one or more IPC clients — Chrome tab, shell-launched apps — plus `displayxr-service`) |
| **D3D11 Device** | App's device (shared) | Service's own device |
| **Swapchain Textures** | Local textures | Cross-process shared (NT handles + KeyedMutex; the D3D11 workspace leg now also uses a shared fence, `workspace_sync_fence`) |
| **View Poses** | Direct from compositor | Via IPC with tracking-aware poses from server |
| **Eye Tracking** | Compositor queries vendor weaver | IPC server queries vendor weaver |
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
│              │  - Owns vendor weaver                         │   │
│              │  - Renders to output window               │   │
│              └───────────────────────────────────────────┘   │
└──────────────────────────────────────────────────────────────┘
```

**Key Points:**
- Everything runs in the same process
- App provides D3D11 device via `XrGraphicsBindingD3D11KHR`
- Compositor adds a reference to app's device
- Swapchain textures are local (no cross-process sharing needed)
- Direct access to vendor weaver for eye tracking

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
│  │ - Submits layers via IPC     │   │     │   │ - Owns vendor weaver            │   │
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
│  3D App #1 (IPC client) │─IPC─│   - Owns vendor weaver                         │
└─────────────────────────┘     │   - Renders combined output                │
┌─────────────────────────┐     │                                            │
│  3D App #2 (IPC client) │─IPC─│                                            │
└─────────────────────────┘     └────────────────────────────────────────────┘
```

- **N clients, not one.** The service runs in multi-compositor mode and composites every connected client (shell plus one or more 3D apps) into a single output.
- **Not sandboxed.** Shell-launched apps run with the user's normal token — no AppContainer. `ipc_server_mainloop_windows.cpp` still sets a named-pipe DACL that grants access to both the normal user SID and AppContainer, so the same pipe works for both clients. The shared-handle `SECURITY_ATTRIBUTES` for Chrome adds a `ALL APPLICATION PACKAGES` ACE; shell-launched apps don't need that ACE but don't reject it either.
- **Shell is a privileged IPC client.** Beyond the standard frame path, the shell sends window pose, focus, layout, and 2D-capture commands over the shell↔service control channel (see [workspace-runtime-contract.md](../roadmap/workspace-runtime-contract.md)).
- **Mode gate.** `DISPLAYXR_WORKSPACE_SESSION=1` is the flag the shell sets in the environment of every app it launches; the runtime DLL sees it in `u_sandbox_should_use_ipc()` and routes to IPC even though the process isn't sandboxed.
- **Step 0 comes before that.** A session with `XR_DXR_spatial_workspace` enabled
  (i.e. the workspace controller itself) is routed to IPC by
  `ext_spatial_workspace_enabled` in `src/xrt/targets/openxr/target.c:49-52`,
  *before* `u_sandbox_should_use_ipc()` is ever consulted. That auto-detection is
  what lets a controller stay runtime-agnostic instead of setting
  `XRT_FORCE_MODE=ipc` itself.

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
comp_d3d11_compositor → vendor weaver → leiasr_d3d11_get_predicted_eye_positions()
        │                                      │
        │                                      ▼
        │                          Eye positions with depth (z=0.6m)
        │                          {-0.032, 0, 0.6} / {0.032, 0, 0.6}
        ▼
View poses returned directly to app
```

**Native compositor** (`comp_d3d11_compositor.cpp`, eye position query in `comp_d3d11_get_eye_positions()`):
```cpp
// Get predicted eye positions from vendor weaver
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

### Service/IPC (WebXR or Shell) — With Eye Tracking

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
vendor weaver eye tracking data            Kooima asymmetric FOV from eye positions
        │                                         │
        └────────────────┬────────────────────────┘
                         ▼
              + qwerty device pose (player transform)
                         │
                         ▼
              tracking-aware view poses with proper depth
                         │
        ▼ (IPC response)
Chrome receives tracking-aware poses + Kooima FOV
        │
        ▼
App renders with correct 3D perspective
```

**IPC Server Eye-Tracking Integration** (`ipc_server_handler.c`):
```cpp
// ipc_handle_device_get_view_poses_2() now tries tracking-aware poses first
#if defined(XRT_HAVE_LEIA_SR_D3D11) && defined(XRT_HAVE_D3D11_SERVICE_COMPOSITOR)
if (ipc_try_get_sr_view_poses(ics->server, xdev, default_eye_relation, at_timestamp_ns,
                               view_count, &out_info->head_relation, out_info->fovs, out_info->poses)) {
    return XRT_SUCCESS;  // Used tracking-aware poses
}
#endif
// Fall back to qwerty device if eye tracking not available
```

The `ipc_try_get_sr_view_poses()` function:
1. Gets eye positions from vendor weaver via `comp_d3d11_service_get_predicted_eye_positions()`
2. Gets display dimensions via `comp_d3d11_service_get_display_dimensions()`
3. Computes Kooima asymmetric FOV from eye positions
4. Gets qwerty device pose as "player transform" (WASD movement)
5. Combines everything into tracking-aware view poses

---

## Eye Position / Convergence

### The Z-Depth Problem

For proper 3D display convergence, eye positions need **depth (z value)**:

| Component | Eye Position Format | Issue |
|-----------|---------------------|-------|
| Vendor Weaver | `{±0.032, 0, 0.6}` | Has depth (60cm from screen) |
| Native compositor | Uses vendor weaver values | Correct |
| Service compositor (before fix) | `{±0.032, 0, 0}` | No depth! |
| Qwerty device | Identity pose | No eye offset at all |

**Result:** WebXR cameras point at infinity instead of converging at the screen plane.

### Correct Eye Position Flow (Implemented)

Eye tracking now flows to Chrome via IPC:

```
Vendor Weaver (eye tracking camera)
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

Chrome now receives tracking-aware poses with proper depth and Kooima FOV.

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
- The service needs a device in *its own* process to own the vendor weaver and render the final composite
- With multiple IPC clients (shell mode), a shared "app device" wouldn't make sense — there is no single app
- Chrome adds an extra constraint: its device lives in an AppContainer with potentially different feature levels, so the service device must be independent

---

## Vendor Weaver Integration

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
| View poses source | vendor weaver via compositor | vendor weaver via IPC server |
| Eye tracking | Direct compositor query | IPC server queries, sends to client |
| FOV computation | `oxr_session_locate_views()` | `ipc_try_get_sr_view_poses()` |
| Player transform / input roles | Presence-ranked **input-provider hierarchy** with the qwerty device as the floor, arbitrated by `target_input_arbiter.c` ([ADR-034](../adr/ADR-034-input-provider-plugins.md), Amendments 1–3) | Same hierarchy, resolved in the service and forwarded over IPC; under a workspace the controller still forwards window input to the focused app |
| Session events | Direct callback | IPC message queue |
| Window ownership | App or compositor | Service's window |
| Process count | 1 | 2 (WebXR: Chrome + service) or N+1 (shell: shell + N apps + service) |
| GPU sync | Local barriers | KeyedMutex (cross-process); the D3D11 workspace leg now also uses a shared fence, `workspace_sync_fence` |

---

## Current Limitations (IPC Paths)

Each of these is expanded, with code anchors, in
[service-architecture.md](service-architecture.md) §9.

1. **Connection cap:** `IPC_MAX_CLIENTS` is 8 (`src/xrt/ipc/shared/ipc_protocol.h`) — a hard array bound, and satellites consume slots too.
2. **No arbitration between concurrent client classes** (#939): workspace, standalone IPC, browser, and bridge clients each assume they own the panel, the display mode, and focus.
3. **In-process plug-ins share the service's fate** (#943): a display processor, input provider, or MCP adapter that calls `exit()`, hangs, or corrupts the heap takes the whole service and every client with it.
4. **The standalone path is single-tenant in practice:** N standalone clients means N display processors, each presenting its own swap chain to the same panel.

---

## Implementation Files

### In-Process Path
- `src/xrt/state_trackers/oxr/oxr_session.c` - `oxr_session_locate_views()` with vendor eye tracking
- `src/xrt/compositor/d3d11/comp_d3d11_compositor.cpp` - Native D3D11 compositor

### Service/IPC Path
- `src/xrt/ipc/server/ipc_server_handler.c` - `ipc_try_get_sr_view_poses()`, `ipc_compute_kooima_fov()`
- `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` - Service compositor with vendor eye-tracking helper functions
- `src/xrt/ipc/client/ipc_client_hmd.c` - Client-side view pose IPC
