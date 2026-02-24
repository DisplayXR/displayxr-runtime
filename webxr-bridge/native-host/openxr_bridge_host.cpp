// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  WebXR-to-OpenXR bridge native host
 *
 * Receives stereo SBS frames from the Chrome extension via WebSocket,
 * uploads them to an OpenXR Vulkan swapchain, and submits via xrEndFrame.
 * Monado's compositor/display processor then processes the content.
 *
 * Architecture:
 *   WebSocket thread: lws_service() loop, receives binary frame data
 *   Main thread: OpenXR frame loop (xrWaitFrame blocks until compositor ready)
 *   Double-buffered frame storage bridges the two threads.
 */

#include <vulkan/vulkan.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/XR_EXT_display_info.h>

#include <libwebsockets.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <openxr/XR_EXT_macos_window_binding.h>
extern "C" void* bridge_create_hidden_metal_view(uint32_t w, uint32_t h);
extern "C" void bridge_render_hud_text(const char* text, uint8_t* buffer,
                                       uint32_t width, uint32_t height);

struct BridgeOverlay;
extern "C" BridgeOverlay* bridge_overlay_create(uint32_t w, uint32_t h, void** outViewHandle);
extern "C" void bridge_overlay_set_frame(BridgeOverlay* ov, double x, double y, double w, double h);
extern "C" void bridge_overlay_set_visible(BridgeOverlay* ov, bool visible);
extern "C" void bridge_overlay_destroy(BridgeOverlay* ov);
extern "C" double bridge_get_main_screen_height(void);
#endif

// ============================================================================
// Logging
// ============================================================================

#define LOG_INFO(fmt, ...)  fprintf(stdout, "[INFO]  " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  fprintf(stderr, "[WARN]  " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)

#define XR_CHECK(call) \
    do { \
        XrResult _r = (call); \
        if (XR_FAILED(_r)) { \
            LOG_ERROR("%s failed: %d", #call, (int)_r); \
            return false; \
        } \
    } while (0)

#define VK_CHECK(call) \
    do { \
        VkResult _r = (call); \
        if (_r != VK_SUCCESS) { \
            LOG_ERROR("%s failed: %d", #call, (int)_r); \
            return false; \
        } \
    } while (0)

// ============================================================================
// Double-buffered frame storage
// ============================================================================

struct FrameBuffer {
    std::mutex mutex;
    std::vector<uint8_t> data[2]; // Double buffer
    uint32_t width = 0;
    uint32_t height = 0;
    int writeIndex = 0;
    bool newFrameAvailable = false;

    // Accumulation buffer for partial WebSocket messages
    std::vector<uint8_t> accumBuf;
};

static FrameBuffer g_frameBuffer;

// ============================================================================
// Readback buffer — composited pixels from Monado (Option A)
// ============================================================================

struct ReadbackBuffer {
    std::mutex mutex;
    std::vector<uint8_t> data;
    uint32_t width = 0;
    uint32_t height = 0;
    std::atomic<bool> ready{false};
};

static ReadbackBuffer g_readbackBuffer;

// ============================================================================
// Overlay mode — borderless click-through window over browser canvas
// ============================================================================

#ifdef __APPLE__
static bool g_overlayMode = false;
static BridgeOverlay* g_overlay = nullptr;
static void* g_overlayViewHandle = nullptr;

// Visibility state machine
static bool g_overlayCanvasRectKnown = false;
static bool g_overlaySessionActive = false;
static bool g_overlayTabVisible = true;
static bool g_overlayWindowFocused = true;

static void updateOverlayVisibility() {
    if (!g_overlay) return;
    // Don't use windowFocused — TAB key causes spurious blur/focus events that
    // flicker the overlay. tabVisible (document.visibilitychange) is sufficient
    // to hide when the user switches browser tabs.
    bool show = g_overlayCanvasRectKnown && g_overlaySessionActive
             && g_overlayTabVisible;
    static bool lastShow = false;
    if (show != lastShow) {
        LOG_INFO("Overlay visible=%s (rect=%s session=%s tab=%s)",
                 show ? "YES" : "NO",
                 g_overlayCanvasRectKnown ? "Y" : "N",
                 g_overlaySessionActive ? "Y" : "N",
                 g_overlayTabVisible ? "Y" : "N");
        lastShow = show;
    }
    bridge_overlay_set_visible(g_overlay, show);
}
#endif

// HUD rendering state
static std::vector<uint8_t> g_hudPixels;
static bool g_hudDirty = true;
static float g_hudUpdateTimer = 0.0f;
static uint32_t g_canvasWidth = 0, g_canvasHeight = 0; // From extension

// FPS tracking
static double g_frameTimeAccum = 0.0;
static uint32_t g_frameTimeCount = 0;
static double g_avgFrameTime = 0.0;

static void readback_callback(const uint8_t* pixels, uint32_t w, uint32_t h, void* /*ud*/) {
    std::lock_guard<std::mutex> lock(g_readbackBuffer.mutex);
    g_readbackBuffer.data.assign(pixels, pixels + (size_t)w * h * 4);
    g_readbackBuffer.width = w;
    g_readbackBuffer.height = h;
    g_readbackBuffer.ready.store(true);
}

// ============================================================================
// OpenXR + Vulkan state
// ============================================================================

struct AppState {
    // OpenXR
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE;
    XrSpace viewSpace = XR_NULL_HANDLE;
    XrSpace displaySpace = XR_NULL_HANDLE; // XR_EXT_display_info: display-anchored space
    XrSwapchain swapchain = XR_NULL_HANDLE;
    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    XrViewConfigurationType viewConfigType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    bool sessionRunning = false;
    bool exitRequested = false;
    bool hasDisplayInfo = false;

    // XR_EXT_display_info: physical display properties for Kooima projection
    float displayWidthM = 0.0f;
    float displayHeightM = 0.0f;
    uint32_t displayPixelWidth = 0;
    uint32_t displayPixelHeight = 0;
    float recommendedViewScaleX = 1.0f;
    float recommendedViewScaleY = 1.0f;
    float nominalViewerX = 0, nominalViewerY = 0, nominalViewerZ = 0; // Nominal viewer pos in display space
#ifdef __APPLE__
    bool hasMacosWindowBinding = false;
#endif

    uint32_t swapchainWidth = 0;
    uint32_t swapchainHeight = 0;
    int64_t swapchainFormat = 0;
    uint32_t swapchainImageCount = 0;
    std::vector<XrSwapchainImageVulkanKHR> swapchainImages;
    std::vector<XrViewConfigurationView> configViews;

    // Vulkan
    VkInstance vkInstance = VK_NULL_HANDLE;
    VkPhysicalDevice physDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    uint32_t queueFamilyIndex = 0;

    // Staging buffer for CPU → GPU upload
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    void* stagingMapped = nullptr;
    VkDeviceSize stagingSize = 0;

    // Command pool for transfer commands
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkFence uploadFence = VK_NULL_HANDLE;

    // HUD overlay (window-space composition layer)
    XrSwapchain hudSwapchain = XR_NULL_HANDLE;
    uint32_t hudSwapchainWidth = 0;
    uint32_t hudSwapchainHeight = 0;
    int64_t hudSwapchainFormat = 0;
    uint32_t hudSwapchainImageCount = 0;
    std::vector<XrSwapchainImageVulkanKHR> hudSwapchainImages;
    bool hasHudSwapchain = false;
    bool hudVisible = true;
    char systemName[XR_MAX_SYSTEM_NAME_SIZE] = {};

    // Controller actions
    XrActionSet actionSet = XR_NULL_HANDLE;
    XrAction triggerAction = XR_NULL_HANDLE;   // float
    XrAction squeezeAction = XR_NULL_HANDLE;   // boolean
    XrAction menuAction = XR_NULL_HANDLE;      // boolean
    XrAction thumbstickAction = XR_NULL_HANDLE; // vec2f
    XrAction thumbstickClickAction = XR_NULL_HANDLE; // boolean
    XrAction gripPoseAction = XR_NULL_HANDLE;  // pose
    XrPath handPaths[2] = {};
    XrSpace gripSpaces[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
};

// ============================================================================
// OpenXR initialization
// ============================================================================

static bool InitializeOpenXR(AppState& app) {
    LOG_INFO("Initializing OpenXR...");

    uint32_t extensionCount = 0;
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr));

    std::vector<XrExtensionProperties> extensions(extensionCount, {XR_TYPE_EXTENSION_PROPERTIES});
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensions.data()));

    bool hasVulkan = false;
    bool hasDisplayInfo = false;
    for (const auto& ext : extensions) {
        if (strcmp(ext.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0) {
            hasVulkan = true;
        }
        if (strcmp(ext.extensionName, XR_EXT_DISPLAY_INFO_EXTENSION_NAME) == 0) {
            hasDisplayInfo = true;
        }
    }

    if (!hasVulkan) {
        LOG_ERROR("XR_KHR_vulkan_enable extension not available");
        return false;
    }

#ifdef __APPLE__
    bool hasMacosWindowBinding = false;
    for (const auto& ext : extensions) {
        if (strcmp(ext.extensionName, XR_EXT_MACOS_WINDOW_BINDING_EXTENSION_NAME) == 0) {
            hasMacosWindowBinding = true;
        }
    }
#endif

    std::vector<const char*> enabledExtensions;
    enabledExtensions.push_back(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);

    if (hasDisplayInfo) {
        enabledExtensions.push_back(XR_EXT_DISPLAY_INFO_EXTENSION_NAME);
        app.hasDisplayInfo = true;
        LOG_INFO("XR_EXT_display_info: available, enabling (DISPLAY space for Kooima)");
    }

#ifdef __APPLE__
    if (hasMacosWindowBinding) {
        enabledExtensions.push_back(XR_EXT_MACOS_WINDOW_BINDING_EXTENSION_NAME);
        app.hasMacosWindowBinding = true;
        LOG_INFO("XR_EXT_macos_window_binding: available, enabling");
    }
#endif

    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    strncpy(createInfo.applicationInfo.applicationName, "WebXRBridge",
            sizeof(createInfo.applicationInfo.applicationName) - 1);
    createInfo.applicationInfo.applicationVersion = 1;
    strncpy(createInfo.applicationInfo.engineName, "None",
            sizeof(createInfo.applicationInfo.engineName) - 1);
    createInfo.applicationInfo.engineVersion = 0;
    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    createInfo.enabledExtensionCount = (uint32_t)enabledExtensions.size();
    createInfo.enabledExtensionNames = enabledExtensions.data();

    XR_CHECK(xrCreateInstance(&createInfo, &app.instance));
    LOG_INFO("OpenXR instance created");

    XrSystemGetInfo systemInfo = {XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(xrGetSystem(app.instance, &systemInfo, &app.systemId));

    // Query system name
    {
        XrSystemProperties sysProps = {XR_TYPE_SYSTEM_PROPERTIES};
        if (XR_SUCCEEDED(xrGetSystemProperties(app.instance, app.systemId, &sysProps))) {
            strncpy(app.systemName, sysProps.systemName, sizeof(app.systemName) - 1);
            LOG_INFO("System: %s", app.systemName);
        }
    }

    // Query display info (XR_EXT_display_info) for physical display dimensions.
    // Kooima projection needs the display rect to compute the asymmetric frustum.
    if (app.hasDisplayInfo) {
        XrSystemProperties sysProps = {XR_TYPE_SYSTEM_PROPERTIES};
        XrDisplayInfoEXT displayInfo = {};
        displayInfo.type = XR_TYPE_DISPLAY_INFO_EXT;
        sysProps.next = &displayInfo;
        if (XR_SUCCEEDED(xrGetSystemProperties(app.instance, app.systemId, &sysProps))) {
            app.displayWidthM = displayInfo.displaySizeMeters.width;
            app.displayHeightM = displayInfo.displaySizeMeters.height;
            app.displayPixelWidth = displayInfo.displayPixelWidth;
            app.displayPixelHeight = displayInfo.displayPixelHeight;
            app.recommendedViewScaleX = displayInfo.recommendedViewScaleX;
            app.recommendedViewScaleY = displayInfo.recommendedViewScaleY;
            app.nominalViewerX = displayInfo.nominalViewerPositionInDisplaySpace.x;
            app.nominalViewerY = displayInfo.nominalViewerPositionInDisplaySpace.y;
            app.nominalViewerZ = displayInfo.nominalViewerPositionInDisplaySpace.z;
            LOG_INFO("Display: %.3fx%.3f m, %ux%u px, viewScale=%.3fx%.3f, nominalViewer=(%.3f,%.3f,%.3f)",
                     app.displayWidthM, app.displayHeightM,
                     app.displayPixelWidth, app.displayPixelHeight,
                     app.recommendedViewScaleX, app.recommendedViewScaleY,
                     app.nominalViewerX, app.nominalViewerY, app.nominalViewerZ);
        }
    }

    uint32_t viewCount = 0;
    XR_CHECK(xrEnumerateViewConfigurationViews(app.instance, app.systemId, app.viewConfigType, 0, &viewCount, nullptr));
    app.configViews.resize(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    XR_CHECK(xrEnumerateViewConfigurationViews(app.instance, app.systemId, app.viewConfigType, viewCount, &viewCount, app.configViews.data()));

    for (uint32_t i = 0; i < viewCount; i++) {
        LOG_INFO("  View %u: %ux%u", i,
            app.configViews[i].recommendedImageRectWidth,
            app.configViews[i].recommendedImageRectHeight);
    }

    return true;
}

// ============================================================================
// Vulkan initialization (runtime-guided)
// ============================================================================

static bool GetVulkanGraphicsRequirements(AppState& app) {
    PFN_xrGetVulkanGraphicsRequirementsKHR pfn = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(app.instance, "xrGetVulkanGraphicsRequirementsKHR",
        (PFN_xrVoidFunction*)&pfn));

    XrGraphicsRequirementsVulkanKHR graphicsReq = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    XR_CHECK(pfn(app.instance, app.systemId, &graphicsReq));

    LOG_INFO("Vulkan requirements: %d.%d.%d - %d.%d.%d",
        VK_VERSION_MAJOR(graphicsReq.minApiVersionSupported),
        VK_VERSION_MINOR(graphicsReq.minApiVersionSupported),
        VK_VERSION_PATCH(graphicsReq.minApiVersionSupported),
        VK_VERSION_MAJOR(graphicsReq.maxApiVersionSupported),
        VK_VERSION_MINOR(graphicsReq.maxApiVersionSupported),
        VK_VERSION_PATCH(graphicsReq.maxApiVersionSupported));

    return true;
}

static bool CreateVulkanInstance(AppState& app) {
    LOG_INFO("Creating Vulkan instance...");

    PFN_xrGetVulkanInstanceExtensionsKHR pfn = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(app.instance, "xrGetVulkanInstanceExtensionsKHR",
        (PFN_xrVoidFunction*)&pfn));

    uint32_t bufferSize = 0;
    pfn(app.instance, app.systemId, 0, &bufferSize, nullptr);
    std::string extensionsStr(bufferSize, '\0');
    pfn(app.instance, app.systemId, bufferSize, &bufferSize, extensionsStr.data());

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

    // MoltenVK portability
    uint32_t availExtCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &availExtCount, nullptr);
    std::vector<VkExtensionProperties> availExts(availExtCount);
    vkEnumerateInstanceExtensionProperties(nullptr, &availExtCount, availExts.data());

    bool hasPortabilityEnum = false;
    for (const auto& ext : availExts) {
        if (strcmp(ext.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0) {
            hasPortabilityEnum = true;
            break;
        }
    }
    if (hasPortabilityEnum) {
        extensionNames.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        LOG_INFO("  Adding VK_KHR_portability_enumeration for MoltenVK");
    }

    std::vector<const char*> extensionPtrs;
    for (auto& name : extensionNames) {
        extensionPtrs.push_back(name.c_str());
        LOG_INFO("  VkInstance extension: %s", name.c_str());
    }

    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "WebXRBridge";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "None";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = (uint32_t)extensionPtrs.size();
    createInfo.ppEnabledExtensionNames = extensionPtrs.data();
    if (hasPortabilityEnum) {
        createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    VK_CHECK(vkCreateInstance(&createInfo, nullptr, &app.vkInstance));
    LOG_INFO("Vulkan instance created");
    return true;
}

static bool GetVulkanPhysicalDevice(AppState& app) {
    PFN_xrGetVulkanGraphicsDeviceKHR pfn = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(app.instance, "xrGetVulkanGraphicsDeviceKHR",
        (PFN_xrVoidFunction*)&pfn));

    XR_CHECK(pfn(app.instance, app.systemId, app.vkInstance, &app.physDevice));

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(app.physDevice, &props);
    LOG_INFO("Vulkan physical device: %s", props.deviceName);

    return true;
}

static bool CreateVulkanDevice(AppState& app) {
    // Get required device extensions from runtime
    PFN_xrGetVulkanDeviceExtensionsKHR pfn = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(app.instance, "xrGetVulkanDeviceExtensionsKHR",
        (PFN_xrVoidFunction*)&pfn));

    uint32_t bufferSize = 0;
    pfn(app.instance, app.systemId, 0, &bufferSize, nullptr);
    std::string extensionsStr(bufferSize, '\0');
    pfn(app.instance, app.systemId, bufferSize, &bufferSize, extensionsStr.data());

    std::vector<std::string> extensionStorage;
    {
        size_t start = 0;
        while (start < extensionsStr.size()) {
            size_t end = extensionsStr.find(' ', start);
            if (end == std::string::npos) end = extensionsStr.size();
            std::string name = extensionsStr.substr(start, end - start);
            if (!name.empty() && name[0] != '\0') {
                extensionStorage.push_back(name);
            }
            start = end + 1;
        }
    }

    // MoltenVK portability
    uint32_t devExtCount = 0;
    vkEnumerateDeviceExtensionProperties(app.physDevice, nullptr, &devExtCount, nullptr);
    std::vector<VkExtensionProperties> devExts(devExtCount);
    vkEnumerateDeviceExtensionProperties(app.physDevice, nullptr, &devExtCount, devExts.data());

    for (const auto& ext : devExts) {
        if (strcmp(ext.extensionName, "VK_KHR_portability_subset") == 0) {
            extensionStorage.push_back("VK_KHR_portability_subset");
            LOG_INFO("  Adding VK_KHR_portability_subset for MoltenVK");
            break;
        }
    }

    std::vector<const char*> deviceExtensions;
    for (auto& name : extensionStorage) {
        deviceExtensions.push_back(name.c_str());
        LOG_INFO("  VkDevice extension: %s", name.c_str());
    }

    // Find graphics queue family
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(app.physDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(app.physDevice, &queueFamilyCount, queueFamilies.data());

    bool found = false;
    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            app.queueFamilyIndex = i;
            found = true;
            break;
        }
    }
    if (!found) {
        LOG_ERROR("No graphics queue family found");
        return false;
    }
    LOG_INFO("Graphics queue family: %u", app.queueFamilyIndex);

    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo = {};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = app.queueFamilyIndex;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &queuePriority;

    VkDeviceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = 1;
    createInfo.pQueueCreateInfos = &queueInfo;
    createInfo.enabledExtensionCount = (uint32_t)deviceExtensions.size();
    createInfo.ppEnabledExtensionNames = deviceExtensions.data();

    VK_CHECK(vkCreateDevice(app.physDevice, &createInfo, nullptr, &app.device));
    vkGetDeviceQueue(app.device, app.queueFamilyIndex, 0, &app.graphicsQueue);
    LOG_INFO("Vulkan device and queue created");

    return true;
}

// ============================================================================
// Action system (controllers)
// ============================================================================

static bool SetupActions(AppState& app) {
    LOG_INFO("Setting up controller actions...");

    // Subaction paths for left/right hands
    XR_CHECK(xrStringToPath(app.instance, "/user/hand/left", &app.handPaths[0]));
    XR_CHECK(xrStringToPath(app.instance, "/user/hand/right", &app.handPaths[1]));

    // Create action set
    XrActionSetCreateInfo asci = {XR_TYPE_ACTION_SET_CREATE_INFO};
    strncpy(asci.actionSetName, "gameplay", sizeof(asci.actionSetName) - 1);
    strncpy(asci.localizedActionSetName, "Gameplay", sizeof(asci.localizedActionSetName) - 1);
    asci.priority = 0;
    XR_CHECK(xrCreateActionSet(app.instance, &asci, &app.actionSet));

    // Helper: create an action with both hand subaction paths
    auto createAction = [&](XrAction& action, const char* name,
                            const char* localName, XrActionType type) -> bool {
        XrActionCreateInfo aci = {XR_TYPE_ACTION_CREATE_INFO};
        aci.actionType = type;
        strncpy(aci.actionName, name, sizeof(aci.actionName) - 1);
        strncpy(aci.localizedActionName, localName, sizeof(aci.localizedActionName) - 1);
        aci.countSubactionPaths = 2;
        aci.subactionPaths = app.handPaths;
        XR_CHECK(xrCreateAction(app.actionSet, &aci, &action));
        return true;
    };

    if (!createAction(app.triggerAction, "trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT)) return false;
    if (!createAction(app.squeezeAction, "squeeze", "Squeeze", XR_ACTION_TYPE_BOOLEAN_INPUT)) return false;
    if (!createAction(app.menuAction, "menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT)) return false;
    if (!createAction(app.thumbstickAction, "thumbstick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT)) return false;
    if (!createAction(app.thumbstickClickAction, "thumbstick_click", "Thumbstick Click", XR_ACTION_TYPE_BOOLEAN_INPUT)) return false;
    if (!createAction(app.gripPoseAction, "grip_pose", "Grip Pose", XR_ACTION_TYPE_POSE_INPUT)) return false;

    // Helper: convert string to XrPath
    auto toPath = [&](const char* str) -> XrPath {
        XrPath p = XR_NULL_PATH;
        xrStringToPath(app.instance, str, &p);
        return p;
    };

    // Suggest bindings for WMR motion controller (qwerty driver's profile)
    XrPath wmrProfile;
    XR_CHECK(xrStringToPath(app.instance, "/interaction_profiles/microsoft/motion_controller", &wmrProfile));

    XrActionSuggestedBinding wmrBindings[] = {
        {app.triggerAction, toPath("/user/hand/left/input/trigger/value")},
        {app.triggerAction, toPath("/user/hand/right/input/trigger/value")},
        {app.squeezeAction, toPath("/user/hand/left/input/squeeze/click")},
        {app.squeezeAction, toPath("/user/hand/right/input/squeeze/click")},
        {app.menuAction, toPath("/user/hand/left/input/menu/click")},
        {app.menuAction, toPath("/user/hand/right/input/menu/click")},
        {app.thumbstickAction, toPath("/user/hand/left/input/thumbstick")},
        {app.thumbstickAction, toPath("/user/hand/right/input/thumbstick")},
        {app.thumbstickClickAction, toPath("/user/hand/left/input/thumbstick/click")},
        {app.thumbstickClickAction, toPath("/user/hand/right/input/thumbstick/click")},
        {app.gripPoseAction, toPath("/user/hand/left/input/grip/pose")},
        {app.gripPoseAction, toPath("/user/hand/right/input/grip/pose")},
    };

    XrInteractionProfileSuggestedBinding wmrSuggestion = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    wmrSuggestion.interactionProfile = wmrProfile;
    wmrSuggestion.suggestedBindings = wmrBindings;
    wmrSuggestion.countSuggestedBindings = sizeof(wmrBindings) / sizeof(wmrBindings[0]);
    XR_CHECK(xrSuggestInteractionProfileBindings(app.instance, &wmrSuggestion));

    // Suggest bindings for simple controller fallback
    XrPath simpleProfile;
    XR_CHECK(xrStringToPath(app.instance, "/interaction_profiles/khr/simple_controller", &simpleProfile));

    XrActionSuggestedBinding simpleBindings[] = {
        {app.triggerAction, toPath("/user/hand/left/input/select/click")},
        {app.triggerAction, toPath("/user/hand/right/input/select/click")},
        {app.menuAction, toPath("/user/hand/left/input/menu/click")},
        {app.menuAction, toPath("/user/hand/right/input/menu/click")},
        {app.gripPoseAction, toPath("/user/hand/left/input/grip/pose")},
        {app.gripPoseAction, toPath("/user/hand/right/input/grip/pose")},
    };

    XrInteractionProfileSuggestedBinding simpleSuggestion = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    simpleSuggestion.interactionProfile = simpleProfile;
    simpleSuggestion.suggestedBindings = simpleBindings;
    simpleSuggestion.countSuggestedBindings = sizeof(simpleBindings) / sizeof(simpleBindings[0]);
    XR_CHECK(xrSuggestInteractionProfileBindings(app.instance, &simpleSuggestion));

    LOG_INFO("Actions and interaction profile bindings set up");
    return true;
}

static bool AttachActionsAndCreateSpaces(AppState& app) {
    // Attach action set to session
    XrSessionActionSetsAttachInfo attachInfo = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attachInfo.countActionSets = 1;
    attachInfo.actionSets = &app.actionSet;
    XR_CHECK(xrAttachSessionActionSets(app.session, &attachInfo));

    // Create grip action spaces for each hand
    for (int i = 0; i < 2; i++) {
        XrActionSpaceCreateInfo spaceInfo = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
        spaceInfo.action = app.gripPoseAction;
        spaceInfo.subactionPath = app.handPaths[i];
        spaceInfo.poseInActionSpace.orientation = {0, 0, 0, 1};
        spaceInfo.poseInActionSpace.position = {0, 0, 0};
        XR_CHECK(xrCreateActionSpace(app.session, &spaceInfo, &app.gripSpaces[i]));
    }

    LOG_INFO("Action sets attached, grip spaces created");
    return true;
}

// ============================================================================
// Session, spaces, swapchain
// ============================================================================

static bool CreateSession(AppState& app) {
    LOG_INFO("Creating OpenXR session...");

    XrGraphicsBindingVulkanKHR vkBinding = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    vkBinding.instance = app.vkInstance;
    vkBinding.physicalDevice = app.physDevice;
    vkBinding.device = app.device;
    vkBinding.queueFamilyIndex = app.queueFamilyIndex;
    vkBinding.queueIndex = 0;

    XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &vkBinding;
    sessionInfo.systemId = app.systemId;

#ifdef __APPLE__
    XrMacOSWindowBindingCreateInfoEXT macosBinding = {};
    macosBinding.type = XR_TYPE_MACOS_WINDOW_BINDING_CREATE_INFO_EXT;
    macosBinding.next = nullptr;

    if (app.hasMacosWindowBinding) {
        if (getenv("READBACK_MODE")) {
            // Offscreen readback — no window, composited pixels via callback
            macosBinding.viewHandle = nullptr;
            macosBinding.readbackCallback = readback_callback;
            macosBinding.readbackUserdata = nullptr;
            LOG_INFO("Chaining XR_EXT_macos_window_binding with offscreen readback");
        } else if (g_overlayMode) {
            // Overlay mode — visible click-through window positioned over browser canvas
            g_overlay = bridge_overlay_create(
                app.swapchainWidth ? app.swapchainWidth : 1920,
                app.swapchainHeight ? app.swapchainHeight : 1080,
                &g_overlayViewHandle);
            macosBinding.viewHandle = g_overlayViewHandle;
            LOG_INFO("Chaining XR_EXT_macos_window_binding with overlay NSView %p",
                     g_overlayViewHandle);
        } else {
            // Default: hidden Metal view for per-session rendering
            macosBinding.viewHandle = bridge_create_hidden_metal_view(
                app.swapchainWidth ? app.swapchainWidth : 1920,
                app.swapchainHeight ? app.swapchainHeight : 1080);
            LOG_INFO("Chaining XR_EXT_macos_window_binding with hidden NSView %p",
                     macosBinding.viewHandle);
        }
        vkBinding.next = &macosBinding;
    }
#endif

    XR_CHECK(xrCreateSession(app.instance, &sessionInfo, &app.session));
    LOG_INFO("Session created");
    return true;
}

static bool CreateSpaces(AppState& app) {
    XrReferenceSpaceCreateInfo localSpaceInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    localSpaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    localSpaceInfo.poseInReferenceSpace.orientation = {0, 0, 0, 1};
    localSpaceInfo.poseInReferenceSpace.position = {0, 0, 0};
    XR_CHECK(xrCreateReferenceSpace(app.session, &localSpaceInfo, &app.localSpace));

    XrReferenceSpaceCreateInfo viewSpaceInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    viewSpaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    viewSpaceInfo.poseInReferenceSpace.orientation = {0, 0, 0, 1};
    viewSpaceInfo.poseInReferenceSpace.position = {0, 0, 0};
    XR_CHECK(xrCreateReferenceSpace(app.session, &viewSpaceInfo, &app.viewSpace));

    // DISPLAY space (XR_EXT_display_info): physically anchored to the display.
    // Views located in this space give display-relative eye positions needed
    // for correct Kooima projection (matching the ext test apps exactly).
    if (app.hasDisplayInfo) {
        XrReferenceSpaceCreateInfo displaySpaceInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        displaySpaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_DISPLAY_EXT;
        displaySpaceInfo.poseInReferenceSpace.orientation = {0, 0, 0, 1};
        displaySpaceInfo.poseInReferenceSpace.position = {0, 0, 0};
        XrResult dispResult = xrCreateReferenceSpace(app.session, &displaySpaceInfo, &app.displaySpace);
        if (XR_SUCCEEDED(dispResult)) {
            LOG_INFO("DISPLAY space created (display-relative eye positions for Kooima)");
        } else {
            LOG_WARN("DISPLAY space creation failed (%d), falling back to LOCAL", (int)dispResult);
            app.displaySpace = XR_NULL_HANDLE;
        }
    }

    LOG_INFO("Reference spaces created (LOCAL + VIEW%s)", app.displaySpace ? " + DISPLAY" : "");
    return true;
}

static bool CreateSwapchain(AppState& app) {
    LOG_INFO("Creating SBS swapchain...");

    uint32_t formatCount = 0;
    XR_CHECK(xrEnumerateSwapchainFormats(app.session, 0, &formatCount, nullptr));

    std::vector<int64_t> formats(formatCount);
    XR_CHECK(xrEnumerateSwapchainFormats(app.session, formatCount, &formatCount, formats.data()));

    // Prefer UNORM — browser pixels are already sRGB-encoded, using _SRGB would
    // double-gamma and make the image too dark
    int64_t selectedFormat = formats[0];
    for (int64_t fmt : formats) {
        if (fmt == VK_FORMAT_R8G8B8A8_UNORM || fmt == VK_FORMAT_B8G8R8A8_UNORM) {
            selectedFormat = fmt;
            break;
        }
    }
    LOG_INFO("Selected swapchain format: %lld (0x%llX)", (long long)selectedFormat, (long long)selectedFormat);

    const auto& view = app.configViews[0];

    // Single SBS swapchain: 2x width for side-by-side stereo
    XrSwapchainCreateInfo swapchainInfo = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                               XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                               XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    swapchainInfo.format = selectedFormat;
    swapchainInfo.sampleCount = 1;
    swapchainInfo.width = view.recommendedImageRectWidth * 2;
    swapchainInfo.height = view.recommendedImageRectHeight;
    swapchainInfo.faceCount = 1;
    swapchainInfo.arraySize = 1;
    swapchainInfo.mipCount = 1;

    app.swapchainWidth = swapchainInfo.width;
    app.swapchainHeight = swapchainInfo.height;
    app.swapchainFormat = selectedFormat;

    LOG_INFO("  SBS swapchain: %ux%u", app.swapchainWidth, app.swapchainHeight);

    XR_CHECK(xrCreateSwapchain(app.session, &swapchainInfo, &app.swapchain));

    uint32_t imageCount = 0;
    XR_CHECK(xrEnumerateSwapchainImages(app.swapchain, 0, &imageCount, nullptr));
    app.swapchainImageCount = imageCount;

    app.swapchainImages.resize(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    XR_CHECK(xrEnumerateSwapchainImages(app.swapchain, imageCount, &imageCount,
        (XrSwapchainImageBaseHeader*)app.swapchainImages.data()));

    LOG_INFO("  %u swapchain images", imageCount);
    return true;
}

// ============================================================================
// Staging buffer for CPU → GPU pixel upload
// ============================================================================

static bool CreateStagingResources(AppState& app) {
    // Size for full SBS RGBA frame
    app.stagingSize = (VkDeviceSize)app.swapchainWidth * app.swapchainHeight * 4;

    VkBufferCreateInfo bufInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufInfo.size = app.stagingSize;
    bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VK_CHECK(vkCreateBuffer(app.device, &bufInfo, nullptr, &app.stagingBuffer));

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(app.device, app.stagingBuffer, &memReqs);

    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(app.physDevice, &memProps);

    uint32_t memTypeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((memReqs.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags &
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
        {
            memTypeIndex = i;
            break;
        }
    }
    if (memTypeIndex == UINT32_MAX) {
        LOG_ERROR("No suitable memory type for staging buffer");
        return false;
    }

    VkMemoryAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = memTypeIndex;

    VK_CHECK(vkAllocateMemory(app.device, &allocInfo, nullptr, &app.stagingMemory));
    VK_CHECK(vkBindBufferMemory(app.device, app.stagingBuffer, app.stagingMemory, 0));
    VK_CHECK(vkMapMemory(app.device, app.stagingMemory, 0, app.stagingSize, 0, &app.stagingMapped));

    // Command pool
    VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = app.queueFamilyIndex;
    VK_CHECK(vkCreateCommandPool(app.device, &poolInfo, nullptr, &app.cmdPool));

    // Fence for upload synchronization
    VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_CHECK(vkCreateFence(app.device, &fenceInfo, nullptr, &app.uploadFence));

    LOG_INFO("Staging buffer created: %llu bytes", (unsigned long long)app.stagingSize);
    return true;
}

// ============================================================================
// HUD swapchain (window-space overlay layer)
// ============================================================================

static const uint32_t HUD_WIDTH  = 512;
static const uint32_t HUD_HEIGHT = 384;

static bool CreateHudSwapchain(AppState& app) {
    LOG_INFO("Creating HUD swapchain for window-space layer (%ux%u)...", HUD_WIDTH, HUD_HEIGHT);

    uint32_t formatCount = 0;
    XR_CHECK(xrEnumerateSwapchainFormats(app.session, 0, &formatCount, nullptr));
    std::vector<int64_t> formats(formatCount);
    XR_CHECK(xrEnumerateSwapchainFormats(app.session, formatCount, &formatCount, formats.data()));

    // Prefer R8G8B8A8_UNORM (VK=37) since HUD pixels are rendered as straight RGBA
    int64_t selectedFormat = formats[0];
    for (uint32_t i = 0; i < formatCount; i++) {
        if (formats[i] == VK_FORMAT_R8G8B8A8_UNORM) {
            selectedFormat = formats[i];
            break;
        }
        if (formats[i] == VK_FORMAT_B8G8R8A8_UNORM) {
            selectedFormat = formats[i]; // fallback
        }
    }
    LOG_INFO("HUD swapchain format: %lld (0x%llX)", (long long)selectedFormat, (long long)selectedFormat);

    XrSwapchainCreateInfo swapchainInfo = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                               XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                               XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    swapchainInfo.format = selectedFormat;
    swapchainInfo.sampleCount = 1;
    swapchainInfo.width = HUD_WIDTH;
    swapchainInfo.height = HUD_HEIGHT;
    swapchainInfo.faceCount = 1;
    swapchainInfo.arraySize = 1;
    swapchainInfo.mipCount = 1;

    XR_CHECK(xrCreateSwapchain(app.session, &swapchainInfo, &app.hudSwapchain));

    app.hudSwapchainWidth = HUD_WIDTH;
    app.hudSwapchainHeight = HUD_HEIGHT;
    app.hudSwapchainFormat = selectedFormat;

    uint32_t imageCount = 0;
    XR_CHECK(xrEnumerateSwapchainImages(app.hudSwapchain, 0, &imageCount, nullptr));
    app.hudSwapchainImageCount = imageCount;
    app.hudSwapchainImages.resize(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    XR_CHECK(xrEnumerateSwapchainImages(app.hudSwapchain, imageCount, &imageCount,
        (XrSwapchainImageBaseHeader*)app.hudSwapchainImages.data()));

    app.hasHudSwapchain = true;
    LOG_INFO("HUD swapchain created: %u images", imageCount);
    return true;
}

// ============================================================================
// Frame upload: CPU pixels → staging buffer → swapchain image
// ============================================================================

// Generic upload: CPU RGBA pixels → staging buffer → VkImage
static bool UploadPixelsToImage(AppState& app, VkImage dstImage,
                                const uint8_t* pixels, uint32_t srcW, uint32_t srcH,
                                uint32_t dstW, uint32_t dstH, int64_t dstFormat)
{
    uint32_t copyW = (srcW < dstW) ? srcW : dstW;
    uint32_t copyH = (srcH < dstH) ? srcH : dstH;

    // Copy pixels to staging buffer contiguously (source row stride = copyW)
    VkFormat fmt = (VkFormat)dstFormat;
    bool needSwizzle = (fmt == VK_FORMAT_B8G8R8A8_SRGB || fmt == VK_FORMAT_B8G8R8A8_UNORM);

    if (needSwizzle) {
        uint8_t* dst = (uint8_t*)app.stagingMapped;
        for (uint32_t y = 0; y < copyH; y++) {
            const uint8_t* srcRow = pixels + y * srcW * 4;
            uint8_t* dstRow = dst + y * copyW * 4;
            for (uint32_t x = 0; x < copyW; x++) {
                dstRow[x * 4 + 0] = srcRow[x * 4 + 2]; // B
                dstRow[x * 4 + 1] = srcRow[x * 4 + 1]; // G
                dstRow[x * 4 + 2] = srcRow[x * 4 + 0]; // R
                dstRow[x * 4 + 3] = srcRow[x * 4 + 3]; // A
            }
        }
    } else {
        if (srcW == copyW) {
            memcpy(app.stagingMapped, pixels, (size_t)copyW * copyH * 4);
        } else {
            uint8_t* dst = (uint8_t*)app.stagingMapped;
            for (uint32_t y = 0; y < copyH; y++) {
                memcpy(dst + y * copyW * 4, pixels + y * srcW * 4, copyW * 4);
            }
        }
    }

    // Record transfer commands
    VkCommandBufferAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = app.cmdPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmdBuf;
    VK_CHECK(vkAllocateCommandBuffers(app.device, &allocInfo, &cmdBuf));

    VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmdBuf, &beginInfo));

    // Barrier: UNDEFINED → TRANSFER_DST
    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = dstImage;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmdBuf,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    // Copy buffer to image (using received frame dimensions)
    VkBufferImageCopy region = {};
    region.bufferRowLength = copyW;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {copyW, copyH, 1};
    vkCmdCopyBufferToImage(cmdBuf, app.stagingBuffer, dstImage,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Barrier: TRANSFER_DST → GENERAL
    // Compositor expects swapchain images in VK_IMAGE_LAYOUT_GENERAL for sampling.
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier(cmdBuf,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    VK_CHECK(vkEndCommandBuffer(cmdBuf));

    // Submit and wait
    VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmdBuf;

    VK_CHECK(vkResetFences(app.device, 1, &app.uploadFence));
    VK_CHECK(vkQueueSubmit(app.graphicsQueue, 1, &submitInfo, app.uploadFence));
    VK_CHECK(vkWaitForFences(app.device, 1, &app.uploadFence, VK_TRUE, UINT64_MAX));

    vkFreeCommandBuffers(app.device, app.cmdPool, 1, &cmdBuf);

    return true;
}

static bool UploadFrameToSwapchain(AppState& app, uint32_t imageIndex,
                                   const uint8_t* pixels, uint32_t width, uint32_t height)
{
    return UploadPixelsToImage(app, app.swapchainImages[imageIndex].image,
                               pixels, width, height,
                               app.swapchainWidth, app.swapchainHeight,
                               app.swapchainFormat);
}

static bool UploadHudToSwapchain(AppState& app, uint32_t imageIndex,
                                 const uint8_t* pixels)
{
    return UploadPixelsToImage(app, app.hudSwapchainImages[imageIndex].image,
                               pixels, HUD_WIDTH, HUD_HEIGHT,
                               HUD_WIDTH, HUD_HEIGHT,
                               app.hudSwapchainFormat);
}

// ============================================================================
// Event polling + session state machine
// ============================================================================

static bool PollEvents(AppState& app) {
    XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};

    while (xrPollEvent(app.instance, &event) == XR_SUCCESS) {
        switch (event.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            auto* stateEvent = (XrEventDataSessionStateChanged*)&event;
            app.sessionState = stateEvent->state;
            LOG_INFO("Session state: %d", (int)app.sessionState);

            switch (app.sessionState) {
            case XR_SESSION_STATE_READY: {
                XrSessionBeginInfo beginInfo = {XR_TYPE_SESSION_BEGIN_INFO};
                beginInfo.primaryViewConfigurationType = app.viewConfigType;
                if (XR_SUCCEEDED(xrBeginSession(app.session, &beginInfo))) {
                    app.sessionRunning = true;
                    LOG_INFO("Session running");
                }
                break;
            }
            case XR_SESSION_STATE_STOPPING:
                xrEndSession(app.session);
                app.sessionRunning = false;
                LOG_INFO("Session stopped");
                break;
            case XR_SESSION_STATE_EXITING:
            case XR_SESSION_STATE_LOSS_PENDING:
                app.exitRequested = true;
                break;
            default:
                break;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            app.exitRequested = true;
            break;
        default:
            break;
        }

        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }

    return true;
}

// ============================================================================
// WebSocket server (libwebsockets)
// ============================================================================

static std::atomic<bool> g_running{true};
static AppState* g_appState = nullptr; // For WS callback to access swapchain dims

// Pose sync: main thread writes pose, WS thread sends to browser
static struct lws* g_clientWsi = nullptr;
static struct lws_context* g_lwsContext = nullptr;
static std::string g_textAccumBuf; // Accumulates text WS message fragments
static std::mutex g_poseMutex;
static char g_poseBuf[LWS_PRE + 2048]; // LWS_PRE padding + JSON (pose + FOV + controllers)
static int g_poseLen = 0;
static std::atomic<bool> g_poseReady{false};

// Handle parsed text messages from browser (JSON, parsed with strstr/sscanf)
static void handleTextMessage(const std::string& msg) {
    LOG_INFO("WS text: %.*s", (int)std::min(msg.size(), (size_t)200), msg.c_str());

    // HUD toggle (any mode)
    if (strstr(msg.c_str(), "\"hudToggle\"")) {
        if (g_appState) {
            g_appState->hudVisible = !g_appState->hudVisible;
            g_hudDirty = true;
            LOG_INFO("HUD %s", g_appState->hudVisible ? "shown" : "hidden");
        }
        return;
    }

    // Canvas/window size (any mode)
    {
        const char* wsp = strstr(msg.c_str(), "\"windowSize\"");
        if (wsp) {
            const char* wp = strstr(wsp, "\"w\":");
            const char* hp = strstr(wsp, "\"h\":");
            if (wp && hp) {
                uint32_t w = 0, h = 0;
                sscanf(wp, "\"w\":%u", &w);
                sscanf(hp, "\"h\":%u", &h);
                if (w > 0 && h > 0) {
                    g_canvasWidth = w;
                    g_canvasHeight = h;
                    g_hudDirty = true;
                }
            }
            return;
        }
    }

#ifdef __APPLE__
    if (!g_overlayMode) return;

    // canvasRect: {"canvasRect":{"x":...,"y":...,"w":...,"h":...}}
    // Use strstr to find the key, then sscanf from the value
    const char* crp = strstr(msg.c_str(), "\"canvasRect\"");
    if (crp) {
        double x = 0, y = 0, w = 0, h = 0;
        // Scan for x/y/w/h values after the canvasRect key
        const char* xp = strstr(crp, "\"x\":");
        const char* yp = strstr(crp, "\"y\":");
        const char* wp = strstr(crp, "\"w\":");
        const char* hp = strstr(crp, "\"h\":");
        if (xp && yp && wp && hp) {
            sscanf(xp, "\"x\":%lf", &x);
            sscanf(yp, "\"y\":%lf", &y);
            sscanf(wp, "\"w\":%lf", &w);
            sscanf(hp, "\"h\":%lf", &h);

            double screenH = bridge_get_main_screen_height();
            double macY = screenH - y - h;
            bridge_overlay_set_frame(g_overlay, x, macY, w, h);
            g_overlayCanvasRectKnown = true;
            updateOverlayVisibility();
            LOG_INFO("Overlay frame: browser(%.0f,%.0f) %.0fx%.0f -> mac(%.0f,%.0f) screenH=%.0f",
                     x, y, w, h, x, macY, screenH);
        }
        return;
    }

    if (strstr(msg.c_str(), "\"tabVisible\"")) {
        g_overlayTabVisible = strstr(msg.c_str(), "true") != nullptr;
        LOG_INFO("Overlay tabVisible=%s", g_overlayTabVisible ? "true" : "false");
        updateOverlayVisibility();
        return;
    }
    // windowFocused messages ignored — spurious blur/focus from TAB key
    if (strstr(msg.c_str(), "\"windowFocused\"")) {
        return;
    }
    if (strstr(msg.c_str(), "\"sessionActive\"")) {
        g_overlaySessionActive = strstr(msg.c_str(), "true") != nullptr;
        LOG_INFO("Overlay sessionActive=%s", g_overlaySessionActive ? "true" : "false");
        updateOverlayVisibility();
        return;
    }
#else
    (void)msg;
#endif
}

// Protocol: 8-byte header [uint32 width][uint32 height] + RGBA pixels
static int ws_callback(struct lws* wsi, enum lws_callback_reasons reason,
                       void* /*user*/, void* in, size_t len)
{
    switch (reason) {
    case LWS_CALLBACK_ESTABLISHED: {
        LOG_INFO("WebSocket client connected");
        g_clientWsi = wsi;
        // Send config message with swapchain dimensions and display settings
        if (g_appState && g_appState->swapchainWidth > 0) {
            const char* simDisplayOutput = getenv("SIM_DISPLAY_OUTPUT");
            const char* dispMode = simDisplayOutput ? simDisplayOutput : "sbs";
            bool isBrowserDisplay = getenv("BROWSER_DISPLAY") != nullptr;
            bool isReadbackMode = getenv("READBACK_MODE") != nullptr;
#ifdef __APPLE__
            bool isOverlayMode = g_overlayMode;
#else
            bool isOverlayMode = false;
#endif

            char cfg[768];
            int cfgLen = snprintf(cfg + LWS_PRE, sizeof(cfg) - LWS_PRE,
                "{\"w\":%u,\"h\":%u,\"displayMode\":\"%s\",\"browserDisplay\":%s,"
                "\"readbackDisplay\":%s,\"overlayDisplay\":%s,"
                "\"displayWidthM\":%.6f,\"displayHeightM\":%.6f,"
                "\"displayPixelW\":%u,\"displayPixelH\":%u,"
                "\"viewScaleX\":%.6f,\"viewScaleY\":%.6f,"
                "\"nominalViewer\":[%.5f,%.5f,%.5f]}",
                g_appState->swapchainWidth, g_appState->swapchainHeight,
                dispMode, isBrowserDisplay ? "true" : "false",
                isReadbackMode ? "true" : "false",
                isOverlayMode ? "true" : "false",
                g_appState->displayWidthM, g_appState->displayHeightM,
                g_appState->displayPixelWidth, g_appState->displayPixelHeight,
                g_appState->recommendedViewScaleX, g_appState->recommendedViewScaleY,
                g_appState->nominalViewerX, g_appState->nominalViewerY, g_appState->nominalViewerZ);
            lws_write(wsi, (unsigned char*)cfg + LWS_PRE, cfgLen, LWS_WRITE_TEXT);
            LOG_INFO("Sent config: %ux%u displayMode=%s browserDisplay=%s overlayDisplay=%s",
                     g_appState->swapchainWidth, g_appState->swapchainHeight,
                     dispMode, isBrowserDisplay ? "true" : "false",
                     isOverlayMode ? "true" : "false");
        }
        break;
    }

    case LWS_CALLBACK_RECEIVE: {
        auto& fb = g_frameBuffer;

        // Text (JSON) messages — parse for overlay positioning, visibility, etc.
        if (lws_frame_is_binary(wsi) == 0) {
            g_textAccumBuf.append((char*)in, len);
            if (lws_is_final_fragment(wsi)) {
                handleTextMessage(g_textAccumBuf);
                g_textAccumBuf.clear();
            }
            break;
        }

        // Accumulate fragments
        fb.accumBuf.insert(fb.accumBuf.end(), (uint8_t*)in, (uint8_t*)in + len);

        // Check if this is the final fragment of the message
        size_t remaining = lws_remaining_packet_payload(wsi);
        bool isFinal = lws_is_final_fragment(wsi);

        if (remaining > 0 || !isFinal) {
            // More data coming for this message
            break;
        }

        // Complete message received — parse it
        if (fb.accumBuf.size() < 8) {
            LOG_WARN("WebSocket message too small: %zu bytes", fb.accumBuf.size());
            fb.accumBuf.clear();
            break;
        }

        uint32_t w, h;
        memcpy(&w, fb.accumBuf.data(), 4);
        memcpy(&h, fb.accumBuf.data() + 4, 4);

        size_t expectedSize = 8 + (size_t)w * h * 4;
        if (fb.accumBuf.size() != expectedSize) {
            LOG_WARN("Frame size mismatch: got %zu, expected %zu (%ux%u)",
                     fb.accumBuf.size(), expectedSize, w, h);
            fb.accumBuf.clear();
            break;
        }

        // Write to double buffer
        {
            std::lock_guard<std::mutex> lock(fb.mutex);
            int wi = fb.writeIndex;
            fb.data[wi].resize(fb.accumBuf.size() - 8);
            memcpy(fb.data[wi].data(), fb.accumBuf.data() + 8, fb.accumBuf.size() - 8);
            fb.width = w;
            fb.height = h;
            fb.writeIndex = 1 - wi;
            fb.newFrameAvailable = true;
        }

        fb.accumBuf.clear();
        break;
    }

    case LWS_CALLBACK_SERVER_WRITEABLE: {
        // Send pending pose data to browser
        if (g_poseReady.load()) {
            std::lock_guard<std::mutex> lock(g_poseMutex);
            if (g_poseLen > 0) {
                lws_write(wsi, (unsigned char*)g_poseBuf + LWS_PRE, g_poseLen, LWS_WRITE_TEXT);
                g_poseReady.store(false);
            }
        }
        // Send readback frame if available (Option A: composited pixels from Monado)
        if (g_readbackBuffer.ready.load()) {
            std::lock_guard<std::mutex> lock(g_readbackBuffer.mutex);
            size_t pixelSize = g_readbackBuffer.data.size();
            if (pixelSize > 0) {
                std::vector<uint8_t> packet(LWS_PRE + 8 + pixelSize);
                uint8_t *p = packet.data() + LWS_PRE;
                memcpy(p, &g_readbackBuffer.width, 4);
                memcpy(p + 4, &g_readbackBuffer.height, 4);
                memcpy(p + 8, g_readbackBuffer.data.data(), pixelSize);
                lws_write(wsi, p, 8 + pixelSize, LWS_WRITE_BINARY);
            }
            g_readbackBuffer.ready.store(false);
        }
        break;
    }

    case LWS_CALLBACK_CLOSED:
        LOG_INFO("WebSocket client disconnected");
        g_clientWsi = nullptr;
#ifdef __APPLE__
        if (g_overlayMode) {
            g_overlaySessionActive = false;
            g_overlayCanvasRectKnown = false;
            updateOverlayVisibility();
        }
#endif
        break;

    default:
        break;
    }

    return 0;
}

static const struct lws_protocols ws_protocols[] = {
    {"webxr-bridge", ws_callback, 0, 1024 * 1024 * 8}, // 8MB rx buffer
    {nullptr, nullptr, 0, 0}
};

static void WebSocketThread() {
    LOG_INFO("WebSocket server starting on port 9013...");

    struct lws_context_creation_info info = {};
    info.port = 9013;
    info.protocols = ws_protocols;
    info.gid = -1;
    info.uid = -1;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

    struct lws_context* context = lws_create_context(&info);
    if (!context) {
        LOG_ERROR("Failed to create WebSocket context");
        return;
    }
    g_lwsContext = context;

    LOG_INFO("WebSocket server listening on ws://localhost:9013");

    while (g_running.load()) {
        lws_service(context, 50); // 50ms timeout

        // If pose data or readback frame is pending and client connected, request writable callback
        if ((g_poseReady.load() || g_readbackBuffer.ready.load()) && g_clientWsi) {
            lws_callback_on_writable(g_clientWsi);
        }
    }

    g_lwsContext = nullptr;
    lws_context_destroy(context);
    LOG_INFO("WebSocket server stopped");
}

// ============================================================================
// Cleanup
// ============================================================================

static void Cleanup(AppState& app) {
    // Controller action spaces
    for (int i = 0; i < 2; i++) {
        if (app.gripSpaces[i] != XR_NULL_HANDLE) {
            xrDestroySpace(app.gripSpaces[i]);
        }
    }
    // Destroying action set auto-destroys child actions
    if (app.actionSet != XR_NULL_HANDLE) {
        xrDestroyActionSet(app.actionSet);
    }

    if (app.uploadFence != VK_NULL_HANDLE) {
        vkDestroyFence(app.device, app.uploadFence, nullptr);
    }
    if (app.cmdPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(app.device, app.cmdPool, nullptr);
    }
    if (app.stagingMapped) {
        vkUnmapMemory(app.device, app.stagingMemory);
    }
    if (app.stagingBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(app.device, app.stagingBuffer, nullptr);
    }
    if (app.stagingMemory != VK_NULL_HANDLE) {
        vkFreeMemory(app.device, app.stagingMemory, nullptr);
    }
    if (app.hudSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(app.hudSwapchain);
    }
    if (app.swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(app.swapchain);
    }
    if (app.displaySpace != XR_NULL_HANDLE) {
        xrDestroySpace(app.displaySpace);
    }
    if (app.viewSpace != XR_NULL_HANDLE) {
        xrDestroySpace(app.viewSpace);
    }
    if (app.localSpace != XR_NULL_HANDLE) {
        xrDestroySpace(app.localSpace);
    }
    if (app.session != XR_NULL_HANDLE) {
        xrDestroySession(app.session);
    }
    if (app.instance != XR_NULL_HANDLE) {
        xrDestroyInstance(app.instance);
    }
    if (app.device != VK_NULL_HANDLE) {
        vkDestroyDevice(app.device, nullptr);
    }
    if (app.vkInstance != VK_NULL_HANDLE) {
        vkDestroyInstance(app.vkInstance, nullptr);
    }
    LOG_INFO("Cleanup complete");
}

// ============================================================================
// Main
// ============================================================================

static void SignalHandler(int sig) {
    (void)sig;
    g_running.store(false);
}

int main() {
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    LOG_INFO("=== WebXR-to-OpenXR Bridge Host ===");

#ifdef __APPLE__
    if (getenv("OVERLAY_MODE")) {
        g_overlayMode = true;
        LOG_INFO("Overlay mode enabled — will create click-through window over browser");
    }
#endif

    AppState app = {};

    // --- OpenXR + Vulkan init ---
    if (!InitializeOpenXR(app)) {
        LOG_ERROR("OpenXR initialization failed");
        return 1;
    }
    if (!GetVulkanGraphicsRequirements(app)) {
        LOG_ERROR("Failed to get Vulkan graphics requirements");
        Cleanup(app);
        return 1;
    }
    if (!CreateVulkanInstance(app)) {
        LOG_ERROR("Vulkan instance creation failed");
        Cleanup(app);
        return 1;
    }
    if (!GetVulkanPhysicalDevice(app)) {
        LOG_ERROR("Failed to get Vulkan physical device");
        Cleanup(app);
        return 1;
    }
    if (!CreateVulkanDevice(app)) {
        LOG_ERROR("Vulkan device creation failed");
        Cleanup(app);
        return 1;
    }
    if (!SetupActions(app)) {
        LOG_ERROR("Action setup failed");
        Cleanup(app);
        return 1;
    }
    if (!CreateSession(app)) {
        LOG_ERROR("Session creation failed");
        Cleanup(app);
        return 1;
    }
    if (!CreateSpaces(app)) {
        LOG_ERROR("Space creation failed");
        Cleanup(app);
        return 1;
    }
    if (!AttachActionsAndCreateSpaces(app)) {
        LOG_ERROR("Action attach failed");
        Cleanup(app);
        return 1;
    }
    if (!CreateSwapchain(app)) {
        LOG_ERROR("Swapchain creation failed");
        Cleanup(app);
        return 1;
    }
    if (!CreateStagingResources(app)) {
        LOG_ERROR("Staging resources creation failed");
        Cleanup(app);
        return 1;
    }
    if (!CreateHudSwapchain(app)) {
        LOG_WARN("HUD swapchain creation failed (HUD disabled)");
    } else {
        g_hudPixels.resize((size_t)HUD_WIDTH * HUD_HEIGHT * 4, 0);
    }

    // --- Start WebSocket server thread ---
    g_appState = &app;
    std::thread wsThread(WebSocketThread);

    LOG_INFO("=== Entering frame loop (Ctrl+C to exit) ===");
    LOG_INFO("Waiting for WebSocket frames on ws://localhost:9013 ...");

    uint32_t frameCount = 0;
    bool loggedInitInfo = false;
    bool hudRenderedOnce = false;
    auto lastFrameTime = std::chrono::steady_clock::now();

    while (g_running.load() && !app.exitRequested) {
        PollEvents(app);

        if (!app.sessionRunning) {
            usleep(100000); // 100ms
            continue;
        }

        // --- Frame timing ---
        auto now = std::chrono::steady_clock::now();
        double deltaTime = std::chrono::duration<double>(now - lastFrameTime).count();
        lastFrameTime = now;
        g_frameTimeAccum += deltaTime;
        g_frameTimeCount++;
        if (g_frameTimeAccum >= 1.0) {
            g_avgFrameTime = g_frameTimeAccum / g_frameTimeCount;
            g_frameTimeAccum = 0;
            g_frameTimeCount = 0;
            g_hudDirty = true;
        }

        // --- OpenXR frame loop ---
        XrFrameState frameState = {XR_TYPE_FRAME_STATE};
        XrFrameWaitInfo waitInfo = {XR_TYPE_FRAME_WAIT_INFO};
        XrResult result = xrWaitFrame(app.session, &waitInfo, &frameState);
        if (XR_FAILED(result)) {
            app.exitRequested = true;
            break;
        }

        XrFrameBeginInfo beginInfo = {XR_TYPE_FRAME_BEGIN_INFO};
        result = xrBeginFrame(app.session, &beginInfo);
        if (XR_FAILED(result)) continue;

        // Locate views in LOCAL space (needed for xrEndFrame submission)
        XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
        locateInfo.viewConfigurationType = app.viewConfigType;
        locateInfo.displayTime = frameState.predictedDisplayTime;
        locateInfo.space = app.localSpace;

        XrViewState viewState = {XR_TYPE_VIEW_STATE};
        uint32_t viewCount = 2;
        XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        xrLocateViews(app.session, &locateInfo, &viewState, 2, &viewCount, views);

        // Locate views in DISPLAY space for display-relative eye positions.
        // These are needed for correct Kooima projection in the browser
        // (matching ext app: rawEyePos from display-space xrLocateViews).
        XrView displayViews[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        if (app.displaySpace != XR_NULL_HANDLE) {
            XrViewLocateInfo dispLocateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
            dispLocateInfo.viewConfigurationType = app.viewConfigType;
            dispLocateInfo.displayTime = frameState.predictedDisplayTime;
            dispLocateInfo.space = app.displaySpace;

            XrViewState dispViewState = {XR_TYPE_VIEW_STATE};
            uint32_t dispViewCount = 2;
            xrLocateViews(app.session, &dispLocateInfo, &dispViewState,
                          2, &dispViewCount, displayViews);
        } else {
            // Fallback: use local-space views (Kooima will be approximate)
            displayViews[0] = views[0];
            displayViews[1] = views[1];
        }

        // One-time init logging: display pose, eye positions, FOV, dimensions
        if (!loggedInitInfo) {
            loggedInitInfo = true;

            // Virtual display pose in LOCAL space
            if (app.displaySpace != XR_NULL_HANDLE) {
                XrSpaceLocation dispLoc = {XR_TYPE_SPACE_LOCATION};
                if (XR_SUCCEEDED(xrLocateSpace(app.displaySpace, app.localSpace,
                        frameState.predictedDisplayTime, &dispLoc))) {
                    auto& dp = dispLoc.pose.position;
                    auto& dq = dispLoc.pose.orientation;
                    LOG_INFO("=== Virtual Display Init ===");
                    LOG_INFO("  Display pose (LOCAL): pos=(%.4f, %.4f, %.4f) ori=(%.4f, %.4f, %.4f, %.4f)",
                             dp.x, dp.y, dp.z, dq.x, dq.y, dq.z, dq.w);
                }
            }

            // Raw eye positions in DISPLAY space (display-relative, for Kooima)
            auto& ld = displayViews[0].pose.position;
            auto& rd = displayViews[1].pose.position;
            LOG_INFO("  Eyes (DISPLAY space): L=(%.5f, %.5f, %.5f) R=(%.5f, %.5f, %.5f)",
                     ld.x, ld.y, ld.z, rd.x, rd.y, rd.z);

            // Eye positions in LOCAL space (world coordinates)
            auto& ll = views[0].pose.position;
            auto& rl = views[1].pose.position;
            LOG_INFO("  Eyes (LOCAL  space): L=(%.5f, %.5f, %.5f) R=(%.5f, %.5f, %.5f)",
                     ll.x, ll.y, ll.z, rl.x, rl.y, rl.z);

            // FOV (from LOCAL-space views, Kooima computed at driver level)
            LOG_INFO("  FOV L: left=%.2f right=%.2f up=%.2f down=%.2f deg",
                     views[0].fov.angleLeft * 180.0f / 3.14159f,
                     views[0].fov.angleRight * 180.0f / 3.14159f,
                     views[0].fov.angleUp * 180.0f / 3.14159f,
                     views[0].fov.angleDown * 180.0f / 3.14159f);
            LOG_INFO("  FOV R: left=%.2f right=%.2f up=%.2f down=%.2f deg",
                     views[1].fov.angleLeft * 180.0f / 3.14159f,
                     views[1].fov.angleRight * 180.0f / 3.14159f,
                     views[1].fov.angleUp * 180.0f / 3.14159f,
                     views[1].fov.angleDown * 180.0f / 3.14159f);

            // Display dimensions and viewport
            LOG_INFO("  Display: %.3fx%.3f m, %ux%u px, viewScale=%.3fx%.3f",
                     app.displayWidthM, app.displayHeightM,
                     app.displayPixelWidth, app.displayPixelHeight,
                     app.recommendedViewScaleX, app.recommendedViewScaleY);
            LOG_INFO("  Swapchain (SBS): %ux%u, viewport: %ux%u per eye",
                     app.swapchainWidth, app.swapchainHeight,
                     app.swapchainWidth / 2, app.swapchainHeight);
            LOG_INFO("  Texture size (viewport * scale): %ux%u",
                     (uint32_t)(app.swapchainWidth / 2 * app.recommendedViewScaleX),
                     (uint32_t)(app.swapchainHeight * app.recommendedViewScaleY));
            LOG_INFO("========================");
        }

        // Sync controller actions every frame
        XrActiveActionSet activeAS = {};
        activeAS.actionSet = app.actionSet;
        activeAS.subactionPath = XR_NULL_PATH;
        XrActionsSyncInfo syncInfo = {XR_TYPE_ACTIONS_SYNC_INFO};
        syncInfo.countActiveActionSets = 1;
        syncInfo.activeActionSets = &activeAS;
        xrSyncActions(app.session, &syncInfo);

        // Locate view space (head pose) — used for both pose sending and HUD
        XrSpaceLocation viewLoc = {XR_TYPE_SPACE_LOCATION};
        bool viewLocValid = false;
        if (XR_SUCCEEDED(xrLocateSpace(app.viewSpace, app.localSpace,
                frameState.predictedDisplayTime, &viewLoc)) &&
            (viewLoc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
            (viewLoc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
        {
            viewLocValid = true;
        }

        // Send composed head pose + per-eye Kooima FOV + controller state to browser.
        // sim_display's get_tracked_pose composes: qwerty_pos + rotate(eye_offset, qwerty_ori)
        // The FOV from xrLocateViews changes with display mode (SBS uses half display width,
        // anaglyph/blend/2D use full width), so we must send it every frame.
        if (g_clientWsi && !g_poseReady.load() && viewLocValid)
            {
                // Query controller state for each hand
                struct { bool hasData; bool hasPose; float pose[7]; float tr; int sq, mn, tc; float ts[2]; } ctrl[2] = {};
                for (int hand = 0; hand < 2; hand++) {
                    XrActionStateGetInfo getInfo = {XR_TYPE_ACTION_STATE_GET_INFO};
                    getInfo.subactionPath = app.handPaths[hand];

                    getInfo.action = app.triggerAction;
                    XrActionStateFloat triggerState = {XR_TYPE_ACTION_STATE_FLOAT};
                    xrGetActionStateFloat(app.session, &getInfo, &triggerState);

                    getInfo.action = app.squeezeAction;
                    XrActionStateBoolean squeezeState = {XR_TYPE_ACTION_STATE_BOOLEAN};
                    xrGetActionStateBoolean(app.session, &getInfo, &squeezeState);

                    getInfo.action = app.menuAction;
                    XrActionStateBoolean menuState = {XR_TYPE_ACTION_STATE_BOOLEAN};
                    xrGetActionStateBoolean(app.session, &getInfo, &menuState);

                    getInfo.action = app.thumbstickAction;
                    XrActionStateVector2f thumbstickState = {XR_TYPE_ACTION_STATE_VECTOR2F};
                    xrGetActionStateVector2f(app.session, &getInfo, &thumbstickState);

                    getInfo.action = app.thumbstickClickAction;
                    XrActionStateBoolean thumbstickClickState = {XR_TYPE_ACTION_STATE_BOOLEAN};
                    xrGetActionStateBoolean(app.session, &getInfo, &thumbstickClickState);

                    getInfo.action = app.gripPoseAction;
                    XrActionStatePose poseState = {XR_TYPE_ACTION_STATE_POSE};
                    xrGetActionStatePose(app.session, &getInfo, &poseState);

                    bool hasPose = false;
                    if (poseState.isActive) {
                        XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
                        if (XR_SUCCEEDED(xrLocateSpace(app.gripSpaces[hand], app.localSpace,
                                frameState.predictedDisplayTime, &loc)) &&
                            (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                            (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
                        {
                            ctrl[hand].pose[0] = loc.pose.position.x;
                            ctrl[hand].pose[1] = loc.pose.position.y;
                            ctrl[hand].pose[2] = loc.pose.position.z;
                            ctrl[hand].pose[3] = loc.pose.orientation.x;
                            ctrl[hand].pose[4] = loc.pose.orientation.y;
                            ctrl[hand].pose[5] = loc.pose.orientation.z;
                            ctrl[hand].pose[6] = loc.pose.orientation.w;
                            hasPose = true;
                        }
                    }

                    bool hasInput = triggerState.isActive || squeezeState.isActive ||
                                    menuState.isActive || thumbstickState.isActive ||
                                    thumbstickClickState.isActive;
                    ctrl[hand].hasData = hasPose || hasInput;
                    ctrl[hand].hasPose = hasPose;
                    ctrl[hand].tr = triggerState.isActive ? triggerState.currentState : 0.0f;
                    ctrl[hand].sq = squeezeState.isActive && squeezeState.currentState ? 1 : 0;
                    ctrl[hand].mn = menuState.isActive && menuState.currentState ? 1 : 0;
                    ctrl[hand].ts[0] = thumbstickState.isActive ? thumbstickState.currentState.x : 0.0f;
                    ctrl[hand].ts[1] = thumbstickState.isActive ? thumbstickState.currentState.y : 0.0f;
                    ctrl[hand].tc = thumbstickClickState.isActive && thumbstickClickState.currentState ? 1 : 0;

                }

                auto& p = viewLoc.pose.position;
                auto& q = viewLoc.pose.orientation;
                std::lock_guard<std::mutex> lock(g_poseMutex);

                char* buf = g_poseBuf + LWS_PRE;
                int bufSize = (int)(sizeof(g_poseBuf) - LWS_PRE);
                int len = 0;

                // Head pose (LOCAL space) + display-relative eye positions + FOV
                // eyes: from DISPLAY-space xrLocateViews (matches ext app rawEyePos)
                // fov: from LOCAL-space views (Kooima FOV computed at driver level)
                auto& lep = displayViews[0].pose.position;
                auto& rep = displayViews[1].pose.position;
                len += snprintf(buf + len, bufSize - len,
                    "{\"pose\":[%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f],"
                    "\"eyes\":[[%.5f,%.5f,%.5f],[%.5f,%.5f,%.5f]],"
                    "\"fov\":[[%.6f,%.6f,%.6f,%.6f],[%.6f,%.6f,%.6f,%.6f]]",
                    p.x, p.y, p.z, q.x, q.y, q.z, q.w,
                    lep.x, lep.y, lep.z, rep.x, rep.y, rep.z,
                    views[0].fov.angleLeft, views[0].fov.angleRight,
                    views[0].fov.angleUp, views[0].fov.angleDown,
                    views[1].fov.angleLeft, views[1].fov.angleRight,
                    views[1].fov.angleUp, views[1].fov.angleDown);

                // Controller data
                len += snprintf(buf + len, bufSize - len, ",\"ctrl\":[");
                for (int hand = 0; hand < 2; hand++) {
                    if (hand > 0) len += snprintf(buf + len, bufSize - len, ",");
                    if (ctrl[hand].hasData) {
                        len += snprintf(buf + len, bufSize - len, "{");
                        if (ctrl[hand].hasPose) {
                            // Qwerty controller position is already rotated by HMD orientation
                            // (via tracking origin), but missing HMD translation. Just add it.
                            float cx = viewLoc.pose.position.x + ctrl[hand].pose[0];
                            float cy = viewLoc.pose.position.y + ctrl[hand].pose[1];
                            float cz = viewLoc.pose.position.z + ctrl[hand].pose[2];
                            len += snprintf(buf + len, bufSize - len,
                                "\"pose\":[%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f],",
                                cx, cy, cz,
                                ctrl[hand].pose[3], ctrl[hand].pose[4],
                                ctrl[hand].pose[5], ctrl[hand].pose[6]);
                        }
                        len += snprintf(buf + len, bufSize - len,
                            "\"tr\":%.2f,\"sq\":%d,\"mn\":%d,\"ts\":[%.4f,%.4f],\"tc\":%d}",
                            ctrl[hand].tr, ctrl[hand].sq, ctrl[hand].mn,
                            ctrl[hand].ts[0], ctrl[hand].ts[1], ctrl[hand].tc);
                    } else {
                        len += snprintf(buf + len, bufSize - len, "null");
                    }
                }
                len += snprintf(buf + len, bufSize - len, "]}");

                g_poseLen = len;
                g_poseReady.store(true);
                if (g_lwsContext) lws_cancel_service(g_lwsContext);
            }

        // --- HUD rendering (throttled to ~2Hz) ---
        g_hudUpdateTimer += (float)deltaTime;
#ifdef __APPLE__
        if (app.hasHudSwapchain && app.hudVisible &&
            (g_hudDirty || g_hudUpdateTimer >= 0.5f))
        {
            g_hudUpdateTimer = 0.0f;
            g_hudDirty = false;

            // Session state name
            const char* stateNames[] = {
                "UNKNOWN", "IDLE", "READY", "SYNCHRONIZED", "VISIBLE",
                "FOCUSED", "STOPPING", "LOSS_PENDING", "EXITING"
            };
            int si = (int)app.sessionState;
            const char* stateName = (si >= 0 && si <= 8) ? stateNames[si] : "?";

            // Output mode
            const char* simDisplayOutput = getenv("SIM_DISPLAY_OUTPUT");
            const char* outputName = "SBS";
            if (simDisplayOutput) {
                if (strcmp(simDisplayOutput, "anaglyph") == 0) outputName = "Anaglyph";
                else if (strcmp(simDisplayOutput, "blend") == 0) outputName = "Blend";
            }

            // FPS
            double fps = (g_avgFrameTime > 0) ? 1.0 / g_avgFrameTime : 0;

            // Forward vector from view orientation quaternion
            float qx = viewLoc.pose.orientation.x;
            float qy = viewLoc.pose.orientation.y;
            float qz = viewLoc.pose.orientation.z;
            float qw = viewLoc.pose.orientation.w;
            float fx = -2.0f * (qx * qz + qw * qy);
            float fy = -2.0f * (qy * qz - qw * qx);
            float fz = 2.0f * (qx * qx + qy * qy) - 1.0f;

            // Virtual display pose (display space in local space)
            float dispX = 0, dispY = 0, dispZ = 0;
            if (app.displaySpace != XR_NULL_HANDLE) {
                XrSpaceLocation dispLoc = {XR_TYPE_SPACE_LOCATION};
                if (XR_SUCCEEDED(xrLocateSpace(app.displaySpace, app.localSpace,
                        frameState.predictedDisplayTime, &dispLoc))) {
                    dispX = dispLoc.pose.position.x;
                    dispY = dispLoc.pose.position.y;
                    dispZ = dispLoc.pose.position.z;
                }
            }

            // Eye positions from display-space views
            auto& lep = displayViews[0].pose.position;
            auto& rep = displayViews[1].pose.position;

            char hudText[2048];
            snprintf(hudText, sizeof(hudText),
                "WebXR Bridge — %s\n"
                "Session: %s\n"
                "XR_EXT_macos_window_binding: %s\n"
                "Mode: 3D  Output: %s\n"
                "FPS: %.0f  (%.1f ms)\n"
                "Render: %ux%u  Canvas: %ux%u\n"
                "Display: %.3f x %.3f m\n"
                "Scale: %.2f x %.2f\n"
                "Nominal: (%.3f, %.3f, %.3f)\n"
                "Eye L: (%.3f, %.3f, %.3f)\n"
                "Eye R: (%.3f, %.3f, %.3f)\n"
                "Virtual Display: (%.2f, %.2f, %.2f)\n"
                "Forward: (%.2f, %.2f, %.2f)\n"
                "Camera: (%.2f, %.2f, %.2f)\n"
                "\n"
                "Tab=HUD  WASD/QE=Move  Scroll=Zoom",
                app.systemName,
                stateName,
                app.hasMacosWindowBinding ? "YES" : "NO",
                outputName,
                fps, g_avgFrameTime * 1000.0,
                app.swapchainWidth / 2, app.swapchainHeight,
                g_canvasWidth, g_canvasHeight,
                app.displayWidthM, app.displayHeightM,
                app.recommendedViewScaleX, app.recommendedViewScaleY,
                app.nominalViewerX, app.nominalViewerY, app.nominalViewerZ,
                lep.x, lep.y, lep.z,
                rep.x, rep.y, rep.z,
                dispX, dispY, dispZ,
                fx, fy, fz,
                viewLoc.pose.position.x, viewLoc.pose.position.y,
                viewLoc.pose.position.z);

            bridge_render_hud_text(hudText, g_hudPixels.data(), HUD_WIDTH, HUD_HEIGHT);

            // Verify HUD pixels are non-zero (first time only)
            if (!hudRenderedOnce) {
                uint32_t nonZero = 0;
                for (uint32_t i = 0; i < HUD_WIDTH * HUD_HEIGHT * 4; i++) {
                    if (g_hudPixels[i] != 0) { nonZero++; break; }
                }
                LOG_INFO("HUD render: %s (text len=%zu)",
                         nonZero ? "has pixels" : "ALL ZERO", strlen(hudText));
            }

            // Upload to HUD swapchain
            uint32_t hudImageIndex;
            XrSwapchainImageAcquireInfo hudAcq = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            XrResult hudAcqResult = xrAcquireSwapchainImage(app.hudSwapchain, &hudAcq, &hudImageIndex);
            if (XR_SUCCEEDED(hudAcqResult)) {
                XrSwapchainImageWaitInfo hudWait = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                hudWait.timeout = XR_INFINITE_DURATION;
                XrResult hudWaitResult = xrWaitSwapchainImage(app.hudSwapchain, &hudWait);
                if (XR_SUCCEEDED(hudWaitResult)) {
                    bool ok = UploadHudToSwapchain(app, hudImageIndex, g_hudPixels.data());
                    if (!hudRenderedOnce) {
                        LOG_INFO("HUD upload %s (image %u, fmt %lld)",
                                 ok ? "OK" : "FAILED", hudImageIndex,
                                 (long long)app.hudSwapchainFormat);
                    }
                } else {
                    LOG_WARN("HUD swapchain wait failed: %d", (int)hudWaitResult);
                }
                XrSwapchainImageReleaseInfo hudRel = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                xrReleaseSwapchainImage(app.hudSwapchain, &hudRel);
                if (!hudRenderedOnce) {
                    LOG_INFO("HUD first render+upload complete");
                }
                hudRenderedOnce = true;
            } else {
                LOG_WARN("HUD swapchain acquire failed: %d", (int)hudAcqResult);
            }
        }
#endif

        bool hasFrame = false;
        const uint8_t* framePixels = nullptr;
        uint32_t frameW = 0, frameH = 0;
        std::vector<uint8_t> localCopy;

        // Check for new frame from WebSocket
        {
            std::lock_guard<std::mutex> lock(g_frameBuffer.mutex);
            if (g_frameBuffer.newFrameAvailable) {
                int ri = 1 - g_frameBuffer.writeIndex; // Read index
                localCopy = g_frameBuffer.data[ri];
                frameW = g_frameBuffer.width;
                frameH = g_frameBuffer.height;
                g_frameBuffer.newFrameAvailable = false;
                hasFrame = true;
            }
        }

        if (hasFrame && frameState.shouldRender) {
            framePixels = localCopy.data();

            uint32_t imageIndex;
            XrSwapchainImageAcquireInfo acquireInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            result = xrAcquireSwapchainImage(app.swapchain, &acquireInfo, &imageIndex);
            if (XR_SUCCEEDED(result)) {
                XrSwapchainImageWaitInfo swWaitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                swWaitInfo.timeout = XR_INFINITE_DURATION;
                result = xrWaitSwapchainImage(app.swapchain, &swWaitInfo);

                if (XR_SUCCEEDED(result)) {
                    UploadFrameToSwapchain(app, imageIndex, framePixels, frameW, frameH);
                }

                XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                xrReleaseSwapchainImage(app.swapchain, &releaseInfo);

                // Build projection views
                uint32_t halfW = app.swapchainWidth / 2;
                XrCompositionLayerProjectionView projViews[2] = {};
                for (int eye = 0; eye < 2; eye++) {
                    projViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                    projViews[eye].subImage.swapchain = app.swapchain;
                    projViews[eye].subImage.imageRect.offset = {(int32_t)(eye * halfW), 0};
                    projViews[eye].subImage.imageRect.extent = {(int32_t)halfW, (int32_t)app.swapchainHeight};
                    projViews[eye].subImage.imageArrayIndex = 0;
                    projViews[eye].pose = views[eye].pose;
                    projViews[eye].fov = views[eye].fov;
                }

                XrCompositionLayerProjection projLayer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
                projLayer.space = app.localSpace;
                projLayer.viewCount = 2;
                projLayer.views = projViews;

                // Build layers array (projection + optional HUD)
                const XrCompositionLayerBaseHeader* layers[2];
                layers[0] = (XrCompositionLayerBaseHeader*)&projLayer;
                uint32_t layerCount = 1;

#ifdef __APPLE__
                XrCompositionLayerWindowSpaceEXT hudLayer = {};
                bool submitHud = app.hasHudSwapchain && app.hudVisible && hudRenderedOnce;
                if (submitHud) {
                    hudLayer.type = (XrStructureType)XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_EXT;
                    hudLayer.next = nullptr;
                    hudLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    hudLayer.subImage.swapchain = app.hudSwapchain;
                    hudLayer.subImage.imageRect.offset = {0, 0};
                    hudLayer.subImage.imageRect.extent = {(int32_t)HUD_WIDTH, (int32_t)HUD_HEIGHT};
                    hudLayer.subImage.imageArrayIndex = 0;
                    hudLayer.x = 0.01f;       // 1% from left
                    hudLayer.y = 0.60f;       // 60% from top (bottom 40%)
                    hudLayer.width = 0.38f;   // 38% of window width
                    hudLayer.height = 0.39f;  // 39% of window height
                    hudLayer.disparity = 0.0f; // screen depth
                    layers[1] = (XrCompositionLayerBaseHeader*)&hudLayer;
                    layerCount = 2;
                }
#endif

                static bool loggedHudSubmit = false;
                if (submitHud && !loggedHudSubmit) {
                    LOG_INFO("HUD layer submitted: x=%.2f y=%.2f w=%.2f h=%.2f (layers=%u)",
                             hudLayer.x, hudLayer.y, hudLayer.width, hudLayer.height, layerCount);
                    loggedHudSubmit = true;
                }

                XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
                endInfo.displayTime = frameState.predictedDisplayTime;
                endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                endInfo.layerCount = layerCount;
                endInfo.layers = layers;
                XrResult endResult = xrEndFrame(app.session, &endInfo);
                if (XR_FAILED(endResult) && submitHud) {
                    LOG_WARN("xrEndFrame with HUD layer failed: %d", (int)endResult);
                }
            } else {
                // Failed to acquire — submit empty frame
                XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
                endInfo.displayTime = frameState.predictedDisplayTime;
                endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                endInfo.layerCount = 0;
                endInfo.layers = nullptr;
                xrEndFrame(app.session, &endInfo);
            }
        } else {
            // No frame from WebSocket or shouldRender=false — submit empty frame
            XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
            endInfo.displayTime = frameState.predictedDisplayTime;
            endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            endInfo.layerCount = 0;
            endInfo.layers = nullptr;
            xrEndFrame(app.session, &endInfo);
        }

        frameCount++;
    }

    // --- Shutdown ---
    LOG_INFO("Shutting down (%u frames submitted)...", frameCount);

    g_running.store(false);

    if (app.session != XR_NULL_HANDLE && app.sessionRunning) {
        xrRequestExitSession(app.session);
        for (int i = 0; i < 100 && !app.exitRequested; i++) {
            PollEvents(app);
            usleep(10000);
        }
    }

    if (wsThread.joinable()) {
        wsThread.join();
    }

#ifdef __APPLE__
    if (g_overlay) {
        bridge_overlay_destroy(g_overlay);
        g_overlay = nullptr;
    }
#endif

    Cleanup(app);

    LOG_INFO("Application shutdown complete");
    return 0;
}
