// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenXR session management (legacy mode — no XR_EXT_display_info, OpenGL)
 *
 * This is the legacy variant: does NOT enable XR_EXT_display_info.
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
    }

    LOG_INFO("XR_KHR_opengl_enable: %s", hasOpenGL ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("Legacy mode: NOT enabling XR_EXT_display_info");

    if (!hasOpenGL) {
        LOG_ERROR("XR_KHR_opengl_enable extension not available - cannot continue");
        return false;
    }

    // Legacy: only enable OpenGL — no display info, no window binding
    std::vector<const char*> enabledExtensions;
    enabledExtensions.push_back(XR_KHR_OPENGL_ENABLE_EXTENSION_NAME);

    LOG_INFO("Enabling %zu extensions:", enabledExtensions.size());
    for (const auto& ext : enabledExtensions) {
        LOG_INFO("  %s", ext);
    }

    LOG_INFO("Creating OpenXR instance...");
    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(createInfo.applicationInfo.applicationName, "SRCubeOpenXR_LegacyGL");
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

    LOG_INFO("OpenXR initialization complete (legacy GL mode)");
    return true;
}

bool CreateSession(XrSessionManager& xr, HDC hDC, HGLRC hGLRC) {
    LOG_INFO("Creating OpenXR session (legacy GL, DisplayXR creates window)...");
    LOG_INFO("  HDC: 0x%p, HGLRC: 0x%p", hDC, hGLRC);

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

    // No rendering mode enumeration — legacy app uses compromise scaling
    // renderingModeCount stays 0, pfnEnumerateDisplayRenderingModesEXT stays null

    return true;
}
