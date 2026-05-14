// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  SR Cube OpenXR Ext VK - OpenXR with XR_EXT_win32_window_binding (Vulkan)
 *
 * Vulkan port of cube_handle_d3d11. Projection layer only, no HUD/quad layer.
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>

#include "logging.h"
#include "input_handler.h"
#include "xr_session.h"
#include "vk_renderer.h"
#include "display3d_view.h"
#include "camera3d_view.h"

#include "hud_renderer.h"
#include "text_overlay.h"
#include "atlas_capture.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace DirectX;

static const char* APP_NAME = "cube_handle_vk_win";

static const wchar_t* WINDOW_CLASS = L"SRCubeOpenXRExtVKClass";
static const wchar_t* WINDOW_TITLE = L"Vulkan Cube \u2014 VK Native Compositor (External Window)";

// HUD overlay fractions: WIDTH_FRACTION anchors how wide the HUD appears on screen;
// HEIGHT_FRACTION sets the HUD texture pixel height (aspect ratio preserved dynamically).
static const float HUD_WIDTH_FRACTION = 0.30f;
static const float HUD_HEIGHT_FRACTION = 0.50f;
static const float CAMERA_HALF_TAN_VFOV = 0.32491969623f; // tan(18°) → 36° vFOV

// Global state (shared between main thread and render thread)
static InputState g_inputState;
static std::mutex g_inputMutex;
static std::atomic<bool> g_running{true};
static XrSessionManager* g_xr = nullptr;

// Set DISPLAYXR_TRANSPARENT_BG=1 in the environment to opt into transparent
// desktop composition: the HWND is created with WS_EX_NOREDIRECTIONBITMAP +
// null background brush, the cube clears RGBA(0,0,0,0), and the win32 window
// binding asks the runtime to chroma-key the woven output back to alpha.
// Pairs with PR #3c (Leia VK chroma-key DP).
static bool TransparentBackgroundEnabled() {
    static const bool e = []() {
        const char *v = getenv("DISPLAYXR_TRANSPARENT_BG");
        return v != nullptr && *v != '\0' && *v != '0';
    }();
    return e;
}

// Vulkan handles needed for the 'I'-key atlas readback (single-writer in
// main() before the render thread launches; render thread only reads).
static VkPhysicalDevice                            g_vkPhysDevice = VK_NULL_HANDLE;
static const std::vector<XrSwapchainImageVulkanKHR>* g_vkSwapchainImages = nullptr;
static VkFormat                                    g_vkColorFormat = VK_FORMAT_UNDEFINED;
static UINT g_windowWidth = 1280;
static UINT g_windowHeight = 720;

// Fullscreen state
static bool g_fullscreen = false;
static RECT g_savedWindowRect = {};
static DWORD g_savedWindowStyle = 0;

// Toggle fullscreen mode for the app window
static void ToggleFullscreen(HWND hwnd) {
    if (g_fullscreen) {
        // Exit fullscreen - restore window style and position
        SetWindowLong(hwnd, GWL_STYLE, g_savedWindowStyle);
        SetWindowPos(hwnd, HWND_TOP,
            g_savedWindowRect.left, g_savedWindowRect.top,
            g_savedWindowRect.right - g_savedWindowRect.left,
            g_savedWindowRect.bottom - g_savedWindowRect.top,
            SWP_FRAMECHANGED);
        g_fullscreen = false;
        LOG_INFO("Exited fullscreen mode");
    } else {
        // Enter fullscreen - save state and go borderless
        g_savedWindowStyle = GetWindowLong(hwnd, GWL_STYLE);
        GetWindowRect(hwnd, &g_savedWindowRect);

        HMONITOR hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfo(hMonitor, &mi);

        SetWindowLong(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_TOP,
            mi.rcMonitor.left, mi.rcMonitor.top,
            mi.rcMonitor.right - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top,
            SWP_FRAMECHANGED);
        g_fullscreen = true;
        LOG_INFO("Entered fullscreen mode");
    }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    {
        std::lock_guard<std::mutex> lock(g_inputMutex);
        UpdateInputState(g_inputState, msg, wParam, lParam);
    }

    switch (msg) {
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);  // Outside mutex — safe from reentrant deadlock
        return 0;

    case WM_LBUTTONUP:
        ReleaseCapture();  // Outside mutex — WM_CAPTURECHANGED can safely re-enter
        return 0;

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_windowWidth = LOWORD(lParam);
            g_windowHeight = HIWORD(lParam);
        }
        return 0;

    case WM_CLOSE:
        // Graceful shutdown: ask OpenXR to end the session so the state machine
        // runs STOPPING -> xrEndSession -> EXITING -> exitRequested before cleanup.
        if (g_xr && g_xr->session != XR_NULL_HANDLE && g_xr->sessionRunning) {
            xrRequestExitSession(g_xr->session);
            return 0;
        }
        g_running.store(false);
        PostQuitMessage(0);
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            PostMessage(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        if (wParam == VK_F11) {
            ToggleFullscreen(hwnd);
            return 0;
        }
        break;

    case WM_TIMER:
        if (wParam == dxr_capture::kFlashTimerId) {
            dxr_capture::TickCaptureFlash(hwnd);
            return 0;
        }
        break;

    case dxr_capture::kFlashUserMsg:
        dxr_capture::TriggerCaptureFlash(hwnd);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static HWND CreateAppWindow(HINSTANCE hInstance, int width, int height) {
    LOG_INFO("Creating application window (%dx%d)", width, height);

    const bool transparent = TransparentBackgroundEnabled();

    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    // For transparent backgrounds the HWND must NOT have a redirection bitmap
    // and must NOT have a background brush — DComp's per-pixel alpha-out
    // composes through to the desktop only when both are absent. See
    // docs/specs/extensions/XR_EXT_win32_window_binding.md "Transparent-window contract".
    wc.hbrBackground = transparent ? nullptr : (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = WINDOW_CLASS;

    if (!RegisterClassEx(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            LOG_ERROR("Failed to register window class, error: %lu", err);
            return nullptr;
        }
    }

    RECT rect = { 0, 0, width, height };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    DWORD exStyle = transparent ? WS_EX_NOREDIRECTIONBITMAP : 0;
    HWND hwnd = CreateWindowEx(exStyle, WINDOW_CLASS, WINDOW_TITLE, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd) {
        LOG_ERROR("Failed to create window, error: %lu", GetLastError());
        return nullptr;
    }

    LOG_INFO("Window created: 0x%p", hwnd);
    return hwnd;
}

struct PerformanceStats {
    std::chrono::high_resolution_clock::time_point lastTime;
    float deltaTime = 0.0f;
    float fps = 0.0f;
    float frameTimeMs = 0.0f;
    int frameCount = 0;
    float fpsAccumulator = 0.0f;
};

static void UpdatePerformanceStats(PerformanceStats& stats) {
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(now - stats.lastTime);
    stats.deltaTime = duration.count() / 1000000.0f;
    stats.frameTimeMs = duration.count() / 1000.0f;
    stats.lastTime = now;
    stats.fpsAccumulator += stats.deltaTime;
    stats.frameCount++;
    if (stats.fpsAccumulator >= 1.0f) {
        stats.fps = stats.frameCount / stats.fpsAccumulator;
        stats.frameCount = 0;
        stats.fpsAccumulator = 0.0f;
    }
}

static void RenderThreadFunc(
    HWND hwnd,
    XrSessionManager* xr,
    VkRenderer* renderer,
    HudRenderer* hud,
    uint32_t hudWidth,
    uint32_t hudHeight,
    VkBuffer hudStagingBuffer,
    void* hudStagingMapped,
    VkCommandPool hudCmdPool,
    std::vector<XrSwapchainImageVulkanKHR>* hudSwapchainImages)
{
    LOG_INFO("[RenderThread] Started");

    PerformanceStats perfStats = {};
    perfStats.lastTime = std::chrono::high_resolution_clock::now();

    while (g_running.load() && !xr->exitRequested) {
        InputState inputSnapshot;
        bool resetRequested = false;
        bool outputModeChanged = false;
        uint32_t windowW, windowH;
        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            inputSnapshot = g_inputState;
            resetRequested = g_inputState.resetViewRequested;
            g_inputState.resetViewRequested = false;
            g_inputState.fullscreenToggleRequested = false;
            g_inputState.eyeTrackingModeToggleRequested = false;
            g_inputState.eyeTrackingModeToggleRequested = false;
            outputModeChanged = g_inputState.renderingModeChangeRequested;
            g_inputState.renderingModeChangeRequested = false;
            windowW = g_windowWidth;
            windowH = g_windowHeight;
        }

        if (outputModeChanged && xr->pfnRequestDisplayRenderingModeEXT &&
            xr->session != XR_NULL_HANDLE) {
            xr->pfnRequestDisplayRenderingModeEXT(xr->session, inputSnapshot.currentRenderingMode);
        }
        // Handle eye tracking mode toggle (T key)
        if (inputSnapshot.eyeTrackingModeToggleRequested) {
            if (xr->pfnRequestEyeTrackingModeEXT && xr->session != XR_NULL_HANDLE) {
                XrEyeTrackingModeEXT newMode = (xr->activeEyeTrackingMode == XR_EYE_TRACKING_MODE_MANAGED_EXT)
                    ? XR_EYE_TRACKING_MODE_MANUAL_EXT : XR_EYE_TRACKING_MODE_MANAGED_EXT;
                XrResult etResult = xr->pfnRequestEyeTrackingModeEXT(xr->session, newMode);
                LOG_INFO("Eye tracking mode -> %s (%s)",
                    newMode == XR_EYE_TRACKING_MODE_MANUAL_EXT ? "MANUAL" : "MANAGED",
                    XR_SUCCEEDED(etResult) ? "OK" : "unsupported");
            }
        }

        UpdatePerformanceStats(perfStats);
        UpdateCameraMovement(inputSnapshot, perfStats.deltaTime, xr->displayHeightM);

        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_inputState.cameraPosX = inputSnapshot.cameraPosX;
            g_inputState.cameraPosY = inputSnapshot.cameraPosY;
            g_inputState.cameraPosZ = inputSnapshot.cameraPosZ;
            if (resetRequested) {
                g_inputState.yaw = inputSnapshot.yaw;
                g_inputState.pitch = inputSnapshot.pitch;
                g_inputState.viewParams = inputSnapshot.viewParams;
            }
        }

        UpdateScene(*renderer, perfStats.deltaTime);
        PollEvents(*xr);

        if (xr->sessionRunning) {
            XrFrameState frameState;
            LOG_INFO("[FRAME] Calling BeginFrame (xrWaitFrame + xrBeginFrame)...");
            if (BeginFrame(*xr, frameState)) {
                LOG_INFO("[FRAME] BeginFrame OK, shouldRender=%d, displayTime=%lld",
                    frameState.shouldRender, (long long)frameState.predictedDisplayTime);
                // Get N-view mode info from enumerated rendering modes
                bool monoMode = (xr->renderingModeCount > 0 && !xr->renderingModeDisplay3D[inputSnapshot.currentRenderingMode]);
                if (xr->renderingModeCount > 0 && inputSnapshot.currentRenderingMode < xr->renderingModeCount) {
                    xr->recommendedViewScaleX = xr->renderingModeScaleX[inputSnapshot.currentRenderingMode];
                    xr->recommendedViewScaleY = xr->renderingModeScaleY[inputSnapshot.currentRenderingMode];
                }
                uint32_t modeViewCount = (xr->renderingModeCount > 0 && inputSnapshot.currentRenderingMode < xr->renderingModeCount)
                    ? xr->renderingModeViewCounts[inputSnapshot.currentRenderingMode] : 2;
                uint32_t tileColumns = (xr->renderingModeCount > 0 && inputSnapshot.currentRenderingMode < xr->renderingModeCount)
                    ? xr->renderingModeTileColumns[inputSnapshot.currentRenderingMode] : 2;
                uint32_t tileRows = (xr->renderingModeCount > 0 && inputSnapshot.currentRenderingMode < xr->renderingModeCount)
                    ? xr->renderingModeTileRows[inputSnapshot.currentRenderingMode] : 1;
                int eyeCount = monoMode ? 1 : (int)modeViewCount;

                // Dynamic arrays for N-view rendering
                std::vector<XrCompositionLayerProjectionView> projectionViews(eyeCount, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
                bool rendered = false;
                bool hudSubmitted = false;

                if (frameState.shouldRender) {
                    LOG_INFO("[FRAME] Calling LocateViews...");
                    if (LocateViews(*xr, frameState.predictedDisplayTime,
                        inputSnapshot.cameraPosX, inputSnapshot.cameraPosY, inputSnapshot.cameraPosZ,
                        inputSnapshot.yaw, inputSnapshot.pitch,
                        inputSnapshot.viewParams)) {
                        LOG_INFO("[FRAME] LocateViews OK");

                        // Get raw view poses for projection views.
                        // Use DISPLAY space when available: it is physically anchored to the
                        // display center and unaffected by recentering, which is the correct
                        XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
                        locateInfo.viewConfigurationType = xr->viewConfigType;
                        locateInfo.displayTime = frameState.predictedDisplayTime;
                        locateInfo.space = xr->localSpace;

                        XrViewState viewState = {XR_TYPE_VIEW_STATE};
                        uint32_t viewCount = 8;
                        XrView rawViews[8];
                        for (uint32_t vi = 0; vi < 8; vi++) rawViews[vi] = {XR_TYPE_VIEW};
                        xrLocateViews(xr->session, &locateInfo, &viewState, 8, &viewCount, rawViews);
                        LOG_INFO("[FRAME] Raw LocateViews done");

                        // Max per-tile capacity from swapchain
                        uint32_t maxTileW = tileColumns > 0 ? xr->swapchain.width / tileColumns : xr->swapchain.width;
                        uint32_t maxTileH = tileRows > 0 ? xr->swapchain.height / tileRows : xr->swapchain.height;

                        // Compute render dims: mono uses full swapchain, stereo uses tile-sized
                        uint32_t renderW, renderH;
                        if (monoMode) {
                            renderW = windowW;
                            renderH = windowH;
                            if (renderW > xr->swapchain.width) renderW = xr->swapchain.width;
                            if (renderH > xr->swapchain.height) renderH = xr->swapchain.height;
                        } else {
                            renderW = (uint32_t)(windowW * xr->recommendedViewScaleX);
                            renderH = (uint32_t)(windowH * xr->recommendedViewScaleY);
                            if (renderW > maxTileW) renderW = maxTileW;
                            if (renderH > maxTileH) renderH = maxTileH;
                        }

                        // --- App-side Kooima via canonical library ---
                        std::vector<Display3DView> stereoViews(eyeCount);
                        bool useAppProjection = (xr->hasDisplayInfoExt && xr->displayWidthM > 0.0f);
                        if (useAppProjection) {
                            float dispPxW = xr->displayPixelWidth > 0 ? (float)xr->displayPixelWidth : (float)xr->swapchain.width;
                            float dispPxH = xr->displayPixelHeight > 0 ? (float)xr->displayPixelHeight : (float)xr->swapchain.height;
                            float pxSizeX = xr->displayWidthM / dispPxW;
                            float pxSizeY = xr->displayHeightM / dispPxH;
                            float winW_m = (float)windowW * pxSizeX;
                            float winH_m = (float)windowH * pxSizeY;

                            // Window-relative Kooima: compute eye offset from window center
                            float eyeOffsetX = 0.0f, eyeOffsetY = 0.0f;
                            {
                                POINT clientOrigin = {0, 0};
                                ClientToScreen(hwnd, &clientOrigin);
                                HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                                MONITORINFO mi = {sizeof(mi)};
                                if (GetMonitorInfo(hMon, &mi)) {
                                    float winCenterX = (float)(clientOrigin.x - mi.rcMonitor.left) + windowW / 2.0f;
                                    float winCenterY = (float)(clientOrigin.y - mi.rcMonitor.top) + windowH / 2.0f;
                                    float dispW = (float)(mi.rcMonitor.right - mi.rcMonitor.left);
                                    float dispH = (float)(mi.rcMonitor.bottom - mi.rcMonitor.top);
                                    eyeOffsetX = (winCenterX - dispW / 2.0f) * pxSizeX;
                                    eyeOffsetY = -((winCenterY - dispH / 2.0f) * pxSizeY);
                                }
                            }

                            // Build per-view eye positions (shifted to window center)
                            std::vector<XrVector3f> rawEyes(eyeCount);
                            for (int v = 0; v < eyeCount; v++) {
                                XrVector3f pos = (v < (int)viewCount) ? rawViews[v].pose.position : rawViews[0].pose.position;
                                rawEyes[v] = {pos.x - eyeOffsetX, pos.y - eyeOffsetY, pos.z};
                            }

                            // For mono: average all views to center eye
                            if (monoMode) {
                                XrVector3f center = {0.0f, 0.0f, 0.0f};
                                for (int v = 0; v < (int)viewCount && v < eyeCount; v++) {
                                    center.x += rawEyes[v].x;
                                    center.y += rawEyes[v].y;
                                    center.z += rawEyes[v].z;
                                }
                                int cnt = (eyeCount < (int)viewCount) ? eyeCount : (int)viewCount;
                                if (cnt < 1) cnt = 1;
                                center.x /= cnt; center.y /= cnt; center.z /= cnt;
                                rawEyes[0] = center;
                            }

                            // Build camera/display pose from player yaw/pitch/position
                            XrPosef cameraPose;
                            XMVECTOR pOri = XMQuaternionRotationRollPitchYaw(
                                inputSnapshot.pitch, inputSnapshot.yaw, 0);
                            XMFLOAT4 q;
                            XMStoreFloat4(&q, pOri);
                            cameraPose.orientation = {q.x, q.y, q.z, q.w};
                            cameraPose.position = {inputSnapshot.cameraPosX, inputSnapshot.cameraPosY, inputSnapshot.cameraPosZ};

                            XrVector3f nominalViewer = {xr->nominalViewerX, xr->nominalViewerY, xr->nominalViewerZ};
                            Display3DScreen screen = {winW_m, winH_m};

                            if (inputSnapshot.cameraMode) {
                                // Camera-centric path
                                Camera3DTunables camTunables;
                                camTunables.ipd_factor = inputSnapshot.viewParams.ipdFactor;
                                camTunables.parallax_factor = inputSnapshot.viewParams.parallaxFactor;
                                camTunables.inv_convergence_distance = inputSnapshot.viewParams.invConvergenceDistance;
                                camTunables.half_tan_vfov = CAMERA_HALF_TAN_VFOV / inputSnapshot.viewParams.zoomFactor;

                                std::vector<Camera3DView> camViews(eyeCount);
                                camera3d_compute_views(
                                    rawEyes.data(), eyeCount, &nominalViewer,
                                    &screen, &camTunables, &cameraPose,
                                    0.01f, 100.0f, camViews.data());

                                // Copy into Display3DView for uniform downstream rendering
                                for (int i = 0; i < eyeCount; i++) {
                                    memcpy(stereoViews[i].view_matrix, camViews[i].view_matrix, sizeof(float) * 16);
                                    memcpy(stereoViews[i].projection_matrix, camViews[i].projection_matrix, sizeof(float) * 16);
                                    stereoViews[i].fov = camViews[i].fov;
                                    stereoViews[i].eye_world = camViews[i].eye_world;
                                }
                            } else {
                                // Display-centric path
                                Display3DTunables tunables;
                                tunables.ipd_factor = inputSnapshot.viewParams.ipdFactor;
                                tunables.parallax_factor = inputSnapshot.viewParams.parallaxFactor;
                                tunables.perspective_factor = inputSnapshot.viewParams.perspectiveFactor;
                                tunables.virtual_display_height = inputSnapshot.viewParams.virtualDisplayHeight / inputSnapshot.viewParams.scaleFactor;

                                display3d_compute_views(
                                    rawEyes.data(), eyeCount, &nominalViewer,
                                    &screen, &tunables, &cameraPose,
                                    0.01f, 100.0f, stereoViews.data());
                            }
                        }

                        rendered = true;

                        // For mono: compute center eye position and projection
                        XMMATRIX monoViewMatrix, monoProjMatrix;
                        XrPosef monoPose = rawViews[0].pose;
                        if (monoMode) {
                            // Center eye = average of all view positions
                            monoPose.position = {0, 0, 0};
                            int cnt = 0;
                            for (uint32_t v = 0; v < viewCount && v < 8; v++) {
                                monoPose.position.x += rawViews[v].pose.position.x;
                                monoPose.position.y += rawViews[v].pose.position.y;
                                monoPose.position.z += rawViews[v].pose.position.z;
                                cnt++;
                            }
                            if (cnt > 1) {
                                monoPose.position.x /= cnt;
                                monoPose.position.y /= cnt;
                                monoPose.position.z /= cnt;
                            }

                            // When useAppProjection, mono view+proj come from stereoViews[0]
                            if (!useAppProjection) {
                                monoProjMatrix = xr->projMatrices[0];

                                XMVECTOR centerLocalPos = XMVectorSet(
                                    monoPose.position.x, monoPose.position.y, monoPose.position.z, 0.0f);
                                XMVECTOR localOri = XMVectorSet(
                                    rawViews[0].pose.orientation.x, rawViews[0].pose.orientation.y,
                                    rawViews[0].pose.orientation.z, rawViews[0].pose.orientation.w);
                                float monoM2vView = 1.0f;
                                if (inputSnapshot.viewParams.virtualDisplayHeight > 0.0f && xr->displayHeightM > 0.0f)
                                    monoM2vView = inputSnapshot.viewParams.virtualDisplayHeight / xr->displayHeightM;
                                float eyeScale = inputSnapshot.viewParams.perspectiveFactor * monoM2vView / inputSnapshot.viewParams.scaleFactor;
                                XMVECTOR playerOri = XMQuaternionRotationRollPitchYaw(
                                    inputSnapshot.pitch, inputSnapshot.yaw, 0);
                                XMVECTOR playerPos = XMVectorSet(
                                    inputSnapshot.cameraPosX, inputSnapshot.cameraPosY,
                                    inputSnapshot.cameraPosZ, 0.0f);
                                XMVECTOR worldPos = XMVector3Rotate(centerLocalPos * eyeScale, playerOri) + playerPos;
                                XMVECTOR worldOri = XMQuaternionMultiply(localOri, playerOri);
                                XMMATRIX rot = XMMatrixTranspose(XMMatrixRotationQuaternion(worldOri));
                                XMFLOAT3 wp;
                                XMStoreFloat3(&wp, worldPos);
                                monoViewMatrix = XMMatrixTranslation(-wp.x, -wp.y, -wp.z) * rot;
                            }
                        }

                        uint32_t imageIndex;
                        LOG_INFO("[FRAME] AcquireSwapchainImage...");
                        if (AcquireSwapchainImage(*xr, imageIndex)) {
                            LOG_INFO("[FRAME] Acquired image %u", imageIndex);

                            // Build per-eye render params for single-pass rendering
                            std::vector<EyeRenderParams> eyeParams(eyeCount);
                            for (int eye = 0; eye < eyeCount; eye++) {
                                // Tile-aware viewport: place each view in the correct tile position
                                uint32_t tileX = monoMode ? 0 : (eye % tileColumns);
                                uint32_t tileY = monoMode ? 0 : (eye / tileColumns);
                                uint32_t vpX = tileX * renderW;
                                uint32_t vpY = tileY * renderH;
                                eyeParams[eye].viewportX = vpX;
                                eyeParams[eye].viewportY = vpY;
                                eyeParams[eye].width = renderW;
                                eyeParams[eye].height = renderH;
                                if (useAppProjection) {
                                    int vi = monoMode ? 0 : eye;
                                    eyeParams[eye].viewMatrix = ColumnMajorToXMMatrix(stereoViews[vi].view_matrix);
                                    eyeParams[eye].projMatrix = ColumnMajorToXMMatrix(stereoViews[vi].projection_matrix);
                                } else if (monoMode) {
                                    eyeParams[eye].viewMatrix = monoViewMatrix;
                                    eyeParams[eye].projMatrix = monoProjMatrix;
                                } else {
                                    int vi = eye < (int)viewCount ? eye : 0;
                                    eyeParams[eye].viewMatrix = xr->viewMatrices[vi];
                                    eyeParams[eye].projMatrix = xr->projMatrices[vi];
                                }
                            }

                            RenderScene(*renderer, imageIndex,
                                xr->swapchain.width, xr->swapchain.height,
                                eyeParams.data(), eyeCount,
                                useAppProjection ? 1.0f : inputSnapshot.viewParams.scaleFactor);

                            for (int eye = 0; eye < eyeCount; eye++) {
                                uint32_t tileX = monoMode ? 0 : (eye % tileColumns);
                                uint32_t tileY = monoMode ? 0 : (eye / tileColumns);
                                uint32_t vpX = tileX * renderW;
                                uint32_t vpY = tileY * renderH;
                                projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                                projectionViews[eye].subImage.swapchain = xr->swapchain.swapchain;
                                projectionViews[eye].subImage.imageRect.offset = {
                                    (int32_t)vpX, (int32_t)vpY};
                                projectionViews[eye].subImage.imageRect.extent = {
                                    (int32_t)renderW,
                                    (int32_t)renderH
                                };
                                projectionViews[eye].subImage.imageArrayIndex = 0;
                                projectionViews[eye].pose = monoMode ? monoPose : rawViews[eye < (int)viewCount ? eye : 0].pose;
                                projectionViews[eye].fov = useAppProjection ?
                                    stereoViews[monoMode ? 0 : eye].fov :
                                    (monoMode ? rawViews[0].fov : rawViews[eye < (int)viewCount ? eye : 0].fov);
                            }

                            // 'I' key: snapshot the multi-view atlas. Skipped
                            // for mono (1×1) layouts.
                            if (inputSnapshot.captureAtlasRequested) {
                                {
                                    std::lock_guard<std::mutex> lk(g_inputMutex);
                                    g_inputState.captureAtlasRequested = false;
                                }
                                if (!monoMode && (tileColumns > 1 || tileRows > 1) &&
                                    g_vkSwapchainImages != nullptr) {
                                    uint32_t atlasW = tileColumns * renderW;
                                    uint32_t atlasH = tileRows * renderH;
                                    if (atlasW <= xr->swapchain.width && atlasH <= xr->swapchain.height) {
                                        std::string outPath = dxr_capture::MakeCapturePath(
                                            APP_NAME, tileColumns, tileRows);
                                        bool ok = dxr_capture::CaptureAtlasRegionVk(
                                            renderer->device, g_vkPhysDevice,
                                            renderer->graphicsQueue, renderer->commandPool,
                                            (*g_vkSwapchainImages)[imageIndex].image,
                                            (int)g_vkColorFormat,
                                            xr->swapchain.width, xr->swapchain.height,
                                            0, 0, atlasW, atlasH, outPath);
                                        if (ok) {
                                            LOG_INFO("Captured atlas %ux%u -> %s",
                                                     atlasW, atlasH, outPath.c_str());
                                            dxr_capture::PostFlashRequest(hwnd);
                                        }
                                    }
                                } else {
                                    LOG_INFO("Capture skipped: need 3D mode with cols/rows > 1");
                                }
                            }

                            LOG_INFO("[FRAME] ReleaseSwapchainImage...");
                            ReleaseSwapchainImage(*xr);
                            LOG_INFO("[FRAME] Released");
                        } else {
                            LOG_WARN("[FRAME] AcquireSwapchainImage FAILED");
                            rendered = false;
                        }

                        // Render HUD to window-space layer swapchain
                        if (rendered && inputSnapshot.hudVisible && hud && xr->hasHudSwapchain && hudSwapchainImages) {
                            LOG_INFO("[FRAME] HUD: Acquiring swapchain image...");
                            uint32_t hudImageIndex;
                            if (AcquireHudSwapchainImage(*xr, hudImageIndex)) {
                                LOG_INFO("[FRAME] HUD: Acquired image %u", hudImageIndex);
                                std::wstring sessionText(xr->systemName, xr->systemName + strlen(xr->systemName));
                                sessionText += L"\nSession: ";
                                sessionText += FormatSessionState((int)xr->sessionState);
                                std::wstring modeText = xr->hasWin32WindowBindingExt ?
                                    L"XR_EXT_win32_window_binding: ACTIVE (Vulkan)" :
                                    L"XR_EXT_win32_window_binding: NOT AVAILABLE (Vulkan)";
                                uint32_t dispRenderW, dispRenderH;
                                if (monoMode) {
                                    dispRenderW = windowW;
                                    dispRenderH = windowH;
                                    if (dispRenderW > xr->swapchain.width) dispRenderW = xr->swapchain.width;
                                    if (dispRenderH > xr->swapchain.height) dispRenderH = xr->swapchain.height;
                                } else {
                                    dispRenderW = (uint32_t)(windowW * xr->recommendedViewScaleX);
                                    dispRenderH = (uint32_t)(windowH * xr->recommendedViewScaleY);
                                    if (dispRenderW > maxTileW) dispRenderW = maxTileW;
                                    if (dispRenderH > maxTileH) dispRenderH = maxTileH;
                                }
                                std::wstring perfText = FormatPerformanceInfo(perfStats.fps, perfStats.frameTimeMs,
                                    dispRenderW, dispRenderH,
                                    windowW, windowH);
                                std::wstring dispText = FormatDisplayInfo(xr->displayWidthM, xr->displayHeightM,
                                    xr->nominalViewerX, xr->nominalViewerY, xr->nominalViewerZ);
                                dispText += L"\n" + FormatScaleInfo(xr->recommendedViewScaleX, xr->recommendedViewScaleY);
                                dispText += L"\n" + FormatMode(inputSnapshot.currentRenderingMode, xr->pfnRequestDisplayRenderingModeEXT != nullptr,
                                    (xr->renderingModeCount > 0 && inputSnapshot.currentRenderingMode < xr->renderingModeCount) ? xr->renderingModeNames[inputSnapshot.currentRenderingMode] : nullptr,
                                    xr->renderingModeCount,
                                    xr->renderingModeCount > 0 ? xr->renderingModeDisplay3D[inputSnapshot.currentRenderingMode] : true);
                                std::wstring eyeText = FormatEyeTrackingInfo(
                                    xr->eyePositions, (uint32_t)eyeCount,
                                    xr->eyeTrackingActive, xr->isEyeTracking,
                                    xr->activeEyeTrackingMode, xr->supportedEyeTrackingModes);

                                float fwdX = -sinf(inputSnapshot.yaw) * cosf(inputSnapshot.pitch);
                                float fwdY =  sinf(inputSnapshot.pitch);
                                float fwdZ = -cosf(inputSnapshot.yaw) * cosf(inputSnapshot.pitch);
                                std::wstring cameraText = FormatCameraInfo(
                                    inputSnapshot.cameraPosX, inputSnapshot.cameraPosY, inputSnapshot.cameraPosZ,
                                    fwdX, fwdY, fwdZ);
                                float hudM2v = 1.0f;
                                if (inputSnapshot.viewParams.virtualDisplayHeight > 0.0f && xr->displayHeightM > 0.0f)
                                    hudM2v = inputSnapshot.viewParams.virtualDisplayHeight / xr->displayHeightM;
                                std::wstring stereoText = FormatViewParams(
                                    inputSnapshot.viewParams.ipdFactor, inputSnapshot.viewParams.parallaxFactor,
                                    inputSnapshot.viewParams.perspectiveFactor, inputSnapshot.viewParams.scaleFactor);
                                {
                                    wchar_t vhBuf[64];
                                    swprintf(vhBuf, 64, L"\nvHeight: %.3f  m2v: %.3f",
                                        inputSnapshot.viewParams.virtualDisplayHeight, hudM2v);
                                    stereoText += vhBuf;
                                }
                                std::wstring helpText = FormatHelpText(xr->pfnRequestDisplayRenderingModeEXT != nullptr, false, xr->renderingModeCount);

                                uint32_t srcRowPitch = 0;
                                const void* pixels = RenderHudAndMap(*hud, &srcRowPitch, sessionText, modeText, perfText, dispText, eyeText,
                                    cameraText, stereoText, helpText);
                                if (pixels) {
                                    const uint8_t* src = (const uint8_t*)pixels;
                                    uint8_t* dst = (uint8_t*)hudStagingMapped;
                                    for (uint32_t row = 0; row < hudHeight; row++) {
                                        memcpy(dst + row * hudWidth * 4, src + row * srcRowPitch, hudWidth * 4);
                                    }
                                    UnmapHud(*hud);
                                }

                                // Copy staging buffer to HUD swapchain image
                                VkCommandBufferAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
                                allocInfo.commandPool = hudCmdPool;
                                allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                                allocInfo.commandBufferCount = 1;

                                VkCommandBuffer cmdBuf;
                                vkAllocateCommandBuffers(renderer->device, &allocInfo, &cmdBuf);

                                VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                                beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                                vkBeginCommandBuffer(cmdBuf, &beginInfo);

                                VkImage hudImg = (*hudSwapchainImages)[hudImageIndex].image;

                                // Barrier: UNDEFINED -> TRANSFER_DST
                                VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                                barrier.srcAccessMask = 0;
                                barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                                barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                                barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                                barrier.image = hudImg;
                                barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                                vkCmdPipelineBarrier(cmdBuf,
                                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    0, 0, nullptr, 0, nullptr, 1, &barrier);

                                // Copy buffer to image
                                VkBufferImageCopy region = {};
                                region.bufferRowLength = hudWidth;
                                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                                region.imageOffset = {0, 0, 0};
                                region.imageExtent = {hudWidth, hudHeight, 1};
                                vkCmdCopyBufferToImage(cmdBuf, hudStagingBuffer, hudImg,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

                                // Barrier: TRANSFER_DST -> COLOR_ATTACHMENT
                                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                                barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                                barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                                barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                                vkCmdPipelineBarrier(cmdBuf,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                    0, 0, nullptr, 0, nullptr, 1, &barrier);

                                vkEndCommandBuffer(cmdBuf);

                                VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
                                submitInfo.commandBufferCount = 1;
                                submitInfo.pCommandBuffers = &cmdBuf;
                                LOG_INFO("[FRAME] HUD: Submitting GPU commands...");
                                vkQueueSubmit(renderer->graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
                                LOG_INFO("[FRAME] HUD: vkQueueWaitIdle...");
                                vkQueueWaitIdle(renderer->graphicsQueue);
                                LOG_INFO("[FRAME] HUD: GPU done");

                                vkFreeCommandBuffers(renderer->device, hudCmdPool, 1, &cmdBuf);

                                LOG_INFO("[FRAME] HUD: Releasing swapchain image...");
                                ReleaseHudSwapchainImage(*xr);
                                LOG_INFO("[FRAME] HUD: Released");
                                hudSubmitted = true;
                            }
                        }
                    }
                }

                // viewCount: 1 for mono (2D mode), 2 for stereo (3D mode)
                uint32_t submitViewCount = (xr->renderingModeCount > 0 && inputSnapshot.currentRenderingMode < xr->renderingModeCount) ? xr->renderingModeViewCounts[inputSnapshot.currentRenderingMode] : 2;
                // When transparent_bg is on, ask the runtime to honor the cube's
                // per-pixel alpha when blending the projection layer. Pre-#213
                // apps default to 0 (no source-alpha blend = treat as opaque).
                XrCompositionLayerFlags projLayerFlags = TransparentBackgroundEnabled()
                    ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
                    : 0;

                if (rendered && hudSubmitted) {
                    LOG_INFO("[FRAME] EndFrameWithWindowSpaceHud (rendered+hud)...");
                    float hudAR = (float)hudWidth / (float)hudHeight;
                    float windowAR = (windowW > 0 && windowH > 0) ? (float)windowW / (float)windowH : 1.0f;
                    float fracW = HUD_WIDTH_FRACTION;
                    float fracH = fracW * windowAR / hudAR;
                    if (fracH > 1.0f) { fracH = 1.0f; fracW = hudAR / windowAR; }
                    EndFrameWithWindowSpaceHud(*xr, frameState.predictedDisplayTime, projectionViews.data(),
                        0.0f, 0.0f, fracW, fracH, 0.0f, submitViewCount,
                        0, 0, -1, -1, projLayerFlags);
                    LOG_INFO("[FRAME] EndFrameWithWindowSpaceHud done");
                } else if (rendered) {
                    LOG_INFO("[FRAME] EndFrame (rendered, no hud)...");
                    EndFrame(*xr, frameState.predictedDisplayTime, projectionViews.data(),
                             submitViewCount, projLayerFlags);
                    LOG_INFO("[FRAME] EndFrame done");
                } else {
                    LOG_INFO("[FRAME] EndFrame (empty frame, rendered=%d shouldRender=%d)...",
                        rendered, frameState.shouldRender);
                    // shouldRender was false or rendering failed - submit empty frame
                    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
                    endInfo.displayTime = frameState.predictedDisplayTime;
                    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                    endInfo.layerCount = 0;
                    endInfo.layers = nullptr;
                    xrEndFrame(xr->session, &endInfo);
                    LOG_INFO("[FRAME] EndFrame (empty) done");
                }
            } else {
                LOG_WARN("[FRAME] BeginFrame FAILED");
            }
        } else {
            Sleep(100);
        }
    }

    if (xr->exitRequested && g_running.load()) {
        PostMessage(hwnd, WM_CLOSE, 0, 0);
    }

    LOG_INFO("[RenderThread] Exiting");
}

// Global crash handler to log unhandled exceptions before process dies
static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* exInfo) {
    const char* excName = "UNKNOWN";
    switch (exInfo->ExceptionRecord->ExceptionCode) {
        case EXCEPTION_ACCESS_VIOLATION:      excName = "ACCESS_VIOLATION"; break;
        case EXCEPTION_STACK_OVERFLOW:        excName = "STACK_OVERFLOW"; break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    excName = "INT_DIVIDE_BY_ZERO"; break;
        case EXCEPTION_ILLEGAL_INSTRUCTION:   excName = "ILLEGAL_INSTRUCTION"; break;
        case EXCEPTION_IN_PAGE_ERROR:         excName = "IN_PAGE_ERROR"; break;
        case EXCEPTION_GUARD_PAGE:            excName = "GUARD_PAGE"; break;
    }
    LOG_ERROR("!!! UNHANDLED EXCEPTION: %s (0x%08X) at address 0x%p !!!",
        excName, exInfo->ExceptionRecord->ExceptionCode,
        exInfo->ExceptionRecord->ExceptionAddress);
    if (exInfo->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        exInfo->ExceptionRecord->NumberParameters >= 2) {
        LOG_ERROR("    %s address 0x%p",
            exInfo->ExceptionRecord->ExceptionInformation[0] == 0 ? "Reading" : "Writing",
            (void*)exInfo->ExceptionRecord->ExceptionInformation[1]);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    SetUnhandledExceptionFilter(CrashHandler);

    if (!InitializeLogging(APP_NAME)) {
        MessageBox(nullptr, L"Failed to initialize logging", L"Warning", MB_OK | MB_ICONWARNING);
    }

    LOG_INFO("=== SR Cube OpenXR Ext Vulkan Application ===");

    HWND hwnd = CreateAppWindow(hInstance, g_windowWidth, g_windowHeight);
    if (!hwnd) {
        LOG_ERROR("Failed to create window");
        ShutdownLogging();
        return 1;
    }

    // Add DisplayXR to DLL search path
    {
        HKEY hKey;
        char installPath[MAX_PATH] = {0};
        DWORD pathSize = sizeof(installPath);
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\DisplayXR\\Runtime", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            if (RegQueryValueExA(hKey, "InstallPath", nullptr, nullptr, (LPBYTE)installPath, &pathSize) == ERROR_SUCCESS) {
                LOG_INFO("Adding DisplayXR install path to DLL search: %s", installPath);
                SetDllDirectoryA(installPath);
            }
            RegCloseKey(hKey);
        }
    }

    // Initialize OpenXR (discovers Vulkan extension availability)
    XrSessionManager xr = {};
    g_xr = &xr;
    if (!InitializeOpenXR(xr)) {
        LOG_ERROR("OpenXR initialization failed");
        g_xr = nullptr;
        ShutdownLogging();
        return 1;
    }

    // Get Vulkan graphics requirements (must be called before creating Vulkan instance per spec)
    if (!GetVulkanGraphicsRequirements(xr)) {
        LOG_ERROR("Failed to get Vulkan graphics requirements");
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create Vulkan instance with required extensions from the runtime
    VkInstance vkInstance = VK_NULL_HANDLE;
    if (!CreateVulkanInstance(xr, vkInstance)) {
        LOG_ERROR("Vulkan instance creation failed");
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Get physical device selected by the runtime
    VkPhysicalDevice physDevice = VK_NULL_HANDLE;
    if (!GetVulkanPhysicalDevice(xr, vkInstance, physDevice)) {
        LOG_ERROR("Failed to get Vulkan physical device");
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Get required device extensions
    std::vector<const char*> deviceExtensions;
    std::vector<std::string> extensionStorage;
    if (!GetVulkanDeviceExtensions(xr, vkInstance, physDevice, deviceExtensions, extensionStorage)) {
        LOG_ERROR("Failed to get Vulkan device extensions");
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Find graphics queue family
    uint32_t queueFamilyIndex = 0;
    if (!FindGraphicsQueueFamily(physDevice, queueFamilyIndex)) {
        LOG_ERROR("No graphics queue family found");
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create logical device
    VkDevice vkDevice = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    if (!CreateVulkanDevice(physDevice, queueFamilyIndex, deviceExtensions, vkDevice, graphicsQueue)) {
        LOG_ERROR("Vulkan device creation failed");
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create session
    if (!CreateSession(xr, vkInstance, physDevice, vkDevice, queueFamilyIndex, 0, hwnd)) {
        LOG_ERROR("OpenXR session creation failed");
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    if (!CreateSpaces(xr)) {
        LOG_ERROR("Reference space creation failed");
        CleanupOpenXR(xr);
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        ShutdownLogging();
        return 1;
    }

    if (!CreateSwapchain(xr)) {
        LOG_ERROR("Swapchain creation failed");
        CleanupOpenXR(xr);
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        ShutdownLogging();
        return 1;
    }

    // Enumerate Vulkan swapchain images (single SBS swapchain)
    std::vector<XrSwapchainImageVulkanKHR> swapchainImages;
    {
        uint32_t count = xr.swapchain.imageCount;
        swapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)swapchainImages.data());
        LOG_INFO("Enumerated %u Vulkan swapchain images", count);
    }

    // Initialize Vulkan renderer
    VkFormat colorFormat = (VkFormat)xr.swapchain.format;
    g_vkPhysDevice = physDevice;
    g_vkSwapchainImages = &swapchainImages;
    g_vkColorFormat = colorFormat;
    VkRenderer vkRenderer = {};
    if (!InitializeVkRenderer(vkRenderer, vkDevice, physDevice, graphicsQueue, queueFamilyIndex, colorFormat)) {
        LOG_ERROR("Vulkan renderer initialization failed");
        CleanupOpenXR(xr);
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        ShutdownLogging();
        return 1;
    }

    // Create framebuffers for swapchain images (single SBS swapchain, eye=0 slot)
    {
        uint32_t count = xr.swapchain.imageCount;
        std::vector<VkImage> images(count);
        for (uint32_t i = 0; i < count; i++) {
            images[i] = swapchainImages[i].image;
        }

        if (!CreateSwapchainFramebuffers(vkRenderer, 0, images.data(), count,
            xr.swapchain.width, xr.swapchain.height, colorFormat)) {
            LOG_ERROR("Failed to create framebuffers");
            CleanupVkRenderer(vkRenderer);
            CleanupOpenXR(xr);
            vkDestroyDevice(vkDevice, nullptr);
            vkDestroyInstance(vkInstance, nullptr);
            ShutdownLogging();
            return 1;
        }
    }

    // Initialize HUD renderer for window-space layer overlay
    uint32_t hudWidth = (uint32_t)(xr.swapchain.width * HUD_WIDTH_FRACTION);
    uint32_t hudHeight = (uint32_t)(xr.swapchain.height * HUD_HEIGHT_FRACTION);

    HudRenderer hudRenderer = {};
    // Skip HUD in shell mode — window-space layers crash in VK IPC path
    bool isWorkspaceSession = (getenv("DISPLAYXR_WORKSPACE_SESSION") != nullptr);
    bool hudOk = !isWorkspaceSession && InitializeHudRenderer(hudRenderer, hudWidth, hudHeight);
    if (isWorkspaceSession) {
        LOG_INFO("Shell mode detected - HUD disabled (window-space layers not supported in VK IPC)");
    } else if (!hudOk) {
        LOG_WARN("HUD renderer init failed - HUD will not be displayed");
    }

    // Create HUD swapchain for window-space layer submission
    std::vector<XrSwapchainImageVulkanKHR> hudSwapImages;
    if (hudOk) {
        if (CreateHudSwapchain(xr, hudWidth, hudHeight)) {
            uint32_t count = xr.hudSwapchain.imageCount;
            hudSwapImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
            xrEnumerateSwapchainImages(xr.hudSwapchain.swapchain, count, &count,
                (XrSwapchainImageBaseHeader*)hudSwapImages.data());
            LOG_INFO("HUD swapchain: enumerated %u Vulkan images", count);
        } else {
            LOG_WARN("HUD swapchain creation failed - HUD will not be displayed");
            hudOk = false;
        }
    }

    // Create Vulkan staging buffer (host-visible, persistently mapped) for HUD pixel upload
    VkBuffer hudStagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory hudStagingMemory = VK_NULL_HANDLE;
    void* hudStagingMapped = nullptr;
    VkCommandPool hudCmdPool = VK_NULL_HANDLE;

    if (hudOk) {
        // Staging buffer
        VkBufferCreateInfo bufInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufInfo.size = (VkDeviceSize)hudWidth * hudHeight * 4;
        bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(vkDevice, &bufInfo, nullptr, &hudStagingBuffer) != VK_SUCCESS) {
            LOG_WARN("Failed to create HUD staging buffer");
            hudOk = false;
        }

        if (hudOk) {
            VkMemoryRequirements memReqs;
            vkGetBufferMemoryRequirements(vkDevice, hudStagingBuffer, &memReqs);

            VkPhysicalDeviceMemoryProperties memProps;
            vkGetPhysicalDeviceMemoryProperties(physDevice, &memProps);

            uint32_t memTypeIndex = UINT32_MAX;
            for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
                if ((memReqs.memoryTypeBits & (1 << i)) &&
                    (memProps.memoryTypes[i].propertyFlags &
                        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                    memTypeIndex = i;
                    break;
                }
            }

            if (memTypeIndex == UINT32_MAX) {
                LOG_WARN("No suitable memory type for HUD staging buffer");
                hudOk = false;
            } else {
                VkMemoryAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                allocInfo.allocationSize = memReqs.size;
                allocInfo.memoryTypeIndex = memTypeIndex;
                vkAllocateMemory(vkDevice, &allocInfo, nullptr, &hudStagingMemory);
                vkBindBufferMemory(vkDevice, hudStagingBuffer, hudStagingMemory, 0);
                vkMapMemory(vkDevice, hudStagingMemory, 0, bufInfo.size, 0, &hudStagingMapped);
            }
        }

        // Command pool for HUD copy operations
        if (hudOk) {
            VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            poolInfo.queueFamilyIndex = queueFamilyIndex;
            if (vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &hudCmdPool) != VK_SUCCESS) {
                LOG_WARN("Failed to create HUD command pool");
                hudOk = false;
            }
        }

        if (hudOk) {
            LOG_INFO("HUD Vulkan resources created (%ux%u)", hudWidth, hudHeight);
        }
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    LOG_INFO("");
    LOG_INFO("=== Entering main loop ===");
    LOG_INFO("Controls: WASD=Fly, QE=Up/Down, Mouse=Look, Space/DblClick=Reset, V=Mode, SHIFT+TAB=HUD, F11=Fullscreen, ESC=Quit");
    LOG_INFO("");

    // Set virtual display height (app units). 0.24 = 4x the 0.06m cube height.
    g_inputState.viewParams.virtualDisplayHeight = 0.24f;
    g_inputState.renderingModeCount = xr.renderingModeCount;

    std::thread renderThread(RenderThreadFunc, hwnd, &xr, &vkRenderer,
        hudOk ? &hudRenderer : nullptr, hudWidth, hudHeight,
        hudStagingBuffer, hudStagingMapped, hudCmdPool,
        hudOk ? &hudSwapImages : nullptr);

    MSG msg = {};
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    g_running.store(false);
    LOG_INFO("Main thread: waiting for render thread...");
    renderThread.join();
    LOG_INFO("Main thread: render thread joined");

    LOG_INFO("");
    LOG_INFO("=== Shutting down ===");

    if (hudCmdPool != VK_NULL_HANDLE) vkDestroyCommandPool(vkDevice, hudCmdPool, nullptr);
    if (hudStagingBuffer != VK_NULL_HANDLE) {
        vkUnmapMemory(vkDevice, hudStagingMemory);
        vkDestroyBuffer(vkDevice, hudStagingBuffer, nullptr);
    }
    if (hudStagingMemory != VK_NULL_HANDLE) vkFreeMemory(vkDevice, hudStagingMemory, nullptr);
    if (hudOk) CleanupHudRenderer(hudRenderer);

    CleanupVkRenderer(vkRenderer);
    g_xr = nullptr;
    CleanupOpenXR(xr);
    vkDestroyDevice(vkDevice, nullptr);
    vkDestroyInstance(vkInstance, nullptr);

    DestroyWindow(hwnd);
    UnregisterClass(WINDOW_CLASS, hInstance);

    LOG_INFO("Application shutdown complete");
    ShutdownLogging();

    return 0;
}
