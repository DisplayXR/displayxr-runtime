// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenXR session management (IPC legacy mode — no XR_EXT_display_info)
 *
 * IPC variant: app code is identical to hosted_legacy — IPC is runtime-transparent.
 * Legacy: does NOT enable XR_EXT_display_info.
 * Swapchain uses recommendedImageRectWidth * 2 (compromise scaling).
 * Only V toggle (2D/3D) works — no 1/2/3 mode switching.
 */

#include "xr_session.h"
#include "logging.h"
#include <cstring>
#include <vector>

// Helper macro for XR error checking with logging
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

    LOG_INFO("D3D11 graphics requirements:");
    LOG_INFO("  Adapter LUID: 0x%08X%08X", graphicsReq.adapterLuid.HighPart, graphicsReq.adapterLuid.LowPart);
    LOG_INFO("  Min Feature Level: %d", graphicsReq.minFeatureLevel);

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
    bool hasD3D11 = false;

    for (const auto& ext : extensions) {
        LOG_DEBUG("  %s (v%u)", ext.extensionName, ext.extensionVersion);
        if (strcmp(ext.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0) {
            hasD3D11 = true;
        }
    }

    LOG_INFO("XR_KHR_D3D11_enable: %s", hasD3D11 ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("IPC legacy mode: NOT enabling XR_EXT_display_info");

    if (!hasD3D11) {
        LOG_ERROR("XR_KHR_D3D11_enable extension not available - cannot continue");
        return false;
    }

    // Legacy: only enable D3D11 — no display info, no window binding
    std::vector<const char*> enabledExtensions;
    enabledExtensions.push_back(XR_KHR_D3D11_ENABLE_EXTENSION_NAME);

    LOG_INFO("Enabling %zu extensions:", enabledExtensions.size());
    for (const auto& ext : enabledExtensions) {
        LOG_INFO("  %s", ext);
    }

    LOG_INFO("Creating OpenXR instance...");
    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(createInfo.applicationInfo.applicationName, "SRCubeOpenXR_IPC_Legacy");
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

    LOG_INFO("OpenXR initialization complete (IPC legacy mode)");
    return true;
}

bool CreateSession(XrSessionManager& xr, ID3D11Device* d3d11Device) {
    LOG_INFO("Creating OpenXR session (IPC legacy, DisplayXR creates window)...");
    LOG_INFO("  D3D11 Device: 0x%p", d3d11Device);

    XrGraphicsBindingD3D11KHR d3d11Binding = {XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    d3d11Binding.device = d3d11Device;
    d3d11Binding.next = nullptr;

    XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &d3d11Binding;
    sessionInfo.systemId = xr.systemId;

    LOG_INFO("Calling xrCreateSession...");
    XR_CHECK_LOG(xrCreateSession(xr.instance, &sessionInfo, &xr.session));
    LOG_INFO("Session created: 0x%p (IPC mode expected — verify with displayxr-service logs)", (void*)xr.session);

    // Legacy app: no rendering mode enumeration, no display info extension

    return true;
}
