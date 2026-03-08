// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenXR session management for OpenGL with shared texture
 *
 * Uses XR_EXT_win32_window_binding with sharedTextureHandle (windowHandle=NULL)
 * for offscreen shared texture compositing via the GL native compositor.
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

    std::vector<XrExtensionProperties> extensions(extensionCount, {XR_TYPE_EXTENSION_PROPERTIES});
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensions.data()));

    bool hasOpenGL = false;
    xr.hasWin32WindowBindingExt = false;

    for (const auto& ext : extensions) {
        LOG_DEBUG("  %s (v%u)", ext.extensionName, ext.extensionVersion);
        if (strcmp(ext.extensionName, XR_KHR_OPENGL_ENABLE_EXTENSION_NAME) == 0) {
            hasOpenGL = true;
        }
        if (strcmp(ext.extensionName, XR_EXT_WIN32_WINDOW_BINDING_EXTENSION_NAME) == 0) {
            xr.hasWin32WindowBindingExt = true;
        }
        if (strcmp(ext.extensionName, XR_EXT_DISPLAY_INFO_EXTENSION_NAME) == 0) {
            xr.hasDisplayInfoExt = true;
        }
    }

    LOG_INFO("XR_KHR_opengl_enable: %s", hasOpenGL ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("XR_EXT_win32_window_binding: %s", xr.hasWin32WindowBindingExt ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("XR_EXT_display_info: %s", xr.hasDisplayInfoExt ? "AVAILABLE" : "NOT FOUND");

    if (!hasOpenGL) {
        LOG_ERROR("XR_KHR_opengl_enable extension not available");
        return false;
    }

    if (!xr.hasWin32WindowBindingExt) {
        LOG_ERROR("XR_EXT_win32_window_binding extension required for shared texture mode");
        return false;
    }

    std::vector<const char*> enabledExtensions;
    enabledExtensions.push_back(XR_KHR_OPENGL_ENABLE_EXTENSION_NAME);
    enabledExtensions.push_back(XR_EXT_WIN32_WINDOW_BINDING_EXTENSION_NAME);
    if (xr.hasDisplayInfoExt) {
        enabledExtensions.push_back(XR_EXT_DISPLAY_INFO_EXTENSION_NAME);
    }

    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(createInfo.applicationInfo.applicationName, "SRCubeSharedGL");
    createInfo.applicationInfo.applicationVersion = 1;
    strcpy_s(createInfo.applicationInfo.engineName, "None");
    createInfo.applicationInfo.engineVersion = 0;
    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    createInfo.enabledExtensionCount = (uint32_t)enabledExtensions.size();
    createInfo.enabledExtensionNames = enabledExtensions.data();

    XR_CHECK_LOG(xrCreateInstance(&createInfo, &xr.instance));
    LOG_INFO("OpenXR instance created");

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
        XrEyeTrackingModeCapabilitiesEXT eyeCaps = {(XrStructureType)XR_TYPE_EYE_TRACKING_MODE_CAPABILITIES_EXT};
        displayInfo.next = &eyeCaps;
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
            xr.supportedEyeTrackingModes = (uint32_t)eyeCaps.supportedModes;
            xr.defaultEyeTrackingMode = (uint32_t)eyeCaps.defaultMode;
            LOG_INFO("Display info: scale=%.3fx%.3f, size=%.3fx%.3fm, pixels=%ux%u, nominal=(%.0f,%.0f,%.0f)mm, modeSwitch=%s",
                xr.recommendedViewScaleX, xr.recommendedViewScaleY,
                xr.displayWidthM, xr.displayHeightM,
                xr.displayPixelWidth, xr.displayPixelHeight,
                xr.nominalViewerX * 1000.0f, xr.nominalViewerY * 1000.0f, xr.nominalViewerZ * 1000.0f,
                xr.supportsDisplayModeSwitch ? "YES" : "NO");
            LOG_INFO("Eye tracking: supported=0x%x, default=%u",
                xr.supportedEyeTrackingModes, xr.defaultEyeTrackingMode);
        }

        if (xr.supportsDisplayModeSwitch) {
            XrResult procResult = xrGetInstanceProcAddr(
                xr.instance, "xrRequestDisplayModeEXT",
                (PFN_xrVoidFunction*)&xr.pfnRequestDisplayModeEXT);
            if (XR_FAILED(procResult)) {
                LOG_WARN("Failed to load xrRequestDisplayModeEXT");
                xr.pfnRequestDisplayModeEXT = nullptr;
            }
        }

        if (xr.supportedEyeTrackingModes != 0) {
            xrGetInstanceProcAddr(xr.instance, "xrRequestEyeTrackingModeEXT",
                (PFN_xrVoidFunction*)&xr.pfnRequestEyeTrackingModeEXT);
        }

        xrGetInstanceProcAddr(xr.instance, "xrRequestDisplayRenderingModeEXT",
            (PFN_xrVoidFunction*)&xr.pfnRequestDisplayRenderingModeEXT);
        xrGetInstanceProcAddr(xr.instance, "xrEnumerateDisplayRenderingModesEXT",
            (PFN_xrVoidFunction*)&xr.pfnEnumerateDisplayRenderingModesEXT);
        LOG_INFO("Display rendering mode: %s",
            xr.pfnRequestDisplayRenderingModeEXT ? "available" : "not available");
    }

    uint32_t viewCount = 0;
    XR_CHECK(xrEnumerateViewConfigurationViews(xr.instance, xr.systemId, xr.viewConfigType, 0, &viewCount, nullptr));
    xr.configViews.resize(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    XR_CHECK(xrEnumerateViewConfigurationViews(xr.instance, xr.systemId, xr.viewConfigType, viewCount, &viewCount, xr.configViews.data()));

    for (uint32_t i = 0; i < viewCount; i++) {
        LOG_INFO("  View %u: %ux%u", i,
            xr.configViews[i].recommendedImageRectWidth,
            xr.configViews[i].recommendedImageRectHeight);
    }

    return true;
}

bool CreateSession(XrSessionManager& xr, HDC hDC, HGLRC hGLRC, HANDLE sharedTextureHandle) {
    LOG_INFO("Creating OpenXR session with OpenGL + shared texture...");
    LOG_INFO("  Shared texture handle: 0x%p", sharedTextureHandle);

    XrGraphicsBindingOpenGLWin32KHR glBinding = {XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR};
    glBinding.hDC = hDC;
    glBinding.hGLRC = hGLRC;

    // Win32 window binding with shared texture (windowHandle=NULL for offscreen)
    XrWin32WindowBindingCreateInfoEXT sessionTarget = {XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_EXT};
    sessionTarget.windowHandle = NULL;
    sessionTarget.sharedTextureHandle = (void*)sharedTextureHandle;

    // Chain: sessionInfo -> glBinding -> sessionTarget
    glBinding.next = &sessionTarget;
    LOG_INFO("Using XR_EXT_win32_window_binding with shared texture (offscreen mode)");

    XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &glBinding;
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
                    LOG_INFO("  [%u] %s", modes[i].modeIndex, modes[i].modeName);
                }
            }
        }
    }

    return true;
}
