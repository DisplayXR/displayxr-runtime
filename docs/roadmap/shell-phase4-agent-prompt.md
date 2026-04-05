# Shell Phase 4A: Agent Prompt — 2D Window Capture Compositor

Use this prompt to start a new Claude Code session for implementing Phase 4A on branch `feature/shell-phase4-ci`.

---

## Prompt

```
I'm working on the DisplayXR shell — a spatial window manager for 3D displays. We're implementing Phase 4A: 2D Window Capture Compositor. This is the first sub-phase of the "Spatial Companion" feature.

## Context

Read these docs first (in order):
1. `CLAUDE.md` — project overview, build commands, architecture
2. `docs/roadmap/shell-phase4-plan.md` — full Phase 4 design (you're implementing 4A)
3. `docs/roadmap/shell-phase3b-status.md` — what was just completed (cross-API shell)
4. `docs/roadmap/shell-runtime-contract.md` — IPC protocol boundary

## Branch

You are on branch `feature/shell-phase4-ci`. All work goes here. Commits must reference #119 (Phase 4 issue) or #43 (shell tracking issue).

## What Phase 4A Needs

Capture any Windows HWND as a D3D11 texture and display it as a "virtual client" in the multi-compositor — a flat textured quad with spatial parallax but no stereo depth (mono content).

### The 5 tasks (implement in order):

**4A.1: Windows.Graphics.Capture integration**
- New file: `src/xrt/compositor/d3d11_service/d3d11_capture.cpp` (C++/WinRT)
- Use `GraphicsCaptureItem::CreateFromWindowId()` (Win10 2004+) to capture a specific HWND
- Create `Direct3D11CaptureFramePool` with the service's D3D11 device
- Deliver frames as `ID3D11Texture2D` — store the latest frame for the multi-comp to read
- Handle window resize (frame pool recreation on `FramePool.Recreate()`)
- API: `d3d11_capture_start(ID3D11Device* device, HWND hwnd)` → returns a capture context
- API: `d3d11_capture_get_texture(context)` → returns latest `ID3D11Texture2D` (or NULL if no new frame)
- API: `d3d11_capture_stop(context)` → tears down capture session

**4A.2: Virtual client slot in multi-compositor**
- Extend `struct d3d11_multi_client_slot` (in `comp_d3d11_service.cpp`, line ~454) to support capture clients
- Add a `client_type` field: `CLIENT_TYPE_IPC` (existing) vs `CLIENT_TYPE_CAPTURE` (new)
- For capture clients: `compositor` pointer is NULL (no IPC compositor), texture comes from capture API
- Add `ID3D11ShaderResourceView* capture_srv` field for the captured texture
- The slot's `active`, `window_pose`, `window_width_m`, `window_height_m`, `app_name` work the same way
- Add functions: `multi_compositor_add_capture_client(mc, hwnd, name)` → returns slot index, `multi_compositor_remove_capture_client(mc, slot_index)`

**4A.3: Mono texture rendering**
- In `multi_compositor_render()` (the main render loop), handle capture client slots:
  - Get latest texture from `d3d11_capture_get_texture()`
  - Create/update SRV if texture changed
  - Blit the SAME texture for both left and right eye passes (mono — no stereo)
  - Level 2 Kooima projection still applies (spatial parallax from window pose)
- The existing blit pipeline (`blit_window_content_*`) already renders textured quads — reuse it with the capture SRV instead of the IPC atlas SRV

**4A.4: Aspect ratio and DPI**
- Captured window dimensions from `GetClientRect(hwnd)` + `GetDpiForWindow(hwnd)`
- Convert pixels to meters: `width_m = (client_width / dpi) * 0.0254f` (inches to meters)
- Set `window_width_m` and `window_height_m` on the slot accordingly
- Handle DPI-aware and DPI-unaware apps correctly

**4A.5: Frame rate adaptation**
- Capture delivers frames asynchronously at the captured app's rate
- Multi-comp renders at display refresh (60Hz)
- Use a single-slot latest-frame model: capture callback stores the latest texture, render loop reads it
- Use `std::atomic<ID3D11Texture2D*>` or a mutex for thread safety (capture callback runs on a WinRT thread pool)

### IPC plumbing (for shell to trigger capture):

Add to `src/xrt/ipc/shared/proto.json`:
- `shell_add_capture_client` — args: `hwnd` (uint64), `name` (string) → returns `client_id` (uint32)
- `shell_remove_capture_client` — args: `client_id` (uint32)

Add handlers in `src/xrt/ipc/server/ipc_server_handler.c` that call the multi-compositor functions.

Update `src/xrt/targets/shell/main.c` to:
- Accept `--capture-hwnd <hwnd_decimal>` CLI arg for manual testing
- Call `ipc_call_shell_add_capture_client()` after connecting

### Key existing code to understand:

1. **Slot model**: `struct d3d11_multi_client_slot` at line ~454 of `comp_d3d11_service.cpp` — each IPC client gets a slot with pose, dimensions, atlas SRV, render state
2. **Render loop**: `multi_compositor_render()` — iterates active slots, blits each window as a textured quad per eye
3. **Blit pipeline**: `blit_window_content_axis_aligned()` and `blit_window_content_perspective()` — take a SRV + source rect + dest rect, draw the quad
4. **IPC protocol**: `proto.json` defines all IPC calls — look at existing `shell_*` calls for the pattern
5. **IPC handlers**: `ipc_server_handler.c` — `ipc_handle_shell_activate`, `ipc_handle_shell_set_window_pose` show the pattern

### Build and test:

```bash
# Build on Windows
scripts\build_windows.bat build

# Test: capture Notepad
# 1. Open Notepad
# 2. Find its HWND: powershell -Command "(Get-Process notepad).MainWindowHandle"
# 3. Launch shell with capture:
_package\bin\displayxr-shell.exe --capture-hwnd <hwnd_decimal> test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe
# 4. Should see: cube app in 3D + Notepad as a flat 2D panel, both in the spatial layout
```

### C++/WinRT build considerations:

`Windows.Graphics.Capture` is a WinRT API. The new `d3d11_capture.cpp` file needs:
- `#include <winrt/Windows.Graphics.Capture.h>`
- `#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>`
- CMake: `target_compile_features(... cxx_std_17)`, link `windowsapp.lib`
- The interop between WinRT `IDirect3DDevice` and raw `ID3D11Device` uses `CreateDirect3D11DeviceFromDXGIDevice` (from `Windows.Graphics.DirectX.Direct3D11.interop.h`)
- Keep this file isolated — the rest of the compositor is C/C++ without WinRT

### What NOT to change:

- Don't modify the existing IPC client compositor path (that's for OpenXR apps)
- Don't change how the display processor / weaver works
- Don't touch the per-app Kooima projection (Level 1) — that's app-side
- Don't break single-app standalone mode

### Commit style:

Each meaningful piece of work gets its own commit with issue ref:
- `Shell 4A: add Windows.Graphics.Capture wrapper (#119)`
- `Shell 4A: virtual client slots in multi-compositor (#119)`
- etc.

Use `/ci-monitor` after each significant commit to verify the build passes.
```

---

## Notes for the developer

- Phase 4A is **runtime-side only** (no shell repo migration yet). All code stays in `dfattal/openxr-3d-display`.
- The `Windows.Graphics.Capture` API is well-documented on Microsoft Learn. Key classes: `GraphicsCaptureItem`, `Direct3D11CaptureFramePool`, `GraphicsCaptureSession`.
- The trickiest part is the WinRT ↔ D3D11 interop. The capture API delivers `IDirect3DSurface` (WinRT) which needs to be converted to `ID3D11Texture2D` (raw COM). Use `IDirect3DDxgiInterfaceAccess::GetInterface()`.
- Thread safety is critical: capture callbacks run on WinRT thread pool, render loop runs on compositor thread. Use atomic pointer swap or lightweight mutex.
