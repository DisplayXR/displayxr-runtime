# Shell Phase 0 — Implementation Status

Last updated: 2026-03-29 (branch `feature/shell-phase0-ci`)

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

### Phase 0C: HWND resize (IN PROGRESS)
- Multi-comp window created at display native resolution (3840x2160 for Leia)
- TAB key cycles focused_slot, DELETE sends EXIT_REQUEST to focused client
- Client-side HWND resize via SetWindowPos in oxr_session_create (before IPC call)
- **CURRENTLY BUILDING** — client-side resize approach to avoid cross-process deadlock

## Known Issues / Lessons Learned

### Cross-process SetWindowPos deadlocks
**Problem:** SetWindowPos on a cross-process HWND sends synchronous WM messages. If called from the IPC handler thread (server-side), the client thread is blocked waiting for the IPC response and can't process WM → deadlock.

**Tried and failed:**
1. SetWindowPos in `system_create_native_compositor` (server-side during session_create) → deadlock
2. Deferred SetWindowPos in `multi_compositor_render` (server-side during layer_commit) → same deadlock (layer_commit runs on IPC handler thread)

**Solution:** Resize the HWND CLIENT-SIDE in `oxr_session_create`, before the IPC `session_create` call. At that point the client's main thread is running and processes WM_SIZE immediately.

### IPC layer_window_space was missing
**Root cause of original "app crashes in xrEndFrame":** The IPC client compositor (`ipc_client_compositor.c`) had no `layer_window_space` vtable entry. When the cube_handle app submitted a window_space layer (HUD), the D3D11 client compositor forwarded to the IPC compositor which had NULL → crash → pipe break.

**Fix:** Added `ipc_compositor_layer_window_space` using the existing `handle_layer` pattern (commit `e8244c9cc`).

### HWND hiding approaches that don't work
DXGI flip-model `Present()` blocks when the window is hidden, cloaked, or off-screen:
- `ShowWindow(SW_HIDE)` → Present blocks
- `SetWindowPos(-32000, -32000)` → Present blocks
- `DwmSetWindowAttribute(DWMWA_CLOAK)` → Present blocks
- `SetWindowPos(1x1)` → not tested but likely blocks

**Solution:** Don't hide the HWND. The shell's multi-comp window is fullscreen and naturally occludes app windows. When shell is dismissed, apps continue in their own windows.

### DP factory timing
`sys->base.info.dp_factory_d3d11` is NULL during `comp_d3d11_service_create_system()`. Set by target builder AFTER. Must create DP lazily — `multi_compositor_ensure_output()` is called on first client connect (after target builder ran).

### Multi-comp rendering
Currently Phase 0A/0B: single client, atlas copy only. Level 2 Kooima quad rendering was implemented but had aspect ratio issues — replaced with simple `CopyResource` for now. Quad rendering needed for Phase 0C.2 (multi-window).

## Architecture

```
App (handle app)
  creates HWND → runtime resizes to display native (client-side, before IPC)
  passes HWND via XR_EXT_win32_window_binding
  DISPLAYXR_SHELL_SESSION=1 → forces IPC mode
  renders to IPC swapchain (shared GPU texture)
  ↓
IPC named pipe (Windows MESSAGE mode)
  ↓
Service (displayxr-service --shell)
  d3d11_service_compositor per client (atlas-only, no window/DP)
  compositor_layer_commit → renders layers to per-client atlas
  → multi_compositor_render → copies atlas to combined atlas → DP weaves → present
```

## Key Files

| File | Role |
|------|------|
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` | Multi-comp struct, render, client slots, lifecycle |
| `src/xrt/state_trackers/oxr/oxr_session.c` | Client-side HWND resize, shell session detection |
| `src/xrt/auxiliary/util/u_sandbox.c` | `DISPLAYXR_SHELL_SESSION` env var check |
| `src/xrt/ipc/client/ipc_client_compositor.c` | IPC layer functions incl. `layer_window_space` |
| `src/xrt/ipc/server/ipc_server_handler.c` | Server-side view pose computation for shell mode |
| `src/xrt/targets/service/main.c` | `--shell` flag parsing |
| `src/xrt/ipc/server/ipc_server_process.c` | Shell mode propagation to system compositor |

## Test Procedure

```bash
# Terminal 1: start service
displayxr-service.exe --shell

# Terminal 2: launch app
set DISPLAYXR_SHELL_SESSION=1
cube_handle_d3d11_win.exe
```

Expected: app's HWND resized to 3840x2160, cube renders in shell window with correct weaving.
Logs: `%LOCALAPPDATA%\DisplayXR\*.log`

## What's Next

### Phase 0C remaining
- Verify client-side HWND resize works (current build)
- If aspect ratio still wrong: check atlas dims vs app render dims
- Multi-window layout (deferred to Phase 0C.2)

### Phase 0D: Input forwarding
- Shell forwards keyboard/mouse from multi-comp window to focused app's HWND via PostMessage
- Mouse coordinate mapping: 3D hit-test UV → HWND client pixels
