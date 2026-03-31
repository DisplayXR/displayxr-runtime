# Shell Phase 1: Dynamic Spatial Shell

## Prerequisites

Phase 0 is complete on branch `feature/shell-phase0-ci` (merged to main). All features working:
- Multi-app compositor with shader-blit windowed rendering
- Live eye tracking via IPC (display-centric Kooima)
- 2D/3D mode switching via IPC shared memory
- Input forwarding with TAB focus cycling + cyan border
- Two-app slot-based layout at hardcoded positions
- ESC dismiss with 2D switch, DELETE close with clear

See `shell-phase0-status.md` for full details and lessons learned.

## Phase 1 Goals

Transform the hardcoded two-app layout into a dynamic spatial shell where windows can be repositioned, and apps gracefully handle shell lifecycle.

## Phase 1A: IPC-to-Standalone Hot-Switch

**Goal:** When the shell exits (ESC), apps seamlessly switch from IPC to standalone mode and render in their own windows.

**Current state:** Apps are alive but invisible after ESC. HWNDs exist but have no DXGI swapchain — content renders to IPC shared textures that nobody displays.

| Task | Description |
|------|-------------|
| 1A.1 | Detect shell dismiss on the client side (session event or shared memory flag) |
| 1A.2 | Restore app HWND: position to shell window rect, original style (WS_OVERLAPPEDWINDOW), correct size |
| 1A.3 | Create native D3D11 compositor in-process (replace IPC proxy compositor) |
| 1A.4 | Migrate swapchains from IPC shared textures to native ownership |
| 1A.5 | Create per-app DP on the restored HWND |
| 1A.6 | Resume rendering loop with native compositor — no frame drops |

**Key challenge:** The OpenXR session must stay alive. The compositor swap (IPC → native) must happen without the app calling `xrDestroySession`/`xrCreateSession`.

**Alternative (simpler):** Don't hot-switch. Instead, push a `SESSION_LOSS` event → app recreates session → detects no shell → creates standalone session naturally. Requires app cooperation but avoids compositor swap complexity.

## Phase 1B: Dynamic Window Poses

**Goal:** Shell can reposition 3D windows dynamically. Windows float in the display plane with adjustable X/Y position.

| Task | Description |
|------|-------------|
| 1B.1 | Store per-client window pose (x, y in display-fraction coordinates) in multi-comp slot |
| 1B.2 | Shell IPC protocol: `shell_set_window_pose(client_id, x, y, w, h)` |
| 1B.3 | On pose change: resize + reposition app HWND (via deferred SetWindowPos) |
| 1B.4 | Update shader blit destination rect in multi_compositor_render |
| 1B.5 | Transform eye positions to window-local coordinates for Kooima |

**Current foundation:** Window rect is already per-slot (`window_rect_x/y/w/h`). The shader blit positions content based on these. Changing them dynamically = window moves.

## Phase 1C: Mouse-Ray Hit-Test

**Goal:** Click on the shell window → determine which 3D window was hit and where.

| Task | Description |
|------|-------------|
| 1C.1 | Shell sends 2D cursor coords via new IPC call |
| 1C.2 | Runtime constructs ray from cyclopean eye through cursor pixel |
| 1C.3 | Intersect ray with window quads (flat rectangles in display plane) |
| 1C.4 | Return hit result: client_id, UV coords, 3D intersection point |
| 1C.5 | Use hit UV to map mouse events to the correct app's HWND client coords |

**Current foundation:** Mouse events are forwarded 1:1 based on `focused_slot`. Hit-test adds spatial awareness — click on a window to focus it.

## Phase 1D: Shell App Skeleton

**Goal:** A minimal shell application (`displayxr-shell.exe`) that manages window layout.

| Task | Description |
|------|-------------|
| 1D.1 | Privileged IPC client in `src/xrt/targets/shell/` |
| 1D.2 | Starts service with `--shell` if not running |
| 1D.3 | Receives app connect/disconnect notifications |
| 1D.4 | Default window placement: cascade with X offset |
| 1D.5 | Mouse drag: click-on-window + drag = translate in display plane |
| 1D.6 | Scroll wheel: adjust window Z depth (scale) |

## Key Architecture Notes

### HWND lifecycle in shell mode
```
App creates HWND → runtime makes borderless (WS_POPUP)
                → positions at display center (for Kooima)
                → pushes to HWND_BOTTOM (behind shell window)
Shell sets window rect → server resizes HWND via deferred SetWindowPos
App receives WM_SIZE → adapts Kooima + viewport
Shell exits → [Phase 1A] restore HWND + create standalone compositor
```

### Eye position transform for windowed apps
```
DP returns: eye_in_display (meters from display center)
Shell knows: window_pose (position in display plane)
Transform:  eye_in_window = eye_in_display - window_center
App receives: eye_in_window via xrLocateViews
App computes: Kooima with window physical dims + window-local eyes
```

### ADRs
- **ADR-013**: Universal app launch model (standalone or shell, same binary)
- **ADR-014**: Shell owns rendering mode control (V key → shell, app API blocked)

## Files from Phase 0 (key reference)

| File | Role |
|------|------|
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` | Multi-comp, render, slots, shader blit |
| `src/xrt/compositor/d3d11_service/d3d11_service_shaders.h` | Blit + border shaders |
| `src/xrt/compositor/d3d11/comp_d3d11_window.cpp` | Window WndProc, key routing, shell_dp |
| `src/xrt/state_trackers/oxr/oxr_session.c` | HWND resize, view pose bypass |
| `src/xrt/ipc/server/ipc_server_handler.c` | SR view poses, window_space layers |
| `src/xrt/ipc/server/ipc_server_process.c` | Shared memory sync, multi-client focus |
| `src/xrt/ipc/shared/ipc_protocol.h` | Rendering modes in shared memory |
| `src/xrt/ipc/client/ipc_client_hmd.c` | Client-side rendering mode sync |

## Test Procedure

```bash
# Build
scripts\build_windows.bat build
scripts\build_windows.bat test-apps

# Single app
Terminal 1: _package\run_shell_service.bat
Terminal 2: _package\run_shell_app.bat

# Two apps
Terminal 1: _package\run_shell_service.bat
Terminal 2: _package\run_shell_app.bat
Terminal 3: _package\run_shell_app.bat

# Or use the combined script:
Terminal 1: _package\run_shell_service.bat
Terminal 2: _package\run_shell_2apps.bat
```
