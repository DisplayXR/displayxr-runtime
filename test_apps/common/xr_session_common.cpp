// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Common OpenXR session management - shared implementations
 */

#include "xr_session_common.h"
#include "logging.h"
#include <cstring>
#include <cmath>
// #include <chrono>  // [Commented out — was only used for convergence plane logging throttle]

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

    // Use runtime's preferred format (first in the list per OpenXR spec)
    int64_t selectedFormat = formats[0];
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

        // Count swapchain images (API-specific enumeration is done by each app)
        uint32_t imageCount = 0;
        XR_CHECK(xrEnumerateSwapchainImages(xr.swapchains[eye].swapchain, 0, &imageCount, nullptr));
        xr.swapchains[eye].imageCount = imageCount;

        LOG_INFO("  Got %u swapchain images", imageCount);
    }

    // Initialize recommended render dims from swapchain dims (updated by dynamic resolution events)
    if (xr.recommendedRenderWidth == 0 || xr.recommendedRenderHeight == 0) {
        xr.recommendedRenderWidth = xr.swapchains[0].width;
        xr.recommendedRenderHeight = xr.swapchains[0].height;
    }

    LOG_INFO("Swapchains created successfully");
    return true;
}

bool CreateQuadLayerSwapchain(XrSessionManager& xr, uint32_t width, uint32_t height) {
    LOG_INFO("Creating quad layer swapchain for UI overlay (%ux%u)...", width, height);

    // Query supported swapchain formats
    uint32_t formatCount = 0;
    XR_CHECK(xrEnumerateSwapchainFormats(xr.session, 0, &formatCount, nullptr));

    std::vector<int64_t> formats(formatCount);
    XR_CHECK(xrEnumerateSwapchainFormats(xr.session, formatCount, &formatCount, formats.data()));

    // Use runtime's preferred format (first in the list per OpenXR spec)
    int64_t selectedFormat = formats[0];
    LOG_INFO("Selected quad swapchain format: %lld (0x%llX)", selectedFormat, selectedFormat);

    XrSwapchainCreateInfo swapchainInfo = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    swapchainInfo.format = selectedFormat;
    swapchainInfo.sampleCount = 1;
    swapchainInfo.width = width;
    swapchainInfo.height = height;
    swapchainInfo.faceCount = 1;
    swapchainInfo.arraySize = 1;
    swapchainInfo.mipCount = 1;

    XR_CHECK_LOG(xrCreateSwapchain(xr.session, &swapchainInfo, &xr.quadSwapchain.swapchain));
    LOG_INFO("Quad swapchain created: 0x%p", (void*)xr.quadSwapchain.swapchain);

    xr.quadSwapchain.format = selectedFormat;
    xr.quadSwapchain.width = width;
    xr.quadSwapchain.height = height;

    // Count swapchain images (API-specific enumeration is done by each app)
    uint32_t imageCount = 0;
    XR_CHECK(xrEnumerateSwapchainImages(xr.quadSwapchain.swapchain, 0, &imageCount, nullptr));
    xr.quadSwapchain.imageCount = imageCount;

    LOG_INFO("Got %u quad swapchain images", imageCount);
    xr.hasQuadLayer = true;

    return true;
}

bool AcquireQuadSwapchainImage(XrSessionManager& xr, uint32_t& imageIndex) {
    if (!xr.hasQuadLayer) return false;

    XrSwapchainImageAcquireInfo acquireInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrResult result = xrAcquireSwapchainImage(xr.quadSwapchain.swapchain, &acquireInfo, &imageIndex);
    if (XR_FAILED(result)) return false;

    XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = XR_INFINITE_DURATION;
    result = xrWaitSwapchainImage(xr.quadSwapchain.swapchain, &waitInfo);
    if (XR_FAILED(result)) return false;

    return true;
}

bool ReleaseQuadSwapchainImage(XrSessionManager& xr) {
    if (!xr.hasQuadLayer) return false;

    XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    return XR_SUCCEEDED(xrReleaseSwapchainImage(xr.quadSwapchain.swapchain, &releaseInfo));
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
        case (XrStructureType)XR_TYPE_EVENT_DATA_RENDER_RESOLUTION_CHANGED_EXT: {
            auto* resEvent = (XrEventDataRenderResolutionChangedEXT*)&event;
            xr.recommendedRenderWidth = resEvent->recommendedImageRectWidth;
            xr.recommendedRenderHeight = resEvent->recommendedImageRectHeight;
            LOG_INFO("Render resolution changed: %ux%u",
                resEvent->recommendedImageRectWidth,
                resEvent->recommendedImageRectHeight);
            break;
        }
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
    XMMATRIX& rightProjMatrix,
    float playerPosX,
    float playerPosY,
    float playerPosZ,
    float playerYaw,
    float playerPitch
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

    // Apply player transform to XR poses (production-engine locomotion pattern).
    // The reference space stays fixed at the physical tracking origin. We apply the
    // player's virtual position/orientation to every pose from OpenXR, so all
    // subsystems (rendering, eye tracking, future controllers) see consistent
    // world-space coordinates. This matches how Unity/Unreal handle XR locomotion.
    XMVECTOR playerOri = XMQuaternionRotationRollPitchYaw(playerPitch, playerYaw, 0);
    XMVECTOR playerPos = XMVectorSet(playerPosX, playerPosY, playerPosZ, 0.0f);

    for (int i = 0; i < 2; i++) {
        // Transform position: worldPos = playerOrientation * localPos + playerPosition
        XMVECTOR localPos = XMVectorSet(
            views[i].pose.position.x, views[i].pose.position.y,
            views[i].pose.position.z, 0.0f);
        XMVECTOR worldPos = XMVector3Rotate(localPos, playerOri) + playerPos;

        // Transform orientation: worldOri = playerOrientation * localOrientation
        XMVECTOR localOri = XMVectorSet(
            views[i].pose.orientation.x, views[i].pose.orientation.y,
            views[i].pose.orientation.z, views[i].pose.orientation.w);
        XMVECTOR worldOri = XMQuaternionMultiply(localOri, playerOri);

        // Write back transformed pose
        XMFLOAT3 pos3;
        XMStoreFloat3(&pos3, worldPos);
        views[i].pose.position = {pos3.x, pos3.y, pos3.z};

        XMFLOAT4 ori4;
        XMStoreFloat4(&ori4, worldOri);
        views[i].pose.orientation = {ori4.x, ori4.y, ori4.z, ori4.w};
    }

    // Convert transformed poses to view matrices
    leftViewMatrix = XrPoseToViewMatrix(views[0].pose);
    leftProjMatrix = XrFovToProjectionMatrix(views[0].fov, 0.01f, 100.0f);
    rightViewMatrix = XrPoseToViewMatrix(views[1].pose);
    rightProjMatrix = XrFovToProjectionMatrix(views[1].fov, 0.01f, 100.0f);

    // Extract eye tracking info from transformed view poses (now in world space)
    xr.eyePosX = (views[0].pose.position.x + views[1].pose.position.x) / 2.0f;
    xr.eyePosY = (views[0].pose.position.y + views[1].pose.position.y) / 2.0f;
    xr.eyePosZ = (views[0].pose.position.z + views[1].pose.position.z) / 2.0f;
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
    if (XR_FAILED(result)) {
        // Release the acquired image to avoid deadlocking the swapchain
        XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(xr.swapchains[eye].swapchain, &releaseInfo);
        return false;
    }

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

bool CreateHudSwapchain(XrSessionManager& xr, uint32_t width, uint32_t height) {
    LOG_INFO("Creating HUD swapchain for window-space layer (%ux%u)...", width, height);

    // Query supported swapchain formats
    uint32_t formatCount = 0;
    XR_CHECK(xrEnumerateSwapchainFormats(xr.session, 0, &formatCount, nullptr));

    std::vector<int64_t> formats(formatCount);
    XR_CHECK(xrEnumerateSwapchainFormats(xr.session, formatCount, &formatCount, formats.data()));

    // Use runtime's preferred format (first in the list per OpenXR spec)
    int64_t selectedFormat = formats[0];
    LOG_INFO("Selected HUD swapchain format: %lld (0x%llX)", selectedFormat, selectedFormat);

    XrSwapchainCreateInfo swapchainInfo = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                               XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    swapchainInfo.format = selectedFormat;
    swapchainInfo.sampleCount = 1;
    swapchainInfo.width = width;
    swapchainInfo.height = height;
    swapchainInfo.faceCount = 1;
    swapchainInfo.arraySize = 1;
    swapchainInfo.mipCount = 1;

    XR_CHECK_LOG(xrCreateSwapchain(xr.session, &swapchainInfo, &xr.hudSwapchain.swapchain));
    LOG_INFO("HUD swapchain created: 0x%p", (void*)xr.hudSwapchain.swapchain);

    xr.hudSwapchain.format = selectedFormat;
    xr.hudSwapchain.width = width;
    xr.hudSwapchain.height = height;

    // Count swapchain images (API-specific enumeration is done by each app)
    uint32_t imageCount = 0;
    XR_CHECK(xrEnumerateSwapchainImages(xr.hudSwapchain.swapchain, 0, &imageCount, nullptr));
    xr.hudSwapchain.imageCount = imageCount;

    LOG_INFO("Got %u HUD swapchain images", imageCount);
    xr.hasHudSwapchain = true;

    return true;
}

bool AcquireHudSwapchainImage(XrSessionManager& xr, uint32_t& imageIndex) {
    if (!xr.hasHudSwapchain) return false;

    XrSwapchainImageAcquireInfo acquireInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrResult result = xrAcquireSwapchainImage(xr.hudSwapchain.swapchain, &acquireInfo, &imageIndex);
    if (XR_FAILED(result)) return false;

    XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = XR_INFINITE_DURATION;
    result = xrWaitSwapchainImage(xr.hudSwapchain.swapchain, &waitInfo);
    if (XR_FAILED(result)) return false;

    return true;
}

bool ReleaseHudSwapchainImage(XrSessionManager& xr) {
    if (!xr.hasHudSwapchain) return false;

    XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    return XR_SUCCEEDED(xrReleaseSwapchainImage(xr.hudSwapchain.swapchain, &releaseInfo));
}

bool EndFrameWithWindowSpaceHud(
    XrSessionManager& xr,
    XrTime displayTime,
    const XrCompositionLayerProjectionView* projViews,
    float hudX, float hudY, float hudWidth, float hudHeight,
    float hudDisparity
) {
    // Projection layer for the 3D scene
    XrCompositionLayerProjection projectionLayer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projectionLayer.space = xr.localSpace;
    projectionLayer.viewCount = 2;
    projectionLayer.views = projViews;

    // Window-space HUD layer
    XrCompositionLayerWindowSpaceEXT hudLayer = {};
    hudLayer.type = (XrStructureType)XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_EXT;
    hudLayer.next = nullptr;
    hudLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    hudLayer.subImage.swapchain = xr.hudSwapchain.swapchain;
    hudLayer.subImage.imageRect.offset = {0, 0};
    hudLayer.subImage.imageRect.extent = {
        (int32_t)xr.hudSwapchain.width,
        (int32_t)xr.hudSwapchain.height
    };
    hudLayer.subImage.imageArrayIndex = 0;
    hudLayer.x = hudX;
    hudLayer.y = hudY;
    hudLayer.width = hudWidth;
    hudLayer.height = hudHeight;
    hudLayer.disparity = hudDisparity;

    // Submit both layers - projection first, then HUD on top
    const XrCompositionLayerBaseHeader* layers[] = {
        (XrCompositionLayerBaseHeader*)&projectionLayer,
        (XrCompositionLayerBaseHeader*)&hudLayer
    };

    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = displayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = xr.hasHudSwapchain ? 2 : 1;
    endInfo.layers = layers;

    return XR_SUCCEEDED(xrEndFrame(xr.session, &endInfo));
}

// [Commented out — will be reused for 3D-positioned HUD later]
#if 0
ConvergencePlane LocateConvergencePlane(const XrView views[2]) {
    // Throttled logging: log every ~5 seconds
    static auto lastLogTime = std::chrono::steady_clock::now() - std::chrono::seconds(5);
    auto now = std::chrono::steady_clock::now();
    bool shouldLog = std::chrono::duration_cast<std::chrono::seconds>(now - lastLogTime).count() >= 5;

    ConvergencePlane result = {};
    result.valid = false;

    // FOV tan angles (positive magnitudes) for each eye
    // Left eye = views[0], Right eye = views[1]
    float u0 = tanf(views[0].fov.angleUp);
    float d0 = -tanf(views[0].fov.angleDown);   // positive magnitude
    float r0 = tanf(views[0].fov.angleRight);
    float l0 = -tanf(views[0].fov.angleLeft);    // positive magnitude

    float u1 = tanf(views[1].fov.angleUp);
    float d1 = -tanf(views[1].fov.angleDown);
    float r1 = tanf(views[1].fov.angleRight);
    float l1 = -tanf(views[1].fov.angleLeft);

    if (shouldLog) {
        LOG_WARN("[ConvPlane] FOV L eye: up=%.4f down=%.4f right=%.4f left=%.4f (rad)",
            views[0].fov.angleUp, views[0].fov.angleDown, views[0].fov.angleRight, views[0].fov.angleLeft);
        LOG_WARN("[ConvPlane] FOV R eye: up=%.4f down=%.4f right=%.4f left=%.4f (rad)",
            views[1].fov.angleUp, views[1].fov.angleDown, views[1].fov.angleRight, views[1].fov.angleLeft);
        LOG_WARN("[ConvPlane] tan: u0=%.4f d0=%.4f r0=%.4f l0=%.4f | u1=%.4f d1=%.4f r1=%.4f l1=%.4f",
            u0, d0, r0, l0, u1, d1, r1, l1);
        LOG_WARN("[ConvPlane] L pos: (%.6f, %.6f, %.6f) R pos: (%.6f, %.6f, %.6f)",
            views[0].pose.position.x, views[0].pose.position.y, views[0].pose.position.z,
            views[1].pose.position.x, views[1].pose.position.y, views[1].pose.position.z);
    }

    // Camera-local frame: use the left eye orientation as the reference frame.
    // Transform both eye positions into this frame via inverse left eye quaternion.
    XMVECTOR leftQuat = XMVectorSet(
        views[0].pose.orientation.x, views[0].pose.orientation.y,
        views[0].pose.orientation.z, views[0].pose.orientation.w);
    XMVECTOR invLeftQuat = XMQuaternionInverse(leftQuat);

    // Eye midpoint in world space
    XMVECTOR leftPos = XMVectorSet(
        views[0].pose.position.x, views[0].pose.position.y,
        views[0].pose.position.z, 0.0f);
    XMVECTOR rightPos = XMVectorSet(
        views[1].pose.position.x, views[1].pose.position.y,
        views[1].pose.position.z, 0.0f);
    XMVECTOR midpoint = XMVectorScale(XMVectorAdd(leftPos, rightPos), 0.5f);

    // Transform eye positions into the left-eye-aligned local frame
    XMVECTOR localLeft = XMVector3Rotate(XMVectorSubtract(leftPos, midpoint), invLeftQuat);
    XMVECTOR localRight = XMVector3Rotate(XMVectorSubtract(rightPos, midpoint), invLeftQuat);

    XMFLOAT3 lp, rp;
    XMStoreFloat3(&lp, localLeft);
    XMStoreFloat3(&rp, localRight);

    float x0 = lp.x, y0 = lp.y, z0 = lp.z;
    float x1 = rp.x, y1 = rp.y, z1 = rp.z;

    if (shouldLog) {
        LOG_WARN("[ConvPlane] local L: (%.6f, %.6f, %.6f) local R: (%.6f, %.6f, %.6f)",
            x0, y0, z0, x1, y1, z1);
    }

    // Compute display center Z from frustum intersection
    // denomX = (r1 - l1) - (r0 - l0)  (difference in horizontal FOV widths)
    float denomX = (r1 - l1) - (r0 - l0);
    if (shouldLog) {
        LOG_WARN("[ConvPlane] (r0-l0)=%.6f (r1-l1)=%.6f denomX=%.6f",
            r0 - l0, r1 - l1, denomX);
    }
    if (fabsf(denomX) < 0.0001f) {
        if (shouldLog) {
            LOG_WARN("[ConvPlane] FAILED: denomX too small (%.6f) - symmetric/degenerate FOVs", denomX);
            lastLogTime = now;
        }
        return result;
    }

    float zd = (2.0f * (x1 - x0) + z1 * (r1 - l1) - z0 * (r0 - l0)) / denomX;
    float xd = x0 - (r0 - l0) * (zd - z0) / 2.0f;
    float yd = y0 - (u0 - d0) * (zd - z0) / 2.0f;

    // Display size
    float W = fabsf((z0 - zd) * (l0 + r0));
    float H = fabsf((z0 - zd) * (u0 + d0));

    if (shouldLog) {
        LOG_WARN("[ConvPlane] display center local: (%.4f, %.4f, %.4f) size: %.4f x %.4f m",
            xd, yd, zd, W, H);
    }

    if (W < 0.001f || H < 0.001f) {
        if (shouldLog) {
            LOG_WARN("[ConvPlane] FAILED: display too small W=%.6f H=%.6f", W, H);
            lastLogTime = now;
        }
        return result;
    }

    // Transform display center back to LOCAL (world) space
    XMVECTOR localCenter = XMVectorSet(xd, yd, zd, 0.0f);
    XMVECTOR worldCenter = XMVectorAdd(XMVector3Rotate(localCenter, leftQuat), midpoint);

    XMFLOAT3 wc;
    XMStoreFloat3(&wc, worldCenter);

    result.pose.position = {wc.x, wc.y, wc.z};
    // Orientation matches the left eye orientation (camera-aligned)
    result.pose.orientation = views[0].pose.orientation;
    result.width = W;
    result.height = H;
    result.valid = true;

    if (shouldLog) {
        LOG_WARN("[ConvPlane] SUCCESS: world pos (%.4f, %.4f, %.4f) size %.4f x %.4f m",
            wc.x, wc.y, wc.z, W, H);
        lastLogTime = now;
    }

    return result;
}

XrPosef ComputeHUDPose(
    const ConvergencePlane& plane,
    float coverageFraction,
    const XrView views[2],
    float& outWidth, float& outHeight
) {
    outWidth = coverageFraction * plane.width;
    outHeight = coverageFraction * plane.height;

    // HUD center in plane-local coords (plane center is origin):
    // Left edge of plane: -W/2, right edge: +W/2
    // Top edge of plane:  +H/2, bottom edge: -H/2
    // HUD is at top-left, so:
    //   x = -W/2 + hudWidth/2  = -0.5*W + 0.5*coverageFraction*W = W*(-0.5 + 0.5*cf)
    //   y = +H/2 - hudHeight/2 = +0.5*H - 0.5*coverageFraction*H = H*(+0.5 - 0.5*cf)
    //   z = 0 (on the plane)
    float localX = plane.width * (-0.5f + 0.5f * coverageFraction);
    float localY = plane.height * (0.5f - 0.5f * coverageFraction);
    float localZ = 0.0f;

    // Rotate local offset by the plane's orientation to get world offset
    XMVECTOR planeQuat = XMVectorSet(
        plane.pose.orientation.x, plane.pose.orientation.y,
        plane.pose.orientation.z, plane.pose.orientation.w);
    XMVECTOR localOffset = XMVectorSet(localX, localY, localZ, 0.0f);
    XMVECTOR worldOffset = XMVector3Rotate(localOffset, planeQuat);

    XMVECTOR planeCenter = XMVectorSet(
        plane.pose.position.x, plane.pose.position.y,
        plane.pose.position.z, 0.0f);
    XMVECTOR hudWorldCenter = XMVectorAdd(planeCenter, worldOffset);

    // Transform from LOCAL (world) space to VIEW space.
    // The Monado state tracker silently drops LOCAL-space quad layers when
    // handle_space() fails to locate the head device. VIEW space always works
    // because it bypasses device locating. We compute the view-relative pose here.
    //
    // VIEW space origin = eye midpoint, orientation = left eye orientation.
    // viewRelativePos = invViewQuat * (worldPos - viewOrigin)
    XMVECTOR leftPos = XMVectorSet(
        views[0].pose.position.x, views[0].pose.position.y,
        views[0].pose.position.z, 0.0f);
    XMVECTOR rightPos = XMVectorSet(
        views[1].pose.position.x, views[1].pose.position.y,
        views[1].pose.position.z, 0.0f);
    XMVECTOR viewOrigin = XMVectorScale(XMVectorAdd(leftPos, rightPos), 0.5f);

    XMVECTOR viewQuat = XMVectorSet(
        views[0].pose.orientation.x, views[0].pose.orientation.y,
        views[0].pose.orientation.z, views[0].pose.orientation.w);
    XMVECTOR invViewQuat = XMQuaternionInverse(viewQuat);

    XMVECTOR viewRelPos = XMVector3Rotate(XMVectorSubtract(hudWorldCenter, viewOrigin), invViewQuat);

    XMFLOAT3 vr;
    XMStoreFloat3(&vr, viewRelPos);

    XrPosef hudPose;
    hudPose.position = {vr.x, vr.y, vr.z};
    hudPose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};  // Identity in VIEW space

    return hudPose;
}

bool EndFrameWithQuadLayer(
    XrSessionManager& xr,
    XrTime displayTime,
    const XrCompositionLayerProjectionView* projViews,
    const XrPosef& quadPose,
    float quadWidth, float quadHeight
) {
    // Projection layer for the 3D scene
    XrCompositionLayerProjection projectionLayer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projectionLayer.space = xr.localSpace;
    projectionLayer.viewCount = 2;
    projectionLayer.views = projViews;

    // Quad layer for UI overlay - positioned in VIEW space (pose is view-relative)
    // We use VIEW space because Monado's handle_space() silently drops LOCAL-space
    // quad layers when the head device relation flags are zero. The caller computes
    // the view-relative pose from the convergence plane each frame.
    XrCompositionLayerQuad quadLayer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    quadLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    quadLayer.space = xr.viewSpace;  // VIEW space = always works in handle_space()
    quadLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quadLayer.subImage.swapchain = xr.quadSwapchain.swapchain;
    quadLayer.subImage.imageRect.offset = {0, 0};
    quadLayer.subImage.imageRect.extent = {
        (int32_t)xr.quadSwapchain.width,
        (int32_t)xr.quadSwapchain.height
    };
    quadLayer.subImage.imageArrayIndex = 0;

    // Position the quad at the computed pose (view-relative, in VIEW space)
    quadLayer.pose = quadPose;

    // Size of the quad in meters
    quadLayer.size = {quadWidth, quadHeight};

    // Submit both layers - projection first, then quad on top
    const XrCompositionLayerBaseHeader* layers[] = {
        (XrCompositionLayerBaseHeader*)&projectionLayer,
        (XrCompositionLayerBaseHeader*)&quadLayer
    };

    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = displayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = xr.hasQuadLayer ? 2 : 1;
    endInfo.layers = layers;

    return XR_SUCCEEDED(xrEndFrame(xr.session, &endInfo));
}
#endif // Commented out convergence plane / quad layer code

void CleanupOpenXR(XrSessionManager& xr) {
    LOG_INFO("Cleaning up OpenXR resources...");

    // Destroy HUD swapchain
    if (xr.hudSwapchain.swapchain != XR_NULL_HANDLE) {
        LOG_INFO("Destroying HUD swapchain...");
        xrDestroySwapchain(xr.hudSwapchain.swapchain);
        xr.hudSwapchain.swapchain = XR_NULL_HANDLE;
        xr.hasHudSwapchain = false;
    }

    // Destroy quad layer swapchain
    if (xr.quadSwapchain.swapchain != XR_NULL_HANDLE) {
        LOG_INFO("Destroying quad swapchain...");
        xrDestroySwapchain(xr.quadSwapchain.swapchain);
        xr.quadSwapchain.swapchain = XR_NULL_HANDLE;
        xr.hasQuadLayer = false;
    }

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
