// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenXR session management with XR_EXT_win32_window_binding extension
 */

#include "xr_session.h"
#include "logging.h"
#include <cstring>

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

    // Query available extensions
    uint32_t extensionCount = 0;
    XR_CHECK_LOG(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr));
    LOG_INFO("Found %u extensions available", extensionCount);

    std::vector<XrExtensionProperties> extensions(extensionCount, {XR_TYPE_EXTENSION_PROPERTIES});
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensions.data()));

    // Log all extensions and check for required ones
    LOG_INFO("Available extensions:");
    bool hasD3D11 = false;
    xr.hasWin32WindowBindingExt = false;

    for (const auto& ext : extensions) {
        LOG_DEBUG("  %s (v%u)", ext.extensionName, ext.extensionVersion);
        if (strcmp(ext.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0) {
            hasD3D11 = true;
        }
        if (strcmp(ext.extensionName, XR_EXT_WIN32_WINDOW_BINDING_EXTENSION_NAME) == 0) {
            xr.hasWin32WindowBindingExt = true;
        }
        if (strcmp(ext.extensionName, XR_EXT_DISPLAY_INFO_EXTENSION_NAME) == 0) {
            xr.hasDisplayInfoExt = true;
        }
    }

    LOG_INFO("XR_KHR_D3D11_enable: %s", hasD3D11 ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("XR_EXT_win32_window_binding: %s", xr.hasWin32WindowBindingExt ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("XR_EXT_display_info: %s", xr.hasDisplayInfoExt ? "AVAILABLE" : "NOT FOUND");

    if (!hasD3D11) {
        LOG_ERROR("XR_KHR_D3D11_enable extension not available - cannot continue");
        return false;
    }

    if (!xr.hasWin32WindowBindingExt) {
        LOG_WARN("XR_EXT_win32_window_binding extension not available - window targeting disabled");
        LOG_WARN("The runtime will create its own window instead of using the app window");
    }

    // Build list of extensions to enable
    std::vector<const char*> enabledExtensions;
    enabledExtensions.push_back(XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
    if (xr.hasWin32WindowBindingExt) {
        enabledExtensions.push_back(XR_EXT_WIN32_WINDOW_BINDING_EXTENSION_NAME);
    }
    if (xr.hasDisplayInfoExt) {
        enabledExtensions.push_back(XR_EXT_DISPLAY_INFO_EXTENSION_NAME);
    }

    LOG_INFO("Enabling %zu extensions", enabledExtensions.size());
    for (const auto& ext : enabledExtensions) {
        LOG_INFO("  %s", ext);
    }

    // Create instance
    LOG_INFO("Creating OpenXR instance...");
    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(createInfo.applicationInfo.applicationName, "SRCubeOpenXRExt");
    createInfo.applicationInfo.applicationVersion = 1;
    strcpy_s(createInfo.applicationInfo.engineName, "None");
    createInfo.applicationInfo.engineVersion = 0;
    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    createInfo.enabledExtensionCount = (uint32_t)enabledExtensions.size();
    createInfo.enabledExtensionNames = enabledExtensions.data();

    LOG_INFO("OpenXR API version: %d.%d.%d",
        XR_VERSION_MAJOR(XR_CURRENT_API_VERSION),
        XR_VERSION_MINOR(XR_CURRENT_API_VERSION),
        XR_VERSION_PATCH(XR_CURRENT_API_VERSION));

    XR_CHECK_LOG(xrCreateInstance(&createInfo, &xr.instance));
    LOG_INFO("OpenXR instance created: 0x%p", (void*)xr.instance);

    // Get system for HMD
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

    // Query display info via XR_EXT_display_info
    if (xr.hasDisplayInfoExt) {
        XrSystemProperties sysProps = {XR_TYPE_SYSTEM_PROPERTIES};
        XrDisplayInfoEXT displayInfo = {(XrStructureType)XR_TYPE_DISPLAY_INFO_EXT};
        sysProps.next = &displayInfo;
        XrResult diResult = xrGetSystemProperties(xr.instance, xr.systemId, &sysProps);
        if (XR_SUCCEEDED(diResult)) {
            xr.recommendedViewScaleX = displayInfo.recommendedViewScaleX;
            xr.recommendedViewScaleY = displayInfo.recommendedViewScaleY;
            xr.displayWidthM = displayInfo.displaySizeMeters.width;
            xr.displayHeightM = displayInfo.displaySizeMeters.height;
            xr.nominalViewerX = displayInfo.nominalViewerPositionInDisplaySpace.x;
            xr.nominalViewerY = displayInfo.nominalViewerPositionInDisplaySpace.y;
            xr.nominalViewerZ = displayInfo.nominalViewerPositionInDisplaySpace.z;
            xr.supportsDisplayModeSwitch = (displayInfo.supportsDisplayModeSwitch == XR_TRUE);
            xr.displayPixelWidth = displayInfo.displayPixelWidth;
            xr.displayPixelHeight = displayInfo.displayPixelHeight;
            LOG_INFO("Display info: scale=%.3fx%.3f, size=%.3fx%.3fm, pixels=%ux%u, nominal=(%.0f,%.0f,%.0f)mm, modeSwitch=%s",
                xr.recommendedViewScaleX, xr.recommendedViewScaleY,
                xr.displayWidthM, xr.displayHeightM,
                xr.displayPixelWidth, xr.displayPixelHeight,
                xr.nominalViewerX * 1000.0f, xr.nominalViewerY * 1000.0f, xr.nominalViewerZ * 1000.0f,
                xr.supportsDisplayModeSwitch ? "YES" : "NO");
        }

        // Load xrRequestDisplayModeEXT function pointer
        if (xr.supportsDisplayModeSwitch) {
            XrResult procResult = xrGetInstanceProcAddr(
                xr.instance, "xrRequestDisplayModeEXT",
                (PFN_xrVoidFunction*)&xr.pfnRequestDisplayModeEXT);
            if (XR_FAILED(procResult)) {
                LOG_WARN("Failed to load xrRequestDisplayModeEXT");
                xr.pfnRequestDisplayModeEXT = nullptr;
            }
        }
    }

    // Get view configuration views
    LOG_INFO("Enumerating view configuration views...");
    uint32_t viewCount = 0;
    XR_CHECK(xrEnumerateViewConfigurationViews(xr.instance, xr.systemId, xr.viewConfigType, 0, &viewCount, nullptr));

    xr.configViews.resize(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    XR_CHECK(xrEnumerateViewConfigurationViews(xr.instance, xr.systemId, xr.viewConfigType, viewCount, &viewCount, xr.configViews.data()));

    LOG_INFO("View configuration: %u views", viewCount);
    for (uint32_t i = 0; i < viewCount; i++) {
        const auto& view = xr.configViews[i];
        LOG_INFO("  View %u: recommended %ux%u, max %ux%u, samples %u",
            i, view.recommendedImageRectWidth, view.recommendedImageRectHeight,
            view.maxImageRectWidth, view.maxImageRectHeight,
            view.recommendedSwapchainSampleCount);
    }

    LOG_INFO("OpenXR initialization complete");
    return true;
}

bool CreateSession(XrSessionManager& xr, ID3D11Device* d3d11Device, HWND hwnd) {
    LOG_INFO("Creating OpenXR session with XR_EXT_win32_window_binding...");
    LOG_INFO("  D3D11 Device: 0x%p", d3d11Device);
    LOG_INFO("  Window handle (HWND): 0x%p", hwnd);

    xr.windowHandle = hwnd;

    // D3D11 binding is required
    XrGraphicsBindingD3D11KHR d3d11Binding = {XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    d3d11Binding.device = d3d11Device;

    // Session target extension - chain it to the D3D11 binding
    XrWin32WindowBindingCreateInfoEXT sessionTarget = {XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_EXT};
    sessionTarget.windowHandle = hwnd;

    if (xr.hasWin32WindowBindingExt && hwnd) {
        // Chain: sessionInfo -> d3d11Binding -> sessionTarget
        d3d11Binding.next = &sessionTarget;
        LOG_INFO("Using XR_EXT_win32_window_binding with window handle");
        LOG_INFO("  Chain: XrSessionCreateInfo -> XrGraphicsBindingD3D11KHR -> XrWin32WindowBindingCreateInfoEXT");
    } else {
        LOG_WARN("NOT using XR_EXT_win32_window_binding (hasExt=%d, hwnd=%p)",
            xr.hasWin32WindowBindingExt, hwnd);
        LOG_WARN("Runtime will create its own window for rendering");
    }

    XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &d3d11Binding;
    sessionInfo.systemId = xr.systemId;

    LOG_INFO("Calling xrCreateSession...");
    XR_CHECK_LOG(xrCreateSession(xr.instance, &sessionInfo, &xr.session));
    LOG_INFO("Session created: 0x%p", (void*)xr.session);

    return true;
}
