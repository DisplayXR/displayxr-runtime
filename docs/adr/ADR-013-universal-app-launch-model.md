# ADR-013: Universal App Launch Model (Hidden HWND Proxy)

**Status:** Accepted
**Date:** 2026-03-27

## Context

The 3D shell needs to host OpenXR apps as 3D windows. Developers should write normal handle apps (create HWND, use `XR_EXT_win32_window_binding` + `XR_EXT_display_info`, do their own Kooima projection) and have them work automatically in the shell with zero code changes.

Previously considered approaches:
- Separate app classes (handle vs IPC) — friction, two code paths
- `XR_EXT_window_resize` extension — requires developer to handle a new event type
- Conditional HWND creation based on env var — app needs shell awareness

## Decision

**Hidden HWND proxy:** The app always creates its HWND. The launch mechanism determines the mode:

- **OS launch** (Explorer, CLI): runtime uses HWND for in-process compositor. Normal handle app behavior.
- **Shell launch**: runtime hides the HWND (`ShowWindow(SW_HIDE)`), routes to IPC. The shell puppets the hidden HWND via `SetWindowPos` (resize) and `PostMessage` (input). The app receives standard Win32 messages and doesn't know it's in a shell.

The env var `DISPLAYXR_SHELL_SESSION=1` signals shell mode to the runtime. The app never reads it.

### Why this works

- `PostMessage` delivers to hidden windows (message queues are independent of visibility)
- `SetWindowPos` on hidden windows fires `WM_SIZE` (via `WM_WINDOWPOSCHANGED` → `DefWindowProc`)
- `GetClientRect` returns correct dimensions on hidden windows
- HWNDs are system-global — the shell (same user session, same integrity) can manipulate any app's HWND via `SetWindowPos`/`PostMessage`
- The app's existing message loop, Kooima code, and rendering pipeline are completely unchanged

### Why a dedicated resize extension (XR_EXT_window_resize) is unnecessary

The hidden HWND proxy pattern delivers resize information via the standard Win32 `WM_SIZE` message in both standalone and shell modes. A dedicated OpenXR extension for resize notification adds complexity without benefit — the OS already has this mechanism, and the proxy pattern makes it work transparently across modes.

## App code (unchanged between modes)

```c
HWND hwnd = CreateWindow(...);
// xrCreateSession with XR_EXT_win32_window_binding (passes HWND)
// Main loop:
//   WM_SIZE → update window dims for Kooima
//   xrLocateViews → window-relative eyes
//   display3d_compute_views → own Kooima projection
//   RenderScene → same rendering code
```

## Runtime behavior

```c
bool u_sandbox_should_use_ipc(void) {
    if (getenv("DISPLAYXR_SHELL_SESSION")) return true;
    if (getenv("XRT_FORCE_MODE") == "ipc") return true;
    if (is_sandboxed()) return true;
    return false;
}
```

In shell mode, the runtime:
1. Receives HWND from `XR_EXT_win32_window_binding`
2. Hides it: `ShowWindow(hwnd, SW_HIDE)`
3. Passes HWND value to service via IPC session creation
4. Creates IPC client compositor (not native)

## Shell behavior

The shell stores each client's HWND and controls it:
- **Resize**: `SetWindowPos(hwnd, ..., new_w, new_h, SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)` → app gets `WM_SIZE`
- **Keyboard**: `PostMessage(hwnd, WM_KEYDOWN, vk, lParam)` → app gets keystrokes
- **Mouse**: `PostMessage(hwnd, WM_MOUSEMOVE, flags, MAKELPARAM(x, y))` → coordinates mapped from 3D hit-test UV to HWND client pixels

## Edge cases

| Issue | Status | Notes |
|-------|--------|-------|
| PostMessage to hidden windows | ✅ Works | Message queues independent of visibility |
| SetWindowPos → WM_SIZE on hidden | ✅ Works | Via WM_WINDOWPOSCHANGED + DefWindowProc |
| Cross-process HWND manipulation | ✅ Works | Same user session + same integrity level |
| DPI mismatch across monitors | ⚠️ Low risk | Query `GetDpiForWindow()`, scale accordingly. Single 3D display = no issue |
| Window decoration (WS_OVERLAPPEDWINDOW) | ⚠️ Manageable | Shell accounts for decoration in SetWindowPos, or runtime changes style to WS_POPUP when hiding |

## Test app migration (Phase 3)

All `_handle` test apps become universal (drop `_handle` suffix). `_ipc` test apps retired. One binary per API:
`cube_d3d11_win`, `cube_d3d12_win`, `cube_gl_win`, `cube_vk_win`, `cube_metal_macos`, etc.

## Consequences

- Developers write normal handle apps — zero shell awareness
- Same binary works standalone or in shell
- WebXR/sandboxed apps unaffected (still use IPC, no HWND)
- Shell launcher sets one env var — that's the entire integration point
- No new OpenXR extensions needed for resize or input
