// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  SR Cube OpenXR Ext D3D12 - OpenXR with XR_EXT_win32_window_binding (D3D12)
 *
 * D3D12 port of cube_handle_d3d11 with window-space HUD overlay.
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>

#include "logging.h"
#include "input_handler.h"
#include "xr_session.h"
#include "d3d12_renderer.h"
#include "hud_renderer.h"
#include "text_overlay.h"
#include "display3d_view.h"
#include "atlas_capture.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <wrl/client.h>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

static const char* APP_NAME = "cube_handle_d3d12_win";

// HUD overlay: WIDTH_FRACTION anchors how wide the HUD appears on screen.
// D3D12 swapchains are imported into the Vulkan native compositor via shared handles.
// Non-power-of-2 dimensions cause a size mismatch between D3D12 and Vulkan memory layouts
// (nvidia bug - see comp_d3d12_client.cpp). Use power-of-2 to avoid this.
static const uint32_t HUD_PIXEL_WIDTH = 512;
static const uint32_t HUD_PIXEL_HEIGHT = 560;
static const float HUD_WIDTH_FRACTION = 0.30f;

static const wchar_t* WINDOW_CLASS = L"SRCubeOpenXRExtD3D12Class";
static const wchar_t* WINDOW_TITLE = L"D3D12 Cube \u2014 D3D12 Native Compositor (External Window)";

// Global state (shared between main thread and render thread)
static InputState g_inputState;
static std::mutex g_inputMutex;
static std::atomic<bool> g_running{true};
static XrSessionManager* g_xr = nullptr;
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

// DISPLAYXR_TRANSPARENT_BG=1 → cube clears RGBA(0,0,0,0), window uses
// WS_EX_NOREDIRECTIONBITMAP + null brush so DComp can show the desktop
// through the cube's transparent regions. Mirrors cube_handle_vk_win and
// cube_handle_d3d11_win.
static bool TransparentBackgroundEnabled() {
    static const bool e = []() {
        const char *v = getenv("DISPLAYXR_TRANSPARENT_BG");
        return v != nullptr && *v != '\0' && *v != '0';
    }();
    return e;
}

static HWND CreateAppWindow(HINSTANCE hInstance, int width, int height) {
    const bool transparent = TransparentBackgroundEnabled();
    LOG_INFO("Creating application window (%dx%d, transparent=%d)", width, height, transparent);

    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
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
    D3D12Renderer* renderer,
    std::vector<XrSwapchainImageD3D12KHR>* swapchainImages,
    int rtvBaseIndex,
    HudRenderer* hud,
    std::vector<XrSwapchainImageD3D12KHR>* hudSwapchainImages,
    ID3D12Resource* hudUploadBuffer,
    uint8_t* hudUploadMapped,
    uint32_t hudUploadRowPitch,
    ID3D12CommandAllocator* hudCmdAllocator,
    ID3D12GraphicsCommandList* hudCmdList,
    ID3D12Fence* hudFence,
    HANDLE hudFenceEvent)
{
    LOG_INFO("[RenderThread] Started");

    PerformanceStats perfStats = {};
    perfStats.lastTime = std::chrono::high_resolution_clock::now();
    UINT64 hudFenceValue = 0;

    while (g_running.load() && !xr->exitRequested) {
        InputState inputSnapshot;
        bool resetRequested = false;
        uint32_t windowW, windowH;
        bool outputModeChanged = false;
        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            inputSnapshot = g_inputState;
            resetRequested = g_inputState.resetViewRequested;
            g_inputState.resetViewRequested = false;
            g_inputState.fullscreenToggleRequested = false;
            g_inputState.eyeTrackingModeToggleRequested = false;
            outputModeChanged = g_inputState.renderingModeChangeRequested;
            g_inputState.renderingModeChangeRequested = false;
            windowW = g_windowWidth;
            windowH = g_windowHeight;
        }

        if (outputModeChanged && xr->pfnRequestDisplayRenderingModeEXT && xr->session != XR_NULL_HANDLE) {
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
            if (BeginFrame(*xr, frameState)) {
                uint32_t modeViewCount = (xr->renderingModeCount > 0 && inputSnapshot.currentRenderingMode < xr->renderingModeCount)
                    ? xr->renderingModeViewCounts[inputSnapshot.currentRenderingMode] : 2;
                uint32_t tileColumns = (xr->renderingModeCount > 0 && inputSnapshot.currentRenderingMode < xr->renderingModeCount)
                    ? xr->renderingModeTileColumns[inputSnapshot.currentRenderingMode] : 2;
                uint32_t tileRows = (xr->renderingModeCount > 0 && inputSnapshot.currentRenderingMode < xr->renderingModeCount)
                    ? xr->renderingModeTileRows[inputSnapshot.currentRenderingMode] : 1;
                bool monoMode = (xr->renderingModeCount > 0 && !xr->renderingModeDisplay3D[inputSnapshot.currentRenderingMode]);
                if (xr->renderingModeCount > 0 && inputSnapshot.currentRenderingMode < xr->renderingModeCount) {
                    xr->recommendedViewScaleX = xr->renderingModeScaleX[inputSnapshot.currentRenderingMode];
                    xr->recommendedViewScaleY = xr->renderingModeScaleY[inputSnapshot.currentRenderingMode];
                }
                int eyeCount = monoMode ? 1 : (int)modeViewCount;
                std::vector<XrCompositionLayerProjectionView> projectionViews(eyeCount, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
                bool rendered = false;
                bool hudSubmitted = false;

                if (frameState.shouldRender) {
                    if (LocateViews(*xr, frameState.predictedDisplayTime,
                        inputSnapshot.cameraPosX, inputSnapshot.cameraPosY, inputSnapshot.cameraPosZ,
                        inputSnapshot.yaw, inputSnapshot.pitch,
                        inputSnapshot.viewParams)) {

                        // Get raw view poses for projection views.
                        XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
                        locateInfo.viewConfigurationType = xr->viewConfigType;
                        locateInfo.displayTime = frameState.predictedDisplayTime;
                        locateInfo.space = xr->localSpace;

                        XrViewState viewState = {XR_TYPE_VIEW_STATE};
                        uint32_t viewCount = 8;
                        XrView rawViews[8];
                        for (uint32_t vi = 0; vi < 8; vi++) rawViews[vi] = {XR_TYPE_VIEW};
                        xrLocateViews(xr->session, &locateInfo, &viewState, 8, &viewCount, rawViews);

                        // Max per-tile capacity from swapchain
                        uint32_t maxTileW = tileColumns > 0 ? xr->swapchain.width / tileColumns : xr->swapchain.width;
                        uint32_t maxTileH = tileRows > 0 ? xr->swapchain.height / tileRows : xr->swapchain.height;

                        // Compute render dims: mono uses full swapchain, stereo uses tile size
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

                            Display3DTunables tunables;
                            tunables.ipd_factor = inputSnapshot.viewParams.ipdFactor;
                            tunables.parallax_factor = inputSnapshot.viewParams.parallaxFactor;
                            tunables.perspective_factor = inputSnapshot.viewParams.perspectiveFactor;
                            tunables.virtual_display_height = inputSnapshot.viewParams.virtualDisplayHeight / inputSnapshot.viewParams.scaleFactor;

                            XrPosef displayPose;
                            XMVECTOR pOri = XMQuaternionRotationRollPitchYaw(
                                inputSnapshot.pitch, inputSnapshot.yaw, 0);
                            XMFLOAT4 q;
                            XMStoreFloat4(&q, pOri);
                            displayPose.orientation = {q.x, q.y, q.z, q.w};
                            displayPose.position = {inputSnapshot.cameraPosX, inputSnapshot.cameraPosY, inputSnapshot.cameraPosZ};

                            XrVector3f nominalViewer = {xr->nominalViewerX, xr->nominalViewerY, xr->nominalViewerZ};
                            Display3DScreen screen = {winW_m, winH_m};

                            display3d_compute_views(
                                rawEyes.data(), eyeCount, &nominalViewer,
                                &screen, &tunables, &displayPose,
                                0.01f, 100.0f, stereoViews.data());
                        }

                        rendered = true;

                        // For mono: compute center eye position and projection
                        XMMATRIX monoViewMatrix, monoProjMatrix;
                        XrPosef monoPose = rawViews[0].pose;
                        if (monoMode) {
                            // Center eye = average of all N views
                            XrVector3f center = {0.0f, 0.0f, 0.0f};
                            for (uint32_t v = 0; v < viewCount; v++) {
                                center.x += rawViews[v].pose.position.x;
                                center.y += rawViews[v].pose.position.y;
                                center.z += rawViews[v].pose.position.z;
                            }
                            if (viewCount > 0) {
                                center.x /= viewCount; center.y /= viewCount; center.z /= viewCount;
                            }
                            monoPose.position = center;

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
                        if (AcquireSwapchainImage(*xr, imageIndex)) {
                            ID3D12Resource* swapchainTexture = (*swapchainImages)[imageIndex].texture;
                            int rtvIdx = rtvBaseIndex + (int)imageIndex;

                            for (int eye = 0; eye < eyeCount; eye++) {
                                // Tile-aware viewport positioning
                                uint32_t tileX = monoMode ? 0 : (eye % tileColumns);
                                uint32_t tileY = monoMode ? 0 : (eye / tileColumns);
                                uint32_t vpX = tileX * renderW;
                                uint32_t vpY = tileY * renderH;

                                XMMATRIX viewMatrix, projMatrix;
                                if (useAppProjection) {
                                    int vi = monoMode ? 0 : eye;
                                    viewMatrix = ColumnMajorToXMMatrix(stereoViews[vi].view_matrix);
                                    projMatrix = ColumnMajorToXMMatrix(stereoViews[vi].projection_matrix);
                                } else if (monoMode) {
                                    viewMatrix = monoViewMatrix;
                                    projMatrix = monoProjMatrix;
                                } else {
                                    int si = eye < 2 ? eye : 0;
                                    viewMatrix = xr->viewMatrices[si];
                                    projMatrix = xr->projMatrices[si];
                                }

                                RenderScene(*renderer, swapchainTexture, rtvIdx,
                                    vpX, vpY,
                                    renderW, renderH,
                                    viewMatrix, projMatrix,
                                    useAppProjection ? 1.0f : inputSnapshot.viewParams.scaleFactor,
                                    eye == 0);  // clear only on first eye

                                projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                                projectionViews[eye].subImage.swapchain = xr->swapchain.swapchain;
                                projectionViews[eye].subImage.imageRect.offset = {
                                    (int32_t)vpX, (int32_t)vpY};
                                projectionViews[eye].subImage.imageRect.extent = {
                                    (int32_t)renderW, (int32_t)renderH};
                                projectionViews[eye].subImage.imageArrayIndex = 0;
                                int rawIdx = (eye < (int)viewCount) ? eye : 0;
                                projectionViews[eye].pose = monoMode ? monoPose : rawViews[rawIdx].pose;
                                projectionViews[eye].fov = useAppProjection ?
                                    stereoViews[monoMode ? 0 : eye].fov :
                                    (monoMode ? rawViews[0].fov : rawViews[rawIdx].fov);
                            }

                            // 'I' key: snapshot the multi-view atlas. Skipped
                            // for mono (1×1) layouts. RenderScene leaves the
                            // texture in D3D12_RESOURCE_STATE_COMMON.
                            if (inputSnapshot.captureAtlasRequested) {
                                {
                                    std::lock_guard<std::mutex> lk(g_inputMutex);
                                    g_inputState.captureAtlasRequested = false;
                                }
                                if (!monoMode && (tileColumns > 1 || tileRows > 1)) {
                                    uint32_t atlasW = tileColumns * renderW;
                                    uint32_t atlasH = tileRows * renderH;
                                    if (atlasW <= xr->swapchain.width && atlasH <= xr->swapchain.height) {
                                        std::string outPath = dxr_capture::MakeCapturePath(
                                            APP_NAME, tileColumns, tileRows);
                                        bool ok = dxr_capture::CaptureAtlasRegionD3D12(
                                            renderer->device.Get(),
                                            renderer->commandQueue.Get(),
                                            swapchainTexture,
                                            xr->swapchain.width, xr->swapchain.height,
                                            (int)D3D12_RESOURCE_STATE_COMMON,
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

                            ReleaseSwapchainImage(*xr);
                        } else {
                            rendered = false;
                        }

                        // Render HUD to window-space layer swapchain
                        if (rendered && inputSnapshot.hudVisible && hud && xr->hasHudSwapchain && hudSwapchainImages) {
                            uint32_t hudImageIndex;
                            if (AcquireHudSwapchainImage(*xr, hudImageIndex)) {
                                std::wstring sessionText(xr->systemName, xr->systemName + strlen(xr->systemName));
                                sessionText += L"\nSession: ";
                                sessionText += FormatSessionState((int)xr->sessionState);
                                std::wstring modeText = xr->hasWin32WindowBindingExt ?
                                    L"XR_EXT_win32_window_binding: ACTIVE (D3D12)" :
                                    L"XR_EXT_win32_window_binding: NOT AVAILABLE (D3D12)";
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
                                    dispRenderW, dispRenderH, windowW, windowH);
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
                                    // Copy pixels row-by-row to D3D12 upload buffer (256-byte aligned rows)
                                    const uint8_t* src = (const uint8_t*)pixels;
                                    for (uint32_t row = 0; row < HUD_PIXEL_HEIGHT; row++) {
                                        memcpy(hudUploadMapped + row * hudUploadRowPitch,
                                            src + row * srcRowPitch,
                                            HUD_PIXEL_WIDTH * 4);
                                    }
                                    UnmapHud(*hud);

                                    // Record D3D12 commands: copy upload buffer to HUD swapchain texture
                                    ID3D12Resource* hudTex = (*hudSwapchainImages)[hudImageIndex].texture;

                                    hudCmdAllocator->Reset();
                                    hudCmdList->Reset(hudCmdAllocator, nullptr);

                                    // Barrier: COMMON -> COPY_DEST
                                    D3D12_RESOURCE_BARRIER barrier = {};
                                    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                                    barrier.Transition.pResource = hudTex;
                                    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                                    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                                    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                                    hudCmdList->ResourceBarrier(1, &barrier);

                                    // CopyTextureRegion from upload buffer
                                    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
                                    srcLoc.pResource = hudUploadBuffer;
                                    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                                    srcLoc.PlacedFootprint.Offset = 0;
                                    srcLoc.PlacedFootprint.Footprint.Format = (DXGI_FORMAT)xr->hudSwapchain.format;
                                    srcLoc.PlacedFootprint.Footprint.Width = HUD_PIXEL_WIDTH;
                                    srcLoc.PlacedFootprint.Footprint.Height = HUD_PIXEL_HEIGHT;
                                    srcLoc.PlacedFootprint.Footprint.Depth = 1;
                                    srcLoc.PlacedFootprint.Footprint.RowPitch = hudUploadRowPitch;

                                    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
                                    dstLoc.pResource = hudTex;
                                    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                                    dstLoc.SubresourceIndex = 0;

                                    hudCmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

                                    // Barrier: COPY_DEST -> COMMON
                                    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                                    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
                                    hudCmdList->ResourceBarrier(1, &barrier);

                                    hudCmdList->Close();

                                    // Execute and wait
                                    ID3D12CommandList* lists[] = { hudCmdList };
                                    renderer->commandQueue->ExecuteCommandLists(1, lists);
                                    hudFenceValue++;
                                    renderer->commandQueue->Signal(hudFence, hudFenceValue);
                                    if (hudFence->GetCompletedValue() < hudFenceValue) {
                                        hudFence->SetEventOnCompletion(hudFenceValue, hudFenceEvent);
                                        WaitForSingleObject(hudFenceEvent, INFINITE);
                                    }

                                    hudSubmitted = true;
                                }

                                ReleaseHudSwapchainImage(*xr);
                            }
                        }
                    }
                }

                // End frame: use window-space HUD layer if available
                uint32_t submitViewCount = (xr->renderingModeCount > 0 && inputSnapshot.currentRenderingMode < xr->renderingModeCount) ? xr->renderingModeViewCounts[inputSnapshot.currentRenderingMode] : 2;
                LOG_INFO("[FRAME] EndFrame: rendered=%d hudSubmitted=%d viewCount=%u", rendered, hudSubmitted, submitViewCount);
                if (rendered && hudSubmitted) {
                    float hudAR = (float)HUD_PIXEL_WIDTH / (float)HUD_PIXEL_HEIGHT;
                    float windowAR = (windowW > 0 && windowH > 0) ? (float)windowW / (float)windowH : 1.0f;
                    float fracW = HUD_WIDTH_FRACTION;
                    float fracH = fracW * windowAR / hudAR;
                    if (fracH > 1.0f) { fracH = 1.0f; fracW = hudAR / windowAR; }
                    EndFrameWithWindowSpaceHud(*xr, frameState.predictedDisplayTime, projectionViews.data(),
                        0.0f, 0.0f, fracW, fracH, 0.0f, submitViewCount);
                } else if (rendered) {
                    EndFrame(*xr, frameState.predictedDisplayTime, projectionViews.data(), submitViewCount);
                } else {
                    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
                    endInfo.displayTime = frameState.predictedDisplayTime;
                    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                    endInfo.layerCount = 0;
                    endInfo.layers = nullptr;
                    xrEndFrame(xr->session, &endInfo);
                }
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

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    if (!InitializeLogging(APP_NAME)) {
        MessageBox(nullptr, L"Failed to initialize logging", L"Warning", MB_OK | MB_ICONWARNING);
    }

    LOG_INFO("=== SR Cube OpenXR Ext D3D12 Application ===");

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

    // Initialize OpenXR
    XrSessionManager xr = {};
    g_xr = &xr;
    if (!InitializeOpenXR(xr)) {
        LOG_ERROR("OpenXR initialization failed");
        g_xr = nullptr;
        ShutdownLogging();
        return 1;
    }

    // Get D3D12 graphics requirements
    LUID adapterLuid;
    if (!GetD3D12GraphicsRequirements(xr, &adapterLuid)) {
        LOG_ERROR("Failed to get D3D12 graphics requirements");
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Initialize D3D12
    D3D12Renderer renderer = {};
    if (!InitializeD3D12WithLUID(renderer, adapterLuid)) {
        LOG_ERROR("D3D12 initialization failed");
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create session
    if (!CreateSession(xr, renderer.device.Get(), renderer.commandQueue.Get(), hwnd)) {
        LOG_ERROR("OpenXR session creation failed");
        CleanupD3D12(renderer);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    if (!CreateSpaces(xr)) {
        LOG_ERROR("Reference space creation failed");
        CleanupOpenXR(xr);
        CleanupD3D12(renderer);
        ShutdownLogging();
        return 1;
    }

    if (!CreateSwapchain(xr)) {
        LOG_ERROR("Swapchain creation failed");
        CleanupOpenXR(xr);
        CleanupD3D12(renderer);
        ShutdownLogging();
        return 1;
    }

    // Enumerate D3D12 swapchain images (single SBS swapchain)
    std::vector<XrSwapchainImageD3D12KHR> swapchainImages;
    int rtvBaseIndex = 0;
    {
        uint32_t count = xr.swapchain.imageCount;
        swapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)swapchainImages.data());
        LOG_INFO("Enumerated %u D3D12 swapchain images", count);

        // Collect ID3D12Resource pointers for RTV creation
        std::vector<ID3D12Resource*> textures(count);
        for (uint32_t i = 0; i < count; i++) {
            textures[i] = swapchainImages[i].texture;
        }

        rtvBaseIndex = (int)renderer.rtvCount;
        if (!CreateSwapchainRTVs(renderer, textures.data(), count,
            xr.swapchain.width, xr.swapchain.height,
            (DXGI_FORMAT)xr.swapchain.format)) {
            LOG_ERROR("Failed to create RTVs");
            CleanupOpenXR(xr);
            CleanupD3D12(renderer);
            ShutdownLogging();
            return 1;
        }
    }

    // Initialize HUD renderer (standalone D3D11 device for text rendering)
    HudRenderer hudRenderer = {};
    bool hudOk = InitializeHudRenderer(hudRenderer, HUD_PIXEL_WIDTH, HUD_PIXEL_HEIGHT);
    if (!hudOk) {
        LOG_WARN("HUD renderer init failed - HUD will not be displayed");
    }

    // Create HUD swapchain for window-space layer submission
    std::vector<XrSwapchainImageD3D12KHR> hudSwapImages;
    if (hudOk) {
        if (CreateHudSwapchain(xr, HUD_PIXEL_WIDTH, HUD_PIXEL_HEIGHT)) {
            uint32_t count = xr.hudSwapchain.imageCount;
            hudSwapImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
            xrEnumerateSwapchainImages(xr.hudSwapchain.swapchain, count, &count,
                (XrSwapchainImageBaseHeader*)hudSwapImages.data());
            LOG_INFO("HUD swapchain: enumerated %u D3D12 images", count);
        } else {
            LOG_WARN("HUD swapchain creation failed - HUD will not be displayed");
            hudOk = false;
        }
    }

    // Create D3D12 upload resources for HUD pixel transfer
    ComPtr<ID3D12Resource> hudUploadBuffer;
    uint8_t* hudUploadMapped = nullptr;
    ComPtr<ID3D12CommandAllocator> hudCmdAllocator;
    ComPtr<ID3D12GraphicsCommandList> hudCmdList;
    ComPtr<ID3D12Fence> hudFence;
    HANDLE hudFenceEvent = nullptr;
    // Row pitch must be aligned to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256 bytes)
    uint32_t hudUploadRowPitch = (HUD_PIXEL_WIDTH * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
        & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);

    if (hudOk) {
        // Upload buffer (UPLOAD heap, persistently mapped)
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bufDesc = {};
        bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufDesc.Width = (UINT64)hudUploadRowPitch * HUD_PIXEL_HEIGHT;
        bufDesc.Height = 1;
        bufDesc.DepthOrArraySize = 1;
        bufDesc.MipLevels = 1;
        bufDesc.SampleDesc.Count = 1;
        bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        HRESULT hr = renderer.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&hudUploadBuffer));
        if (FAILED(hr)) {
            LOG_WARN("Failed to create HUD upload buffer: 0x%08X", hr);
            hudOk = false;
        }

        if (hudOk) {
            D3D12_RANGE readRange = {0, 0}; // no CPU reads
            hr = hudUploadBuffer->Map(0, &readRange, (void**)&hudUploadMapped);
            if (FAILED(hr)) {
                LOG_WARN("Failed to map HUD upload buffer: 0x%08X", hr);
                hudOk = false;
            }
        }

        if (hudOk) {
            hr = renderer.device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&hudCmdAllocator));
            if (FAILED(hr)) {
                LOG_WARN("Failed to create HUD command allocator: 0x%08X", hr);
                hudOk = false;
            }
        }

        if (hudOk) {
            hr = renderer.device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT, hudCmdAllocator.Get(), nullptr,
                IID_PPV_ARGS(&hudCmdList));
            if (FAILED(hr)) {
                LOG_WARN("Failed to create HUD command list: 0x%08X", hr);
                hudOk = false;
            } else {
                hudCmdList->Close(); // start in closed state
            }
        }

        if (hudOk) {
            hr = renderer.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&hudFence));
            if (FAILED(hr)) {
                LOG_WARN("Failed to create HUD fence: 0x%08X", hr);
                hudOk = false;
            } else {
                hudFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            }
        }

        if (hudOk) {
            LOG_INFO("HUD D3D12 resources created (%ux%u, row pitch %u)", HUD_PIXEL_WIDTH, HUD_PIXEL_HEIGHT, hudUploadRowPitch);
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

    std::thread renderThread(RenderThreadFunc, hwnd, &xr, &renderer,
        &swapchainImages, rtvBaseIndex,
        hudOk ? &hudRenderer : nullptr,
        hudOk ? &hudSwapImages : nullptr,
        hudUploadBuffer.Get(), hudUploadMapped, hudUploadRowPitch,
        hudCmdAllocator.Get(), hudCmdList.Get(), hudFence.Get(), hudFenceEvent);

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

    // Clean up HUD resources
    if (hudFenceEvent) CloseHandle(hudFenceEvent);
    hudFence.Reset();
    hudCmdList.Reset();
    hudCmdAllocator.Reset();
    if (hudUploadMapped && hudUploadBuffer) {
        hudUploadBuffer->Unmap(0, nullptr);
        hudUploadMapped = nullptr;
    }
    hudUploadBuffer.Reset();
    if (hudOk) CleanupHudRenderer(hudRenderer);

    g_xr = nullptr;
    CleanupOpenXR(xr);
    CleanupD3D12(renderer);

    DestroyWindow(hwnd);
    UnregisterClass(WINDOW_CLASS, hInstance);

    LOG_INFO("Application shutdown complete");
    ShutdownLogging();

    return 0;
}
