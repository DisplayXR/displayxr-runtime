// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenXR session management for OpenGL (standard mode, no win32_window_binding)
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

bool GetOpenGLGraphicsRequirements(XrSessionManager& xr) {
    LOG_INFO("Getting OpenGL graphics requirements...");

    PFN_xrGetOpenGLGraphicsRequirementsKHR xrGetOpenGLGraphicsRequirementsKHR = nullptr;
    XrResult result = xrGetInstanceProcAddr(xr.instance, "xrGetOpenGLGraphicsRequirementsKHR",
        (PFN_xrVoidFunction*)&xrGetOpenGLGraphicsRequirementsKHR);
    if (XR_FAILED(result) || !xrGetOpenGLGraphicsRequirementsKHR) {
        LOG_ERROR("Failed to get xrGetOpenGLGraphicsRequirementsKHR function pointer");
        return false;
    }

    XrGraphicsRequirementsOpenGLKHR graphicsReq = {XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
    result = xrGetOpenGLGraphicsRequirementsKHR(xr.instance, xr.systemId, &graphicsReq);
    if (XR_FAILED(result)) {
        LogXrResult("xrGetOpenGLGraphicsRequirementsKHR", result);
        return false;
    }

    LOG_INFO("OpenGL graphics requirements:");
    LOG_INFO("  Min API version: %d.%d.%d",
        XR_VERSION_MAJOR(graphicsReq.minApiVersionSupported),
        XR_VERSION_MINOR(graphicsReq.minApiVersionSupported),
        XR_VERSION_PATCH(graphicsReq.minApiVersionSupported));
    LOG_INFO("  Max API version: %d.%d.%d",
        XR_VERSION_MAJOR(graphicsReq.maxApiVersionSupported),
        XR_VERSION_MINOR(graphicsReq.maxApiVersionSupported),
        XR_VERSION_PATCH(graphicsReq.maxApiVersionSupported));

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
    bool hasOpenGL = false;

    for (const auto& ext : extensions) {
        LOG_DEBUG("  %s (v%u)", ext.extensionName, ext.extensionVersion);
        if (strcmp(ext.extensionName, XR_KHR_OPENGL_ENABLE_EXTENSION_NAME) == 0) {
            hasOpenGL = true;
        }
        if (strcmp(ext.extensionName, XR_EXT_DISPLAY_INFO_EXTENSION_NAME) == 0) {
            xr.hasDisplayInfoExt = true;
        }
    }

    LOG_INFO("XR_KHR_opengl_enable: %s", hasOpenGL ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("XR_EXT_display_info: %s", xr.hasDisplayInfoExt ? "AVAILABLE" : "NOT FOUND");

    if (!hasOpenGL) {
        LOG_ERROR("XR_KHR_opengl_enable extension not available - cannot continue");
        return false;
    }

    // Enable OpenGL and display info (NOT win32_window_binding)
    std::vector<const char*> enabledExtensions;
    enabledExtensions.push_back(XR_KHR_OPENGL_ENABLE_EXTENSION_NAME);
    if (xr.hasDisplayInfoExt) {
        enabledExtensions.push_back(XR_EXT_DISPLAY_INFO_EXTENSION_NAME);
    }

    LOG_INFO("Enabling %zu extensions:", enabledExtensions.size());
    for (const auto& ext : enabledExtensions) {
        LOG_INFO("  %s", ext);
    }

    LOG_INFO("Creating OpenXR instance...");
    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(createInfo.applicationInfo.applicationName, "SRCubeGL");
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
            xr.displayPixelWidth = displayInfo.displayPixelWidth;
            xr.displayPixelHeight = displayInfo.displayPixelHeight;
            LOG_INFO("Display info: scale=%.3fx%.3f, size=%.3fx%.3fm, pixels=%ux%u, nominal=(%.0f,%.0f,%.0f)mm",
                xr.recommendedViewScaleX, xr.recommendedViewScaleY,
                xr.displayWidthM, xr.displayHeightM,
                xr.displayPixelWidth, xr.displayPixelHeight,
                xr.nominalViewerX * 1000.0f, xr.nominalViewerY * 1000.0f, xr.nominalViewerZ * 1000.0f);
        }
    }

    // Get rendering mode enumeration function pointer
    if (xr.hasDisplayInfoExt) {
        xrGetInstanceProcAddr(xr.instance, "xrEnumerateDisplayRenderingModesEXT",
            (PFN_xrVoidFunction*)&xr.pfnEnumerateDisplayRenderingModesEXT);
    }

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

    LOG_INFO("OpenXR initialization complete");
    return true;
}

bool CreateSession(XrSessionManager& xr, HDC hDC, HGLRC hGLRC) {
    LOG_INFO("Creating OpenXR session (DisplayXR creates window)...");
    LOG_INFO("  HDC: 0x%p", hDC);
    LOG_INFO("  HGLRC: 0x%p", hGLRC);

    XrGraphicsBindingOpenGLWin32KHR glBinding = {XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR};
    glBinding.hDC = hDC;
    glBinding.hGLRC = hGLRC;
    glBinding.next = nullptr;

    XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &glBinding;
    sessionInfo.systemId = xr.systemId;

    LOG_INFO("Calling xrCreateSession...");
    XR_CHECK_LOG(xrCreateSession(xr.instance, &sessionInfo, &xr.session));
    LOG_INFO("Session created: 0x%p", (void*)xr.session);

    // Enumerate rendering modes for tile layout
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
                    xr.renderingModeTileColumns[i] = modes[i].tileColumns;
                    xr.renderingModeTileRows[i] = modes[i].tileRows;
                    xr.renderingModeScaleX[i] = modes[i].viewScaleX;
                    xr.renderingModeScaleY[i] = modes[i].viewScaleY;
                    xr.renderingModeDisplay3D[i] = modes[i].hardwareDisplay3D ? true : false;
                    LOG_INFO("  [%u] %s (views=%u, tiles=%ux%u, scale=%.2fx%.2f, 3D=%d)",
                        modes[i].modeIndex, modes[i].modeName, modes[i].viewCount,
                        modes[i].tileColumns, modes[i].tileRows,
                        modes[i].viewScaleX, modes[i].viewScaleY, modes[i].hardwareDisplay3D);
                }
            }
        }
    }

    return true;
}
