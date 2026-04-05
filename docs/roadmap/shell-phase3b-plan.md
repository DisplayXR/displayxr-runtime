# Shell Phase 3B: Cross-API Shell Support

## Prerequisites

Phase 3 is complete on branch `feature/shell-phase3-ci`. The spatial shell (multi-app compositor with 3D window positioning, rotation, animated layouts) works perfectly with D3D11 apps. The D3D11 multithread protection fix (#108) enables 4+ simultaneous apps.

Phase 3B fixes cross-API texture sharing so GL, Vulkan, and D3D12 apps also work in shell/IPC mode.

## Architecture: How Shell Mode Works

```
App (any API: D3D11, D3D12, GL, VK)
    ↓ xrCreateSwapchain → IPC → Service
    ↓
D3D11 Service Compositor (per-client)
    ├─ Creates D3D11 shared textures (NT handles + KeyedMutex)
    ├─ Exports handles back to app via IPC
    ↓
App imports shared textures into its native API
    ├─ D3D11 → OpenSharedResource1 (works ✅)
    ├─ D3D12 → OpenSharedHandle (fails: protected content flag ❌)
    ├─ VK → vkAllocateMemory+vkBindImageMemory (fails: size=0 ❌)
    ├─ GL → ??? (no import path exists ❌)
    ↓
App renders to shared texture → KeyedMutex release
    ↓
Multi-compositor (D3D11 service)
    ├─ Imports all clients' atlas textures via D3D11 SRV
    ├─ Blits per-eye with Kooima projection
    └─ Presents to display via vendor DP
```

## Failure Analysis

### Vulkan: Size Mismatch (CRITICAL — one-line fix)

**Error:** `vk_create_image_from_native: size mismatch, exported 0 but requires 33423360`

**Root cause:** When the D3D11 service creates shared textures and exports NT handles, it sets `images[i].size = 0`:

```c
// comp_d3d11_service.cpp:2175
sc->base.images[i].size = 0;  // ← BUG: D3D11 doesn't know allocation size
```

The VK client calls `vkGetImageMemoryRequirements` which returns 33423360 bytes (3840×2160×4 + alignment), then compares against the exported size (0) and fails.

**Fix:** Calculate the actual texture memory size from the D3D11 texture descriptor:
```c
// After texture creation:
D3D11_TEXTURE2D_DESC desc;
texture->GetDesc(&desc);
UINT bpp = dxgi_format_bits_per_pixel(desc.Format);
sc->base.images[i].size = (uint64_t)desc.Width * desc.Height * (bpp / 8);
// Note: may need row pitch alignment (typically 256-byte for D3D12, varies for D3D11)
```

**Files:**
- `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` — `compositor_create_swapchain` (~line 2175)
- May need: `src/xrt/auxiliary/d3d/d3d_helpers.hpp` for `dxgi_format_bits_per_pixel()` utility

**VK import code (for reference):**
- `src/xrt/auxiliary/vk/vk_helpers.c:1076-1332` — `vk_create_image_from_native`
- Size check at line 1316: `requirements.size > image_native->size`

### D3D12: Protected Content Flag (small fix)

**Error:** `XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED`

**Root cause:** Unity's OpenXR plugin requests `XRT_SWAPCHAIN_CREATE_PROTECTED_CONTENT` flag by default. The D3D12 client rejects it:

```c
// comp_d3d12_client.cpp:474-478
if ((info->create & XRT_SWAPCHAIN_CREATE_PROTECTED_CONTENT) != 0) {
    return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
}
```

**Fix:** Strip the protected content flag on the server side before creating the swapchain. Content protection is not needed for shell mode — the textures are composited by the service, not sent to a DRM-protected output.

**Files:**
- `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` — `compositor_create_swapchain`, strip `XRT_SWAPCHAIN_CREATE_PROTECTED_CONTENT` from `info->create`
- OR `src/xrt/ipc/server/ipc_server_handler.c` — strip before forwarding

### OpenGL: No Import Path (larger fix)

**Behavior:** GL app connects, creates swapchain, but then exits silently after a few frames.

**Root cause:** The GL client converts its format to VK format for IPC (`gl_format_to_vk`), then expects a VK-compatible swapchain back. But the service is D3D11 and exports D3D11 NT handles. The GL client has no mechanism to import D3D11 shared textures as GL textures.

**GL client flow (`src/xrt/compositor/client/comp_gl_client.c:456-525`):**
1. Converts GL format → VK format
2. Sends swapchain create request via IPC
3. Receives NT handles back
4. Tries to import as VK images → fails (GL, not VK)

**Fix options (in order of preference):**

a) **WGL_NV_DX_interop** (NVIDIA + recent AMD/Intel):
   - GL client creates/opens D3D11 device
   - Calls `wglDXOpenDeviceNV(d3d11_device)` to register interop
   - For each shared texture: `wglDXRegisterObjectNV(gl_name, d3d11_texture, GL_TEXTURE_2D, WGL_ACCESS_READ_WRITE_NV)`
   - GL can then bind and render to the texture normally
   - Widely supported on Windows (NVIDIA since ~2012, AMD/Intel recent drivers)

b) **CPU staging fallback** (universal but slow):
   - D3D11 service maps texture to CPU staging buffer
   - Copies pixels to GL via `glTexSubImage2D`
   - Functional but too slow for real-time 3D rendering

c) **VK interop on client** (if VK available):
   - GL client creates a VK device alongside GL
   - Imports D3D11 shared textures into VK
   - Uses `GL_EXT_memory_object` + `GL_EXT_semaphore` to import VK images into GL
   - Complex but doesn't require WGL_NV_DX_interop

**Recommended:** Option (a) — WGL_NV_DX_interop. It's the standard Windows approach, well-supported, and the simplest correct solution.

**Files:**
- `src/xrt/compositor/client/comp_gl_client.c` — add D3D11 import path
- New: WGL interop helper (load extension, register textures)
- `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` — may need to adjust shared texture flags for GL compatibility

### Multi-API Compositor Atlas Import

**Potential issue:** When the multi-compositor renders, it accesses each client's `cc->render.atlas_texture` and `cc->render.atlas_srv` as D3D11 SRVs. For D3D11 clients, these are native. For GL/VK/D3D12 clients going through IPC, the textures are shared — the client writes via its API, the service reads via D3D11.

**KeyedMutex synchronization:** The shared textures use `D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX`. The multi-compositor must acquire the mutex before reading each client's atlas and release after. Currently, the per-client compositor handles this in `compositor_layer_commit`, but the multi-compositor render path may not coordinate correctly for cross-API clients.

**Files to check:**
- `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` — `multi_compositor_render` blit loop
- Look for KeyedMutex acquire/release around atlas texture access

## Task Checklist

| Task | Priority | Complexity | Description | Status |
|------|----------|-----------|-------------|--------|
| 3B.1 | CRITICAL | Small | VK swapchain import: size check + dispatch table fix | ✅ Done |
| 3B.2 | CRITICAL | Medium | D3D12 swapchain: server-creates-swapchain restructure | ✅ Done |
| 3B.3 | HIGH | Medium | GL client: WGL_NV_DX_interop2 staging texture + Y-flip | ✅ Done |
| 3B.4 | MEDIUM | Small | Multi-comp: verify KeyedMutex coordination | ✅ Verified |
| 3B.5 | — | — | Smoke test: all 4 APIs simultaneously | ✅ Passed |
| Bonus | — | Small | Fix #116: multi-client race in ensure_output | ✅ Fixed |
| Bonus | — | Small | 2x2 grid default layout for 4+ apps | ✅ Done |

## Implementation Order

1. **3B.1** (VK size) — unblocks Vulkan apps immediately
2. **3B.2** (D3D12 flag) — unblocks Unity D3D12 apps
3. **3B.4** (KeyedMutex check) — verify multi-comp handles cross-API correctly
4. **3B.3** (GL import) — largest task, requires WGL interop
5. **3B.5** (smoke test) — final validation

## Key Source Files Reference

| File | Role |
|------|------|
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` | Service compositor: swapchain creation, multi-comp render |
| `src/xrt/compositor/client/comp_d3d11_client.cpp` | D3D11 client: imports shared textures via OpenSharedResource1 |
| `src/xrt/compositor/client/comp_d3d12_client.cpp` | D3D12 client: swapchain flag check (line 474-478) |
| `src/xrt/compositor/client/comp_gl_client.c` | GL client: swapchain creation (line 456-525), needs D3D11 import |
| `src/xrt/auxiliary/vk/vk_helpers.c` | VK helper: image import from native (line 1076-1332) |
| `src/xrt/ipc/server/ipc_server_handler.c` | IPC server: swapchain handle propagation |

## Related Issues

- #108 (FIXED) — D3D11 multithread protection for 3+ apps
- #116 (FIXED) — Multi-client crash: race condition in `multi_compositor_ensure_output`
- #16 (FIXED) — D3D12 swapchain creation in IPC/shell mode (server-creates-swapchain)

## Test Procedure

```bash
scripts\build_windows.bat build && scripts\build_windows.bat test-apps

# Single-API smoke tests
_package\bin\displayxr-shell.exe test_apps\cube_handle_vk_win\build\cube_handle_vk_win.exe
_package\bin\displayxr-shell.exe test_apps\cube_handle_gl_win\build\cube_handle_gl_win.exe
_package\bin\displayxr-shell.exe test_apps\cube_handle_d3d12_win\build\cube_handle_d3d12_win.exe

# Multi-API stress test
_package\bin\displayxr-shell.exe test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe test_apps\cube_handle_gl_win\build\cube_handle_gl_win.exe test_apps\cube_handle_vk_win\build\cube_handle_vk_win.exe test_apps\cube_handle_d3d12_win\build\cube_handle_d3d12_win.exe
```

Expected: all apps connect, render visible content, no service crash. Carousel mode (Ctrl+5) should spin all 4 windows.
