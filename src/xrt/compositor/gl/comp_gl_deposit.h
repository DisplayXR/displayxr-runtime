// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The OpenGL deposit (#2317 P3): GL renders its atlas INTO a D3D11 texture the hub opens.
 * @ingroup comp_gl
 *
 * The GL twin of comp_vk_native_deposit (VK-0). The GL compositor's atlas render
 * target IS an NT-shared `ID3D11Texture2D` created on the GL context's own
 * adapter, so handing a frame to the D3D11 hub (comp_hub_d3d11) costs no copy
 * on the GL side. The deposit's D3D11 device and immediate context are the
 * hub's "app end": the hub opens the texture from it (DIRECT, same adapter) or
 * binds it to comp_xbridge (cross adapter), exactly as it does the D3D11 tier's
 * stable atlas (`comp_hub_d3d11_info::stable_atlas`).
 *
 * ## Placement — read, never chosen (ADR-037 §5)
 *
 * OpenGL has no device-selection API, so the deposit device is created on the
 * adapter the GL context ALREADY runs on: the caller passes the adapter it
 * resolved from `GL_DEVICE_LUID_EXT` (#1159). Never `D3D11CreateDevice(NULL)`.
 *
 * ## Two import paths, best first
 *
 * 1. **memory_object** — `GL_EXT_memory_object_win32` imports the D3D11 texture
 *    (`GL_HANDLE_TYPE_D3D11_IMAGE_EXT`) as GL texture storage, and
 *    `GL_EXT_semaphore_win32` imports two shared `ID3D11Fence`s as
 *    D3D12-fence-type semaphores (`GL_HANDLE_TYPE_D3D12_FENCE_EXT`). Both edges
 *    are GPU-side: GL signals the PRODUCER fence after its last atlas write;
 *    the deposit's D3D11 context signals a RELEASE fence once the hub's
 *    back-fence (`comp_hub_d3d11_pre_render`) is satisfied, and GL waits on it
 *    before its first atlas write. The same mechanism the VK deposit uses
 *    (ADR-055).
 * 2. **nv_dx_interop** — `WGL_NV_DX_interop2` registers the D3D11 texture as a
 *    GL texture on the deposit device. `wglDXLockObjectsNV` /
 *    `wglDXUnlockObjectsNV` bracket the GL writes; the interop contract orders
 *    them against the D3D11 commands on that device on either side (the hub's
 *    back-fence wait queued before the lock, the producer-fence signal queued
 *    after the unlock). No `glFinish`, no keyed mutex. Whether a driver honours
 *    the lock with a GPU-side or a driver-internal wait is the driver's
 *    business; the placement line names the path so a measurement can tell.
 *
 * Neither path ever calls `glFinish` or `glClientWaitSync` on the frame path.
 *
 * ## Orientation
 *
 * GL's framebuffer origin is the bottom row; D3D's is the top. The caller paints
 * the atlas TOP-DOWN while the deposit is its target (`glClipControl` upper-left
 * plus mirrored viewports — see comp_gl_compositor.cpp), so the D3D11 consumer
 * reads an ordinary top-down atlas with view 0 at the top left.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct comp_gl_deposit;
struct comp_hub_d3d11;
struct comp_hub_d3d11_handoff;

//! The two import paths, for the placement line (`import=`).
#define COMP_GL_DEPOSIT_IMPORT_MEMORY_OBJECT "memory_object"
#define COMP_GL_DEPOSIT_IMPORT_NV_DX_INTEROP "nv_dx_interop"

/*!
 * @name Why a session took `import=nv_dx_interop` (`import_reason=`)
 * @{
 */
//! `DXR_HUB_GL_IMPORT=interop` forced the fallback (the A/B control).
#define COMP_GL_DEPOSIT_IMPORT_REASON_FORCED "forced_by_env"
//! No `GL_EXT_memory_object_win32` (or no `GL_EXT_memory_object`).
#define COMP_GL_DEPOSIT_IMPORT_REASON_NO_MEMORY_OBJECT "gl_no_memory_object"
//! `GL_EXT_memory_object_win32` without `GL_EXT_semaphore_win32`.
#define COMP_GL_DEPOSIT_IMPORT_REASON_NO_SEMAPHORE "gl_no_semaphore"
//! The GL context's adapter is known only heuristically (no `GL_DEVICE_LUID_EXT`).
#define COMP_GL_DEPOSIT_IMPORT_REASON_LUID_HEURISTIC "gl_luid_heuristic"
//! The memory-object or semaphore import itself failed on this driver.
#define COMP_GL_DEPOSIT_IMPORT_REASON_IMPORT_FAILED "memory_import_failed"
/*! @} */

struct comp_gl_deposit_info
{
	//! `IDXGIAdapter *` the GL context runs on (borrowed; the deposit AddRefs).
	void *dxgi_adapter;
	//! True when @ref dxgi_adapter came from `GL_DEVICE_LUID_EXT` (exact), false
	//! when from the GL_VENDOR / GL_RENDERER heuristics. memory_object requires it.
	bool luid_exact;
	//! The atlas extent (the GL compositor's worst-case atlas). Never resized.
	uint32_t width;
	uint32_t height;
};

/*!
 * Create the deposit: the D3D11 device on @p info->dxgi_adapter, the NT-shared
 * `R8G8B8A8_UNORM` texture, the fences, and the GL import. The compositor's GL
 * context must be current.
 *
 * @return NULL on success, else a `COMP_HUB_REASON_GL_*` token
 *         (comp_hub_d3d11.h) naming why no import path worked.
 */
const char *
comp_gl_deposit_create(const struct comp_gl_deposit_info *info, struct comp_gl_deposit **out_dep);

//! Free everything (GL objects first). GL context current. NULLs the pointer.
void
comp_gl_deposit_destroy(struct comp_gl_deposit **dep_ptr);

//! The GL texture name the atlas passes render into.
uint32_t
comp_gl_deposit_gl_texture(struct comp_gl_deposit *dep);

//! `COMP_GL_DEPOSIT_IMPORT_*`.
const char *
comp_gl_deposit_import_name(struct comp_gl_deposit *dep);

//! `COMP_GL_DEPOSIT_IMPORT_REASON_*` when the fallback was taken, else NULL.
const char *
comp_gl_deposit_import_reason(struct comp_gl_deposit *dep);

/*!
 * The handoff's static fields (device, context, adapter, texture, shares,
 * format, extent, generation). The per-frame `fence_value` comes from
 * @ref comp_gl_deposit_end.
 */
void
comp_gl_deposit_fill_handoff(struct comp_gl_deposit *dep, struct comp_hub_d3d11_handoff *h);

/*!
 * Before the FIRST GL write into the atlas this frame: order it after the
 * hub's last read. Queues the hub's back-fence on the deposit's D3D11 context
 * (`comp_hub_d3d11_pre_render`), then makes GL wait for it — GPU-side on the
 * memory_object path, through `wglDXLockObjectsNV` on the interop path. The
 * caller holds the compositor mutex (serialised with every hub weave) and has
 * the GL context current.
 */
void
comp_gl_deposit_begin(struct comp_gl_deposit *dep, struct comp_hub_d3d11 *hub);

/*!
 * After the LAST GL write into the atlas this frame: publish it. Signals the
 * producer fence (from GL on memory_object, from the D3D11 context after the
 * unlock on interop), flushes (never finishes), and fills @p h's per-frame
 * fields. When the hub's ingress is NOT direct, also queues the GPU-side wait
 * on the deposit's D3D11 context that orders comp_xbridge's read after GL's
 * write (the hub's stable-atlas xbridge route waits on nothing itself: it
 * assumes the producer wrote on that context).
 */
void
comp_gl_deposit_end(struct comp_gl_deposit *dep, bool hub_direct, struct comp_hub_d3d11_handoff *h);

#ifdef __cplusplus
}
#endif
