// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Cross-platform helpers for XR_EXT_window_space_layer HUD swapchains.
 *
 * Pure OpenXR. No Win32 / Cocoa / graphics-API dependencies — usable from any
 * test app on any platform. The Windows test apps reach this through
 * xr_session_common.{h,cpp}; the macOS test apps include this header directly
 * because xr_session_common.h is Win32-specific.
 */

#pragma once

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#ifdef __APPLE__
#  include <openxr/XR_EXT_cocoa_window_binding.h>
#else
#  include <openxr/XR_EXT_win32_window_binding.h>
#endif

#include <stdint.h>

struct XrHudSwapchain {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    int64_t format = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t imageCount = 0;
};

/*!
 * Create a HUD swapchain that prefers an RGBA8_UNORM format. The CPU-side
 * rasterizer (HudRenderer on Windows / HudRendererMacOS on macOS) emits
 * R8G8B8A8 pixels, so a format-family mismatch would silently corrupt the
 * upload on Vulkan/D3D12 backends.
 */
bool CreateHudSwapchain(XrSession session, uint32_t width, uint32_t height, XrHudSwapchain& out);

bool AcquireHudSwapchainImage(const XrHudSwapchain& sc, uint32_t& outIndex);
bool ReleaseHudSwapchainImage(const XrHudSwapchain& sc);

/*!
 * Submit one frame with a projection layer plus an XR_EXT_window_space_layer
 * HUD layer on top. `disparity` is a horizontal per-eye shift, in fractions
 * of window width (0 = screen depth).
 *
 * `srcW` / `srcH` < 0 mean "use the full HUD swapchain image".
 */
bool SubmitWindowSpaceHudFrame(
    XrSession session,
    XrSpace localSpace,
    XrTime displayTime,
    XrEnvironmentBlendMode envBlendMode,
    const XrCompositionLayerProjectionView* projViews,
    uint32_t viewCount,
    const XrHudSwapchain& hud,
    float x, float y, float width, float height,
    float disparity,
    int32_t srcX = 0, int32_t srcY = 0,
    int32_t srcW = -1, int32_t srcH = -1);
