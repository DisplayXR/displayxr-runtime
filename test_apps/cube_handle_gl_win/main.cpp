// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  SR Cube OpenXR Ext GL - OpenXR with XR_EXT_win32_window_binding (OpenGL)
 *
 * OpenGL port of cube_handle_d3d11. Projection layer + window-space HUD overlay.
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>

#include "logging.h"
#include "input_handler.h"
#include "xr_session.h"
#include "gl_renderer.h"
#include "hud_renderer.h"
#include "text_overlay.h"
#include "display3d_view.h"
#include "atlas_capture.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <string>
#include <mutex>
#include <thread>
#include <vector>

using namespace DirectX;

static const char* APP_NAME = "cube_handle_gl_win";

// HUD overlay fractions: WIDTH_FRACTION anchors how wide the HUD appears on screen;
// HEIGHT_FRACTION sets the HUD texture pixel height (aspect ratio preserved dynamically).
static const float HUD_WIDTH_FRACTION = 0.30f;
static const float HUD_HEIGHT_FRACTION = 0.50f;

static const wchar_t* WINDOW_CLASS = L"SRCubeOpenXRExtGLClass";
static const wchar_t* WINDOW_TITLE = L"OpenGL Cube \u2014 GL Native Compositor (External Window)";

// Global state (shared between main thread and render thread)
static InputState g_inputState;
static std::mutex g_inputMutex;
static std::atomic<bool> g_running{true};
static XrSessionManager* g_xr = nullptr;
static UINT g_windowWidth = 1280;
static UINT g_windowHeight = 720;

// Set DISPLAYXR_TRANSPARENT_BG=1 in the environment to opt into transparent
// desktop composition: the HWND is created with WS_EX_NOREDIRECTIONBITMAP +
// null background brush, the cube clears RGBA(0,0,0,0), and the win32 window
// binding asks the runtime to chroma-key the woven output back to alpha.
// Pairs with PR #3b (Leia GL chroma-key + DComp present path).
static bool TransparentBackgroundEnabled() {
    static const bool e = []() {
        const char *v = getenv("DISPLAYXR_TRANSPARENT_BG");
        return v != nullptr && *v != '\0' && *v != '0';
    }();
    return e;
}

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
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS | CS_OWNDC;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    // For transparent backgrounds the HWND must NOT have a redirection bitmap
    // and must NOT have a background brush — DComp's per-pixel alpha-out
    // composes through to the desktop only when both are absent.
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

// Create OpenGL context: temp legacy context → load wglCreateContextAttribsARB → core profile 3.3
static bool CreateOpenGLContext(HWND hwnd, HDC& hDC, HGLRC& hGLRC) {
    hDC = GetDC(hwnd);
    if (!hDC) {
        LOG_ERROR("GetDC failed");
        return false;
    }

    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;

    int pixelFormat = ChoosePixelFormat(hDC, &pfd);
    if (!pixelFormat) {
        LOG_ERROR("ChoosePixelFormat failed");
        return false;
    }

    if (!SetPixelFormat(hDC, pixelFormat, &pfd)) {
        LOG_ERROR("SetPixelFormat failed");
        return false;
    }

    // Create temporary legacy context to load WGL extensions
    HGLRC tempRC = wglCreateContext(hDC);
    if (!tempRC) {
        LOG_ERROR("wglCreateContext (temp) failed");
        return false;
    }

    if (!wglMakeCurrent(hDC, tempRC)) {
        LOG_ERROR("wglMakeCurrent (temp) failed");
        wglDeleteContext(tempRC);
        return false;
    }

    // Load wglCreateContextAttribsARB
    PFNWGLCREATECONTEXTATTRIBSARBPROC wglCreateContextAttribsARB =
        (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress("wglCreateContextAttribsARB");

    if (!wglCreateContextAttribsARB) {
        LOG_ERROR("wglCreateContextAttribsARB not available");
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(tempRC);
        return false;
    }

    // Create core profile 3.3 context
    int attribs[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
        WGL_CONTEXT_MINOR_VERSION_ARB, 3,
        WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
        0
    };

    wglMakeCurrent(nullptr, nullptr);

    hGLRC = wglCreateContextAttribsARB(hDC, nullptr, attribs);
    wglDeleteContext(tempRC);

    if (!hGLRC) {
        LOG_ERROR("wglCreateContextAttribsARB failed");
        return false;
    }

    if (!wglMakeCurrent(hDC, hGLRC)) {
        LOG_ERROR("wglMakeCurrent (core profile) failed");
        wglDeleteContext(hGLRC);
        hGLRC = nullptr;
        return false;
    }

    const char* vendor = (const char*)glGetString(GL_VENDOR);
    const char* rendererStr = (const char*)glGetString(GL_RENDERER);
    const char* version = (const char*)glGetString(GL_VERSION);
    LOG_INFO("OpenGL context created:");
    LOG_INFO("  Vendor: %s", vendor ? vendor : "unknown");
    LOG_INFO("  Renderer: %s", rendererStr ? rendererStr : "unknown");
    LOG_INFO("  Version: %s", version ? version : "unknown");

    return true;
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
    HDC hDC,
    HGLRC hGLRC,
    XrSessionManager* xr,
    GLRenderer* renderer,
    std::vector<XrSwapchainImageOpenGLKHR>* swapchainImages,
    HudRenderer* hud,
    uint32_t hudWidth,
    uint32_t hudHeight,
    std::vector<XrSwapchainImageOpenGLKHR>* hudSwapchainImages)
{
    LOG_INFO("[RenderThread] Started");

    // Make the GL context current on this thread
    if (!wglMakeCurrent(hDC, hGLRC)) {
        LOG_ERROR("[RenderThread] wglMakeCurrent failed");
        return;
    }

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
            outputModeChanged = g_inputState.renderingModeChangeRequested;
            g_inputState.renderingModeChangeRequested = false;
            windowW = g_windowWidth;
            windowH = g_windowHeight;
        }

        if (outputModeChanged && xr->pfnRequestDisplayRenderingModeEXT && xr->session != XR_NULL_HANDLE) {
            xr->pfnRequestDisplayRenderingModeEXT(xr->session, inputSnapshot.currentRenderingMode);
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

        UpdateScene(*renderer, perfStats.deltaTime);
        PollEvents(*xr);

        if (xr->sessionRunning) {
            XrFrameState frameState;
            if (BeginFrame(*xr, frameState)) {
                bool rendered = false;
                bool hudSubmitted = false;

                // Get N-view mode info from enumerated rendering modes
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

                if (frameState.shouldRender) {
                    if (LocateViews(*xr, frameState.predictedDisplayTime,
                        inputSnapshot.cameraPosX, inputSnapshot.cameraPosY, inputSnapshot.cameraPosZ,
                        inputSnapshot.yaw, inputSnapshot.pitch,
                        inputSnapshot.viewParams)) {

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

                        // Build N-view raw eye positions
                        std::vector<XrVector3f> rawEyes(eyeCount);
                        for (int v = 0; v < eyeCount; v++) {
                            rawEyes[v] = (v < (int)viewCount) ? rawViews[v].pose.position : rawViews[0].pose.position;
                        }

                        // In mono mode, average all eye positions to center
                        if (monoMode && modeViewCount >= 2) {
                            XrVector3f center = {0, 0, 0};
                            for (uint32_t v = 0; v < modeViewCount; v++) {
                                XrVector3f pos = (v < viewCount) ? rawViews[v].pose.position : rawViews[0].pose.position;
                                center.x += pos.x;
                                center.y += pos.y;
                                center.z += pos.z;
                            }
                            center.x /= modeViewCount;
                            center.y /= modeViewCount;
                            center.z /= modeViewCount;
                            rawEyes[0] = center;
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

                            // Apply window center offset to raw eye positions
                            for (int v = 0; v < eyeCount; v++) {
                                rawEyes[v].x -= eyeOffsetX;
                                rawEyes[v].y -= eyeOffsetY;
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
                        XrFovf monoFov = {};
                        XrPosef monoPose = rawViews[0].pose;
                        if (monoMode) {
                            monoPose.position.x = rawEyes[0].x;
                            monoPose.position.y = rawEyes[0].y;
                            monoPose.position.z = rawEyes[0].z;

                            // When useAppProjection, mono view+proj come from stereoViews[0]
                            if (!useAppProjection) {
                                monoProjMatrix = xr->projMatrices[0];
                                monoFov = rawViews[0].fov;

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
                            for (int eye = 0; eye < eyeCount; eye++) {
                                XMMATRIX viewMatrix, projMatrix;
                                if (useAppProjection) {
                                    int vi = monoMode ? 0 : eye;
                                    viewMatrix = ColumnMajorToXMMatrix(stereoViews[vi].view_matrix);
                                    projMatrix = ColumnMajorToXMMatrix(stereoViews[vi].projection_matrix);
                                } else if (monoMode) {
                                    viewMatrix = monoViewMatrix;
                                    projMatrix = monoProjMatrix;
                                } else {
                                    viewMatrix = xr->viewMatrices[eye];
                                    projMatrix = xr->projMatrices[eye];
                                }

                                // Tile-aware viewport: place each view in the correct tile position
                                uint32_t tileX = monoMode ? 0 : (eye % tileColumns);
                                uint32_t tileY = monoMode ? 0 : (eye / tileColumns);
                                uint32_t vpX = tileX * renderW;
                                uint32_t vpY = tileY * renderH;

                                RenderScene(*renderer, imageIndex,
                                    vpX, vpY,
                                    renderW, renderH,
                                    viewMatrix, projMatrix,
                                    useAppProjection ? 1.0f : inputSnapshot.viewParams.scaleFactor);

                                projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                                projectionViews[eye].subImage.swapchain = xr->swapchain.swapchain;
                                projectionViews[eye].subImage.imageRect.offset = {
                                    (int32_t)vpX, (int32_t)vpY
                                };
                                projectionViews[eye].subImage.imageRect.extent = {
                                    (int32_t)renderW,
                                    (int32_t)renderH
                                };
                                projectionViews[eye].subImage.imageArrayIndex = 0;
                                projectionViews[eye].pose = monoMode ? monoPose : rawViews[eye < (int)viewCount ? eye : 0].pose;
                                projectionViews[eye].fov = useAppProjection ?
                                    stereoViews[monoMode ? 0 : eye].fov :
                                    (monoMode ? monoFov : rawViews[eye < (int)viewCount ? eye : 0].fov);
                            }

                            // 'I' key: snapshot the multi-view atlas. Skipped
                            // for mono (1×1) layouts.
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
                                        bool ok = dxr_capture::CaptureAtlasRegionGL(
                                            (uint32_t)(*swapchainImages)[imageIndex].image,
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
                                    L"XR_EXT_win32_window_binding: ACTIVE (OpenGL)" :
                                    L"XR_EXT_win32_window_binding: NOT AVAILABLE (OpenGL)";
                                uint32_t dispMaxTileW = tileColumns > 0 ? xr->swapchain.width / tileColumns : xr->swapchain.width;
                                uint32_t dispMaxTileH = tileRows > 0 ? xr->swapchain.height / tileRows : xr->swapchain.height;
                                uint32_t dispRenderW, dispRenderH;
                                if (monoMode) {
                                    dispRenderW = windowW;
                                    dispRenderH = windowH;
                                    if (dispRenderW > xr->swapchain.width) dispRenderW = xr->swapchain.width;
                                    if (dispRenderH > xr->swapchain.height) dispRenderH = xr->swapchain.height;
                                } else {
                                    dispRenderW = (uint32_t)(windowW * xr->recommendedViewScaleX);
                                    dispRenderH = (uint32_t)(windowH * xr->recommendedViewScaleY);
                                    if (dispRenderW > dispMaxTileW) dispRenderW = dispMaxTileW;
                                    if (dispRenderH > dispMaxTileH) dispRenderH = dispMaxTileH;
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
                                const void* pixels = RenderHudAndMap(*hud, &srcRowPitch,
                                    sessionText, modeText, perfText, dispText, eyeText,
                                    cameraText, stereoText, helpText);
                                bool uploadOk = false;
                                if (pixels) {
                                    // Clear any prior GL errors
                                    while (glGetError() != GL_NO_ERROR) {}

                                    GLuint hudTexId = (*hudSwapchainImages)[hudImageIndex].image;
                                    glBindTexture(GL_TEXTURE_2D, hudTexId);
                                    // GL native compositor's window-space vertex shader already
                                    // flips UV.y because it expects HUD pixels in top-down
                                    // (D2D/CG bitmap) order — match the macOS GL cube and
                                    // upload straight without a manual row-flip.
                                    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                                    if (srcRowPitch == hudWidth * 4) {
                                        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, hudWidth, hudHeight,
                                            GL_RGBA, GL_UNSIGNED_BYTE, pixels);
                                    } else {
                                        const uint8_t* src = (const uint8_t*)pixels;
                                        for (uint32_t row = 0; row < hudHeight; row++) {
                                            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row, hudWidth, 1,
                                                GL_RGBA, GL_UNSIGNED_BYTE,
                                                src + row * srcRowPitch);
                                        }
                                    }
                                    // Force GL to flush and check for errors
                                    glFlush();
                                    GLenum glErr = glGetError();
                                    glBindTexture(GL_TEXTURE_2D, 0);
                                    UnmapHud(*hud);

                                    if (glErr != GL_NO_ERROR) {
                                        LOG_WARN("[HUD] glTexSubImage2D error 0x%X on HUD swapchain texture %u — skipping HUD layer",
                                            glErr, hudTexId);
                                    } else {
                                        uploadOk = true;
                                    }
                                }

                                bool releaseOk = ReleaseHudSwapchainImage(*xr);
                                if (!releaseOk) {
                                    LOG_WARN("[HUD] ReleaseHudSwapchainImage failed — skipping HUD layer");
                                }
                                hudSubmitted = uploadOk && releaseOk;
                            }
                        }
                    }
                }

                // submitViewCount = eyeCount (mono=1, stereo=N)
                uint32_t submitViewCount = (uint32_t)eyeCount;
                if (hudSubmitted) {
                    LOG_DEBUG("[Frame] Submitting EndFrame with HUD (layerCount=2)");
                    float hudAR = (float)hudWidth / (float)hudHeight;
                    float windowAR = (windowW > 0 && windowH > 0) ? (float)windowW / (float)windowH : 1.0f;
                    float fracW = HUD_WIDTH_FRACTION;
                    float fracH = fracW * windowAR / hudAR;
                    if (fracH > 1.0f) { fracH = 1.0f; fracW = hudAR / windowAR; }
                    XrCompositionLayerFlags projLayerFlags = TransparentBackgroundEnabled()
                        ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
                        : 0;
                    if (!EndFrameWithWindowSpaceHud(*xr, frameState.predictedDisplayTime, projectionViews.data(),
                        0.0f, 0.0f, fracW, fracH, 0.0f, submitViewCount,
                        0, 0, -1, -1, projLayerFlags)) {
                        LOG_WARN("[Frame] EndFrameWithWindowSpaceHud FAILED — disabling HUD for this session");
                        hud = nullptr;  // Disable HUD for subsequent frames
                    }
                    LOG_DEBUG("[Frame] EndFrame with HUD returned");
                } else {
                    XrCompositionLayerFlags projLayerFlags = TransparentBackgroundEnabled()
                        ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
                        : 0;
                    EndFrame(*xr, frameState.predictedDisplayTime, projectionViews.data(),
                             submitViewCount, projLayerFlags);
                }
            }
        } else {
            Sleep(100);
        }
    }

    // Release GL context from this thread
    wglMakeCurrent(nullptr, nullptr);

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

    LOG_INFO("=== SR Cube OpenXR Ext OpenGL Application ===");

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

    // Create OpenGL context (temp → core profile 3.3)
    HDC hDC = nullptr;
    HGLRC hGLRC = nullptr;
    if (!CreateOpenGLContext(hwnd, hDC, hGLRC)) {
        LOG_ERROR("OpenGL context creation failed");
        ShutdownLogging();
        return 1;
    }

    // Load GL function pointers (context must be current)
    if (!LoadGLFunctions()) {
        LOG_ERROR("Failed to load GL function pointers");
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(hGLRC);
        ShutdownLogging();
        return 1;
    }

    // Initialize OpenXR (must happen after GL context is current for requirements query)
    XrSessionManager xr = {};
    g_xr = &xr;
    if (!InitializeOpenXR(xr)) {
        LOG_ERROR("OpenXR initialization failed");
        g_xr = nullptr;
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(hGLRC);
        ShutdownLogging();
        return 1;
    }

    // Get OpenGL graphics requirements
    if (!GetOpenGLGraphicsRequirements(xr)) {
        LOG_ERROR("Failed to get OpenGL graphics requirements");
        CleanupOpenXR(xr);
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(hGLRC);
        ShutdownLogging();
        return 1;
    }

    // Create session with GL context + window handle
    if (!CreateSession(xr, hDC, hGLRC, hwnd)) {
        LOG_ERROR("OpenXR session creation failed");
        CleanupOpenXR(xr);
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(hGLRC);
        ShutdownLogging();
        return 1;
    }

    if (!CreateSpaces(xr)) {
        LOG_ERROR("Reference space creation failed");
        CleanupOpenXR(xr);
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(hGLRC);
        ShutdownLogging();
        return 1;
    }

    if (!CreateSwapchain(xr)) {
        LOG_ERROR("Swapchain creation failed");
        CleanupOpenXR(xr);
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(hGLRC);
        ShutdownLogging();
        return 1;
    }

    // Enumerate OpenGL swapchain images (single SBS swapchain)
    std::vector<XrSwapchainImageOpenGLKHR> swapchainImages;
    {
        uint32_t count = xr.swapchain.imageCount;
        swapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
        xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)swapchainImages.data());
        LOG_INFO("Enumerated %u OpenGL swapchain images", count);
    }

    // Initialize GL renderer (shaders, geometry)
    GLRenderer glRenderer = {};
    if (!InitializeGLRenderer(glRenderer)) {
        LOG_ERROR("GL renderer initialization failed");
        CleanupOpenXR(xr);
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(hGLRC);
        ShutdownLogging();
        return 1;
    }

    // Create FBOs for swapchain images (single SBS swapchain)
    {
        uint32_t count = xr.swapchain.imageCount;
        std::vector<GLuint> textures(count);
        for (uint32_t i = 0; i < count; i++) {
            textures[i] = swapchainImages[i].image;
        }

        if (!CreateSwapchainFBOs(glRenderer, textures.data(), count,
            xr.swapchain.width, xr.swapchain.height)) {
            LOG_ERROR("Failed to create FBOs for swapchain");
            CleanupGLRenderer(glRenderer);
            CleanupOpenXR(xr);
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(hGLRC);
            ShutdownLogging();
            return 1;
        }
    }

    // Initialize HUD renderer for window-space layer overlay
    uint32_t hudWidth = (uint32_t)(xr.swapchain.width * HUD_WIDTH_FRACTION);
    uint32_t hudHeight = (uint32_t)(xr.swapchain.height * HUD_HEIGHT_FRACTION);

    HudRenderer hudRenderer = {};
    bool hudOk = InitializeHudRenderer(hudRenderer, hudWidth, hudHeight);
    if (!hudOk) {
        LOG_WARN("HUD renderer init failed - HUD will not be displayed");
    }

    // Create HUD swapchain for window-space layer submission
    std::vector<XrSwapchainImageOpenGLKHR> hudSwapImages;
    if (hudOk) {
        if (CreateHudSwapchain(xr, hudWidth, hudHeight)) {
            uint32_t count = xr.hudSwapchain.imageCount;
            hudSwapImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
            xrEnumerateSwapchainImages(xr.hudSwapchain.swapchain, count, &count,
                (XrSwapchainImageBaseHeader*)hudSwapImages.data());
            LOG_INFO("HUD swapchain: enumerated %u OpenGL images", count);
        } else {
            LOG_WARN("HUD swapchain creation failed - HUD will not be displayed");
            hudOk = false;
        }
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    LOG_INFO("");
    LOG_INFO("=== Entering main loop ===");
    LOG_INFO("Controls: WASD=Fly, QE=Up/Down, Mouse=Look, Space/DblClick=Reset, V=Mode, SHIFT+TAB=HUD, F11=Fullscreen, ESC=Quit");
    LOG_INFO("");

    // Release GL context from main thread before handing to render thread
    wglMakeCurrent(nullptr, nullptr);

    // Set virtual display height (app units). 0.24 = 4x the 0.06m cube height.
    g_inputState.viewParams.virtualDisplayHeight = 0.24f;
    g_inputState.renderingModeCount = xr.renderingModeCount;

    std::thread renderThread(RenderThreadFunc, hwnd, hDC, hGLRC, &xr, &glRenderer,
        &swapchainImages,
        hudOk ? &hudRenderer : nullptr, hudWidth, hudHeight,
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

    // Re-acquire GL context for cleanup
    wglMakeCurrent(hDC, hGLRC);

    if (hudOk) CleanupHudRenderer(hudRenderer);
    CleanupGLRenderer(glRenderer);
    g_xr = nullptr;
    CleanupOpenXR(xr);

    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(hGLRC);
    ReleaseDC(hwnd, hDC);

    DestroyWindow(hwnd);
    UnregisterClass(WINDOW_CLASS, hInstance);

    LOG_INFO("Application shutdown complete");
    ShutdownLogging();

    return 0;
}
