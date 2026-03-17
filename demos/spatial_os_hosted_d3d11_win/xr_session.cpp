// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenXR session management for Spatial OS demo
 */

#include "xr_session.h"
#include "logging.h"
#include <cstring>

#define XR_CHECK(call) \
    do { \
        XrResult result = (call); \
        if (XR_FAILED(result)) { \
            LogXrResult(#call, result); \
            return false; \
        } \
    } while (0)

#define XR_CHECK_LOG(call) \
    do { \
        XrResult result = (call); \
        LogXrResult(#call, result); \
        if (XR_FAILED(result)) { \
            return false; \
        } \
    } while (0)

bool GetD3D11GraphicsRequirements(XrSessionManager& xr, LUID* outAdapterLuid) {
    LOG_INFO("Getting D3D11 graphics requirements...");

    PFN_xrGetD3D11GraphicsRequirementsKHR xrGetD3D11GraphicsRequirementsKHR = nullptr;
    XrResult result = xrGetInstanceProcAddr(xr.instance, "xrGetD3D11GraphicsRequirementsKHR",
        (PFN_xrVoidFunction*)&xrGetD3D11GraphicsRequirementsKHR);
    if (XR_FAILED(result) || !xrGetD3D11GraphicsRequirementsKHR) {
        LOG_ERROR("Failed to get xrGetD3D11GraphicsRequirementsKHR function pointer");
        return false;
    }

    XrGraphicsRequirementsD3D11KHR graphicsReq = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    result = xrGetD3D11GraphicsRequirementsKHR(xr.instance, xr.systemId, &graphicsReq);
    if (XR_FAILED(result)) {
        LogXrResult("xrGetD3D11GraphicsRequirementsKHR", result);
        return false;
    }

    LOG_INFO("D3D11 adapter LUID: 0x%08X%08X", graphicsReq.adapterLuid.HighPart, graphicsReq.adapterLuid.LowPart);
    *outAdapterLuid = graphicsReq.adapterLuid;
    return true;
}

bool InitializeOpenXR(XrSessionManager& xr) {
    LOG_INFO("Querying OpenXR extensions...");

    uint32_t extensionCount = 0;
    XR_CHECK_LOG(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr));

    std::vector<XrExtensionProperties> extensions(extensionCount, {XR_TYPE_EXTENSION_PROPERTIES});
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensions.data()));

    bool hasD3D11 = false;
    xr.hasWin32WindowBindingExt = false;

    for (const auto& ext : extensions) {
        if (strcmp(ext.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0) hasD3D11 = true;
        if (strcmp(ext.extensionName, XR_EXT_WIN32_WINDOW_BINDING_EXTENSION_NAME) == 0) xr.hasWin32WindowBindingExt = true;
        if (strcmp(ext.extensionName, XR_EXT_DISPLAY_INFO_EXTENSION_NAME) == 0) xr.hasDisplayInfoExt = true;
    }

    LOG_INFO("D3D11: %s, win32_window_binding: %s, display_info: %s",
        hasD3D11 ? "YES" : "NO", xr.hasWin32WindowBindingExt ? "YES" : "NO", xr.hasDisplayInfoExt ? "YES" : "NO");

    if (!hasD3D11) {
        LOG_ERROR("XR_KHR_D3D11_enable not available");
        return false;
    }

    std::vector<const char*> enabledExtensions;
    enabledExtensions.push_back(XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
    if (xr.hasWin32WindowBindingExt) enabledExtensions.push_back(XR_EXT_WIN32_WINDOW_BINDING_EXTENSION_NAME);
    if (xr.hasDisplayInfoExt) enabledExtensions.push_back(XR_EXT_DISPLAY_INFO_EXTENSION_NAME);

    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(createInfo.applicationInfo.applicationName, "SpatialOS");
    createInfo.applicationInfo.applicationVersion = 1;
    strcpy_s(createInfo.applicationInfo.engineName, "None");
    createInfo.applicationInfo.engineVersion = 0;
    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    createInfo.enabledExtensionCount = (uint32_t)enabledExtensions.size();
    createInfo.enabledExtensionNames = enabledExtensions.data();

    XR_CHECK_LOG(xrCreateInstance(&createInfo, &xr.instance));

    XrSystemGetInfo systemInfo = {XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK_LOG(xrGetSystem(xr.instance, &systemInfo, &xr.systemId));

    {
        XrSystemProperties sysProps = {XR_TYPE_SYSTEM_PROPERTIES};
        if (XR_SUCCEEDED(xrGetSystemProperties(xr.instance, xr.systemId, &sysProps))) {
            memcpy(xr.systemName, sysProps.systemName, sizeof(xr.systemName));
            LOG_INFO("System: %s", xr.systemName);
        }
    }

    if (xr.hasDisplayInfoExt) {
        XrSystemProperties sysProps = {XR_TYPE_SYSTEM_PROPERTIES};
        XrDisplayInfoEXT displayInfo = {(XrStructureType)XR_TYPE_DISPLAY_INFO_EXT};
        XrEyeTrackingModeCapabilitiesEXT eyeCaps = {(XrStructureType)XR_TYPE_EYE_TRACKING_MODE_CAPABILITIES_EXT};
        displayInfo.next = &eyeCaps;
        sysProps.next = &displayInfo;
        if (XR_SUCCEEDED(xrGetSystemProperties(xr.instance, xr.systemId, &sysProps))) {
            xr.recommendedViewScaleX = displayInfo.recommendedViewScaleX;
            xr.recommendedViewScaleY = displayInfo.recommendedViewScaleY;
            xr.displayWidthM = displayInfo.displaySizeMeters.width;
            xr.displayHeightM = displayInfo.displaySizeMeters.height;
            xr.nominalViewerX = displayInfo.nominalViewerPositionInDisplaySpace.x;
            xr.nominalViewerY = displayInfo.nominalViewerPositionInDisplaySpace.y;
            xr.nominalViewerZ = displayInfo.nominalViewerPositionInDisplaySpace.z;
            xr.displayPixelWidth = displayInfo.displayPixelWidth;
            xr.displayPixelHeight = displayInfo.displayPixelHeight;
            xr.supportedEyeTrackingModes = (uint32_t)eyeCaps.supportedModes;
            xr.defaultEyeTrackingMode = (uint32_t)eyeCaps.defaultMode;
            LOG_INFO("Display: %.3fx%.3fm, %ux%u px, scale=%.3fx%.3f",
                xr.displayWidthM, xr.displayHeightM,
                xr.displayPixelWidth, xr.displayPixelHeight,
                xr.recommendedViewScaleX, xr.recommendedViewScaleY);
        }

        xrGetInstanceProcAddr(xr.instance, "xrRequestDisplayModeEXT",
            (PFN_xrVoidFunction*)&xr.pfnRequestDisplayModeEXT);
        if (xr.supportedEyeTrackingModes != 0) {
            xrGetInstanceProcAddr(xr.instance, "xrRequestEyeTrackingModeEXT",
                (PFN_xrVoidFunction*)&xr.pfnRequestEyeTrackingModeEXT);
        }

        // Load unified rendering mode function pointers (v7)
        xrGetInstanceProcAddr(xr.instance, "xrRequestDisplayRenderingModeEXT",
            (PFN_xrVoidFunction*)&xr.pfnRequestDisplayRenderingModeEXT);
        xrGetInstanceProcAddr(xr.instance, "xrEnumerateDisplayRenderingModesEXT",
            (PFN_xrVoidFunction*)&xr.pfnEnumerateDisplayRenderingModesEXT);
    }

    uint32_t viewCount = 0;
    XR_CHECK(xrEnumerateViewConfigurationViews(xr.instance, xr.systemId, xr.viewConfigType, 0, &viewCount, nullptr));
    xr.configViews.resize(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    XR_CHECK(xrEnumerateViewConfigurationViews(xr.instance, xr.systemId, xr.viewConfigType, viewCount, &viewCount, xr.configViews.data()));

    return true;
}

bool CreateSession(XrSessionManager& xr, ID3D11Device* d3d11Device, HWND hwnd) {
    xr.windowHandle = hwnd;

    XrGraphicsBindingD3D11KHR d3d11Binding = {XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    d3d11Binding.device = d3d11Device;

    XrWin32WindowBindingCreateInfoEXT sessionTarget = {XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_EXT};
    sessionTarget.windowHandle = hwnd;

    if (xr.hasWin32WindowBindingExt && hwnd) {
        d3d11Binding.next = &sessionTarget;
        LOG_INFO("Using XR_EXT_win32_window_binding with HWND: 0x%p", hwnd);
    }

    XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &d3d11Binding;
    sessionInfo.systemId = xr.systemId;

    XR_CHECK_LOG(xrCreateSession(xr.instance, &sessionInfo, &xr.session));
    LOG_INFO("Session created: 0x%p", (void*)xr.session);

    // Enumerate available rendering modes and store names
    if (xr.pfnEnumerateDisplayRenderingModesEXT && xr.session != XR_NULL_HANDLE) {
        uint32_t modeCount = 0;
        XrResult enumRes = xr.pfnEnumerateDisplayRenderingModesEXT(xr.session, 0, &modeCount, nullptr);
        if (XR_SUCCEEDED(enumRes) && modeCount > 0) {
            std::vector<XrDisplayRenderingModeInfoEXT> modes(modeCount);
            for (uint32_t i = 0; i < modeCount; i++) {
                modes[i].type = XR_TYPE_DISPLAY_RENDERING_MODE_INFO_EXT;
                modes[i].next = nullptr;
            }
            enumRes = xr.pfnEnumerateDisplayRenderingModesEXT(xr.session, modeCount, &modeCount, modes.data());
            if (XR_SUCCEEDED(enumRes)) {
                xr.renderingModeCount = modeCount > 8 ? 8 : modeCount;
                LOG_INFO("Display rendering modes (%u):", modeCount);
                for (uint32_t i = 0; i < xr.renderingModeCount; i++) {
                    strncpy(xr.renderingModeNames[i], modes[i].modeName, XR_MAX_SYSTEM_NAME_SIZE - 1);
                    xr.renderingModeNames[i][XR_MAX_SYSTEM_NAME_SIZE - 1] = '\0';
                    xr.renderingModeViewCounts[i] = modes[i].viewCount;
                    xr.renderingModeScaleX[i] = modes[i].viewScaleX;
                    xr.renderingModeScaleY[i] = modes[i].viewScaleY;
                    xr.renderingModeDisplay3D[i] = (modes[i].hardwareDisplay3D == XR_TRUE);
                    LOG_INFO("  [%u] %s (views=%u, scale=%.2fx%.2f, 3D=%d)",
                        modes[i].modeIndex, modes[i].modeName, modes[i].viewCount,
                        modes[i].viewScaleX, modes[i].viewScaleY, modes[i].hardwareDisplay3D);
                }
            }
        }
    }

    return true;
}

bool CreatePanelSwapchain(XrSessionManager& xr, Panel& panel, uint32_t pixelW, uint32_t pixelH) {
    LOG_INFO("Creating panel swapchain '%ls' (%ux%u)", panel.title.c_str(), pixelW, pixelH);

    // Query supported formats
    uint32_t formatCount = 0;
    XR_CHECK(xrEnumerateSwapchainFormats(xr.session, 0, &formatCount, nullptr));
    std::vector<int64_t> formats(formatCount);
    XR_CHECK(xrEnumerateSwapchainFormats(xr.session, formatCount, &formatCount, formats.data()));

    // Prefer R8G8B8A8_UNORM (DXGI=28) for D2D compatibility
    int64_t selectedFormat = formats[0];
    for (uint32_t i = 0; i < formatCount; i++) {
        if (formats[i] == 28) { selectedFormat = 28; break; }
    }

    XrSwapchainCreateInfo swapchainInfo = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                               XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    swapchainInfo.format = selectedFormat;
    swapchainInfo.sampleCount = 1;
    swapchainInfo.width = pixelW;
    swapchainInfo.height = pixelH;
    swapchainInfo.faceCount = 1;
    swapchainInfo.arraySize = 1;
    swapchainInfo.mipCount = 1;

    XR_CHECK_LOG(xrCreateSwapchain(xr.session, &swapchainInfo, &panel.swapchainInfo.swapchain));
    panel.swapchainInfo.format = selectedFormat;
    panel.swapchainInfo.width = pixelW;
    panel.swapchainInfo.height = pixelH;

    uint32_t imageCount = 0;
    XR_CHECK(xrEnumerateSwapchainImages(panel.swapchainInfo.swapchain, 0, &imageCount, nullptr));
    panel.swapchainInfo.imageCount = imageCount;

    panel.swapchainImages.resize(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    XR_CHECK(xrEnumerateSwapchainImages(panel.swapchainInfo.swapchain, imageCount, &imageCount,
        (XrSwapchainImageBaseHeader*)panel.swapchainImages.data()));

    LOG_INFO("Panel '%ls' swapchain created: %u images", panel.title.c_str(), imageCount);
    return true;
}

bool AcquirePanelSwapchainImage(Panel& panel, uint32_t& imageIndex) {
    XrSwapchainImageAcquireInfo acquireInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrResult result = xrAcquireSwapchainImage(panel.swapchainInfo.swapchain, &acquireInfo, &imageIndex);
    if (XR_FAILED(result)) return false;

    XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = XR_INFINITE_DURATION;
    result = xrWaitSwapchainImage(panel.swapchainInfo.swapchain, &waitInfo);
    return XR_SUCCEEDED(result);
}

bool ReleasePanelSwapchainImage(Panel& panel) {
    XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    return XR_SUCCEEDED(xrReleaseSwapchainImage(panel.swapchainInfo.swapchain, &releaseInfo));
}

bool EndFrameMultiLayer(
    XrSessionManager& xr,
    XrTime displayTime,
    const XrCompositionLayerProjectionView* projViews,
    uint32_t viewCount,
    Panel* panels,
    int panelCount
) {
    // Projection layer for center 3D scene
    XrCompositionLayerProjection projectionLayer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projectionLayer.space = xr.localSpace;
    projectionLayer.viewCount = viewCount;
    projectionLayer.views = projViews;

    // Build window-space layers for each 2D panel
    std::vector<XrCompositionLayerWindowSpaceEXT> windowLayers;
    for (int i = 0; i < panelCount; i++) {
        if (panels[i].type == PanelType::Scene3D) continue;
        if (panels[i].swapchainInfo.swapchain == XR_NULL_HANDLE) continue;

        XrCompositionLayerWindowSpaceEXT layer = {};
        layer.type = (XrStructureType)XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_EXT;
        layer.next = nullptr;
        layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        layer.subImage.swapchain = panels[i].swapchainInfo.swapchain;
        layer.subImage.imageRect.offset = {0, 0};
        layer.subImage.imageRect.extent = {
            (int32_t)panels[i].swapchainInfo.width,
            (int32_t)panels[i].swapchainInfo.height
        };
        layer.subImage.imageArrayIndex = 0;
        layer.x = panels[i].current.x;
        layer.y = panels[i].current.y;
        layer.width = panels[i].current.width;
        layer.height = panels[i].current.height;
        layer.disparity = panels[i].disparity;
        windowLayers.push_back(layer);
    }

    // Build layer pointer array: projection first, then all window-space layers
    std::vector<const XrCompositionLayerBaseHeader*> layerPtrs;
    layerPtrs.push_back((const XrCompositionLayerBaseHeader*)&projectionLayer);
    for (auto& wl : windowLayers) {
        layerPtrs.push_back((const XrCompositionLayerBaseHeader*)&wl);
    }

    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = displayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = (uint32_t)layerPtrs.size();
    endInfo.layers = layerPtrs.data();

    return XR_SUCCEEDED(xrEndFrame(xr.session, &endInfo));
}
