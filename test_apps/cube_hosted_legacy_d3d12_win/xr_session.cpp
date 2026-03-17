// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenXR session management for D3D12 (legacy mode — no XR_EXT_display_info)
 *
 * This is the legacy variant: does NOT enable XR_EXT_display_info.
 * Swapchain uses recommendedImageRectWidth * 2 (compromise scaling).
 * Only V toggle (2D/3D) works — no 1/2/3 mode switching.
 */

#include "xr_session.h"
#include "logging.h"
#include <cstring>
#include <vector>

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

bool GetD3D12GraphicsRequirements(XrSessionManager& xr, LUID* outAdapterLuid) {
    LOG_INFO("Getting D3D12 graphics requirements...");

    PFN_xrGetD3D12GraphicsRequirementsKHR xrGetD3D12GraphicsRequirementsKHR = nullptr;
    XrResult result = xrGetInstanceProcAddr(xr.instance, "xrGetD3D12GraphicsRequirementsKHR",
        (PFN_xrVoidFunction*)&xrGetD3D12GraphicsRequirementsKHR);
    if (XR_FAILED(result) || !xrGetD3D12GraphicsRequirementsKHR) {
        LOG_ERROR("Failed to get xrGetD3D12GraphicsRequirementsKHR function pointer");
        return false;
    }

    XrGraphicsRequirementsD3D12KHR graphicsReq = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    result = xrGetD3D12GraphicsRequirementsKHR(xr.instance, xr.systemId, &graphicsReq);
    if (XR_FAILED(result)) {
        LogXrResult("xrGetD3D12GraphicsRequirementsKHR", result);
        return false;
    }

    LOG_INFO("D3D12 graphics requirements:");
    LOG_INFO("  Adapter LUID: 0x%08X%08X", graphicsReq.adapterLuid.HighPart, graphicsReq.adapterLuid.LowPart);
    LOG_INFO("  Min Feature Level: 0x%X", graphicsReq.minFeatureLevel);

    *outAdapterLuid = graphicsReq.adapterLuid;
    return true;
}

bool InitializeOpenXR(XrSessionManager& xr) {
    LOG_INFO("Querying OpenXR instance extension properties...");

    uint32_t extensionCount = 0;
    XR_CHECK_LOG(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr));
    LOG_INFO("Found %u extensions available", extensionCount);

    std::vector<XrExtensionProperties> extensions(extensionCount, {XR_TYPE_EXTENSION_PROPERTIES});
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensions.data()));

    LOG_INFO("Available extensions:");
    bool hasD3D12 = false;

    for (const auto& ext : extensions) {
        LOG_DEBUG("  %s (v%u)", ext.extensionName, ext.extensionVersion);
        if (strcmp(ext.extensionName, XR_KHR_D3D12_ENABLE_EXTENSION_NAME) == 0) {
            hasD3D12 = true;
        }
    }

    LOG_INFO("XR_KHR_D3D12_enable: %s", hasD3D12 ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("Legacy mode: NOT enabling XR_EXT_display_info");

    if (!hasD3D12) {
        LOG_ERROR("XR_KHR_D3D12_enable extension not available - cannot continue");
        return false;
    }

    // Legacy: only enable D3D12 — no display info, no window binding
    std::vector<const char*> enabledExtensions;
    enabledExtensions.push_back(XR_KHR_D3D12_ENABLE_EXTENSION_NAME);

    LOG_INFO("Enabling %zu extensions:", enabledExtensions.size());
    for (const auto& ext : enabledExtensions) {
        LOG_INFO("  %s", ext);
    }

    LOG_INFO("Creating OpenXR instance...");
    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(createInfo.applicationInfo.applicationName, "SRCubeD3D12_Legacy");
    createInfo.applicationInfo.applicationVersion = 1;
    strcpy_s(createInfo.applicationInfo.engineName, "None");
    createInfo.applicationInfo.engineVersion = 0;
    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    createInfo.enabledExtensionCount = (uint32_t)enabledExtensions.size();
    createInfo.enabledExtensionNames = enabledExtensions.data();

    XR_CHECK_LOG(xrCreateInstance(&createInfo, &xr.instance));
    LOG_INFO("OpenXR instance created: 0x%p", (void*)xr.instance);

    LOG_INFO("Getting system for HMD form factor...");
    XrSystemGetInfo systemInfo = {XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK_LOG(xrGetSystem(xr.instance, &systemInfo, &xr.systemId));
    LOG_INFO("System ID: %llu", (unsigned long long)xr.systemId);

    // Get system name
    {
        XrSystemProperties sysProps = {XR_TYPE_SYSTEM_PROPERTIES};
        if (XR_SUCCEEDED(xrGetSystemProperties(xr.instance, xr.systemId, &sysProps))) {
            memcpy(xr.systemName, sysProps.systemName, sizeof(xr.systemName));
            LOG_INFO("System name: %s", xr.systemName);
        }
    }

    // No display info query — legacy app does not enable the extension
    // hasDisplayInfoExt stays false, no rendering mode enumeration

    LOG_INFO("Enumerating view configuration views...");
    uint32_t viewCount = 0;
    XR_CHECK(xrEnumerateViewConfigurationViews(xr.instance, xr.systemId, xr.viewConfigType, 0, &viewCount, nullptr));

    xr.configViews.resize(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    XR_CHECK(xrEnumerateViewConfigurationViews(xr.instance, xr.systemId, xr.viewConfigType, viewCount, &viewCount, xr.configViews.data()));

    LOG_INFO("View configuration: %u views", viewCount);
    for (uint32_t i = 0; i < viewCount; i++) {
        const auto& view = xr.configViews[i];
        LOG_INFO("  View %u: recommended %ux%u, max %ux%u",
            i, view.recommendedImageRectWidth, view.recommendedImageRectHeight,
            view.maxImageRectWidth, view.maxImageRectHeight);
    }

    LOG_INFO("OpenXR initialization complete (legacy mode)");
    return true;
}

bool CreateSession(XrSessionManager& xr, ID3D12Device* device, ID3D12CommandQueue* queue) {
    LOG_INFO("Creating OpenXR session (legacy, DisplayXR creates window)...");
    LOG_INFO("  D3D12 Device: 0x%p", device);
    LOG_INFO("  Command Queue: 0x%p", queue);

    XrGraphicsBindingD3D12KHR d3d12Binding = {XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    d3d12Binding.device = device;
    d3d12Binding.queue = queue;
    d3d12Binding.next = nullptr;

    XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &d3d12Binding;
    sessionInfo.systemId = xr.systemId;

    LOG_INFO("Calling xrCreateSession...");
    XR_CHECK_LOG(xrCreateSession(xr.instance, &sessionInfo, &xr.session));
    LOG_INFO("Session created: 0x%p", (void*)xr.session);

    // No rendering mode enumeration — legacy app uses compromise scaling
    // renderingModeCount stays 0, pfnEnumerateDisplayRenderingModesEXT stays null

    return true;
}
