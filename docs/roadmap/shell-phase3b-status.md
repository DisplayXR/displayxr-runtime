# Shell Phase 3B — Implementation Status

Last updated: 2026-04-04 (branch `feature/shell-phase3b-ci`)

## Summary

**Phase 3B is complete.** All 4 graphics APIs (D3D11, D3D12, Vulkan, OpenGL) work in shell/IPC mode, both individually and simultaneously. The multi-client crash (#116) is fixed.

## Test Matrix

| API | Single App | Multi-Client | Notes |
|-----|-----------|-------------|-------|
| D3D11 | ✅ | ✅ (4 apps) | Baseline, unchanged |
| D3D12 | ✅ | ✅ (with D3D11) | Server-creates-swapchain + OpenSharedHandle |
| Vulkan | ✅ | ✅ (with D3D11) | Fallback submit path, HUD disabled in shell |
| OpenGL | ✅ | ✅ (with D3D11) | WGL_NV_DX_interop2 staging texture + Y-flip |
| All 4 simultaneous | ✅ | ✅ | 5 clients (shell + 4 apps), stable |

## Completed Tasks

### 3B.1: Fix Vulkan swapchain import
- Added `dxgi_format_bytes_per_pixel()` helper, calculate texture memory size
- Skip VK size check for `OPAQUE_WIN32_BIT` handles in `vk_helpers.c`
- Fix VK client dispatch table crash: use `submit_fallback` (vkQueueWaitIdle) before semaphore/fence paths — app's VK device may lack extension function pointers
- Disable HUD (window-space layers) in shell mode — separate IPC issue

### 3B.2: Fix D3D12 swapchain
- Strip `XRT_SWAPCHAIN_CREATE_PROTECTED_CONTENT` in service and D3D12 client
- Restructured D3D12 client to server-creates-swapchain pattern (matching D3D11): service creates shared textures, client imports via `OpenSharedHandle`

### 3B.3: Fix GL client import
- New `comp_gl_d3d11_swapchain.c`: WGL_NV_DX_interop2 import path
- Staging texture approach: shared texture has KeyedMutex (incompatible with WGL), so create a plain staging texture for WGL registration, copy between them in acquire/release
- KeyedMutex acquire/release in copy for cross-device memory barrier
- Y-flip: GL renders bottom-up; multi-comp blit flips source UV via `atlas_flip_y` flag
- Auto-detects WGL_NV_DX_interop2, falls back to GL_EXT_memory_object

### 3B.4: KeyedMutex coordination — verified correct
- Atlas textures are service-local (not cross-process shared)
- layer_commit handles KeyedMutex on swapchain images
- multi_compositor_render reads atlas SRVs on same D3D11 device — no mutex needed

### 3B.5: Multi-API smoke test — passed
- All 4 APIs running simultaneously in shell (screenshot verified)
- 2x2 grid default layout so apps don't overlap

## Bonus Fixes

### #116: Multi-client crash (FIXED)
**Root cause:** Race condition in `multi_compositor_ensure_output`. Multiple IPC client threads called it concurrently when apps connected simultaneously, causing double display processor creation and SR SDK state corruption.
**Fix:** `std::lock_guard<std::recursive_mutex>` on `render_mutex` at the top of `ensure_output`.

### Default window layout
Apps 3+ were placed on top of app 2 (all at top-right). Now uses 2x2 grid: top-left, top-right, bottom-left, bottom-right. Slots 5+ cascade with small offset.

## Known Limitations

- **VK window-space layers:** HUD (XrCompositionLayerWindowSpaceEXT) disabled in shell mode due to null function pointer in VK IPC path. Standalone VK app HUD works fine.
- **VK standalone regression:** None — VK native compositor path unaffected.

## Key Source Files

| File | Role |
|------|------|
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` | Service compositor, multi-comp render, ensure_output mutex, Y-flip, grid layout |
| `src/xrt/compositor/client/comp_gl_d3d11_swapchain.c` | NEW: GL WGL_NV_DX_interop2 swapchain import |
| `src/xrt/compositor/client/comp_d3d12_client.cpp` | D3D12 server-creates-swapchain restructure |
| `src/xrt/compositor/client/comp_vk_client.c` | VK fallback submit path |
| `src/xrt/auxiliary/vk/vk_helpers.c` | VK size check skip for OPAQUE_WIN32_BIT |
| `src/xrt/auxiliary/d3d/d3d_dxgi_formats.h` | dxgi_format_bytes_per_pixel() |
| `src/xrt/compositor/client/comp_gl_win32_client.c` | GL interop path selection |
| `src/xrt/compositor/CMakeLists.txt` | GL D3D11 swapchain build config |
| `test_apps/cube_handle_vk_win/main.cpp` | VK HUD skip in shell mode |
