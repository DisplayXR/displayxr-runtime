// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#include "xr_window_space_hud.h"

#include <stdio.h>
#include <vector>

bool CreateHudSwapchain(XrSession session, uint32_t width, uint32_t height, XrHudSwapchain& out)
{
    uint32_t formatCount = 0;
    if (XR_FAILED(xrEnumerateSwapchainFormats(session, 0, &formatCount, nullptr)) || formatCount == 0) {
        return false;
    }
    std::vector<int64_t> formats(formatCount);
    if (XR_FAILED(xrEnumerateSwapchainFormats(session, formatCount, &formatCount, formats.data()))) {
        return false;
    }

    // Prefer R8G8B8A8_UNORM in any of the well-known per-API codes; the CPU
    // rasterizer always emits RGBA8 pixels and a family mismatch silently
    // corrupts the upload (CopyTextureRegion on D3D12 / vkCmdCopy on Vulkan).
    // DXGI_FORMAT_R8G8B8A8_UNORM=28, VK_FORMAT_R8G8B8A8_UNORM=37,
    // GL_RGBA8=0x8058, MTLPixelFormatRGBA8Unorm=70.
    const int64_t preferred[] = { 28, 37, 0x8058, 70 };
    int64_t selected = formats[0];
    for (int64_t pref : preferred) {
        for (int64_t f : formats) {
            if (f == pref) { selected = pref; goto found; }
        }
    }
found:

    XrSwapchainCreateInfo sci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                     XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                     XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sci.format = selected;
    sci.sampleCount = 1;
    sci.width = width;
    sci.height = height;
    sci.faceCount = 1;
    sci.arraySize = 1;
    sci.mipCount = 1;

    if (XR_FAILED(xrCreateSwapchain(session, &sci, &out.swapchain))) {
        return false;
    }
    out.format = selected;
    out.width = width;
    out.height = height;

    uint32_t imageCount = 0;
    if (XR_FAILED(xrEnumerateSwapchainImages(out.swapchain, 0, &imageCount, nullptr))) {
        return false;
    }
    out.imageCount = imageCount;
    return true;
}

bool AcquireHudSwapchainImage(const XrHudSwapchain& sc, uint32_t& outIndex)
{
    if (sc.swapchain == XR_NULL_HANDLE) return false;
    XrSwapchainImageAcquireInfo acq = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (XR_FAILED(xrAcquireSwapchainImage(sc.swapchain, &acq, &outIndex))) return false;
    XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wait.timeout = XR_INFINITE_DURATION;
    return XR_SUCCEEDED(xrWaitSwapchainImage(sc.swapchain, &wait));
}

bool ReleaseHudSwapchainImage(const XrHudSwapchain& sc)
{
    if (sc.swapchain == XR_NULL_HANDLE) return false;
    XrSwapchainImageReleaseInfo rel = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    return XR_SUCCEEDED(xrReleaseSwapchainImage(sc.swapchain, &rel));
}

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
    int32_t srcX, int32_t srcY,
    int32_t srcW, int32_t srcH)
{
    if (srcW < 0) srcW = (int32_t)hud.width;
    if (srcH < 0) srcH = (int32_t)hud.height;

    XrCompositionLayerProjection projLayer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    projLayer.space = localSpace;
    projLayer.viewCount = viewCount;
    projLayer.views = projViews;

    XrCompositionLayerWindowSpaceEXT hudLayer = {};
    hudLayer.type = (XrStructureType)XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_EXT;
    hudLayer.next = nullptr;
    hudLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    hudLayer.subImage.swapchain = hud.swapchain;
    hudLayer.subImage.imageRect.offset = { srcX, srcY };
    hudLayer.subImage.imageRect.extent = { srcW, srcH };
    hudLayer.subImage.imageArrayIndex = 0;
    hudLayer.x = x;
    hudLayer.y = y;
    hudLayer.width = width;
    hudLayer.height = height;
    hudLayer.disparity = disparity;

    const XrCompositionLayerBaseHeader* layers[] = {
        (XrCompositionLayerBaseHeader*)&projLayer,
        (XrCompositionLayerBaseHeader*)&hudLayer,
    };

    XrFrameEndInfo endInfo = { XR_TYPE_FRAME_END_INFO };
    endInfo.displayTime = displayTime;
    endInfo.environmentBlendMode = envBlendMode;
    endInfo.layerCount = (hud.swapchain != XR_NULL_HANDLE) ? 2 : 1;
    endInfo.layers = layers;

    return XR_SUCCEEDED(xrEndFrame(session, &endInfo));
}
