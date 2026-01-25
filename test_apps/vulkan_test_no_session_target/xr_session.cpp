// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenXR session management implementation (Vulkan)
 */

#include "xr_session.h"
#include "logging.h"
#include <cstring>
#include <cmath>
#include <sstream>

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

static Mat4 XrPoseToViewMatrix(const XrPosef& pose) {
    // Convert XR pose to view matrix
    float qx = pose.orientation.x;
    float qy = pose.orientation.y;
    float qz = pose.orientation.z;
    float qw = pose.orientation.w;

    // Rotation matrix from quaternion
    Mat4 rotation = Mat4::Identity();
    rotation.m[0][0] = 1.0f - 2.0f * (qy * qy + qz * qz);
    rotation.m[0][1] = 2.0f * (qx * qy + qz * qw);
    rotation.m[0][2] = 2.0f * (qx * qz - qy * qw);
    rotation.m[1][0] = 2.0f * (qx * qy - qz * qw);
    rotation.m[1][1] = 1.0f - 2.0f * (qx * qx + qz * qz);
    rotation.m[1][2] = 2.0f * (qy * qz + qx * qw);
    rotation.m[2][0] = 2.0f * (qx * qz + qy * qw);
    rotation.m[2][1] = 2.0f * (qy * qz - qx * qw);
    rotation.m[2][2] = 1.0f - 2.0f * (qx * qx + qy * qy);

    // Transpose for inverse rotation (view matrix)
    rotation = rotation.Transpose();

    // Translation
    Mat4 translation = Mat4::Translation(-pose.position.x, -pose.position.y, -pose.position.z);

    return translation * rotation;
}

static Mat4 XrFovToProjectionMatrix(const XrFovf& fov, float nearZ, float farZ) {
    // Create asymmetric projection matrix from FOV angles
    float left = nearZ * tanf(fov.angleLeft);
    float right = nearZ * tanf(fov.angleRight);
    float top = nearZ * tanf(fov.angleUp);
    float bottom = nearZ * tanf(fov.angleDown);

    float width = right - left;
    float height = top - bottom;

    Mat4 proj = {};
    proj.m[0][0] = 2.0f * nearZ / width;
    proj.m[1][1] = 2.0f * nearZ / height;
    proj.m[2][0] = (right + left) / width;
    proj.m[2][1] = (top + bottom) / height;
    proj.m[2][2] = -(farZ + nearZ) / (farZ - nearZ);
    proj.m[2][3] = -1.0f;
    proj.m[3][2] = -2.0f * farZ * nearZ / (farZ - nearZ);

    return proj;
}

// Helper to split extension string into vector
static std::vector<std::string> SplitExtensionString(const char* str) {
    std::vector<std::string> result;
    std::istringstream iss(str);
    std::string token;
    while (iss >> token) {
        result.push_back(token);
    }
    return result;
}

bool GetVulkanInstanceExtensions(XrSessionManager& xr, std::vector<const char*>& extensions) {
    LOG_INFO("Getting Vulkan instance extensions required by OpenXR...");

    // First, create a minimal XR instance just to query Vulkan requirements
    // Query available extensions
    uint32_t extensionCount = 0;
    XR_CHECK_LOG(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr));
    LOG_INFO("Found %u OpenXR extensions available", extensionCount);

    std::vector<XrExtensionProperties> xrExtensions(extensionCount, {XR_TYPE_EXTENSION_PROPERTIES});
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, xrExtensions.data()));

    // Check for required extensions
    bool hasVulkan = false;
    bool hasSessionTarget = false;

    LOG_INFO("Available OpenXR extensions:");
    for (const auto& ext : xrExtensions) {
        LOG_DEBUG("  %s (v%u)", ext.extensionName, ext.extensionVersion);
        if (strcmp(ext.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0) {
            hasVulkan = true;
        }
        if (strcmp(ext.extensionName, XR_EXT_SESSION_TARGET_EXTENSION_NAME) == 0) {
            hasSessionTarget = true;
        }
    }

    LOG_INFO("XR_KHR_vulkan_enable: %s", hasVulkan ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("XR_EXT_session_target: %s (NOT USING - this test deliberately skips it)",
             hasSessionTarget ? "AVAILABLE" : "NOT FOUND");

    if (!hasVulkan) {
        LOG_ERROR("XR_KHR_vulkan_enable extension not available - cannot continue");
        return false;
    }

    xr.hasVulkanExt = hasVulkan;
    // Force session_target to false - we're explicitly NOT using it in this test
    // This allows us to test if issues are with the extension or base OpenXR
    xr.hasSessionTargetExt = false;

    // Build list of extensions to enable - only Vulkan, NOT session_target
    std::vector<const char*> enabledExtensions;
    enabledExtensions.push_back(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
    // Deliberately NOT enabling XR_EXT_session_target - Monado will create its own window

    LOG_INFO("Enabling %zu OpenXR extensions (session_target deliberately excluded)", enabledExtensions.size());
    for (const auto& ext : enabledExtensions) {
        LOG_INFO("  %s", ext);
    }

    // Create temporary instance to query Vulkan requirements
    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(createInfo.applicationInfo.applicationName, "VulkanTestNoSessionTarget");
    createInfo.applicationInfo.applicationVersion = 1;
    strcpy_s(createInfo.applicationInfo.engineName, "None");
    createInfo.applicationInfo.engineVersion = 0;
    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    createInfo.enabledExtensionCount = (uint32_t)enabledExtensions.size();
    createInfo.enabledExtensionNames = enabledExtensions.data();

    XR_CHECK_LOG(xrCreateInstance(&createInfo, &xr.instance));
    LOG_INFO("Temporary OpenXR instance created: 0x%p", (void*)xr.instance);

    // Get system
    XrSystemGetInfo systemInfo = {XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK_LOG(xrGetSystem(xr.instance, &systemInfo, &xr.systemId));
    LOG_INFO("System ID: %llu", (unsigned long long)xr.systemId);

    // Get Vulkan extension function pointers
    XR_CHECK(xrGetInstanceProcAddr(xr.instance, "xrGetVulkanInstanceExtensionsKHR",
        (PFN_xrVoidFunction*)&xr.xrGetVulkanInstanceExtensionsKHR));
    XR_CHECK(xrGetInstanceProcAddr(xr.instance, "xrGetVulkanDeviceExtensionsKHR",
        (PFN_xrVoidFunction*)&xr.xrGetVulkanDeviceExtensionsKHR));
    XR_CHECK(xrGetInstanceProcAddr(xr.instance, "xrGetVulkanGraphicsDeviceKHR",
        (PFN_xrVoidFunction*)&xr.xrGetVulkanGraphicsDeviceKHR));
    XR_CHECK(xrGetInstanceProcAddr(xr.instance, "xrGetVulkanGraphicsRequirementsKHR",
        (PFN_xrVoidFunction*)&xr.xrGetVulkanGraphicsRequirementsKHR));

    // Query required Vulkan instance extensions
    uint32_t vkExtLen = 0;
    XR_CHECK(xr.xrGetVulkanInstanceExtensionsKHR(xr.instance, xr.systemId, 0, &vkExtLen, nullptr));

    static std::string vkExtStr;
    vkExtStr.resize(vkExtLen);
    XR_CHECK(xr.xrGetVulkanInstanceExtensionsKHR(xr.instance, xr.systemId, vkExtLen, &vkExtLen, vkExtStr.data()));

    LOG_INFO("Vulkan instance extensions required by OpenXR: %s", vkExtStr.c_str());

    // Parse extension string and add to output
    static std::vector<std::string> vkExtNames = SplitExtensionString(vkExtStr.c_str());
    for (const auto& ext : vkExtNames) {
        extensions.push_back(ext.c_str());
    }

    return true;
}

bool InitializeOpenXR(XrSessionManager& xr, VkInstance vkInstance) {
    LOG_INFO("Initializing OpenXR with existing Vulkan instance...");

    // Instance should already be created by GetVulkanInstanceExtensions
    if (xr.instance == XR_NULL_HANDLE) {
        LOG_ERROR("OpenXR instance not created - call GetVulkanInstanceExtensions first");
        return false;
    }

    // Get Vulkan graphics requirements
    LOG_INFO("Getting Vulkan graphics requirements...");
    XrGraphicsRequirementsVulkanKHR graphicsReq = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    XR_CHECK_LOG(xr.xrGetVulkanGraphicsRequirementsKHR(xr.instance, xr.systemId, &graphicsReq));

    LOG_INFO("Vulkan graphics requirements:");
    LOG_INFO("  minApiVersionSupported: %d.%d.%d",
        XR_VERSION_MAJOR(graphicsReq.minApiVersionSupported),
        XR_VERSION_MINOR(graphicsReq.minApiVersionSupported),
        XR_VERSION_PATCH(graphicsReq.minApiVersionSupported));
    LOG_INFO("  maxApiVersionSupported: %d.%d.%d",
        XR_VERSION_MAJOR(graphicsReq.maxApiVersionSupported),
        XR_VERSION_MINOR(graphicsReq.maxApiVersionSupported),
        XR_VERSION_PATCH(graphicsReq.maxApiVersionSupported));

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

bool GetVulkanPhysicalDevice(XrSessionManager& xr, VkInstance vkInstance, VkPhysicalDevice& physicalDevice) {
    LOG_INFO("Getting Vulkan physical device from OpenXR...");

    XR_CHECK_LOG(xr.xrGetVulkanGraphicsDeviceKHR(xr.instance, xr.systemId, vkInstance, &physicalDevice));
    LOG_INFO("Vulkan physical device: 0x%p", (void*)physicalDevice);

    return true;
}

bool GetVulkanDeviceExtensions(XrSessionManager& xr, VkInstance vkInstance, VkPhysicalDevice physicalDevice, std::vector<const char*>& extensions) {
    LOG_INFO("Getting Vulkan device extensions required by OpenXR...");

    uint32_t vkExtLen = 0;
    XR_CHECK(xr.xrGetVulkanDeviceExtensionsKHR(xr.instance, xr.systemId, 0, &vkExtLen, nullptr));

    static std::string vkExtStr;
    vkExtStr.resize(vkExtLen);
    XR_CHECK(xr.xrGetVulkanDeviceExtensionsKHR(xr.instance, xr.systemId, vkExtLen, &vkExtLen, vkExtStr.data()));

    LOG_INFO("Vulkan device extensions required by OpenXR: %s", vkExtStr.c_str());

    // Parse extension string and add to output
    static std::vector<std::string> vkExtNames = SplitExtensionString(vkExtStr.c_str());
    for (const auto& ext : vkExtNames) {
        extensions.push_back(ext.c_str());
    }

    return true;
}

bool CreateSession(
    XrSessionManager& xr,
    VkInstance vkInstance,
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    uint32_t queueFamilyIndex,
    uint32_t queueIndex,
    HWND hwnd
) {
    LOG_INFO("Creating OpenXR session...");
    LOG_INFO("  Vulkan instance: 0x%p", (void*)vkInstance);
    LOG_INFO("  Vulkan physical device: 0x%p", (void*)physicalDevice);
    LOG_INFO("  Vulkan device: 0x%p", (void*)device);
    LOG_INFO("  Queue family: %u, Queue index: %u", queueFamilyIndex, queueIndex);
    LOG_INFO("  Window handle (HWND): 0x%p", hwnd);

    xr.windowHandle = hwnd;

    // Build the next chain for session creation
    LOG_INFO("Building session create info chain...");

    // Vulkan binding is required
    XrGraphicsBindingVulkanKHR vulkanBinding = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    vulkanBinding.instance = vkInstance;
    vulkanBinding.physicalDevice = physicalDevice;
    vulkanBinding.device = device;
    vulkanBinding.queueFamilyIndex = queueFamilyIndex;
    vulkanBinding.queueIndex = queueIndex;

    // Session target extension - chain it to the Vulkan binding
    XrSessionTargetCreateInfoEXT sessionTarget = {XR_TYPE_SESSION_TARGET_CREATE_INFO_EXT};
    sessionTarget.windowHandle = hwnd;

    if (xr.hasSessionTargetExt && hwnd) {
        // Chain: sessionInfo -> vulkanBinding -> sessionTarget
        vulkanBinding.next = &sessionTarget;
        LOG_INFO("Using XR_EXT_session_target with window handle");
        LOG_INFO("  Chain: XrSessionCreateInfo -> XrGraphicsBindingVulkanKHR -> XrSessionTargetCreateInfoEXT");
    } else {
        LOG_INFO("NOT using XR_EXT_session_target (hasExt=%d, hwnd=%p)",
            xr.hasSessionTargetExt, hwnd);
    }

    XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &vulkanBinding;
    sessionInfo.systemId = xr.systemId;

    LOG_INFO("Calling xrCreateSession...");
    XR_CHECK_LOG(xrCreateSession(xr.instance, &sessionInfo, &xr.session));
    LOG_INFO("Session created: 0x%p", (void*)xr.session);

    return true;
}

bool CreateSpaces(XrSessionManager& xr) {
    LOG_INFO("Creating reference spaces...");

    // Create LOCAL reference space
    LOG_INFO("Creating LOCAL reference space...");
    XrReferenceSpaceCreateInfo localSpaceInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    localSpaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    localSpaceInfo.poseInReferenceSpace.orientation = {0, 0, 0, 1};
    localSpaceInfo.poseInReferenceSpace.position = {0, 0, 0};

    XR_CHECK_LOG(xrCreateReferenceSpace(xr.session, &localSpaceInfo, &xr.localSpace));
    LOG_INFO("LOCAL space created: 0x%p", (void*)xr.localSpace);

    // Create VIEW reference space
    LOG_INFO("Creating VIEW reference space...");
    XrReferenceSpaceCreateInfo viewSpaceInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    viewSpaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    viewSpaceInfo.poseInReferenceSpace.orientation = {0, 0, 0, 1};
    viewSpaceInfo.poseInReferenceSpace.position = {0, 0, 0};

    XR_CHECK_LOG(xrCreateReferenceSpace(xr.session, &viewSpaceInfo, &xr.viewSpace));
    LOG_INFO("VIEW space created: 0x%p", (void*)xr.viewSpace);

    LOG_INFO("Reference spaces created successfully");
    return true;
}

bool CreateSwapchains(XrSessionManager& xr) {
    LOG_INFO("Creating swapchains...");

    // Query supported swapchain formats
    LOG_INFO("Enumerating swapchain formats...");
    uint32_t formatCount = 0;
    XR_CHECK_LOG(xrEnumerateSwapchainFormats(xr.session, 0, &formatCount, nullptr));
    LOG_INFO("Found %u swapchain formats", formatCount);

    std::vector<int64_t> formats(formatCount);
    XR_CHECK(xrEnumerateSwapchainFormats(xr.session, formatCount, &formatCount, formats.data()));

    // Log available formats
    LOG_INFO("Available swapchain formats:");
    for (uint32_t i = 0; i < formatCount; i++) {
        LOG_DEBUG("  Format[%u]: %lld (0x%llX)", i, formats[i], formats[i]);
    }

    // Prefer SRGB format for Vulkan
    int64_t selectedFormat = formats[0];
    for (int64_t format : formats) {
        // VK_FORMAT_R8G8B8A8_SRGB = 43, VK_FORMAT_B8G8R8A8_SRGB = 50
        if (format == 43 || format == 50) {
            selectedFormat = format;
            break;
        }
    }
    LOG_INFO("Selected swapchain format: %lld (0x%llX)", selectedFormat, selectedFormat);

    // Create swapchain for each eye
    for (int eye = 0; eye < 2 && eye < (int)xr.configViews.size(); eye++) {
        LOG_INFO("Creating swapchain for eye %d...", eye);
        const auto& view = xr.configViews[eye];

        XrSwapchainCreateInfo swapchainInfo = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
        swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        swapchainInfo.format = selectedFormat;
        swapchainInfo.sampleCount = view.recommendedSwapchainSampleCount;
        swapchainInfo.width = view.recommendedImageRectWidth;
        swapchainInfo.height = view.recommendedImageRectHeight;
        swapchainInfo.faceCount = 1;
        swapchainInfo.arraySize = 1;
        swapchainInfo.mipCount = 1;

        LOG_INFO("  Size: %ux%u, samples: %u", swapchainInfo.width, swapchainInfo.height, swapchainInfo.sampleCount);

        XR_CHECK_LOG(xrCreateSwapchain(xr.session, &swapchainInfo, &xr.swapchains[eye].swapchain));
        LOG_INFO("  Swapchain created: 0x%p", (void*)xr.swapchains[eye].swapchain);

        xr.swapchains[eye].format = selectedFormat;
        xr.swapchains[eye].width = swapchainInfo.width;
        xr.swapchains[eye].height = swapchainInfo.height;

        // Get swapchain images
        uint32_t imageCount = 0;
        XR_CHECK(xrEnumerateSwapchainImages(xr.swapchains[eye].swapchain, 0, &imageCount, nullptr));

        xr.swapchains[eye].images.resize(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        XR_CHECK(xrEnumerateSwapchainImages(xr.swapchains[eye].swapchain, imageCount, &imageCount,
            (XrSwapchainImageBaseHeader*)xr.swapchains[eye].images.data()));

        LOG_INFO("  Got %u swapchain images", imageCount);
        for (uint32_t i = 0; i < imageCount; i++) {
            LOG_DEBUG("    Image[%u]: VkImage 0x%p", i, (void*)xr.swapchains[eye].images[i].image);
        }
    }

    LOG_INFO("Swapchains created successfully");
    return true;
}

std::vector<VkImage> GetSwapchainImages(XrSessionManager& xr, int eye) {
    std::vector<VkImage> images;
    for (const auto& img : xr.swapchains[eye].images) {
        images.push_back(img.image);
    }
    return images;
}

bool PollEvents(XrSessionManager& xr) {
    XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};

    while (xrPollEvent(xr.instance, &event) == XR_SUCCESS) {
        switch (event.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            auto* stateEvent = (XrEventDataSessionStateChanged*)&event;
            XrSessionState oldState = xr.sessionState;
            xr.sessionState = stateEvent->state;

            LOG_INFO("Session state changed: %s -> %s",
                GetSessionStateString(oldState),
                GetSessionStateString(xr.sessionState));

            switch (xr.sessionState) {
            case XR_SESSION_STATE_READY: {
                // Begin session
                LOG_INFO("Session READY - calling xrBeginSession...");
                XrSessionBeginInfo beginInfo = {XR_TYPE_SESSION_BEGIN_INFO};
                beginInfo.primaryViewConfigurationType = xr.viewConfigType;
                XrResult result = xrBeginSession(xr.session, &beginInfo);
                LogXrResult("xrBeginSession", result);
                if (XR_SUCCEEDED(result)) {
                    xr.sessionRunning = true;
                    LOG_INFO("Session is now running");
                }
                break;
            }
            case XR_SESSION_STATE_STOPPING:
                LOG_INFO("Session STOPPING - calling xrEndSession...");
                xrEndSession(xr.session);
                xr.sessionRunning = false;
                LOG_INFO("Session stopped");
                break;
            case XR_SESSION_STATE_EXITING:
                LOG_INFO("Session EXITING - requesting exit");
                xr.exitRequested = true;
                break;
            case XR_SESSION_STATE_LOSS_PENDING:
                LOG_WARN("Session LOSS_PENDING - requesting exit");
                xr.exitRequested = true;
                break;
            default:
                break;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            LOG_WARN("Instance loss pending - requesting exit");
            xr.exitRequested = true;
            break;
        default:
            LOG_DEBUG("Received event type: %d", event.type);
            break;
        }

        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }

    return true;
}

bool BeginFrame(XrSessionManager& xr, XrFrameState& frameState) {
    frameState = {XR_TYPE_FRAME_STATE};

    XrFrameWaitInfo waitInfo = {XR_TYPE_FRAME_WAIT_INFO};
    XrResult result = xrWaitFrame(xr.session, &waitInfo, &frameState);
    if (XR_FAILED(result)) return false;

    xr.predictedDisplayTime = frameState.predictedDisplayTime;
    xr.predictedDisplayPeriod = frameState.predictedDisplayPeriod;

    XrFrameBeginInfo beginInfo = {XR_TYPE_FRAME_BEGIN_INFO};
    result = xrBeginFrame(xr.session, &beginInfo);
    if (XR_FAILED(result)) return false;

    return true;
}

bool LocateViews(
    XrSessionManager& xr,
    XrTime displayTime,
    Mat4& leftViewMatrix,
    Mat4& leftProjMatrix,
    Mat4& rightViewMatrix,
    Mat4& rightProjMatrix
) {
    XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
    locateInfo.viewConfigurationType = xr.viewConfigType;
    locateInfo.displayTime = displayTime;
    locateInfo.space = xr.localSpace;

    XrViewState viewState = {XR_TYPE_VIEW_STATE};
    uint32_t viewCount = 2;
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};

    XrResult result = xrLocateViews(xr.session, &locateInfo, &viewState, 2, &viewCount, views);
    if (XR_FAILED(result)) return false;

    // Check if views are valid
    if ((viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) == 0 ||
        (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0) {
        return false;
    }

    // Convert to matrices
    leftViewMatrix = XrPoseToViewMatrix(views[0].pose);
    leftProjMatrix = XrFovToProjectionMatrix(views[0].fov, 0.01f, 100.0f);
    rightViewMatrix = XrPoseToViewMatrix(views[1].pose);
    rightProjMatrix = XrFovToProjectionMatrix(views[1].fov, 0.01f, 100.0f);

    // Extract eye tracking info from view poses
    // Average the eye positions for display
    xr.eyePosX = (views[0].pose.position.x + views[1].pose.position.x) / 2.0f;
    xr.eyePosY = (views[0].pose.position.y + views[1].pose.position.y) / 2.0f;
    xr.eyeTrackingActive = (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_TRACKED_BIT) != 0;

    return true;
}

bool AcquireSwapchainImage(XrSessionManager& xr, int eye, uint32_t& imageIndex) {
    XrSwapchainImageAcquireInfo acquireInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrResult result = xrAcquireSwapchainImage(xr.swapchains[eye].swapchain, &acquireInfo, &imageIndex);
    if (XR_FAILED(result)) return false;

    XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = XR_INFINITE_DURATION;
    result = xrWaitSwapchainImage(xr.swapchains[eye].swapchain, &waitInfo);
    if (XR_FAILED(result)) return false;

    return true;
}

bool ReleaseSwapchainImage(XrSessionManager& xr, int eye) {
    XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    return XR_SUCCEEDED(xrReleaseSwapchainImage(xr.swapchains[eye].swapchain, &releaseInfo));
}

bool EndFrame(XrSessionManager& xr, XrTime displayTime, const XrCompositionLayerProjectionView* views) {
    XrCompositionLayerProjection projectionLayer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projectionLayer.space = xr.localSpace;
    projectionLayer.viewCount = 2;
    projectionLayer.views = views;

    const XrCompositionLayerBaseHeader* layers[] = {
        (XrCompositionLayerBaseHeader*)&projectionLayer
    };

    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = displayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = 1;
    endInfo.layers = layers;

    return XR_SUCCEEDED(xrEndFrame(xr.session, &endInfo));
}

void CleanupOpenXR(XrSessionManager& xr) {
    LOG_INFO("Cleaning up OpenXR resources...");

    for (int eye = 0; eye < 2; eye++) {
        if (xr.swapchains[eye].swapchain != XR_NULL_HANDLE) {
            LOG_INFO("Destroying swapchain %d...", eye);
            xrDestroySwapchain(xr.swapchains[eye].swapchain);
            xr.swapchains[eye].swapchain = XR_NULL_HANDLE;
        }
    }

    if (xr.viewSpace != XR_NULL_HANDLE) {
        LOG_INFO("Destroying VIEW space...");
        xrDestroySpace(xr.viewSpace);
        xr.viewSpace = XR_NULL_HANDLE;
    }

    if (xr.localSpace != XR_NULL_HANDLE) {
        LOG_INFO("Destroying LOCAL space...");
        xrDestroySpace(xr.localSpace);
        xr.localSpace = XR_NULL_HANDLE;
    }

    if (xr.session != XR_NULL_HANDLE) {
        LOG_INFO("Destroying session...");
        xrDestroySession(xr.session);
        xr.session = XR_NULL_HANDLE;
    }

    if (xr.instance != XR_NULL_HANDLE) {
        LOG_INFO("Destroying instance...");
        xrDestroyInstance(xr.instance);
        xr.instance = XR_NULL_HANDLE;
    }

    LOG_INFO("OpenXR cleanup complete");
}

const char* GetSessionStateString(XrSessionState state) {
    switch (state) {
    case XR_SESSION_STATE_UNKNOWN: return "UNKNOWN";
    case XR_SESSION_STATE_IDLE: return "IDLE";
    case XR_SESSION_STATE_READY: return "READY";
    case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
    case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
    case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
    case XR_SESSION_STATE_STOPPING: return "STOPPING";
    case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
    case XR_SESSION_STATE_EXITING: return "EXITING";
    default: return "INVALID";
    }
}

void RequestExit(XrSessionManager& xr) {
    if (xr.session != XR_NULL_HANDLE && xr.sessionRunning) {
        xrRequestExitSession(xr.session);
    }
    xr.exitRequested = true;
}
