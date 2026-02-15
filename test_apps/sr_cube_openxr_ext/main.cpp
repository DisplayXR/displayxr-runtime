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

#include <chrono>
#include <string>
#include <sstream>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

// Application name for logging
static const char* APP_NAME = "sr_cube_openxr_ext";

// Window settings
static const wchar_t* WINDOW_CLASS = L"SRCubeOpenXRExtClass";
static const wchar_t* WINDOW_TITLE = L"SR Cube OpenXR Ext - XR_EXT_win32_window_binding (Press ESC to exit)";

// Global state (single-threaded — all accessed from the main thread only)
static InputState g_inputState;
static bool g_running = true;
static XrSessionManager* g_xr = nullptr;
static UINT g_windowWidth = 1280;
static UINT g_windowHeight = 720;
static bool g_inSizeMove = false;  // True while user is dragging/resizing the window
static const uint32_t HUD_PIXEL_WIDTH = 380;
static const uint32_t HUD_PIXEL_HEIGHT = 280;
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

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            PostMessage(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        break;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// Create the application window
static HWND CreateAppWindow(HINSTANCE hInstance, int width, int height) {
    LOG_INFO("Creating application window (%dx%d)", width, height);

    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
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

    HWND hwnd = CreateWindowEx(
        0,
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
    ComPtr<ID3D11Texture2D>* depthTextures;
    ComPtr<ID3D11DepthStencilView>* depthDSVs;
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
    UpdateCameraMovement(g_inputState, rs.perfStats->deltaTime);

    // Handle fullscreen toggle (F11)
    if (g_inputState.fullscreenToggleRequested) {
        ToggleFullscreen(rs.hwnd);
        g_inputState.fullscreenToggleRequested = false;
    }

    // Handle display mode toggle (V key)
    if (g_inputState.displayModeToggleRequested) {
        g_inputState.displayModeToggleRequested = false;
        if (xr.pfnRequestDisplayModeEXT && xr.session != XR_NULL_HANDLE) {
            XrDisplayModeEXT mode = g_inputState.displayMode3D ?
                XR_DISPLAY_MODE_3D_EXT : XR_DISPLAY_MODE_2D_EXT;
            xr.pfnRequestDisplayModeEXT(xr.session, mode);
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
            XrCompositionLayerProjectionView projectionViews[2] = {};
            bool hudSubmitted = false;

            if (frameState.shouldRender) {
                XMMATRIX leftViewMatrix, leftProjMatrix;
                XMMATRIX rightViewMatrix, rightProjMatrix;

                if (LocateViews(xr, frameState.predictedDisplayTime,
                    leftViewMatrix, leftProjMatrix,
                    rightViewMatrix, rightProjMatrix,
                    g_inputState.cameraPosX, g_inputState.cameraPosY, g_inputState.cameraPosZ,
                    g_inputState.yaw, g_inputState.pitch,
                    g_inputState.zoomScale)) {

                    // Get raw view poses (pre-player-transform) for projection views.
                    // Use DISPLAY space when available: it is physically anchored to the
                    // display center and unaffected by recentering, which is the correct
                    // reference for compositing on tracked 3D displays.
                    // Falls back to LOCAL space if XR_EXT_display_info is not enabled.
                    XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
                    locateInfo.viewConfigurationType = xr.viewConfigType;
                    locateInfo.displayTime = frameState.predictedDisplayTime;
                    locateInfo.space = (xr.displaySpace != XR_NULL_HANDLE) ? xr.displaySpace : xr.localSpace;

                    XrViewState viewState = {XR_TYPE_VIEW_STATE};
                    uint32_t viewCount = 2;
                    XrView rawViews[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
                    xrLocateViews(xr.session, &locateInfo, &viewState, 2, &viewCount, rawViews);

                    // Store raw per-eye positions in display space for HUD
                    xr.leftEyeX = rawViews[0].pose.position.x;
                    xr.leftEyeY = rawViews[0].pose.position.y;
                    xr.leftEyeZ = rawViews[0].pose.position.z;
                    xr.rightEyeX = rawViews[1].pose.position.x;
                    xr.rightEyeY = rawViews[1].pose.position.y;
                    xr.rightEyeZ = rawViews[1].pose.position.z;

                    // --- App-side Kooima projection (RAW mode, app-owned camera model) ---
                    uint32_t renderW_pre = (uint32_t)(g_windowWidth * xr.recommendedViewScaleX);
                    uint32_t renderH_pre = (uint32_t)(g_windowHeight * xr.recommendedViewScaleY);
                    if (renderW_pre > xr.swapchains[0].width) renderW_pre = xr.swapchains[0].width;
                    if (renderH_pre > xr.swapchains[0].height) renderH_pre = xr.swapchains[0].height;

                    XrFovf appFov[2];
                    bool useAppProjection = (xr.hasDisplayInfoExt && xr.displayWidthM > 0.0f);
                    if (useAppProjection) {
                        // Viewport-scale FOV (SRHydra): convert window pixels to meters,
                        // then apply isotropic scale so FOV stays consistent across window
                        // sizes on the 3D display. Matches the non-extension runtime path.
                        float pxSizeX = xr.displayWidthM / (float)xr.swapchains[0].width;
                        float pxSizeY = xr.displayHeightM / (float)xr.swapchains[0].height;
                        float winW_m = (float)g_windowWidth * pxSizeX;
                        float winH_m = (float)g_windowHeight * pxSizeY;
                        float minDisp = fminf(xr.displayWidthM, xr.displayHeightM);
                        float minWin  = fminf(winW_m, winH_m);
                        float vs = minDisp / minWin;
                        float screenWidthM  = winW_m * vs;
                        float screenHeightM = winH_m * vs;

                        // Alternative: content-preserving FOV — keeps rendered content at
                        // constant physical size on display regardless of window size.
                        // float screenWidthM = xr.displayWidthM * (float)renderW_pre / (float)xr.swapchains[0].width;
                        // float screenHeightM = xr.displayHeightM * (float)renderH_pre / (float)xr.swapchains[0].height;

                        // Display-center zoom: scale eye positions and screen size by
                        // 1/zoomScale. These cancel in Kooima (same ratio), but are kept
                        // explicit for upcoming baseline/parallax/perspective modifiers.
                        float zs = g_inputState.zoomScale;
                        for (int e = 0; e < 2; e++) {
                            XrVector3f eyePos = rawViews[e].pose.position;
                            eyePos.x /= zs;
                            eyePos.y /= zs;
                            eyePos.z /= zs;
                            if (e == 0)
                                leftProjMatrix = ComputeKooimaProjection(
                                    eyePos, screenWidthM / zs, screenHeightM / zs, 0.01f, 100.0f);
                            else
                                rightProjMatrix = ComputeKooimaProjection(
                                    eyePos, screenWidthM / zs, screenHeightM / zs, 0.01f, 100.0f);
                            appFov[e] = ComputeKooimaFov(
                                eyePos, screenWidthM / zs, screenHeightM / zs);
                        }
                    }

                    // [Commented out — will be reused for 3D-positioned HUD later]
                    // ConvergencePlane convPlane = LocateConvergencePlane(rawViews);

                    // Render HUD to window-space layer swapchain (once per frame, before eye loop)
                    if (g_inputState.hudVisible && xr.hasHudSwapchain && rs.hudSwapchainImages && !rs.hudSwapchainImages->empty() && rs.hudOk) {
                        uint32_t hudImageIndex;
                        if (AcquireHudSwapchainImage(xr, hudImageIndex)) {
                            std::wstring sessionText = L"Session: ";
                            sessionText += FormatSessionState((int)xr.sessionState);
                            std::wstring modeText = xr.hasWin32WindowBindingExt ?
                                L"XR_EXT_win32_window_binding: ACTIVE (D3D11)" :
                                L"XR_EXT_win32_window_binding: NOT AVAILABLE (D3D11)";
                            if (xr.supportsDisplayModeSwitch) {
                                modeText += g_inputState.displayMode3D ?
                                    L"\nDisplay Mode: 3D Stereo [V=Toggle]" :
                                    L"\nDisplay Mode: 2D Mono [V=Toggle]";
                            }

                            uint32_t dispRenderW, dispRenderH;
                            if (!g_inputState.displayMode3D) {
                                // Mono: full window resolution
                                dispRenderW = g_windowWidth;
                                dispRenderH = g_windowHeight;
                            } else {
                                dispRenderW = (uint32_t)(g_windowWidth * xr.recommendedViewScaleX);
                                dispRenderH = (uint32_t)(g_windowHeight * xr.recommendedViewScaleY);
                            }
                            if (dispRenderW > xr.swapchains[0].width) dispRenderW = xr.swapchains[0].width;
                            if (dispRenderH > xr.swapchains[0].height) dispRenderH = xr.swapchains[0].height;
                            std::wstring perfText = FormatPerformanceInfo(rs.perfStats->fps, rs.perfStats->frameTimeMs,
                                dispRenderW, dispRenderH,
                                g_windowWidth, g_windowHeight);
                            std::wstring dispText = FormatDisplayInfo(xr.displayWidthM, xr.displayHeightM,
                                xr.nominalViewerX, xr.nominalViewerY, xr.nominalViewerZ);
                            std::wstring eyeText = FormatEyeTrackingInfo(
                                xr.leftEyeX, xr.leftEyeY, xr.leftEyeZ,
                                xr.rightEyeX, xr.rightEyeY, xr.rightEyeZ,
                                xr.eyeTrackingActive);

                            uint32_t srcRowPitch = 0;
                            const void* pixels = RenderHudAndMap(*rs.hudRenderer, &srcRowPitch,
                                sessionText, modeText, perfText, dispText, eyeText);
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

                    // Determine mono vs stereo rendering
                    bool monoMode = !g_inputState.displayMode3D;
                    int eyeCount = monoMode ? 1 : 2;

                    // For mono: compute center eye position and projection
                    XMMATRIX monoViewMatrix, monoProjMatrix;
                    XrFovf monoFov = {};
                    XrPosef monoPose = rawViews[0].pose;
                    if (monoMode) {
                        // Center eye = average of left and right eye positions
                        monoPose.position.x = (rawViews[0].pose.position.x + rawViews[1].pose.position.x) * 0.5f;
                        monoPose.position.y = (rawViews[0].pose.position.y + rawViews[1].pose.position.y) * 0.5f;
                        monoPose.position.z = (rawViews[0].pose.position.z + rawViews[1].pose.position.z) * 0.5f;

                        if (useAppProjection) {
                            XrVector3f centerEye = monoPose.position;
                            float pxSizeX = xr.displayWidthM / (float)xr.swapchains[0].width;
                            float pxSizeY = xr.displayHeightM / (float)xr.swapchains[0].height;
                            float winW_m = (float)g_windowWidth * pxSizeX;
                            float winH_m = (float)g_windowHeight * pxSizeY;
                            float minDisp = fminf(xr.displayWidthM, xr.displayHeightM);
                            float minWin  = fminf(winW_m, winH_m);
                            float vs = minDisp / minWin;
                            float screenWidthM  = winW_m * vs;
                            float screenHeightM = winH_m * vs;
                            float zs = g_inputState.zoomScale;
                            centerEye.x /= zs;
                            centerEye.y /= zs;
                            centerEye.z /= zs;
                            monoProjMatrix = ComputeKooimaProjection(
                                centerEye, screenWidthM / zs, screenHeightM / zs, 0.01f, 100.0f);
                            monoFov = ComputeKooimaFov(
                                centerEye, screenWidthM / zs, screenHeightM / zs);
                        } else {
                            // Use average of left/right view/proj matrices
                            monoProjMatrix = leftProjMatrix;  // Close enough for 2D
                            monoFov = rawViews[0].fov;
                        }
                        // View matrix: use LocateViews result for center eye
                        monoViewMatrix = leftViewMatrix;  // Recomputed from center position by LocateViews
                    }

                    // Render each view (1 for mono, 2 for stereo)
                    for (int eye = 0; eye < eyeCount; eye++) {
                        uint32_t imageIndex;
                        if (AcquireSwapchainImage(xr, eye, imageIndex)) {
                            ID3D11Texture2D* swapchainTexture = rs.swapchainImages[eye][imageIndex].texture;

                            ID3D11RenderTargetView* rtv = nullptr;
                            CreateRenderTargetView(renderer, swapchainTexture, &rtv);

                            // Compute render dims: mono uses full window res, stereo uses scaled
                            uint32_t renderW, renderH;
                            if (monoMode) {
                                renderW = g_windowWidth;
                                renderH = g_windowHeight;
                            } else {
                                renderW = (uint32_t)(g_windowWidth * xr.recommendedViewScaleX);
                                renderH = (uint32_t)(g_windowHeight * xr.recommendedViewScaleY);
                            }
                            if (renderW > xr.swapchains[eye].width) renderW = xr.swapchains[eye].width;
                            if (renderH > xr.swapchains[eye].height) renderH = xr.swapchains[eye].height;

                            D3D11_VIEWPORT vp = {};
                            vp.Width = (FLOAT)renderW;
                            vp.Height = (FLOAT)renderH;
                            vp.MaxDepth = 1.0f;
                            renderer.context->RSSetViewports(1, &vp);

                            float clearColor[4] = {0.05f, 0.05f, 0.25f, 1.0f};
                            renderer.context->ClearRenderTargetView(rtv, clearColor);
                            renderer.context->ClearDepthStencilView(rs.depthDSVs[eye].Get(),
                                D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

                            XMMATRIX viewMatrix = monoMode ? monoViewMatrix :
                                ((eye == 0) ? leftViewMatrix : rightViewMatrix);
                            XMMATRIX projMatrix = monoMode ? monoProjMatrix :
                                ((eye == 0) ? leftProjMatrix : rightProjMatrix);

                            // Extension apps: cube base rests on grid at y=0
                            // Cube is 0.06m, so center at y=0.03 puts base at y=0
                            RenderScene(renderer, rtv, rs.depthDSVs[eye].Get(),
                                renderW, renderH,
                                viewMatrix, projMatrix,
                                useAppProjection ? 1.0f : g_inputState.zoomScale,
                                0.03f);  // cubeHeight = 0.03 (half cube size)

                            if (rtv) rtv->Release();

                            ReleaseSwapchainImage(xr, eye);

                            projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                            projectionViews[eye].subImage.swapchain = xr.swapchains[eye].swapchain;
                            projectionViews[eye].subImage.imageRect.offset = {0, 0};
                            projectionViews[eye].subImage.imageRect.extent = {
                                (int32_t)renderW,
                                (int32_t)renderH
                            };
                            projectionViews[eye].subImage.imageArrayIndex = 0;

                            projectionViews[eye].pose = monoMode ? monoPose : rawViews[eye].pose;
                            projectionViews[eye].fov = monoMode ? monoFov :
                                (useAppProjection ? appFov[eye] : rawViews[eye].fov);
                        }
                    }
                }
            }

            // Submit frame with window-space HUD layer if visible
            // submitViewCount: 1 for mono (2D mode), 2 for stereo (3D mode)
            uint32_t submitViewCount = g_inputState.displayMode3D ? 2 : 1;
            if (hudSubmitted) {
                float hudAR = (float)HUD_PIXEL_WIDTH / (float)HUD_PIXEL_HEIGHT;
                float windowAR = (g_windowWidth > 0 && g_windowHeight > 0) ? (float)g_windowWidth / (float)g_windowHeight : 1.0f;
                float fracW = HUD_WIDTH_FRACTION;
                float fracH = fracW * windowAR / hudAR;
                if (fracH > 1.0f) { fracH = 1.0f; fracW = hudAR / windowAR; }
                EndFrameWithWindowSpaceHud(xr, frameState.predictedDisplayTime, projectionViews,
                    0.0f, 0.0f, fracW, fracH, 0.0f, submitViewCount);
            } else {
                EndFrame(xr, frameState.predictedDisplayTime, projectionViews, submitViewCount);
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

    // Add SRMonado to DLL search path
    {
        HKEY hKey;
        char installPath[MAX_PATH] = {0};
        DWORD pathSize = sizeof(installPath);
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\LeiaSR\\SRMonado", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            if (RegQueryValueExA(hKey, "InstallPath", nullptr, nullptr, (LPBYTE)installPath, &pathSize) == ERROR_SUCCESS) {
                LOG_INFO("Adding SRMonado install path to DLL search: %s", installPath);
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

    // Create swapchains — pass window dimensions as minimum so the swapchain
    // is large enough for mono (2D) rendering at full window resolution.
    if (!CreateSwapchains(xr, g_windowWidth, g_windowHeight)) {
        LOG_ERROR("Swapchain creation failed");
        CleanupOpenXR(xr);
        if (hudOk) CleanupHudRenderer(hudRenderer);
        CleanupD3D11(renderer);
        ShutdownLogging();
        return 1;
    }

    // Enumerate D3D11 swapchain images (now done per-app since common is API-agnostic)
    std::vector<XrSwapchainImageD3D11KHR> swapchainImages[2];
    for (int eye = 0; eye < 2; eye++) {
        uint32_t count = xr.swapchains[eye].imageCount;
        swapchainImages[eye].resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        xrEnumerateSwapchainImages(xr.swapchains[eye].swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)swapchainImages[eye].data());
        LOG_INFO("Eye %d: enumerated %u D3D11 swapchain images", eye, count);
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

    // Create depth buffers for each eye
    ComPtr<ID3D11Texture2D> depthTextures[2];
    ComPtr<ID3D11DepthStencilView> depthDSVs[2];

    for (int eye = 0; eye < 2; eye++) {
        ID3D11Texture2D* depthTex = nullptr;
        ID3D11DepthStencilView* dsv = nullptr;
        if (!CreateDepthStencilView(renderer, xr.swapchains[eye].width, xr.swapchains[eye].height, &depthTex, &dsv)) {
            LOG_ERROR("Failed to create depth buffer for eye %d", eye);
            CleanupOpenXR(xr);
            if (hudOk) CleanupHudRenderer(hudRenderer);
            CleanupD3D11(renderer);
            ShutdownLogging();
            return 1;
        }
        depthTextures[eye].Attach(depthTex);
        depthDSVs[eye].Attach(dsv);
    }

    LOG_INFO("");
    LOG_INFO("=== Entering main loop ===");
    LOG_INFO("XR rendering happens in the application window (XR_EXT_win32_window_binding)");
    LOG_INFO("Single-threaded: message pump + render on the main thread (WM_PAINT during drag/resize)");
    LOG_INFO("Controls: WASD=Fly, QE=Up/Down, Mouse=Look, Space/DblClick=Reset, P=Parallax, V=2D/3D, TAB=HUD, F11=Fullscreen, ESC=Quit");
    LOG_INFO("");

    PerformanceStats perfStats = {};
    perfStats.lastTime = std::chrono::high_resolution_clock::now();

    RenderState rs = {};
    rs.hwnd = hwnd;
    rs.xr = &xr;
    rs.renderer = &renderer;
    rs.hudRenderer = &hudRenderer;
    rs.hudOk = hudOk;
    rs.hudSwapchainImages = &hudSwapchainImages;
    rs.depthTextures = depthTextures;
    rs.depthDSVs = depthDSVs;
    rs.swapchainImages = swapchainImages;
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

    for (int eye = 0; eye < 2; eye++) {
        depthDSVs[eye].Reset();
        depthTextures[eye].Reset();
    }

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
