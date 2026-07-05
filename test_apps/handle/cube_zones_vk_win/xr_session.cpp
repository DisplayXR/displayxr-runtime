// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenXR session management for the XR_EXT_display_zones exerciser —
 *         native Vulkan HANDLE leg (array / single-pass-instanced stereo).
 */

#include "xr_session.h"
#include "logging.h"
#include <cstring>

bool g_hasViewRigExt = false;

// XR_EXT_local_3d_zone harness (see xr_session.h).
ZoneMaskHarness g_zone;

// XR_EXT_display_zones (ADR-027) — see xr_session.h.
bool g_hasDisplayZonesExt = false;
DisplayZonesHarness g_zones;

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

bool InitializeOpenXR(XrSessionManager& xr) {
    LOG_INFO("Querying OpenXR instance extension properties...");

    uint32_t extensionCount = 0;
    XR_CHECK_LOG(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr));

    std::vector<XrExtensionProperties> extensions(extensionCount, {XR_TYPE_EXTENSION_PROPERTIES});
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensions.data()));

    bool hasVulkan = false;
    bool displayZonesAdvertised = false;
    xr.hasWin32WindowBindingExt = false;

    for (const auto& ext : extensions) {
        LOG_DEBUG("  %s (v%u)", ext.extensionName, ext.extensionVersion);
        if (strcmp(ext.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0) {
            hasVulkan = true;
        }
        if (strcmp(ext.extensionName, XR_EXT_WIN32_WINDOW_BINDING_EXTENSION_NAME) == 0) {
            xr.hasWin32WindowBindingExt = true;
        }
        if (strcmp(ext.extensionName, XR_EXT_DISPLAY_INFO_EXTENSION_NAME) == 0) {
            xr.hasDisplayInfoExt = true;
        }
        if (strcmp(ext.extensionName, XR_EXT_ATLAS_CAPTURE_EXTENSION_NAME) == 0) {
            xr.hasAtlasCaptureExt = true;
        }
        if (strcmp(ext.extensionName, XR_EXT_MCP_TOOLS_EXTENSION_NAME) == 0) {
            xr.hasMcpToolsExt = true;
        }
        if (strcmp(ext.extensionName, XR_EXT_VIEW_RIG_EXTENSION_NAME) == 0) {
            g_hasViewRigExt = true;
        }
        if (strcmp(ext.extensionName, XR_EXT_LOCAL_3D_ZONE_EXTENSION_NAME) == 0) {
            g_zone.available = true;
        }
        if (strcmp(ext.extensionName, XR_EXT_DISPLAY_ZONES_EXTENSION_NAME) == 0) {
            displayZonesAdvertised = true;
        }
    }

    // XR_EXT_display_zones requires local_3d_zone + view_rig — enable it only
    // when the whole composition is available.
    g_hasDisplayZonesExt = displayZonesAdvertised && g_zone.available && g_hasViewRigExt;

    LOG_INFO("XR_KHR_vulkan_enable: %s", hasVulkan ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("XR_EXT_win32_window_binding: %s", xr.hasWin32WindowBindingExt ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("XR_EXT_display_info: %s", xr.hasDisplayInfoExt ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("XR_EXT_mcp_tools: %s", xr.hasMcpToolsExt ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("XR_EXT_view_rig: %s", g_hasViewRigExt ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("XR_EXT_local_3d_zone: %s", g_zone.available ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("XR_EXT_display_zones: %s", g_hasDisplayZonesExt ? "AVAILABLE" : "NOT FOUND");

    if (!displayZonesAdvertised) {
        // The runtime gates the extension behind the P2 dev flag. Graceful
        // degrade: the app runs but submits empty frames.
        LOG_ERROR("XR_EXT_display_zones not advertised - run with DISPLAYXR_ZONES=1");
    } else if (!g_hasDisplayZonesExt) {
        LOG_ERROR("XR_EXT_display_zones advertised but a prerequisite is missing "
                  "(local_3d_zone=%d, view_rig=%d) - zones path disabled",
                  g_zone.available, g_hasViewRigExt);
    }

    if (!hasVulkan) {
        LOG_ERROR("XR_KHR_vulkan_enable extension not available");
        return false;
    }

    std::vector<const char*> enabledExtensions;
    enabledExtensions.push_back(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
    if (xr.hasWin32WindowBindingExt) {
        enabledExtensions.push_back(XR_EXT_WIN32_WINDOW_BINDING_EXTENSION_NAME);
    }
    if (xr.hasDisplayInfoExt) {
        enabledExtensions.push_back(XR_EXT_DISPLAY_INFO_EXTENSION_NAME);
    }
    if (xr.hasAtlasCaptureExt) {
        enabledExtensions.push_back(XR_EXT_ATLAS_CAPTURE_EXTENSION_NAME);
    }
    if (xr.hasMcpToolsExt) {
        enabledExtensions.push_back(XR_EXT_MCP_TOOLS_EXTENSION_NAME);
    }
    if (g_hasViewRigExt) {
        enabledExtensions.push_back(XR_EXT_VIEW_RIG_EXTENSION_NAME);
    }
    if (g_zone.available) {
        enabledExtensions.push_back(XR_EXT_LOCAL_3D_ZONE_EXTENSION_NAME);
    }
    if (g_hasDisplayZonesExt) {
        enabledExtensions.push_back(XR_EXT_DISPLAY_ZONES_EXTENSION_NAME);
    }

    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(createInfo.applicationInfo.applicationName, "DXRCubeZonesVK");
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
            xr.displayPixelWidth = displayInfo.displayPixelWidth;
            xr.displayPixelHeight = displayInfo.displayPixelHeight;
            xr.supportedEyeTrackingModes = (uint32_t)eyeCaps.supportedModes;
            xr.defaultEyeTrackingMode = (uint32_t)eyeCaps.defaultMode;
            LOG_INFO("Display info: scale=%.3fx%.3f, size=%.3fx%.3fm, pixels=%ux%u, nominal=(%.0f,%.0f,%.0f)mm",
                xr.recommendedViewScaleX, xr.recommendedViewScaleY,
                xr.displayWidthM, xr.displayHeightM,
                xr.displayPixelWidth, xr.displayPixelHeight,
                xr.nominalViewerX * 1000.0f, xr.nominalViewerY * 1000.0f, xr.nominalViewerZ * 1000.0f);
            LOG_INFO("Eye tracking: supported=0x%x, default=%u",
                xr.supportedEyeTrackingModes, xr.defaultEyeTrackingMode);
        }

        // Load xrRequestDisplayModeEXT function pointer
        xrGetInstanceProcAddr(xr.instance, "xrRequestDisplayModeEXT",
            (PFN_xrVoidFunction*)&xr.pfnRequestDisplayModeEXT);

        // Load xrRequestEyeTrackingModeEXT function pointer
        if (xr.supportedEyeTrackingModes != 0) {
            xrGetInstanceProcAddr(xr.instance, "xrRequestEyeTrackingModeEXT",
                (PFN_xrVoidFunction*)&xr.pfnRequestEyeTrackingModeEXT);
        }

        // Load xrRequestDisplayRenderingModeEXT function pointer (v7)
        xrGetInstanceProcAddr(xr.instance, "xrRequestDisplayRenderingModeEXT",
            (PFN_xrVoidFunction*)&xr.pfnRequestDisplayRenderingModeEXT);
        xrGetInstanceProcAddr(xr.instance, "xrEnumerateDisplayRenderingModesEXT",
            (PFN_xrVoidFunction*)&xr.pfnEnumerateDisplayRenderingModesEXT);
    }

    // XR_EXT_atlas_capture (#396 W6): resolve the runtime-owned capture entry.
    if (xr.hasAtlasCaptureExt) {
        xrGetInstanceProcAddr(xr.instance, "xrCaptureAtlasEXT",
            (PFN_xrVoidFunction*)&xr.pfnCaptureAtlasEXT);
        LOG_INFO("xrCaptureAtlasEXT: %s", xr.pfnCaptureAtlasEXT ? "resolved" : "NULL");
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

bool GetVulkanGraphicsRequirements(XrSessionManager& xr) {
    LOG_INFO("Getting Vulkan graphics requirements...");

    PFN_xrGetVulkanGraphicsRequirementsKHR xrGetVulkanGraphicsRequirementsKHR = nullptr;
    XrResult result = xrGetInstanceProcAddr(xr.instance, "xrGetVulkanGraphicsRequirementsKHR",
        (PFN_xrVoidFunction*)&xrGetVulkanGraphicsRequirementsKHR);
    if (XR_FAILED(result) || !xrGetVulkanGraphicsRequirementsKHR) {
        LOG_ERROR("Failed to get xrGetVulkanGraphicsRequirementsKHR function pointer");
        return false;
    }

    XrGraphicsRequirementsVulkanKHR graphicsReq = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    result = xrGetVulkanGraphicsRequirementsKHR(xr.instance, xr.systemId, &graphicsReq);
    if (XR_FAILED(result)) {
        LogXrResult("xrGetVulkanGraphicsRequirementsKHR", result);
        return false;
    }

    LOG_INFO("Vulkan graphics requirements:");
    LOG_INFO("  Min API version: %d.%d.%d",
        VK_VERSION_MAJOR(graphicsReq.minApiVersionSupported),
        VK_VERSION_MINOR(graphicsReq.minApiVersionSupported),
        VK_VERSION_PATCH(graphicsReq.minApiVersionSupported));
    LOG_INFO("  Max API version: %d.%d.%d",
        VK_VERSION_MAJOR(graphicsReq.maxApiVersionSupported),
        VK_VERSION_MINOR(graphicsReq.maxApiVersionSupported),
        VK_VERSION_PATCH(graphicsReq.maxApiVersionSupported));

    return true;
}

bool CreateVulkanInstance(XrSessionManager& xr, VkInstance& vkInstance) {
    LOG_INFO("Creating Vulkan instance with OpenXR required extensions...");

    // Get required Vulkan instance extensions from the runtime
    PFN_xrGetVulkanInstanceExtensionsKHR xrGetVulkanInstanceExtensionsKHR = nullptr;
    XrResult result = xrGetInstanceProcAddr(xr.instance, "xrGetVulkanInstanceExtensionsKHR",
        (PFN_xrVoidFunction*)&xrGetVulkanInstanceExtensionsKHR);
    if (XR_FAILED(result) || !xrGetVulkanInstanceExtensionsKHR) {
        LOG_ERROR("Failed to get xrGetVulkanInstanceExtensionsKHR");
        return false;
    }

    uint32_t bufferSize = 0;
    xrGetVulkanInstanceExtensionsKHR(xr.instance, xr.systemId, 0, &bufferSize, nullptr);
    std::string extensionsStr(bufferSize, '\0');
    xrGetVulkanInstanceExtensionsKHR(xr.instance, xr.systemId, bufferSize, &bufferSize, extensionsStr.data());

    // Parse space-separated extension names
    std::vector<const char*> extensionPtrs;
    // Store parsed strings so pointers remain valid
    std::vector<std::string> extensionNames;
    {
        size_t start = 0;
        while (start < extensionsStr.size()) {
            size_t end = extensionsStr.find(' ', start);
            if (end == std::string::npos) end = extensionsStr.size();
            std::string name = extensionsStr.substr(start, end - start);
            if (!name.empty() && name[0] != '\0') {
                extensionNames.push_back(name);
            }
            start = end + 1;
        }
    }
    for (auto& name : extensionNames) {
        extensionPtrs.push_back(name.c_str());
        LOG_INFO("  Required VkInstance extension: %s", name.c_str());
    }

    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "SRCubeOpenXRExtVK";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "None";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = (uint32_t)extensionPtrs.size();
    createInfo.ppEnabledExtensionNames = extensionPtrs.data();

    VkResult vkResult = vkCreateInstance(&createInfo, nullptr, &vkInstance);
    if (vkResult != VK_SUCCESS) {
        LOG_ERROR("vkCreateInstance failed: %d", vkResult);
        return false;
    }

    LOG_INFO("Vulkan instance created");
    return true;
}

bool GetVulkanPhysicalDevice(XrSessionManager& xr, VkInstance vkInstance, VkPhysicalDevice& physDevice) {
    LOG_INFO("Getting Vulkan physical device from OpenXR runtime...");

    PFN_xrGetVulkanGraphicsDeviceKHR xrGetVulkanGraphicsDeviceKHR = nullptr;
    XrResult result = xrGetInstanceProcAddr(xr.instance, "xrGetVulkanGraphicsDeviceKHR",
        (PFN_xrVoidFunction*)&xrGetVulkanGraphicsDeviceKHR);
    if (XR_FAILED(result) || !xrGetVulkanGraphicsDeviceKHR) {
        LOG_ERROR("Failed to get xrGetVulkanGraphicsDeviceKHR");
        return false;
    }

    result = xrGetVulkanGraphicsDeviceKHR(xr.instance, xr.systemId, vkInstance, &physDevice);
    if (XR_FAILED(result)) {
        LogXrResult("xrGetVulkanGraphicsDeviceKHR", result);
        return false;
    }

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physDevice, &props);
    LOG_INFO("Vulkan physical device: %s", props.deviceName);
    LOG_INFO("  API version: %d.%d.%d",
        VK_VERSION_MAJOR(props.apiVersion),
        VK_VERSION_MINOR(props.apiVersion),
        VK_VERSION_PATCH(props.apiVersion));

    return true;
}

bool GetVulkanDeviceExtensions(XrSessionManager& xr, VkInstance vkInstance, VkPhysicalDevice physDevice,
    std::vector<const char*>& deviceExtensions, std::vector<std::string>& extensionStorage)
{
    PFN_xrGetVulkanDeviceExtensionsKHR xrGetVulkanDeviceExtensionsKHR = nullptr;
    XrResult result = xrGetInstanceProcAddr(xr.instance, "xrGetVulkanDeviceExtensionsKHR",
        (PFN_xrVoidFunction*)&xrGetVulkanDeviceExtensionsKHR);
    if (XR_FAILED(result) || !xrGetVulkanDeviceExtensionsKHR) {
        LOG_ERROR("Failed to get xrGetVulkanDeviceExtensionsKHR");
        return false;
    }

    uint32_t bufferSize = 0;
    xrGetVulkanDeviceExtensionsKHR(xr.instance, xr.systemId, 0, &bufferSize, nullptr);

    std::string extensionsStr(bufferSize, '\0');
    xrGetVulkanDeviceExtensionsKHR(xr.instance, xr.systemId, bufferSize, &bufferSize, extensionsStr.data());

    // Parse space-separated extension names
    std::vector<std::string> requested;
    {
        size_t start = 0;
        while (start < extensionsStr.size()) {
            size_t end = extensionsStr.find(' ', start);
            if (end == std::string::npos) end = extensionsStr.size();
            std::string name = extensionsStr.substr(start, end - start);
            if (!name.empty() && name[0] != '\0') {
                requested.push_back(name);
            }
            start = end + 1;
        }
    }

    // Query which extensions the device actually supports
    uint32_t availCount = 0;
    vkEnumerateDeviceExtensionProperties(physDevice, nullptr, &availCount, nullptr);
    std::vector<VkExtensionProperties> availExts(availCount);
    vkEnumerateDeviceExtensionProperties(physDevice, nullptr, &availCount, availExts.data());

    // Filter: only request extensions the device actually exposes
    // (extensions promoted to Vulkan core may not be listed)
    extensionStorage.clear();
    deviceExtensions.clear();
    for (auto& name : requested) {
        bool available = false;
        for (auto& ext : availExts) {
            if (name == ext.extensionName) { available = true; break; }
        }
        if (available) {
            extensionStorage.push_back(name);
            LOG_INFO("  Required VkDevice extension: %s", name.c_str());
        } else {
            LOG_INFO("  Skipping promoted-to-core extension: %s", name.c_str());
        }
    }
    for (auto& name : extensionStorage) {
        deviceExtensions.push_back(name.c_str());
    }

    return true;
}

bool FindGraphicsQueueFamily(VkPhysicalDevice physDevice, uint32_t& queueFamilyIndex) {
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physDevice, &queueFamilyCount, nullptr);

    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physDevice, &queueFamilyCount, queueFamilies.data());

    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            queueFamilyIndex = i;
            LOG_INFO("Graphics queue family: %u", i);
            return true;
        }
    }

    LOG_ERROR("No graphics queue family found");
    return false;
}

bool CreateVulkanDevice(VkPhysicalDevice physDevice, uint32_t queueFamilyIndex,
    const std::vector<const char*>& deviceExtensions,
    VkDevice& device, VkQueue& graphicsQueue)
{
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo = {};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = queueFamilyIndex;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &queuePriority;

    VkDeviceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = 1;
    createInfo.pQueueCreateInfos = &queueInfo;
    createInfo.enabledExtensionCount = (uint32_t)deviceExtensions.size();
    createInfo.ppEnabledExtensionNames = deviceExtensions.data();

    VkResult vkResult = vkCreateDevice(physDevice, &createInfo, nullptr, &device);
    if (vkResult != VK_SUCCESS) {
        LOG_ERROR("vkCreateDevice failed: %d", vkResult);
        return false;
    }

    vkGetDeviceQueue(device, queueFamilyIndex, 0, &graphicsQueue);
    LOG_INFO("Vulkan device and graphics queue created");
    return true;
}

bool CreateSession(XrSessionManager& xr, VkInstance vkInstance, VkPhysicalDevice physDevice,
    VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, HWND hwnd)
{
    LOG_INFO("Creating OpenXR session with Vulkan + XR_EXT_win32_window_binding...");

    xr.windowHandle = hwnd;

    XrGraphicsBindingVulkanKHR vkBinding = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    vkBinding.instance = vkInstance;
    vkBinding.physicalDevice = physDevice;
    vkBinding.device = device;
    vkBinding.queueFamilyIndex = queueFamilyIndex;
    vkBinding.queueIndex = queueIndex;

    XrWin32WindowBindingCreateInfoEXT sessionTarget = {XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_EXT};
    sessionTarget.windowHandle = hwnd;

    // Opt-in transparency. Pair with WS_EX_NOREDIRECTIONBITMAP + null brush
    // (main.cpp) and the per-zone α clear (zone B clears RGBA(0,0,0,0)). The VK
    // native compositor presents through a transparent compositeAlpha so
    // α=0 pixels show the desktop.
    {
        // Match main.cpp's TransparentBackgroundEnabled() — ON BY DEFAULT (=0 opts
        // out). Must agree with the WINDOW's WS_EX_NOREDIRECTIONBITMAP choice: a
        // transparent window + opaque binding makes the runtime use the opaque
        // swapchain path (opaque black floor) instead of the transparent path —
        // the two defaults MUST stay in lockstep.
        const char *e = getenv("DISPLAYXR_TRANSPARENT_BG");
        if (e == nullptr || *e == '\0' || *e != '0') {
            sessionTarget.transparentBackgroundEnabled = XR_TRUE;
            LOG_INFO("Transparent background ENABLED (default; DISPLAYXR_TRANSPARENT_BG=0 opts out)");
        }
    }

    if (xr.hasWin32WindowBindingExt && hwnd) {
        vkBinding.next = &sessionTarget;
        LOG_INFO("Using XR_EXT_win32_window_binding with window handle");
    }

    XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &vkBinding;
    sessionInfo.systemId = xr.systemId;

    XR_CHECK_LOG(xrCreateSession(xr.instance, &sessionInfo, &xr.session));
    LOG_INFO("Session created: 0x%p", (void*)xr.session);

    // XR_EXT_mcp_tools (#457): declare identity + register agent tools. The
    // appId MUST match `id` in displayxr/cube_zones_vk_win.displayxr.json
    // (INV-10.1). Failure is non-fatal by design — the MCP capability gate
    // may simply be off on this machine.
    if (xr.hasMcpToolsExt) {
        xrGetInstanceProcAddr(xr.instance, "xrSetMCPAppInfoEXT",
            (PFN_xrVoidFunction*)&xr.pfnSetMCPAppInfoEXT);
        xrGetInstanceProcAddr(xr.instance, "xrRegisterMCPToolEXT",
            (PFN_xrVoidFunction*)&xr.pfnRegisterMCPToolEXT);
        xrGetInstanceProcAddr(xr.instance, "xrGetMCPToolCallArgsEXT",
            (PFN_xrVoidFunction*)&xr.pfnGetMCPToolCallArgsEXT);
        xrGetInstanceProcAddr(xr.instance, "xrSubmitMCPToolResultEXT",
            (PFN_xrVoidFunction*)&xr.pfnSubmitMCPToolResultEXT);
        if (xr.pfnSetMCPAppInfoEXT && xr.pfnRegisterMCPToolEXT && xr.pfnSubmitMCPToolResultEXT) {
            XrMCPAppInfoEXT mcpAppInfo = {XR_TYPE_MCP_APP_INFO_EXT};
            strncpy(mcpAppInfo.appId, "cube-zones-vk", sizeof(mcpAppInfo.appId) - 1);
            XrResult ar = xr.pfnSetMCPAppInfoEXT(xr.session, &mcpAppInfo);

            XrMCPToolInfoEXT setSpin = {XR_TYPE_MCP_TOOL_INFO_EXT};
            setSpin.name = "set_spin";
            setSpin.description =
                "Set the cube's spin speed. Takes effect immediately; the change is "
                "visually verifiable via capture_frame. Returns the applied speed.";
            setSpin.inputSchemaJson =
                "{\"type\":\"object\",\"properties\":{\"speed_rad_per_sec\":{\"type\":\"number\","
                "\"minimum\":0,\"maximum\":10,\"description\":\"Spin speed in radians/second; "
                "0 freezes the cube. Default at launch is 0.5.\"}},"
                "\"required\":[\"speed_rad_per_sec\"]}";
            XrResult tr1 = xr.pfnRegisterMCPToolEXT(xr.session, &setSpin);

            XrMCPToolInfoEXT getStatus = {XR_TYPE_MCP_TOOL_INFO_EXT};
            getStatus.name = "get_status";
            getStatus.description =
                "Read the cube app's live state: spin speed (rad/s), whether the XR "
                "session is running, and the active rendering-mode index.";
            getStatus.inputSchemaJson = "{\"type\":\"object\"}";
            XrResult tr2 = xr.pfnRegisterMCPToolEXT(xr.session, &getStatus);

            LOG_INFO("XR_EXT_mcp_tools: appId=%d set_spin=%d get_status=%d", ar, tr1, tr2);
        }
    }

    // #439 Phase 3 — XR_EXT_local_3d_zone entry points (app-local harness; the
    // shared XrSessionManager doesn't carry this extension yet).
    if (g_zone.available) {
        xrGetInstanceProcAddr(xr.instance, "xrCreateLocal3DZoneMaskEXT",
            (PFN_xrVoidFunction*)&g_zone.pfnCreate);
        xrGetInstanceProcAddr(xr.instance, "xrSetLocal3DZoneFromRectsEXT",
            (PFN_xrVoidFunction*)&g_zone.pfnSetRects);
        xrGetInstanceProcAddr(xr.instance, "xrSubmitLocal3DZoneEXT",
            (PFN_xrVoidFunction*)&g_zone.pfnSubmit);
        xrGetInstanceProcAddr(xr.instance, "xrDestroyLocal3DZoneMaskEXT",
            (PFN_xrVoidFunction*)&g_zone.pfnDestroy);
        if (!g_zone.pfnCreate || !g_zone.pfnSetRects || !g_zone.pfnSubmit || !g_zone.pfnDestroy) {
            LOG_WARN("XR_EXT_local_3d_zone advertised but entry points missing — harness disabled");
            g_zone.available = false;
            g_hasDisplayZonesExt = false;
        }
    }

    // XR_EXT_display_zones entry points (ADR-027): capabilities + per-zone
    // recommended view size.
    if (g_hasDisplayZonesExt) {
        xrGetInstanceProcAddr(xr.instance, "xrGetDisplayZoneCapabilitiesEXT",
            (PFN_xrVoidFunction*)&g_zones.pfnGetCaps);
        xrGetInstanceProcAddr(xr.instance, "xrGetDisplayZoneRecommendedViewSizeEXT",
            (PFN_xrVoidFunction*)&g_zones.pfnGetViewSize);
        if (!g_zones.pfnGetCaps || !g_zones.pfnGetViewSize) {
            LOG_WARN("XR_EXT_display_zones advertised but entry points missing — zones path disabled");
            g_hasDisplayZonesExt = false;
        }
    }

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
                    LOG_INFO("  [%u] %s (views=%u, tiles=%ux%u, scale=%.2fx%.2f, 3D=%d)", modes[i].modeIndex, modes[i].modeName, modes[i].viewCount, modes[i].tileColumns, modes[i].tileRows, modes[i].viewScaleX, modes[i].viewScaleY, modes[i].hardwareDisplay3D);
                    xr.renderingModeViewCounts[i] = modes[i].viewCount;
                    xr.renderingModeTileColumns[i] = modes[i].tileColumns;
                    xr.renderingModeTileRows[i] = modes[i].tileRows;
                    xr.renderingModeScaleX[i] = modes[i].viewScaleX;
                    xr.renderingModeScaleY[i] = modes[i].viewScaleY;
                    xr.renderingModeDisplay3D[i] = modes[i].hardwareDisplay3D ? true : false;
                    xr.renderingModeIsRequestable[i] = modes[i].isRequestable ? true : false;
                    // v13 initial-mode-sync: trust runtime-reported active mode.
                    if (modes[i].isActive) {
                        xr.currentModeIndex = modes[i].modeIndex;
                    }
                }
            }
        }
    }

    return true;
}
