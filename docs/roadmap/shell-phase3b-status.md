# Shell Phase 3B — Implementation Status

Last updated: 2026-04-04 (branch `feature/shell-phase3b-ci`)

## Prerequisites

Phase 3 (3D window positioning) is complete and merged to main. Shell mode works with D3D11 apps (1-4 simultaneous). Phase 3B adds cross-API support (GL, VK, D3D12).

## Current Test Matrix

| API | Shell Mode | Error | Fix |
|-----|-----------|-------|-----|
| D3D11 (1-4 apps) | ✅ Stable | — | — |
| Vulkan | ❌ | `size mismatch, exported 0 but requires 33423360` | 3B.1: Calculate texture size |
| D3D12 (Unity) | ❌ | `SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED` | 3B.2: Strip protected content flag |
| OpenGL | ❌ | Silent exit — no D3D11 import path in GL client | 3B.3: WGL_NV_DX_interop |

## Phase 3B Progress

### 3B.1: Fix Vulkan swapchain import (size=0)
**Status:** Not started

| Task | Status | Notes |
|------|--------|-------|
| Calculate texture memory size in compositor_create_swapchain | | Set `images[i].size` from D3D11 texture desc instead of 0 |
| Test: single VK app in shell | | `cube_handle_vk_win.exe` should render |
| Test: VK + D3D11 together | | Both visible, no crash |

### 3B.2: Fix D3D12 swapchain creation (protected content flag)
**Status:** Not started

| Task | Status | Notes |
|------|--------|-------|
| Strip XRT_SWAPCHAIN_CREATE_PROTECTED_CONTENT in service | | Before creating shared texture |
| Test: single D3D12 app in shell | | `cube_handle_d3d12_win.exe` should render |
| Test: Unity D3D12 app in shell | | `DisplayXR-test.exe` should render |

### 3B.3: Fix GL client import (WGL_NV_DX_interop)
**Status:** Not started

| Task | Status | Notes |
|------|--------|-------|
| Add WGL_NV_DX_interop extension loading | | Check driver support, fallback gracefully |
| GL client: import D3D11 shared textures as GL textures | | wglDXOpenDeviceNV + wglDXRegisterObjectNV |
| Test: single GL app in shell | | `cube_handle_gl_win.exe` should render |
| Test: GL + D3D11 together | | Both visible, no crash |

### 3B.4: KeyedMutex coordination
**Status:** Not started

| Task | Status | Notes |
|------|--------|-------|
| Verify multi-comp acquires/releases mutex per client | | Check blit loop in multi_compositor_render |
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
