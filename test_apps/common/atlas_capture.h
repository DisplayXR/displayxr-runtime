// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  In-app per-projection-layer atlas capture (the 'I' key feature).
 *
 * Reads back a sub-rect of an OpenXR swapchain image to host memory and
 * writes a PNG via stb_image_write. Captures land in the user's Pictures
 * folder and auto-increment as `<stem>-<N>_<cols>x<rows>.png`.
 *
 * **What this captures**: the *app's primary projection swapchain* —
 * the per-tile multi-view atlas the app rendered, *before* the runtime
 * composites any other layers. So one projection layer, no HUD / quad /
 * window-space overlays, no per-eye disparity, no compose-time blending.
 * Useful for debugging an app's own tile-aware rendering.
 *
 * **For runtime-side captures**, use the trigger files (or the MCP
 * @c capture_frame tool with a @c mode parameter):
 *   - `${TMPDIR:-/tmp}/displayxr_atlas_trigger` (POSIX) or
 *     `%TEMP%\displayxr_atlas_trigger` (Windows) → @b post-compose
 *     atlas the DP weaver receives — projection + window-space (HUD) +
 *     quads, composed across every tile. Output PNG:
 *     `displayxr_atlas.png`.
 *   - `…\displayxr_atlas_trigger.projection` → @b projection-only:
 *     atlas state before window-space layers are composed in (per-tile
 *     projection content only). Output PNG:
 *     `displayxr_atlas.projection.png`. Useful for verifying tile-aware
 *     app rendering independent of chrome.
 *
 * See `u_capture_intent.h` and the per-compositor
 * `*_compositor_dispatch_capture` plumbing.
 *
 * Each backend (D3D11/D3D12/GL/Metal/Vulkan) lives in its own `.cpp`/`.mm`
 * and is only compiled in by apps that need it. Filename helpers and the
 * platform flash overlay live in `atlas_capture.cpp` (Windows) and
 * `atlas_capture_macos.mm` (macOS).
 */

#pragma once

#include <stdint.h>
#include <string>

#ifdef _WIN32
#include <windows.h>
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12Resource;
// DXGI_FORMAT is intentionally NOT forward-declared — it's an enum in
// dxgiformat.h and any redeclaration here would clash with includers
// that already pulled in the real header. The D3D readback helpers
// don't take format params; they query it from the texture itself.
#endif

// Vulkan handles — forward-declare so the header doesn't pull in vulkan.h.
// VK_DEFINE_HANDLE expands to the same typedef shape, so these are
// compatible whether or not the caller has also included vulkan.h.
//
// VkFormat is intentionally NOT forward-declared (it's an enum in vulkan.h
// and a redeclaration as `int` would conflict). The Vulkan helper takes
// the format as a plain `int` — cast `(int)VK_FORMAT_…` at the call site.
#ifndef VULKAN_CORE_H_
typedef struct VkDevice_T*         VkDevice;
typedef struct VkPhysicalDevice_T* VkPhysicalDevice;
typedef struct VkQueue_T*          VkQueue;
typedef struct VkCommandPool_T*    VkCommandPool;
typedef struct VkImage_T*          VkImage;
#endif

// Forward-declared (global scope) so the runtime-capture helper below can take
// it without this header pulling in xr_session_common.h; atlas_capture.cpp
// includes that header for the full definition.
struct XrSessionManager;

namespace dxr_capture {

// ---------------------------------------------------------------------------
// Output path / filename helpers (cross-platform)
// ---------------------------------------------------------------------------

// Returns "<user pictures>/DisplayXR" (Windows: %USERPROFILE%\Pictures\DisplayXR;
// macOS: ~/Pictures/DisplayXR), creating it if missing. Empty on failure.
std::string PicturesDirectory();

// Scan `dir` for files matching "<stem>-<N>_<cols>x<rows>.png" and return
// max(N) + 1 (or 1 if there are no matches). Lets users accumulate captures
// without overwriting prior ones.
int NextCaptureNum(const std::string& dir,
                   const std::string& stem,
                   uint32_t cols,
                   uint32_t rows);

// Convenience: PicturesDirectory() + NextCaptureNum() + assemble full path.
// Falls back to working directory if Pictures resolution fails.
std::string MakeCapturePath(const std::string& stem,
                            uint32_t cols,
                            uint32_t rows);

// Path PREFIX (no extension) for xrCaptureAtlasEXT, which appends
// "_atlas_<viewCount>_<cols>x<rows>.png" (viewCount == cols*rows). Numbers
// against existing "<stem>-<N>_atlas_<viewCount>_<cols>x<rows>.png" files (the
// runtime-produced names) so repeat captures accumulate instead of overwriting.
// Returns "<dir>/<stem>-<N>" (no "_atlas", no ".png") — the runtime owns the
// layout tokens. The legacy readback path keeps MakeCapturePath; the two name
// spaces don't collide.
std::string MakeCaptureAtlasPrefix(const std::string& stem,
                                   uint32_t cols,
                                   uint32_t rows);

// ---------------------------------------------------------------------------
// Visual feedback — brief white flash overlay (~250 ms fade).
// ---------------------------------------------------------------------------

#ifdef _WIN32
// WM_TIMER ID used by the fade animation. App's WindowProc must dispatch
// `case WM_TIMER: if (wParam == kFlashTimerId) { TickCaptureFlash(hwnd); return 0; }`.
constexpr UINT_PTR kFlashTimerId = 0xDF1A5;

// Custom message ID for cross-thread flash request. Apps add a case in
// WindowProc that calls TriggerCaptureFlash(hwnd). Render thread fires
// PostFlashRequest(hwnd) — all HWND ops then run on the message-pump thread.
constexpr UINT kFlashUserMsg = WM_USER + 0x51;

// Show the white overlay over `parent`'s client area and start the fade
// timer. MUST run on the message-pump thread that owns `parent`. From the
// render thread, post a kFlashUserMsg to the window and call this from
// WindowProc instead.
void TriggerCaptureFlash(HWND parent);

// Tick the fade. Call from WM_TIMER when wParam == kFlashTimerId.
void TickCaptureFlash(HWND parent);

// Convenience wrapper for the cross-thread post.
inline void PostFlashRequest(HWND hwnd) {
    PostMessageW(hwnd, kFlashUserMsg, 0, 0);
}

// ---------------------------------------------------------------------------
// Runtime-owned atlas capture (XR_EXT_atlas_capture). The single, graphics-
// API-agnostic capture path: the runtime does the GPU readback, so apps no
// longer need a per-API CaptureAtlasRegion* helper. Handles the 3D-mode guard,
// filename numbering (MakeCaptureAtlasPrefix), the xrCaptureAtlasEXT call
// (PROJECTION_ONLY = the app's own projection atlas), the flash overlay, and
// logging. Call from the render loop when the 'I' key flag is set.
//
// Returns true iff a capture was requested. No-ops (returns false) when the
// runtime didn't expose the extension (pfn NULL) or for mono/1×1 layouts.
// ---------------------------------------------------------------------------
bool RequestRuntimeAtlasCapture(const ::XrSessionManager& xr,
                                const char* appName,
                                uint32_t tileColumns,
                                uint32_t tileRows,
                                HWND flashHwnd);
#endif

#ifdef __APPLE__
// macOS: pass an `NSView*` (the content view that should be flashed). Safe
// to call from any thread — internally dispatches to the main queue, where
// AppKit / Core Animation must be touched.
void TriggerCaptureFlash(void* nsviewBridged);
#endif

// ---------------------------------------------------------------------------
// All per-API readback helpers (CaptureAtlasRegion{D3D11,D3D12,GL,VK,Metal})
// have been removed (#396 W6). Atlas capture is runtime-owned now: the Windows
// cube_handle apps call dxr_capture::RequestRuntimeAtlasCapture (above) and the
// macOS cube_handle apps call xrCaptureAtlasEXT inline — no app does its own
// GPU readback. (The D3D / Vulkan handle forward-declarations near the top are
// now unused leftovers; harmless, kept to minimize churn.)
// ---------------------------------------------------------------------------

}  // namespace dxr_capture
