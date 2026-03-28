# Shell Implementation Context

Comprehensive context for implementing the 3D shell multi-compositor. This document captures lessons learned from the first implementation attempt (branch `feature/multi-compositor-ci`, now deleted) and the architectural decisions made.

## What Was Built (First Attempt)

Branch `feature/multi-compositor-ci` proved the multi-comp rendering pipeline works:
- `d3d11_multi_compositor` struct in `comp_d3d11_service.cpp` (~800 lines added)
- Per-client atlas compositing → multi comp quad rendering → Level 2 Kooima → DP weaving → present
- Window lifecycle: deferred creation, ESC dismiss, re-open on new client
- TAB/DELETE focus cycling with cyan border indicator
- Level 1 eye transform in `ipc_server_handler.c`
- Commit `c15a306` had visible content with correct weaving

### What Worked
- Multi comp rendering pipeline (quad rendering with existing `quad_vs/quad_ps` shaders)
- `display3d_compute_projection()` for Level 2 Kooima (NOT `math_matrix_4x4_projection_vulkan_infinite_reverse` — that's Vulkan-specific)
- Lazy DP creation (DP factory set by target builder AFTER `comp_d3d11_service_create_system` returns)
- Window teardown+recreate when DP reports different dims than initial window
- Per-client atlas resize when system dims change
- `render_mutex` on `d3d11_service_system` for D3D11 context serialization

### What Broke
- IPC view pose path: client-side `oxr_session_locate_views` has a 3D-GATE that checks for LOCAL eye tracking data — returns false in IPC mode, causing blue screen
- `oxr_space_locate_device` returns zero relation_flags in IPC on early frames
- Qwerty head pose (y=1.6) applied on top of server-computed poses
- Multiple patches-on-patches made the code fragile

### Key Bugs to Avoid
1. **DP factory timing**: `sys->base.info.dp_factory_d3d11` is NULL during `comp_d3d11_service_create_system()`. It's set by `target_instance.c` AFTER the function returns. Create DP lazily on first render frame.
2. **Window dims mismatch**: First window created at default 1920x1080, DP reports 3840x2160. Must teardown and recreate window+DP at correct size. Don't just resize swap chain — the DP/weaver is initialized with the window and may cache the initial size.
3. **Per-client atlas resize**: After DP updates system dims, per-client atlas textures (created at old dims) must be recreated.
4. **Y-flip in quad rendering**: The `quad_vs` shader does `pos.y = -pos.y` for OpenXR conventions. For multi-comp window quads, need BOTH model Y negate (`-height_m`) AND V flip (`post_transform = {u_off, 1.0, u_scale, -1.0}`).
5. **Don't modify oxr_session_locate_views 3D-GATE**: It works correctly for in-process apps. Shell apps (universal/handle) do their own Kooima via their HWND — they don't need the server-computed view poses.

## Architecture Decisions

### ADR-012: Window-Relative Kooima (on main)
Apps use actual window physical dims as screen for Kooima, offset eyes to window center. No viewport-scale hack. See `docs/adr/ADR-012-window-relative-kooima-projection.md`.

### ADR-013: Universal App / Hidden HWND Proxy (on main)
One binary works standalone or in shell. Shell puppets app's hidden HWND via `SetWindowPos` (resize → `WM_SIZE`) and `PostMessage` (input). Zero app code changes. See `docs/adr/ADR-013-universal-app-launch-model.md`.

### Input Architecture
- **Qwerty is shell-only**: window management, view tuning (V, P, vHeight). NOT app cameras.
- **App input**: forwarded via hidden HWND `PostMessage` — app processes `WM_KEYDOWN`, `WM_MOUSEMOVE` normally.
- **No XR_EXT_window_resize needed**: hidden HWND proxy delivers `WM_SIZE` in both modes.

### Service Modes
```
displayxr-service              → WebXR/sandbox IPC (current, no multi-comp)
displayxr-service --shell      → Shell mode (multi-comp, universal apps)
```

### App Routing
```
App launched from OS            → in-process native compositor (handle app)
App launched from shell         → DISPLAYXR_SHELL_SESSION=1 → IPC → multi-comp
Sandboxed app (Chrome)          → auto-IPC (sandbox detection, no shell needed)
```

## Key Code Patterns (from first implementation)

### Multi compositor struct
```cpp
#define D3D11_MULTI_MAX_CLIENTS 8

struct d3d11_multi_client_slot {
    d3d11_service_compositor *compositor;
    struct xrt_pose window_pose;
    float window_width_m, window_height_m;
    HWND app_hwnd;  // NEW: app's hidden HWND for input forwarding
    bool active;
};

struct d3d11_multi_compositor {
    comp_d3d11_window *window;
    HWND hwnd;
    IDXGISwapChain1 *swap_chain;
    ID3D11RenderTargetView *back_buffer_rtv;
    ID3D11Texture2D *combined_atlas;  // + srv, rtv
    xrt_display_processor_d3d11 *display_processor;
    d3d11_multi_client_slot clients[D3D11_MULTI_MAX_CLIENTS];
    uint32_t client_count;
    int32_t focused_slot;
    bool window_dismissed;
};
```

### Render pipeline (d3d11_multi_compositor_render_frame)
1. `multi_compositor_ensure_output()` — lazy create window+swapchain+DP (or recreate if dismissed)
2. Resize per-client atlases if dims changed
3. Handle window resize (swap chain)
4. TAB/DELETE focus handling via `GetAsyncKeyState`
5. Get eye positions from DP (`xrt_eye_positions`)
6. Get physical display dims from DP
7. Clear combined atlas
8. For each view (L/R): set viewport → for each client: compute Level 2 Kooima MVP → render quad with client's atlas_srv → draw focus border if focused
9. Call DP `process_atlas` on combined atlas → back buffer
10. Present

### Level 2 Kooima projection
```cpp
// Use display3d_compute_projection (standard OpenGL/D3D11 frustum)
// NOT math_matrix_4x4_projection_vulkan_infinite_reverse (Vulkan Y-down)
display3d_compute_projection(eye_pos, screen_width_m, screen_height_m,
                              0.01f, 100.0f, (float *)&proj);

// View matrix from eye pose (identity orientation)
math_matrix_4x4_view_from_pose(&eye_pose, &view);

// Model matrix: negate Y to cancel shader's pos.y = -pos.y
struct xrt_vec3 scale = {window_width_m, -window_height_m, 1.0f};
math_matrix_4x4_model(&window_pose, &scale, &model);

// UV: V flipped to fix texture mapping
constants.post_transform = {tile_u_offset, 1.0f, tile_u_scale, -1.0f};
```

### process_atlas call (post ADR-012 merge, has canvas sub-rect params)
```cpp
xrt_display_processor_d3d11_process_atlas(
    dp, context, combined_atlas_srv,
    view_width, view_height, tile_columns, tile_rows,
    DXGI_FORMAT_R8G8B8A8_UNORM,
    back_buffer_width, back_buffer_height,
    0, 0,  // canvas_offset: full atlas
    view_width * tile_columns, view_height * tile_rows);
```

## Implementation Order (from plan)

**Phase 0A**: Service `--shell` flag + multi-comp (test with `cube_ipc` via `XRT_FORCE_MODE=ipc`)
**Phase 0B**: Shell app skeleton + handle app routing (`DISPLAYXR_SHELL_SESSION=1`)
**Phase 0C**: Two apps + focus cycling
**Phase 0D**: Input forwarding via hidden HWND `PostMessage`

**Key principle**: Develop with handle apps first. They do their own Kooima and bypass all IPC view pose issues.

## Files Reference

| File | Role |
|------|------|
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` | Multi compositor lives here (3979 lines on main) |
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.h` | Public APIs |
| `src/xrt/compositor/d3d11_service/d3d11_service_shaders.h` | `QuadLayerConstants` + `quad_vs/quad_ps` (reuse for window quads) |
| `src/xrt/compositor/d3d11/comp_d3d11_window.h/cpp` | Window creation for multi-comp output |
| `src/xrt/auxiliary/math/m_display3d_view.h/c` | `display3d_compute_projection()` for Level 2 Kooima |
| `src/xrt/include/xrt/xrt_display_processor_d3d11.h` | DP vtable (process_atlas has canvas sub-rect params) |
| `src/xrt/auxiliary/util/u_sandbox.c` | `u_sandbox_should_use_ipc()` — add DISPLAYXR_SHELL_SESSION check |
| `src/xrt/targets/service/main.c` | Service entry point — add `--shell` flag |
| `src/xrt/ipc/server/ipc_server_handler.c` | `ipc_try_get_sr_view_poses` — Level 1 eye transform |
| `src/xrt/state_trackers/oxr/oxr_session.c` | DO NOT modify 3D-GATE |
| `test_apps/cube_handle_d3d11_win/main.cpp` | Test app (use with DISPLAYXR_SHELL_SESSION=1) |
