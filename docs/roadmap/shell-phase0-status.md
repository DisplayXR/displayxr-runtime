# Shell Phase 0 — Implementation Status

Last updated: 2026-03-30 (branch `feature/shell-phase0-ci`)

## What Works

### Phase 0A: Multi-compositor (DONE)
- `--shell` flag on `displayxr-service` enables shell mode
- `d3d11_multi_compositor` creates shared window + DP on first client connect
- Per-client compositors create atlas-only resources (no window/DP)
- Multi-comp copies client atlas → combined atlas → DP weaves → present
- ESC closes shell window (apps continue in own windows)

### Phase 0B: Shell session routing (DONE)
- `DISPLAYXR_SHELL_SESSION=1` env var forces IPC mode for handle apps
- IPC client compositor has `layer_window_space` (was missing — caused NULL vtable crash)
- App's HWND flows through IPC to server via `xrt_session_info.external_window_handle`
- Identity pose fallback for ext_win apps when relation_flags=0 on early IPC frames

### Phase 0C: Rendering pipeline (DONE)
- App HWND resized to native display (3840×2160) borderless for correct Kooima projection
- Shell-mode client atlas + combined atlas sized to native display resolution
- Tile positions in `compositor_layer_commit` use actual content view dimensions (not `sys->view_width`)
- Multi-comp crops combined atlas to content dims before DP (3840×1080 for 2×1 SBS)
- DP receives correct per-view dimensions (1920×1080 per eye for current Leia display)
- Window-space layers handled in IPC server (NULL vtable guard — no crash, silently skipped)
- TAB cycles focused slot, DELETE sends EXIT_REQUEST to focused client

**Visual result:** Cube renders centered, correct proportions, correct 3D weaving on Leia display. Head tracking active — scene responds to head movement via display-centric Kooima with DP eye positions.

### Phase 0C.3: Live eye tracking (DONE)
- `comp_d3d11_service_owns_window` returns false for shell mode → display-centric Kooima path
- Server computes view poses with DP tracked eye positions, sends via IPC
- Client bypasses T_base_head transform for ext_win IPC sessions (avoids -1.6m Y offset)
- Nominal eye override (`have_eye_override`) gated by `have_eyes` — only fires for standalone apps with local tracking, not IPC shell sessions where server provides tracked poses

## Known Issues / Lessons Learned

### HWND must be borderless for shell fullscreen
The app computes Kooima screen corners from `g_windowWidth × pxSizeX`. If the HWND has title bar/borders, client area < outer size, causing view rects smaller than expected (e.g., 1904×1036 instead of 1920×1080). Fix: strip window style to `WS_POPUP | WS_VISIBLE` before resizing.

### Atlas tile positions must use content dimensions in shell mode
The `compositor_layer_commit` blit loop computes tile positions via `u_tiling_view_origin(eye, tile_columns, sys->view_width, ...)`. In shell mode, `sys->view_width` (960) is the DP's internal resolution, not the app's rendered view width (1920). Using 960 causes views to overlap. Fix: use actual content view width from projection layer submission.

### Shell-mode atlas must be native-display-sized
The app HWND is fullscreen → swapchain is 3840×2160 → view rects are 1920×1080. The per-client atlas and combined atlas must be large enough to hold these without clipping. Sized to `display_pixel_width × display_pixel_height`.

### Cross-process SetWindowPos deadlocks
**Problem:** SetWindowPos on a cross-process HWND sends synchronous WM messages. If called from the IPC handler thread (server-side), the client thread is blocked waiting for the IPC response and can't process WM → deadlock.

**Solution:** Resize the HWND CLIENT-SIDE in `oxr_session_create`, before the IPC `session_create` call.

### IPC layer_window_space was missing
The IPC server handler had no case for `XRT_LAYER_WINDOW_SPACE` (type 8). Added handler with NULL vtable guard — silently skips if compositor doesn't implement it.

### HWND hiding approaches that don't work
DXGI flip-model `Present()` blocks when the window is hidden, cloaked, or off-screen. **Solution:** Don't hide the HWND. Shell window naturally occludes app windows.

### DP factory timing
`sys->base.info.dp_factory_d3d11` is NULL during `comp_d3d11_service_create_system()`. Set by target builder AFTER. Must create DP lazily in `multi_compositor_ensure_output()`.

### Nominal eye override must be gated by have_eyes
`oxr_session_locate_views` has an ext_win fallback that sets `have_eye_override=true` with nominal `±IPD/2` positions. This is correct for standalone apps (which get tracked eyes via native compositor). But in IPC/shell mode, `have_eyes=false` and the fallback fires, overwriting the server's tracked poses with nominal values. Fix: add `&& have_eyes` so the override only fires when the client has local tracking data.

### T_base_head offset breaks shell IPC view poses
The standard `oxr_session_locate_views` applies `T_base_head` (which includes qwerty's Y=1.6m world position). For shell IPC sessions, the server already returns display-relative poses — applying T_base_head adds a spurious -1.6m Y offset, placing the eye below the screen. Fix: skip T_base_head for `has_external_window && !have_eyes && !have_eye_override`.

## Architecture

```
App (handle app)
  creates HWND → runtime makes borderless + resizes to display native (client-side)
  passes HWND via XR_EXT_win32_window_binding
  DISPLAYXR_SHELL_SESSION=1 → forces IPC mode
  renders to IPC swapchain (shared GPU texture, 3840×2160)
  view rects: (0,0,1920,1080) and (1920,0,1920,1080) for 2×1 SBS with scale 0.5
  ↓
IPC named pipe (Windows MESSAGE mode)
  ↓
Service (displayxr-service --shell)
  d3d11_service_compositor per client (atlas-only at native res, no window/DP)
  compositor_layer_commit → copies views to atlas using content-sized tile layout
  → multi_compositor_render → crops atlas to content dims (3840×1080) → DP weaves → present
```

## Key Files

| File | Role |
|------|------|
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` | Multi-comp struct, render, client slots, lifecycle, atlas crop |
| `src/xrt/state_trackers/oxr/oxr_session.c` | Client-side HWND borderless resize, shell session detection |
| `src/xrt/auxiliary/util/u_sandbox.c` | `DISPLAYXR_SHELL_SESSION` env var check |
| `src/xrt/ipc/client/ipc_client_compositor.c` | IPC layer functions incl. `layer_window_space` |
| `src/xrt/ipc/server/ipc_server_handler.c` | Server-side view pose computation, window_space layer handler |
| `src/xrt/ipc/server/ipc_server_process.c` | Shell mode propagation to system compositor |
| `src/xrt/targets/service/main.c` | `--shell` flag parsing |

## Test Procedure

After `scripts\build_windows.bat test-apps`, run scripts are generated in `_package/`:

```bash
# Terminal 1: start shell service
_package\run_shell_service.bat

# Terminal 2: launch app in shell mode
_package\run_shell_app.bat
```

Run scripts set `XR_RUNTIME_JSON` to the dev build automatically (required to bypass the installed runtime from CI builds).

Expected: Cube renders centered with correct proportions, 3D weaving, and head tracking (scene responds to head movement after ~3 second face tracking warmup).
Logs: `%LOCALAPPDATA%\DisplayXR\*.log`

## What's Next

### Phase 0D: Input forwarding
- 0D.1: Shell captures keyboard from multi-comp window, `PostMessage(focused_hwnd, WM_KEYDOWN, ...)`
- 0D.2: Mouse coordinate mapping: 3D hit-test UV → HWND client pixels → `PostMessage(WM_MOUSEMOVE, ...)`
- 0D.3: WASD/mouse in focused app moves app's camera (app handles it via its own WndProc)
- Test: Focus cube app → WASD moves camera within cube scene → works identically to standalone
