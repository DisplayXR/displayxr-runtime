# Shell Phase 4A Status: 2D Window Capture Compositor

**Branch:** `feature/shell-phase4-ci`
**Status:** Complete (11 commits, CI green)
**Date:** 2026-04-05

## What Was Built

Phase 4A adds the ability to capture any Windows HWND as a D3D11 texture and display it as a virtual client in the multi-compositor — a flat textured quad with spatial parallax but no stereo depth.

### Commits

1. `5fd64a7eb` — Full capture pipeline (4A.1-4A.5 + IPC + CLI)
2. `4cc8b596f` — Fix: shell_mode activation for capture-before-IPC-client
3. `de564c84d` — Disable cursor capture (dual-cursor fix)
4. `82dd93b03` — Mouse coordinate scaling for capture clients
5. `60450f44d` — Auto 2D/3D display mode switch on capture focus
6. `e84ecac93` — Title bar offset for top-row windows
7. `21f51c449` — Skip shell title bar for capture clients (native chrome)
8. `9867fb8c9` — Border, drag, and close for capture clients
9. `3f2234287` — HWND resize sync with frame pool recreation
10. `66fdd7c69` — Input forwarding infrastructure

### Architecture

```
Any Windows App (renders to HWND normally)
    │
    ├─ Windows.Graphics.Capture (d3d11_capture.cpp, C++/WinRT)
    │   ├─ GraphicsCaptureItem::CreateForWindow(HWND)
    │   ├─ Direct3D11CaptureFramePool (free-threaded, 1 buffer)
    │   ├─ FrameArrived callback → CopyResource to staging texture
    │   └─ Thread-safe handoff via mutex
    │
    ├─ Virtual client slot (CLIENT_TYPE_CAPTURE in multi-compositor)
    │   ├─ No compositor, no IPC, no OpenXR session
    │   ├─ Texture from capture API → SRV for blit pipeline
    │   └─ Same pose/animation/drag/resize as IPC slots
    │
    └─ Rendered as mono textured quad
        ├─ Same texture for all views (no stereo)
        ├─ Level 2 Kooima parallax from window pose
        └─ Display switches to 2D mode when focused
```

### New Files

- `src/xrt/compositor/d3d11_service/d3d11_capture.h` — C-linkage capture API
- `src/xrt/compositor/d3d11_service/d3d11_capture.cpp` — C++/WinRT implementation

### IPC Additions

- `shell_add_capture_client` (hwnd → client_id) — proto.json + handler
- `shell_remove_capture_client` (client_id) — proto.json + handler
- Capture client IDs use offset ≥ 1000 to distinguish from IPC client IDs
- Existing `shell_set/get_window_pose` handlers updated for capture IDs

### Shell CLI

```
displayxr-shell.exe --capture-hwnd <decimal|0xhex> [--capture-hwnd ...] app1.exe ...
```

### Features Working

- Capture any HWND via `--capture-hwnd`
- Mono rendering (same texture both eyes, spatial parallax from pose)
- Title bar: native window chrome shown, no redundant shell title bar
- Cyan focus border wraps content only (no title bar extension)
- Drag by clicking native title bar area of captured content
- Resize via edge drag or scroll wheel (syncs captured HWND size)
- Frame pool recreation on window resize (no black flash)
- DELETE key and close button remove capture client
- Auto 2D/3D display mode switch when capture client focused
  - Changes active_rendering_mode_index (mode 0 = 2D)
  - Extension apps receive XrEventDataRenderingModeChangedEXT
  - Display processor switches hardware 3D mode
- Capture render timer thread for capture-only scenarios
- DPI-aware initial sizing (meters from pixels via DPI)
- No duplicate cursor (IsCursorCaptureEnabled=false)

### Known Limitations (Phase 4B)

- **Keyboard/mouse input to WinUI apps**: PostMessage WM_CHAR/WM_KEYDOWN works for classic Win32 apps and DisplayXR IPC apps, but not for modern WinUI/XAML apps (Notepad, Paint, Windows Terminal). WinUI uses a different input pipeline that requires OS-level keyboard focus. Needs UI Automation, input driver, or alternative approach.
- **Capture-only mode**: When only capture clients are active (no IPC apps), the render timer thread calls ensure_output which may fail if display info isn't available yet. Works once an IPC client has connected at least once.

### Key Design Decisions

- **C++/WinRT isolation**: `d3d11_capture.cpp` is the only WinRT file — rest of compositor stays C/C++ without WinRT headers.
- **Staging texture model**: Capture callback copies to a staging texture under mutex. Render thread reads it. No atomic pointer swap (too risky with D3D11 resource lifetime).
- **Pool size tracking**: `pool_width/pool_height` tracked separately from staging texture size to prevent infinite Recreate loops on resize.
- **No shell title bar for capture**: Captured windows include their own native chrome. Shell renders no title bar but maps the top strip of content as a drag zone.
- **Client ID offset**: Capture client IDs start at 1000 to avoid collision with IPC client IDs from the thread pool.

---

# Shell Phase 4B Status: Input Forwarding & Window Adoption

**Branch:** `feature/shell-phase4-ci`
**Status:** Partial — infrastructure complete, input forwarding unsolved
**Date:** 2026-04-05

## What Was Built

1. **Client capacity increase** — `D3D11_MULTI_MAX_CLIENTS` 8→24, `MAX_CAPTURES` 8→24
2. **Input event ring buffer** — Lock-free SPSC buffer in WndProc for capture client keyboard events. `shell_input_event` struct, `consume_input_events()` and `request_foreground()` APIs.
3. **SendInput dispatch** — Render loop drains ring buffer and calls `SendInput` with `KEYEVENTF_UNICODE` for WM_CHAR and proper scan codes for WM_KEYDOWN/UP.
4. **Saved window placement** — `WINDOWPLACEMENT` and `saved_exstyle` stored per capture slot for future off-screen management and restore.
5. **Window enumeration** — `EnumWindows` + filtering in shell `main.c`. Skips system windows (taskbar, desktop, WorkerW, etc.), own/service PIDs, IPC client PIDs, tiny windows, owned popups.
6. **Auto-adoption** — When no `--capture-hwnd` args, shell auto-adopts all visible 2D windows on startup.
7. **Dynamic window tracking** — Poll loop checks for new/closed windows every 1 second.

## What Does NOT Work

**Keyboard input forwarding to WinUI/XAML apps** (Notepad, Paint, Terminal, Chrome on Windows 11).

### The Problem

`PostMessage(WM_KEYDOWN/WM_CHAR)` is ignored by WinUI apps — they consume input from the OS hardware input queue, not from posted messages. `SendInput` injects into the hardware queue but always delivers to the **foreground window**.

### Approaches Tried

| Approach | Result |
|----------|--------|
| PostMessage | Works for Win32 apps, ignored by WinUI |
| SendInput + SetForegroundWindow | Steals keyboard focus from shell — WndProc stops receiving keystrokes |
| Foreground flash (set→inject→restore per frame) | Works but visible blinking, unusable UX |
| HWND_TOPMOST + SetForegroundWindow | Swap chain creation fails (`DXGI_ERROR_NOT_CURRENTLY_AVAILABLE`); topmost-after-creation untested |
| Off-screen HWND (-32000,-32000) | Caused partial black capture rendering |

### Approaches To Investigate

1. **Low-level keyboard hook (`WH_KEYBOARD_LL`)** — System-wide hook intercepts all keystrokes before delivery. Shell hook captures keystrokes, suppresses them from reaching the shell window, and injects into the foreground capture HWND via SendInput. Shell window stays topmost (applied after swap chain creation). This is the most promising approach.
2. **Virtual HID driver** — Kernel-level input injection like RDP. Most robust, but requires driver signing.
3. **`AttachThreadInput`** — Share input queues between shell and target window threads. May allow `SetFocus` without `SetForegroundWindow`.
4. **UI Automation** — Fallback for text-only input (not general keyboard/mouse).
