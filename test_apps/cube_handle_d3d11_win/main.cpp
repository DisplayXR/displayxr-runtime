// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  SR Cube OpenXR Ext - OpenXR with XR_EXT_win32_window_binding extension
 *
 * This application demonstrates OpenXR with the XR_EXT_win32_window_binding extension.
 * The application creates and controls its own window for rendering.
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <wrl/client.h>

#include "logging.h"
#include "input_handler.h"
#include "d3d11_renderer.h"
#include "text_overlay.h"
#include "hud_renderer.h"
#include "xr_session.h"
#include "display3d_view.h"
#include "camera3d_view.h"
#include "atlas_capture.h"

#include <chrono>
#include <cmath>
#include <string>
#include <sstream>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

// Application name for logging
static const char* APP_NAME = "cube_handle_d3d11_win";

// Window settings
static const wchar_t* WINDOW_CLASS = L"SRCubeOpenXRExtClass";
static const wchar_t* WINDOW_TITLE = L"D3D11 Cube \u2014 D3D11 Native Compositor (External Window)";

// Global state (single-threaded — all accessed from the main thread only)
static InputState g_inputState;
static bool g_running = true;
static XrSessionManager* g_xr = nullptr;
static UINT g_windowWidth = 1280;
static UINT g_windowHeight = 720;
static bool g_inSizeMove = false;  // True while user is dragging/resizing the window
static const uint32_t HUD_PIXEL_WIDTH = 380;
static const uint32_t HUD_PIXEL_HEIGHT = 470;
static const float CAMERA_HALF_TAN_VFOV = 0.32491969623f; // tan(18°) → 36° vFOV
static const float HUD_WIDTH_FRACTION = 0.30f;

// Fullscreen state
static bool g_fullscreen = false;
static RECT g_savedWindowRect = {};
static DWORD g_savedWindowStyle = 0;

// Forward declaration — defined after PerformanceStats
struct RenderState;
static RenderState* g_renderState = nullptr;
static void RenderOneFrame(RenderState& rs);

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

// Window procedure (runs on main thread — single-threaded, no locking needed)
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    UpdateInputState(g_inputState, msg, wParam, lParam);

    switch (msg) {
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        return 0;

    case WM_LBUTTONUP:
        ReleaseCapture();
        return 0;

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            g_windowWidth = LOWORD(lParam);
            g_windowHeight = HIWORD(lParam);
        }
        return 0;

    case WM_ENTERSIZEMOVE:
        g_inSizeMove = true;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_EXITSIZEMOVE:
        g_inSizeMove = false;
        return 0;

    case WM_PAINT:
        // During drag/resize, DefWindowProc runs a modal loop that blocks our
        // main message pump.  By leaving the window invalidated (no
        // BeginPaint/EndPaint), Windows keeps sending WM_PAINT inside that
        // modal loop, giving us a chance to keep rendering frames.
        if (g_inSizeMove && g_renderState != nullptr) {
            RenderOneFrame(*g_renderState);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;

    case WM_CLOSE:
        // Graceful shutdown: ask OpenXR to end the session so the state machine
        // runs STOPPING -> xrEndSession -> EXITING -> exitRequested before cleanup.
        if (g_xr && g_xr->session != XR_NULL_HANDLE && g_xr->sessionRunning) {
            xrRequestExitSession(g_xr->session);
            return 0;
        }
        g_running = false;
        PostQuitMessage(0);
        return 0;

    case WM_SYSKEYDOWN:
        // Prevent ALT from activating the system menu modal loop, which would
        // freeze rendering on this single-threaded app.  The ALT key state is
        // still readable via GetKeyState() for our input handler.
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            PostMessage(hwnd, WM_CLOSE, 0, 0);
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
        // Render thread requested a capture-flash; start it on this thread
        // (the message-pump thread that owns the HWND).
        dxr_capture::TriggerCaptureFlash(hwnd);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// DISPLAYXR_TRANSPARENT_BG=1 → cube clears RGBA(0,0,0,0), window uses
// WS_EX_NOREDIRECTIONBITMAP + null brush so DComp can show the desktop
// through the cube's transparent regions. Mirrors cube_handle_vk_win.
static bool TransparentBackgroundEnabled() {
    static const bool e = []() {
        const char *v = getenv("DISPLAYXR_TRANSPARENT_BG");
        return v != nullptr && *v != '\0' && *v != '0';
    }();
    return e;
}

// Create the application window
static HWND CreateAppWindow(HINSTANCE hInstance, int width, int height) {
    const bool transparent = TransparentBackgroundEnabled();
    LOG_INFO("Creating application window (%dx%d, transparent=%d)", width, height, transparent);

    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    // Null brush in transparent mode so the redirection bitmap doesn't paint
    // black under the DComp composition swap chain.
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
    HWND hwnd = CreateWindowEx(
        exStyle,
        WINDOW_CLASS,
        WINDOW_TITLE,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left,
        rect.bottom - rect.top,
        nullptr,
        nullptr,
        hInstance,
        nullptr
    );

    if (!hwnd) {
        LOG_ERROR("Failed to create window, error: %lu", GetLastError());
        return nullptr;
    }

    LOG_INFO("Window created: 0x%p", hwnd);
    return hwnd;
}

// Performance tracking
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

// State passed to RenderOneFrame (and accessible from WM_PAINT via g_renderState)
struct RenderState {
    HWND hwnd;
    XrSessionManager* xr;
    D3D11Renderer* renderer;
    HudRenderer* hudRenderer;
    bool hudOk;
    std::vector<XrSwapchainImageD3D11KHR>* hudSwapchainImages;
    ComPtr<ID3D11Texture2D> depthTexture;
    ComPtr<ID3D11DepthStencilView> depthDSV;
    std::vector<XrSwapchainImageD3D11KHR>* swapchainImages;
    PerformanceStats* perfStats;
};

// Render a single frame — called from the main loop and from WM_PAINT during
// drag/resize so that rendering never stalls.
static void RenderOneFrame(RenderState& rs) {
    XrSessionManager& xr = *rs.xr;
    D3D11Renderer& renderer = *rs.renderer;

    // Update performance stats
    UpdatePerformanceStats(*rs.perfStats);

    // Update input-based camera movement (clears resetViewRequested internally)
    UpdateCameraMovement(g_inputState, rs.perfStats->deltaTime, rs.xr->displayHeightM);

    // Handle fullscreen toggle (F11)
    if (g_inputState.fullscreenToggleRequested) {
        ToggleFullscreen(rs.hwnd);
        g_inputState.fullscreenToggleRequested = false;
    }

    // Handle rendering mode change (V=cycle, 0-8=direct)
    if (g_inputState.renderingModeChangeRequested) {
        g_inputState.renderingModeChangeRequested = false;
        if (xr.pfnRequestDisplayRenderingModeEXT && xr.session != XR_NULL_HANDLE) {
            xr.pfnRequestDisplayRenderingModeEXT(xr.session, g_inputState.currentRenderingMode);
        }
    }

    // Handle eye tracking mode toggle (T key)
    if (g_inputState.eyeTrackingModeToggleRequested) {
        g_inputState.eyeTrackingModeToggleRequested = false;
        if (xr.pfnRequestEyeTrackingModeEXT && xr.session != XR_NULL_HANDLE) {
            XrEyeTrackingModeEXT newMode = (xr.activeEyeTrackingMode == XR_EYE_TRACKING_MODE_MANAGED_EXT)
                ? XR_EYE_TRACKING_MODE_MANUAL_EXT : XR_EYE_TRACKING_MODE_MANAGED_EXT;
            XrResult etResult = xr.pfnRequestEyeTrackingModeEXT(xr.session, newMode);
            LOG_INFO("Eye tracking mode -> %s (%s)",
                newMode == XR_EYE_TRACKING_MODE_MANUAL_EXT ? "MANUAL" : "MANAGED",
                XR_SUCCEEDED(etResult) ? "OK" : "unsupported");
        }
    }

    // Update scene (cube rotation)
    UpdateScene(renderer, rs.perfStats->deltaTime);

    // Poll OpenXR events
    PollEvents(xr);

    // Only render if session is running
    if (xr.sessionRunning) {
        XrFrameState frameState;
        if (BeginFrame(xr, frameState)) {
                uint32_t modeViewCount = (xr.renderingModeCount > 0 && g_inputState.currentRenderingMode < xr.renderingModeCount)
                    ? xr.renderingModeViewCounts[g_inputState.currentRenderingMode] : 2;
                uint32_t tileColumns = (xr.renderingModeCount > 0 && g_inputState.currentRenderingMode < xr.renderingModeCount)
                    ? xr.renderingModeTileColumns[g_inputState.currentRenderingMode] : 2;
                uint32_t tileRows = (xr.renderingModeCount > 0 && g_inputState.currentRenderingMode < xr.renderingModeCount)
                    ? xr.renderingModeTileRows[g_inputState.currentRenderingMode] : 1;
                bool monoMode = (xr.renderingModeCount > 0 && !xr.renderingModeDisplay3D[g_inputState.currentRenderingMode]);
                if (xr.renderingModeCount > 0 && g_inputState.currentRenderingMode < xr.renderingModeCount) {
                    xr.recommendedViewScaleX = xr.renderingModeScaleX[g_inputState.currentRenderingMode];
                    xr.recommendedViewScaleY = xr.renderingModeScaleY[g_inputState.currentRenderingMode];
                }
                int eyeCount = monoMode ? 1 : (int)modeViewCount;
                std::vector<XrCompositionLayerProjectionView> projectionViews(eyeCount, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
            bool hudSubmitted = false;

            if (frameState.shouldRender) {
                if (LocateViews(xr, frameState.predictedDisplayTime,
                    g_inputState.cameraPosX, g_inputState.cameraPosY, g_inputState.cameraPosZ,
                    g_inputState.yaw, g_inputState.pitch,
                    g_inputState.viewParams)) {

                    // Get raw view poses (pre-player-transform) for projection views.
                    XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
                    locateInfo.viewConfigurationType = xr.viewConfigType;
                    locateInfo.displayTime = frameState.predictedDisplayTime;
                    locateInfo.space = xr.localSpace;

                    XrViewState viewState = {XR_TYPE_VIEW_STATE};
                    uint32_t viewCount = 8;
                    XrView rawViews[8];
                    for (uint32_t vi = 0; vi < 8; vi++) rawViews[vi] = {XR_TYPE_VIEW};
                    xrLocateViews(xr.session, &locateInfo, &viewState, 8, &viewCount, rawViews);

                    // --- App-side Kooima projection (RAW mode, app-owned camera model) ---
                    // Max per-tile capacity from swapchain
                    uint32_t maxTileW = tileColumns > 0 ? xr.swapchain.width / tileColumns : xr.swapchain.width;
                    uint32_t maxTileH = tileRows > 0 ? xr.swapchain.height / tileRows : xr.swapchain.height;

                    std::vector<Display3DView> stereoViews(eyeCount);
                    bool useAppProjection = (xr.hasDisplayInfoExt && xr.displayWidthM > 0.0f);
                    if (useAppProjection) {
                        // Window-relative Kooima: use actual window physical dims as screen,
                        // offset eyes to be relative to window center (not display center)
                        float pxSizeX = xr.displayWidthM / (float)xr.swapchain.width;
                        float pxSizeY = xr.displayHeightM / (float)xr.swapchain.height;
                        float winW_m = (float)g_windowWidth * pxSizeX;
                        float winH_m = (float)g_windowHeight * pxSizeY;

                        // Compute window center offset from display center (meters)
                        float eyeOffsetX = 0.0f, eyeOffsetY = 0.0f;
                        {
                            POINT clientOrigin = {0, 0};
                            ClientToScreen(rs.hwnd, &clientOrigin);
                            HMONITOR hMon = MonitorFromWindow(rs.hwnd, MONITOR_DEFAULTTONEAREST);
                            MONITORINFO mi = {sizeof(mi)};
                            if (GetMonitorInfo(hMon, &mi)) {
                                float winCenterX = (float)(clientOrigin.x - mi.rcMonitor.left) + g_windowWidth / 2.0f;
                                float winCenterY = (float)(clientOrigin.y - mi.rcMonitor.top) + g_windowHeight / 2.0f;
                                float dispW = (float)(mi.rcMonitor.right - mi.rcMonitor.left);
                                float dispH = (float)(mi.rcMonitor.bottom - mi.rcMonitor.top);
                                eyeOffsetX = (winCenterX - dispW / 2.0f) * pxSizeX;
                                eyeOffsetY = -((winCenterY - dispH / 2.0f) * pxSizeY); // Y flip: screen Y-down, eye Y-up
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
                            g_inputState.pitch, g_inputState.yaw, 0);
                        XMFLOAT4 q;
                        XMStoreFloat4(&q, pOri);
                        cameraPose.orientation = {q.x, q.y, q.z, q.w};
                        cameraPose.position = {g_inputState.cameraPosX, g_inputState.cameraPosY, g_inputState.cameraPosZ};

                        XrVector3f nominalViewer = {xr.nominalViewerX, xr.nominalViewerY, xr.nominalViewerZ};
                        Display3DScreen screen = {winW_m, winH_m};

                        if (g_inputState.cameraMode) {
                            // Camera-centric path
                            Camera3DTunables camTunables;
                            camTunables.ipd_factor = g_inputState.viewParams.ipdFactor;
                            camTunables.parallax_factor = g_inputState.viewParams.parallaxFactor;
                            camTunables.inv_convergence_distance = g_inputState.viewParams.invConvergenceDistance;
                            camTunables.half_tan_vfov = CAMERA_HALF_TAN_VFOV / g_inputState.viewParams.zoomFactor;

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
                            tunables.ipd_factor = g_inputState.viewParams.ipdFactor;
                            tunables.parallax_factor = g_inputState.viewParams.parallaxFactor;
                            tunables.perspective_factor = g_inputState.viewParams.perspectiveFactor;
                            tunables.virtual_display_height = g_inputState.viewParams.virtualDisplayHeight / g_inputState.viewParams.scaleFactor;

                            display3d_compute_views(
                                rawEyes.data(), eyeCount, &nominalViewer,
                                &screen, &tunables, &cameraPose,
                                0.01f, 100.0f, stereoViews.data());
                        }
                    }

                    // [Commented out — will be reused for 3D-positioned HUD later]
                    // ConvergencePlane convPlane = LocateConvergencePlane(rawViews);

                    // Render HUD to window-space layer swapchain (once per frame, before eye loop)
                    if (g_inputState.hudVisible && xr.hasHudSwapchain && rs.hudSwapchainImages && !rs.hudSwapchainImages->empty() && rs.hudOk) {
                        uint32_t hudImageIndex;
                        if (AcquireHudSwapchainImage(xr, hudImageIndex)) {
                            std::wstring sessionText(xr.systemName, xr.systemName + strlen(xr.systemName));
                            sessionText += L"\nSession: ";
                            sessionText += FormatSessionState((int)xr.sessionState);
                            std::wstring modeText = xr.hasWin32WindowBindingExt ?
                                L"XR_EXT_win32_window_binding: ACTIVE (D3D11)" :
                                L"XR_EXT_win32_window_binding: NOT AVAILABLE (D3D11)";
                            modeText += g_inputState.cameraMode ?
                                L"\nKooima: Camera-Centric [C=Toggle]" :
                                L"\nKooima: Display-Centric [C=Toggle]";

                            // Dynamic render dims matching the actual viewport computation
                            bool dispMonoMode = (xr.renderingModeCount > 0 && !xr.renderingModeDisplay3D[g_inputState.currentRenderingMode]);
                            uint32_t dispRenderW, dispRenderH;
                            if (dispMonoMode) {
                                dispRenderW = g_windowWidth;
                                dispRenderH = g_windowHeight;
                                if (dispRenderW > xr.swapchain.width) dispRenderW = xr.swapchain.width;
                                if (dispRenderH > xr.swapchain.height) dispRenderH = xr.swapchain.height;
                            } else {
                                dispRenderW = (uint32_t)(g_windowWidth * xr.recommendedViewScaleX);
                                dispRenderH = (uint32_t)(g_windowHeight * xr.recommendedViewScaleY);
                                if (dispRenderW > maxTileW) dispRenderW = maxTileW;
                                if (dispRenderH > maxTileH) dispRenderH = maxTileH;
                            }
                            std::wstring perfText = FormatPerformanceInfo(rs.perfStats->fps, rs.perfStats->frameTimeMs,
                                dispRenderW, dispRenderH,
                                g_windowWidth, g_windowHeight);
                            std::wstring dispText = FormatDisplayInfo(xr.displayWidthM, xr.displayHeightM,
                                xr.nominalViewerX, xr.nominalViewerY, xr.nominalViewerZ);
                            dispText += L"\n" + FormatScaleInfo(xr.recommendedViewScaleX, xr.recommendedViewScaleY);
                            dispText += L"\n" + FormatMode(g_inputState.currentRenderingMode, xr.pfnRequestDisplayRenderingModeEXT != nullptr,
                                (xr.renderingModeCount > 0 && g_inputState.currentRenderingMode < xr.renderingModeCount) ? xr.renderingModeNames[g_inputState.currentRenderingMode] : nullptr,
                                xr.renderingModeCount,
                                xr.renderingModeCount > 0 ? xr.renderingModeDisplay3D[g_inputState.currentRenderingMode] : true);
                            std::wstring eyeText = FormatEyeTrackingInfo(
                                xr.eyePositions, (uint32_t)eyeCount,
                                xr.eyeTrackingActive, xr.isEyeTracking,
                                xr.activeEyeTrackingMode, xr.supportedEyeTrackingModes);

                            float fwdX = -sinf(g_inputState.yaw) * cosf(g_inputState.pitch);
                            float fwdY =  sinf(g_inputState.pitch);
                            float fwdZ = -cosf(g_inputState.yaw) * cosf(g_inputState.pitch);
                            std::wstring cameraText = FormatCameraInfo(
                                g_inputState.cameraPosX, g_inputState.cameraPosY, g_inputState.cameraPosZ,
                                fwdX, fwdY, fwdZ, g_inputState.cameraMode);
                            float dispP1 = g_inputState.cameraMode ? g_inputState.viewParams.invConvergenceDistance : g_inputState.viewParams.perspectiveFactor;
                            float dispP2 = g_inputState.cameraMode ? g_inputState.viewParams.zoomFactor : g_inputState.viewParams.scaleFactor;
                            std::wstring stereoText = FormatViewParams(
                                g_inputState.viewParams.ipdFactor, g_inputState.viewParams.parallaxFactor,
                                dispP1, dispP2, g_inputState.cameraMode);
                            {
                                wchar_t vhBuf[64];
                                if (g_inputState.cameraMode) {
                                    float tanHFOV = CAMERA_HALF_TAN_VFOV / g_inputState.viewParams.zoomFactor;
                                    swprintf(vhBuf, 64, L"\ntanHFOV: %.3f", tanHFOV);
                                } else {
                                    float hudM2v = 1.0f;
                                    if (g_inputState.viewParams.virtualDisplayHeight > 0.0f && xr.displayHeightM > 0.0f)
                                        hudM2v = g_inputState.viewParams.virtualDisplayHeight / xr.displayHeightM;
                                    swprintf(vhBuf, 64, L"\nvHeight: %.3f  m2v: %.3f",
                                        g_inputState.viewParams.virtualDisplayHeight, hudM2v);
                                }
                                stereoText += vhBuf;
                            }
                            std::wstring helpText = FormatHelpText(xr.pfnRequestDisplayRenderingModeEXT != nullptr, g_inputState.cameraMode, xr.renderingModeCount);

                            uint32_t srcRowPitch = 0;
                            const void* pixels = RenderHudAndMap(*rs.hudRenderer, &srcRowPitch,
                                sessionText, modeText, perfText, dispText, eyeText,
                                cameraText, stereoText, helpText);
                            if (pixels) {
                                ID3D11Texture2D* hudTexture = (*rs.hudSwapchainImages)[hudImageIndex].texture;
                                D3D11_BOX box = {0, 0, 0, xr.hudSwapchain.width, xr.hudSwapchain.height, 1};
                                renderer.context->UpdateSubresource(hudTexture, 0, &box, pixels, srcRowPitch, 0);
                                UnmapHud(*rs.hudRenderer);
                            }

                            ReleaseHudSwapchainImage(xr);
                            hudSubmitted = true;
                        }
                    }

                    // For mono: compute center eye position and projection
                    XMMATRIX monoViewMatrix, monoProjMatrix;
                    XrFovf monoFov = {};
                    XrPosef monoPose = rawViews[0].pose;
                    if (monoMode) {
                        // Center eye = average of all view positions
                        monoPose.position = {0.0f, 0.0f, 0.0f};
                        int cnt = (int)viewCount;
                        if (cnt < 1) cnt = 1;
                        for (int v = 0; v < cnt; v++) {
                            monoPose.position.x += rawViews[v].pose.position.x;
                            monoPose.position.y += rawViews[v].pose.position.y;
                            monoPose.position.z += rawViews[v].pose.position.z;
                        }
                        monoPose.position.x /= cnt;
                        monoPose.position.y /= cnt;
                        monoPose.position.z /= cnt;

                        // When useAppProjection, mono view+proj come from stereoViews[0]
                        // (library was called with center eye as both L/R above).
                        // Only need fallback when !useAppProjection.
                        if (!useAppProjection) {
                            monoProjMatrix = xr.projMatrices[0];  // Close enough for 2D
                            monoFov = rawViews[0].fov;

                            // Build center-eye view matrix from scratch (same as LocateViews)
                            XMVECTOR centerLocalPos = XMVectorSet(
                                monoPose.position.x, monoPose.position.y, monoPose.position.z, 0.0f);
                            XMVECTOR localOri = XMVectorSet(
                                rawViews[0].pose.orientation.x, rawViews[0].pose.orientation.y,
                                rawViews[0].pose.orientation.z, rawViews[0].pose.orientation.w);

                            float monoM2vView = 1.0f;
                            if (g_inputState.viewParams.virtualDisplayHeight > 0.0f && xr.displayHeightM > 0.0f)
                                monoM2vView = g_inputState.viewParams.virtualDisplayHeight / xr.displayHeightM;
                            float eyeScale = g_inputState.viewParams.perspectiveFactor * monoM2vView / g_inputState.viewParams.scaleFactor;
                            XMVECTOR playerOri = XMQuaternionRotationRollPitchYaw(
                                g_inputState.pitch, g_inputState.yaw, 0);
                            XMVECTOR playerPos = XMVectorSet(
                                g_inputState.cameraPosX, g_inputState.cameraPosY,
                                g_inputState.cameraPosZ, 0.0f);

                            XMVECTOR worldPos = XMVector3Rotate(centerLocalPos * eyeScale, playerOri) + playerPos;
                            XMVECTOR worldOri = XMQuaternionMultiply(localOri, playerOri);

                            XMMATRIX rot = XMMatrixTranspose(XMMatrixRotationQuaternion(worldOri));
                            XMFLOAT3 wp;
                            XMStoreFloat3(&wp, worldPos);
                            monoViewMatrix = XMMatrixTranslation(-wp.x, -wp.y, -wp.z) * rot;
                        }
                    }

                    // Single swapchain: acquire once, render all views, release once
                    uint32_t imageIndex;
                    if (AcquireSwapchainImage(xr, imageIndex)) {
                        ID3D11Texture2D* swapchainTexture = (*rs.swapchainImages)[imageIndex].texture;

                        ID3D11RenderTargetView* rtv = nullptr;
                        CreateRenderTargetView(renderer, swapchainTexture,
                            static_cast<DXGI_FORMAT>(xr.swapchain.format), &rtv);

                        // Clear entire color+depth once before eye loop.
                        // Transparent mode (DISPLAYXR_TRANSPARENT_BG=1): clear to
                        // RGBA(0,0,0,0) so the Leia DP's compose-under-bg pass
                        // shows the desktop wherever the cube didn't draw.
                        float clearColor[4];
                        if (TransparentBackgroundEnabled()) {
                            clearColor[0] = 0.0f; clearColor[1] = 0.0f;
                            clearColor[2] = 0.0f; clearColor[3] = 0.0f;
                        } else {
                            clearColor[0] = 0.05f; clearColor[1] = 0.05f;
                            clearColor[2] = 0.25f; clearColor[3] = 1.0f;
                        }
                        renderer.context->ClearRenderTargetView(rtv, clearColor);
                        renderer.context->ClearDepthStencilView(rs.depthDSV.Get(),
                            D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

                        // Dynamic render dims based on window size, clamped to swapchain capacity
                        uint32_t renderW, renderH;
                        if (monoMode) {
                            renderW = g_windowWidth;
                            renderH = g_windowHeight;
                            if (renderW > xr.swapchain.width) renderW = xr.swapchain.width;
                            if (renderH > xr.swapchain.height) renderH = xr.swapchain.height;
                        } else {
                            renderW = (uint32_t)(g_windowWidth * xr.recommendedViewScaleX);
                            renderH = (uint32_t)(g_windowHeight * xr.recommendedViewScaleY);
                            if (renderW > maxTileW) renderW = maxTileW;
                            if (renderH > maxTileH) renderH = maxTileH;
                        }

                        for (int eye = 0; eye < eyeCount; eye++) {
                            uint32_t tileX = monoMode ? 0 : (eye % tileColumns);
                            uint32_t tileY = monoMode ? 0 : (eye / tileColumns);
                            D3D11_VIEWPORT vp = {};
                            vp.TopLeftX = (FLOAT)(tileX * renderW);
                            vp.TopLeftY = (FLOAT)(tileY * renderH);
                            vp.Width = (FLOAT)renderW;
                            vp.Height = (FLOAT)renderH;
                            vp.MaxDepth = 1.0f;
                            renderer.context->RSSetViewports(1, &vp);

                            XMMATRIX viewMatrix, projMatrix;
                            if (useAppProjection) {
                                int vi = monoMode ? 0 : eye;
                                viewMatrix = ColumnMajorToXMMatrix(stereoViews[vi].view_matrix);
                                projMatrix = ColumnMajorToXMMatrix(stereoViews[vi].projection_matrix);
                            } else if (monoMode) {
                                viewMatrix = monoViewMatrix;
                                projMatrix = monoProjMatrix;
                            } else {
                                int vi = (eye < (int)viewCount) ? eye : 0;
                                viewMatrix = xr.viewMatrices[vi];
                                projMatrix = xr.projMatrices[vi];
                            }

                            RenderScene(renderer, rtv, rs.depthDSV.Get(),
                                renderW, renderH,
                                viewMatrix, projMatrix,
                                useAppProjection ? 1.0f : g_inputState.viewParams.scaleFactor,
                                0.03f);

                            projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                            projectionViews[eye].subImage.swapchain = xr.swapchain.swapchain;
                            projectionViews[eye].subImage.imageRect.offset = {
                                (int32_t)(tileX * renderW), (int32_t)(tileY * renderH)
                            };
                            projectionViews[eye].subImage.imageRect.extent = {
                                (int32_t)renderW,
                                (int32_t)renderH
                            };
                            projectionViews[eye].subImage.imageArrayIndex = 0;

                            int safeIdx = (eye < (int)viewCount) ? eye : 0;
                            projectionViews[eye].pose = monoMode ? monoPose : rawViews[safeIdx].pose;
                            projectionViews[eye].fov = useAppProjection ?
                                stereoViews[monoMode ? 0 : eye].fov :
                                (monoMode ? monoFov : rawViews[safeIdx].fov);
                        }

                        if (rtv) rtv->Release();

                        // 'I' key: snapshot the multi-view atlas to a PNG.
                        // Skipped for mono (1×1) layouts.
                        if (g_inputState.captureAtlasRequested) {
                            g_inputState.captureAtlasRequested = false;
                            if (!monoMode && (tileColumns > 1 || tileRows > 1)) {
                                uint32_t atlasW = tileColumns * renderW;
                                uint32_t atlasH = tileRows * renderH;
                                if (atlasW <= xr.swapchain.width && atlasH <= xr.swapchain.height) {
                                    std::string outPath = dxr_capture::MakeCapturePath(
                                        APP_NAME, tileColumns, tileRows);
                                    bool ok = dxr_capture::CaptureAtlasRegionD3D11(
                                        renderer.device.Get(), renderer.context.Get(),
                                        swapchainTexture,
                                        0, 0, atlasW, atlasH, outPath);
                                    if (ok) {
                                        LOG_INFO("Captured atlas %ux%u -> %s",
                                                 atlasW, atlasH, outPath.c_str());
                                        dxr_capture::PostFlashRequest(rs.hwnd);
                                    }
                                }
                            } else {
                                LOG_INFO("Capture skipped: need 3D mode with cols/rows > 1");
                            }
                        }

                        ReleaseSwapchainImage(xr);
                    }
                }
            }

            // Submit frame with window-space HUD layer if visible
            if (hudSubmitted) {
                float hudAR = (float)HUD_PIXEL_WIDTH / (float)HUD_PIXEL_HEIGHT;
                float windowAR = (g_windowWidth > 0 && g_windowHeight > 0) ? (float)g_windowWidth / (float)g_windowHeight : 1.0f;
                float fracW = HUD_WIDTH_FRACTION;
                float fracH = fracW * windowAR / hudAR;
                if (fracH > 1.0f) { fracH = 1.0f; fracW = hudAR / windowAR; }
                EndFrameWithWindowSpaceHud(xr, frameState.predictedDisplayTime, projectionViews.data(),
                    0.0f, 0.0f, fracW, fracH, 0.0f, eyeCount);
            } else {
                EndFrame(xr, frameState.predictedDisplayTime, projectionViews.data(), eyeCount);
            }
        }
    } else {
        Sleep(100);
    }
}

// Main entry point
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    // Initialize logging
    if (!InitializeLogging(APP_NAME)) {
        MessageBox(nullptr, L"Failed to initialize logging", L"Warning", MB_OK | MB_ICONWARNING);
    }

    LOG_INFO("=== SR Cube OpenXR Ext Application ===");
    LOG_INFO("OpenXR with XR_EXT_win32_window_binding extension");
    LOG_INFO("Application creates and controls its own window");

    // Create window FIRST (needed for XR_EXT_win32_window_binding)
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
    LOG_INFO("Initializing OpenXR...");
    XrSessionManager xr = {};
    g_xr = &xr;
    if (!InitializeOpenXR(xr)) {
        LOG_ERROR("OpenXR initialization failed");
        MessageBox(hwnd, L"Failed to initialize OpenXR", L"Error", MB_OK | MB_ICONERROR);
        g_xr = nullptr;
        ShutdownLogging();
        return 1;
    }

    // Check for session target extension
    if (!xr.hasWin32WindowBindingExt) {
        LOG_WARN("XR_EXT_win32_window_binding not available - runtime will create its own window");
        MessageBox(hwnd, L"XR_EXT_win32_window_binding extension not available.\nRuntime will create its own window.",
            L"Warning", MB_OK | MB_ICONWARNING);
    } else {
        LOG_INFO("XR_EXT_win32_window_binding extension is available - using app window");
    }

    // Get the required GPU adapter LUID from OpenXR
    LUID adapterLuid;
    if (!GetD3D11GraphicsRequirements(xr, &adapterLuid)) {
        LOG_ERROR("Failed to get D3D11 graphics requirements");
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Initialize D3D11 on the correct adapter
    LOG_INFO("Initializing D3D11...");
    D3D11Renderer renderer = {};
    if (!InitializeD3D11WithLUID(renderer, adapterLuid)) {
        LOG_ERROR("D3D11 initialization failed");
        MessageBox(hwnd, L"Failed to initialize D3D11", L"Error", MB_OK | MB_ICONERROR);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Initialize HUD renderer (standalone D3D11 device for text rendering)
    HudRenderer hudRenderer = {};
    bool hudOk = InitializeHudRenderer(hudRenderer, HUD_PIXEL_WIDTH, HUD_PIXEL_HEIGHT);
    if (!hudOk) {
        LOG_WARN("HUD renderer init failed - HUD will not be displayed");
    }

    // Create OpenXR session WITH window handle (XR_EXT_win32_window_binding)
    LOG_INFO("Creating OpenXR session with XR_EXT_win32_window_binding (HWND: 0x%p)...", hwnd);
    if (!CreateSession(xr, renderer.device.Get(), hwnd)) {
        LOG_ERROR("OpenXR session creation failed");
        MessageBox(hwnd, L"Failed to create OpenXR session", L"Error", MB_OK | MB_ICONERROR);
        if (hudOk) CleanupHudRenderer(hudRenderer);
        CleanupD3D11(renderer);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create reference spaces
    if (!CreateSpaces(xr)) {
        LOG_ERROR("Reference space creation failed");
        CleanupOpenXR(xr);
        if (hudOk) CleanupHudRenderer(hudRenderer);
        CleanupD3D11(renderer);
        ShutdownLogging();
        return 1;
    }

    // Create single swapchain at native display resolution
    if (!CreateSwapchain(xr)) {
        LOG_ERROR("Swapchain creation failed");
        CleanupOpenXR(xr);
        if (hudOk) CleanupHudRenderer(hudRenderer);
        CleanupD3D11(renderer);
        ShutdownLogging();
        return 1;
    }

    // Enumerate D3D11 swapchain images
    std::vector<XrSwapchainImageD3D11KHR> swapchainImages;
    {
        uint32_t count = xr.swapchain.imageCount;
        swapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)swapchainImages.data());
        LOG_INFO("Enumerated %u D3D11 swapchain images", count);
    }

    // Create HUD swapchain for window-space layer submission
    if (!CreateHudSwapchain(xr, HUD_PIXEL_WIDTH, HUD_PIXEL_HEIGHT)) {
        LOG_WARN("Failed to create HUD swapchain - HUD will not be displayed");
    }

    // Enumerate HUD swapchain images
    std::vector<XrSwapchainImageD3D11KHR> hudSwapchainImages;
    if (xr.hasHudSwapchain) {
        uint32_t count = xr.hudSwapchain.imageCount;
        hudSwapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        xrEnumerateSwapchainImages(xr.hudSwapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)hudSwapchainImages.data());
        LOG_INFO("HUD: enumerated %u D3D11 swapchain images", count);
    }

    // Show window
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // Create single depth buffer at full swapchain dimensions
    ComPtr<ID3D11Texture2D> depthTexture;
    ComPtr<ID3D11DepthStencilView> depthDSV;
    {
        ID3D11Texture2D* depthTex = nullptr;
        ID3D11DepthStencilView* dsv = nullptr;
        if (!CreateDepthStencilView(renderer, xr.swapchain.width, xr.swapchain.height, &depthTex, &dsv)) {
            LOG_ERROR("Failed to create depth buffer");
            CleanupOpenXR(xr);
            if (hudOk) CleanupHudRenderer(hudRenderer);
            CleanupD3D11(renderer);
            ShutdownLogging();
            return 1;
        }
        depthTexture.Attach(depthTex);
        depthDSV.Attach(dsv);
    }

    LOG_INFO("");
    LOG_INFO("=== Entering main loop ===");
    LOG_INFO("XR rendering happens in the application window (XR_EXT_win32_window_binding)");
    LOG_INFO("Single-threaded: message pump + render on the main thread (WM_PAINT during drag/resize)");
    LOG_INFO("Controls: WASD=Fly, QE=Up/Down, Mouse=Look, Space/DblClick=Reset, P=Parallax, V=Mode, SHIFT+TAB=HUD, F11=Fullscreen, ESC=Quit");
    LOG_INFO("");

    PerformanceStats perfStats = {};
    perfStats.lastTime = std::chrono::high_resolution_clock::now();

    // Set virtual display height (app units). 0.24 = 4x the 0.06m cube height.
    g_inputState.viewParams.virtualDisplayHeight = 0.24f;
    g_inputState.nominalViewerZ = xr.nominalViewerZ;
    g_inputState.renderingModeCount = xr.renderingModeCount;

    RenderState rs = {};
    rs.hwnd = hwnd;
    rs.xr = &xr;
    rs.renderer = &renderer;
    rs.hudRenderer = &hudRenderer;
    rs.hudOk = hudOk;
    rs.hudSwapchainImages = &hudSwapchainImages;
    rs.depthTexture = depthTexture;
    rs.depthDSV = depthDSV;
    rs.swapchainImages = &swapchainImages;
    rs.perfStats = &perfStats;
    g_renderState = &rs;

    // Single-threaded main loop: pump messages, then render one frame.
    // During drag/resize, DefWindowProc enters a modal loop that blocks
    // PeekMessage — WM_PAINT fires inside that modal loop to keep rendering.
    MSG msg = {};
    while (g_running && !xr.exitRequested) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!g_running) break;

        RenderOneFrame(rs);
    }

    g_renderState = nullptr;

    // Cleanup
    LOG_INFO("");
    LOG_INFO("=== Shutting down ===");

    depthDSV.Reset();
    depthTexture.Reset();

    g_xr = nullptr;
    CleanupOpenXR(xr);
    if (hudOk) CleanupHudRenderer(hudRenderer);
    CleanupD3D11(renderer);

    DestroyWindow(hwnd);
    UnregisterClass(WINDOW_CLASS, hInstance);

    LOG_INFO("Application shutdown complete");
    ShutdownLogging();

    return 0;
}
