// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Windows Graphics Capture (WGC) helper for Leia DP transparency.
 *
 * Captures the desktop region behind the app's HWND and exposes the
 * latest frame as a cross-API shared D3D11 texture (SHARED_NTHANDLE +
 * KEYEDMUTEX). The D3D11 and D3D12 Leia DPs import that handle and
 * sample the desktop content into the per-tile compose-under-bg pass —
 * replacing the older chroma-key trick.
 *
 * On any failure path (Windows < 10 2004, WGC unavailable, DRM, env
 * override), create() returns NULL and the DP falls back to chroma-key.
 *
 * Self-capture defense: SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)
 * is applied to the HWND so WGC does not recursively capture our own
 * woven output back into the background.
 *
 * @author David Fattal
 * @ingroup drv_leia
 */

#pragma once

#ifdef _WIN32

#include <windows.h>
#include <stdbool.h>
#include <stdint.h>

struct ID3D11Device;
struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;
struct ID3D11Fence;
struct ID3D12Device;
struct ID3D12Resource;
struct ID3D12Fence;

#ifdef __cplusplus
extern "C" {
#endif

struct leia_bg_capture;

/*!
 * Create a WGC capture session targeting the monitor containing @p hwnd.
 * Returns NULL on any failure — caller falls back to chroma-key.
 *
 * Side-effect on success: SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE).
 */
struct leia_bg_capture *leia_bg_capture_create(HWND hwnd);

/*!
 * Open the shared staging texture on the caller's D3D11 device + create an SRV.
 * Call once at DP init. *out_tex and *out_srv are owned by the caller.
 */
long leia_bg_capture_open_d3d11(struct leia_bg_capture *c,
                                struct ID3D11Device *dev,
                                struct ID3D11Texture2D **out_tex,
                                struct ID3D11ShaderResourceView **out_srv);

/*!
 * Open the shared staging texture on the caller's D3D12 device.
 * Caller creates the SRV in its own descriptor heap.
 */
long leia_bg_capture_open_d3d12(struct leia_bg_capture *c,
                                struct ID3D12Device *dev,
                                struct ID3D12Resource **out_res);

/*!
 * Open the producer's shared fence on the caller's D3D11 device. Consumer
 * waits on this fence (via @c ID3D11DeviceContext4::Wait) before sampling
 * the shared staging texture so it sees the producer's copy result.
 */
long leia_bg_capture_open_fence_d3d11(struct leia_bg_capture *c,
                                      struct ID3D11Device *dev,
                                      struct ID3D11Fence **out_fence);

/*!
 * Open the producer's shared fence on the caller's D3D12 device. Consumer
 * waits via @c ID3D12CommandQueue::Wait before sampling.
 */
long leia_bg_capture_open_fence_d3d12(struct leia_bg_capture *c,
                                      struct ID3D12Device *dev,
                                      struct ID3D12Fence **out_fence);

/*!
 * Expose the shared NT handle of the staging texture so the caller can
 * import it into Vulkan via @c VK_KHR_external_memory_win32 (handle type
 * @c VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT). The handle is owned
 * by the capture module; do NOT @c CloseHandle on it.
 */
HANDLE leia_bg_capture_get_shared_handle(struct leia_bg_capture *c);

/*!
 * Monitor dimensions used to size the staging texture (BGRA8). Use these
 * for the imported VkImage extent.
 */
void leia_bg_capture_get_size(struct leia_bg_capture *c,
                              uint32_t *out_width,
                              uint32_t *out_height);

/*!
 * Per-frame: pull the latest WGC frame into the shared staging tex, return
 * window-on-monitor region as normalized UVs and the fence value the caller
 * must Wait on before sampling.
 *
 * @return  true if a captured frame is available; false if no frame yet
 *          or the window has crossed monitors (caller should skip compose
 *          for this frame and either fall back or pass-through).
 */
bool leia_bg_capture_poll(struct leia_bg_capture *c,
                          float out_bg_uv_origin[2],
                          float out_bg_uv_extent[2],
                          uint64_t *out_fence_wait_value);

void leia_bg_capture_destroy(struct leia_bg_capture *c);

#ifdef __cplusplus
}
#endif

#endif // _WIN32
