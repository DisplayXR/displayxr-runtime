// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenXR session management implementation
 */

#include "xr_session.h"
#include "logging.h"
#include <cstring>
#include <cmath>

using namespace DirectX;

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

static XMMATRIX XrPoseToViewMatrix(const XrPosef& pose) {
    // Convert XR pose to view matrix
    XMVECTOR orientation = XMVectorSet(pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w);
    XMVECTOR position = XMVectorSet(pose.position.x, pose.position.y, pose.position.z, 1.0f);

    // Create rotation matrix from quaternion and invert for view
    XMMATRIX rotation = XMMatrixRotationQuaternion(orientation);
    rotation = XMMatrixTranspose(rotation); // Invert rotation

    // Create translation and apply inverted rotation
    XMMATRIX translation = XMMatrixTranslation(-pose.position.x, -pose.position.y, -pose.position.z);

    return translation * rotation;
}

static XMMATRIX XrFovToProjectionMatrix(const XrFovf& fov, float nearZ, float farZ) {
    // Create asymmetric projection matrix from FOV angles
    float left = nearZ * tanf(fov.angleLeft);
    float right = nearZ * tanf(fov.angleRight);
    float top = nearZ * tanf(fov.angleUp);
    float bottom = nearZ * tanf(fov.angleDown);

    float width = right - left;
    float height = top - bottom;

    XMMATRIX proj = XMMatrixIdentity();
    proj.r[0] = XMVectorSet(2.0f * nearZ / width, 0, 0, 0);
    proj.r[1] = XMVectorSet(0, 2.0f * nearZ / height, 0, 0);
    proj.r[2] = XMVectorSet((right + left) / width, (top + bottom) / height, -(farZ + nearZ) / (farZ - nearZ), -1);
    proj.r[3] = XMVectorSet(0, 0, -2.0f * farZ * nearZ / (farZ - nearZ), 0);

    return proj;
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
    xr.hasSessionTargetExt = false;

    for (const auto& ext : extensions) {
        LOG_DEBUG("  %s (v%u)", ext.extensionName, ext.extensionVersion);
        if (strcmp(ext.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0) {
            hasD3D11 = true;
        }
        if (strcmp(ext.extensionName, XR_EXT_SESSION_TARGET_EXTENSION_NAME) == 0) {
            xr.hasSessionTargetExt = true;
        }
    }

    LOG_INFO("XR_KHR_D3D11_enable: %s", hasD3D11 ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("XR_EXT_session_target: %s", xr.hasSessionTargetExt ? "AVAILABLE" : "NOT FOUND");

    if (!hasD3D11) {
        LOG_ERROR("XR_KHR_D3D11_enable extension not available - cannot continue");
        return false;
    }

    if (!xr.hasSessionTargetExt) {
        LOG_WARN("XR_EXT_session_target extension not available - window targeting disabled");
        // Continue anyway - will work without windowed mode
    }

    // Build list of extensions to enable
    std::vector<const char*> enabledExtensions;
    enabledExtensions.push_back(XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
    if (xr.hasSessionTargetExt) {
        enabledExtensions.push_back(XR_EXT_SESSION_TARGET_EXTENSION_NAME);
    }

    LOG_INFO("Enabling %zu extensions", enabledExtensions.size());
    for (const auto& ext : enabledExtensions) {
        LOG_INFO("  %s", ext);
    }

    // Create instance
    LOG_INFO("Creating OpenXR instance...");
    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(createInfo.applicationInfo.applicationName, "SessionTargetTest");
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

bool GetD3D11GraphicsRequirements(XrSessionManager& xr, LUID* outAdapterLuid) {
    LOG_INFO("Getting D3D11 graphics requirements...");

    // Get the function pointer for xrGetD3D11GraphicsRequirementsKHR
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

bool CreateSession(XrSessionManager& xr, ID3D11Device* d3d11Device, HWND hwnd) {
    LOG_INFO("Creating OpenXR session...");
    LOG_INFO("  D3D11 Device: 0x%p", d3d11Device);
    LOG_INFO("  Window handle (HWND): 0x%p", hwnd);

    xr.windowHandle = hwnd;

    // Get D3D11 graphics requirements
    LOG_INFO("Getting D3D11 graphics requirements...");
    XrGraphicsRequirementsD3D11KHR graphicsReq = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};

    // Get the function pointer for xrGetD3D11GraphicsRequirementsKHR
    PFN_xrGetD3D11GraphicsRequirementsKHR xrGetD3D11GraphicsRequirementsKHR = nullptr;
    XR_CHECK_LOG(xrGetInstanceProcAddr(xr.instance, "xrGetD3D11GraphicsRequirementsKHR",
        (PFN_xrVoidFunction*)&xrGetD3D11GraphicsRequirementsKHR));

    XR_CHECK_LOG(xrGetD3D11GraphicsRequirementsKHR(xr.instance, xr.systemId, &graphicsReq));
    LOG_INFO("D3D11 graphics requirements: adapter LUID 0x%08X%08X, minFeatureLevel %d",
        graphicsReq.adapterLuid.HighPart, graphicsReq.adapterLuid.LowPart,
        graphicsReq.minFeatureLevel);

    // Build the next chain for session creation
    LOG_INFO("Building session create info chain...");

    // D3D11 binding is required
    XrGraphicsBindingD3D11KHR d3d11Binding = {XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    d3d11Binding.device = d3d11Device;

    // Session target extension - chain it to the D3D11 binding
    XrSessionTargetCreateInfoEXT sessionTarget = {XR_TYPE_SESSION_TARGET_CREATE_INFO_EXT};
    sessionTarget.windowHandle = hwnd;

    if (xr.hasSessionTargetExt && hwnd) {
        // Chain: sessionInfo -> d3d11Binding -> sessionTarget
        d3d11Binding.next = &sessionTarget;
        LOG_INFO("Using XR_EXT_session_target with window handle");
        LOG_INFO("  Chain: XrSessionCreateInfo -> XrGraphicsBindingD3D11KHR -> XrSessionTargetCreateInfoEXT");
    } else {
        LOG_INFO("NOT using XR_EXT_session_target (hasExt=%d, hwnd=%p)",
            xr.hasSessionTargetExt, hwnd);
    }

    XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &d3d11Binding;
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

    // Prefer UNORM formats for better Vulkan compatibility (especially on Intel Iris GPUs)
    // SRGB formats can have format mapping issues in the D3D11->Vulkan compositor path
    int64_t selectedFormat = formats[0];
    for (int64_t format : formats) {
        // First choice: non-SRGB UNORM formats (best compatibility)
        if (format == DXGI_FORMAT_R8G8B8A8_UNORM ||
            format == DXGI_FORMAT_B8G8R8A8_UNORM) {
            selectedFormat = format;
            break;
        }
    }
    // If no UNORM found, try SRGB as fallback
    if (selectedFormat == formats[0]) {
        for (int64_t format : formats) {
            if (format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
                selectedFormat = format;
                break;
            }
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

        xr.swapchains[eye].images.resize(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        XR_CHECK(xrEnumerateSwapchainImages(xr.swapchains[eye].swapchain, imageCount, &imageCount,
            (XrSwapchainImageBaseHeader*)xr.swapchains[eye].images.data()));

        LOG_INFO("  Got %u swapchain images", imageCount);
        for (uint32_t i = 0; i < imageCount; i++) {
            LOG_DEBUG("    Image[%u]: texture 0x%p", i, xr.swapchains[eye].images[i].texture);
        }
    }

    LOG_INFO("Swapchains created successfully");
    return true;
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
    XMMATRIX& leftViewMatrix,
    XMMATRIX& leftProjMatrix,
    XMMATRIX& rightViewMatrix,
    XMMATRIX& rightProjMatrix
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
