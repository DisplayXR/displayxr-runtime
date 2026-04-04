# Shell Phase 3B — Implementation Status

Last updated: 2026-04-04 01:19 (branch `feature/shell-phase3b-ci`)

## Prerequisites

Phase 3 (3D window positioning) is complete and merged to main. Shell mode works with D3D11 apps (1-4 simultaneous). Phase 3B adds cross-API support (GL, VK, D3D12).

## Current Test Matrix

| API | Shell Mode | Error | Fix |
|-----|-----------|-------|-----|
| D3D11 (1-4 apps) | ✅ Stable | — | — |
| Vulkan | ✅ Working | Fallback submit path (vkQueueWaitIdle), HUD disabled in shell mode | 3B.1: Done. VK dispatch table fix + HUD skip in shell. |
| D3D12 | ✅ Working | Restructured to server-creates-swapchain + OpenSharedHandle import | 3B.2: Done |
| OpenGL | ✅ Working | WGL_NV_DX_interop2 staging texture + Y-flip in multi-comp | 3B.3: Done |

## Phase 3B Progress

### 3B.1: Fix Vulkan swapchain import (size=0)
**Status:** Done — tested ✅

| Task | Status | Notes |
|------|--------|-------|
| Calculate texture memory size in compositor_create_swapchain | ✅ | 1MB-aligned size from D3D11 tex desc via `dxgi_format_bytes_per_pixel()` |
| Skip size check for opaque Win32 handles in vk_helpers.c | ✅ | Added `VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT` to skip list |
| Test: single VK app in shell | ✅ | `cube_handle_vk_win.exe` renders correctly — cube visible, continuous frames |
| Test: VK + D3D11 together | | Both visible, no crash |

### 3B.2: Fix D3D12 swapchain creation (protected content flag)
**Status:** Partial — flag stripped, but D3D12 import has deeper issue

| Task | Status | Notes |
|------|--------|-------|
| Strip XRT_SWAPCHAIN_CREATE_PROTECTED_CONTENT in service | ✅ | Stripped in `compositor_create_swapchain` via local_info copy |
| Strip XRT_SWAPCHAIN_CREATE_PROTECTED_CONTENT in D3D12 client | ✅ | Stripped in `comp_d3d12_client.cpp` — Unity apps no longer rejected |
| Test: single D3D12 app in shell | ✅ | `cube_handle_d3d12_win.exe` renders correctly — cube visible, right-side up |
| Test: Unity D3D12 app in shell | | Needs testing |

### 3B.3: Fix GL client import (WGL_NV_DX_interop)
**Status:** Done (code complete, needs runtime test)

| Task | Status | Notes |
|------|--------|-------|
| Add WGL_NV_DX_interop extension loading | ✅ | `comp_gl_d3d11_swapchain.c` — cached function pointers, graceful fallback to memobj |
| GL client: import D3D11 shared textures as GL textures | ✅ | `wglDXOpenDeviceNV` + `OpenSharedResource1` + `wglDXRegisterObjectNV` per image |
| GL client: DX lock/unlock in acquire/release | ✅ | `wglDXLockObjectsNV` in acquire, `glFlush` + `wglDXUnlockObjectsNV` in release |
| comp_gl_win32_client.c: prefer DX interop over memobj | ✅ | Auto-detects WGL_NV_DX_interop2, falls back to GL_EXT_memory_object |
| CMakeLists.txt: add new files and link d3d11 dxgi | ✅ | |
| Test: single GL app in shell | ✅ | `cube_handle_gl_win.exe` renders correctly — cube visible, right-side up |
| Test: GL + D3D11 together | ❌ | Service crashes when GL client joins alongside D3D11 client — pre-existing #116 |

### 3B.4: KeyedMutex coordination
**Status:** Verified — no code changes needed

| Task | Status | Notes |
|------|--------|-------|
| Verify multi-comp acquires/releases mutex per client | ✅ | Atlas textures are service-local (not cross-process shared). layer_commit handles KeyedMutex on swapchain images. multi_compositor_render reads atlas SRVs on same D3D11 device — no mutex needed. |
| Test: 4-API simultaneous | | D3D11 + GL + VK + D3D12 all rendering |

### 3B.5: Multi-API smoke test
**Status:** Not started

| Task | Status | Notes |
|------|--------|-------|
| All 4 APIs in carousel mode | | Ctrl+5 with 4 different API apps |

## Key Source Files

| File | Role |
|------|------|
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` | Service compositor: swapchain creation (~line 2061), multi-comp render |
| `src/xrt/compositor/client/comp_d3d12_client.cpp` | D3D12 client: flag check (line 474-478) |
| `src/xrt/compositor/client/comp_gl_client.c` | GL client: swapchain creation (line 456-525) |
| `src/xrt/auxiliary/vk/vk_helpers.c` | VK import: `vk_create_image_from_native` (line 1076-1332) |

## How to Build and Test

```bash
scripts\build_windows.bat build && scripts\build_windows.bat test-apps
_package\bin\displayxr-shell.exe test_apps\cube_handle_vk_win\build\cube_handle_vk_win.exe
```

See `shell-phase3b-plan.md` for full test procedure and architecture details.
