// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Spatial OS Demo - "Collaboration War Room"
 *
 * Demonstrates the lightfield display's ability to show depth-aware UI alongside
 * flat content. Multiple floating panels (2D text via DirectWrite) surround a
 * center 3D stereo scene, all composited via OpenXR multi-layer submission.
 *
 * Layers:
 *   - XrCompositionLayerProjection (center 3D cube, stereo)
 *   - 4x XrCompositionLayerWindowSpaceEXT (2D panels: participants, chat, agenda, actions)
 *
 * Controls:
 *   Tab     = cycle selected panel
 *   Space   = focus mode (enlarge selected panel)
 *   3       = toggle selected panel disparity (promote to 3D)
 *   Escape  = reset layout
 *   WASD/QE = fly camera (3D scene)
 *   Mouse   = look around (3D scene)
 *   V       = toggle 2D/3D display mode
 *   F11     = fullscreen
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <wrl/client.h>

#include "logging.h"
#include "input_handler.h"
#include "d3d11_renderer.h"
#include "xr_session.h"
#include "spatial_app.h"
#include "panel_render.h"
#include "display3d_view.h"
#include "camera3d_view.h"

#include <chrono>
#include <cmath>
#include <string>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

static const char* APP_NAME = "spatial_os";
static const wchar_t* WINDOW_CLASS = L"SpatialOSClass";
static const wchar_t* WINDOW_TITLE = L"Spatial OS - Collaboration War Room (Tab=Select, Space=Focus, 3=Depth, ESC=Reset)";
static const float CAMERA_HALF_TAN_VFOV = 0.32491969623f; // tan(18 deg)

// Global state
static InputState g_inputState;
static bool g_running = true;
static XrSessionManager* g_xr = nullptr;
static UINT g_windowWidth = 1280;
static UINT g_windowHeight = 720;
static bool g_inSizeMove = false;
static SpatialAppState g_app;

// Fullscreen state
static bool g_fullscreen = false;
static RECT g_savedWindowRect = {};
static DWORD g_savedWindowStyle = 0;

struct RenderState;
static RenderState* g_renderState = nullptr;
static void RenderOneFrame(RenderState& rs);

// Panel content — static war room data
static const wchar_t* PARTICIPANTS_BODY =
    L"  Alice Chen (Host)\n"
    L"  Bob Martinez\n"
    L"  Carol Wu\n"
    L"  David Kim\n"
    L"  Eva Johansson";

static const wchar_t* CHAT_BODY =
    L"Bob: Updated the CAD model\n"
    L"Carol: Looks good, checking\n   structural loads now\n"
    L"Alice: Let's review the\n   foundation plan\n"
    L"David: I'll pull up the\n   site survey data";

static const wchar_t* AGENDA_BODY =
    L"1. Site survey review\n"
    L"2. Structural analysis\n"
    L"3. 3D model walkthrough\n"
    L"4. Timeline update\n"
    L"5. Action items";

static const wchar_t* ACTION_ITEMS_BODY =
    L"  Bob: Finalize structural model by Friday  |  "
    L"Carol: Run load simulation on updated design  |  "
    L"David: Schedule site visit for next week  |  "
    L"Eva: Prepare cost estimate for Phase 2";

static void ToggleFullscreen(HWND hwnd) {
    if (g_fullscreen) {
        SetWindowLong(hwnd, GWL_STYLE, g_savedWindowStyle);
        SetWindowPos(hwnd, HWND_TOP,
            g_savedWindowRect.left, g_savedWindowRect.top,
            g_savedWindowRect.right - g_savedWindowRect.left,
            g_savedWindowRect.bottom - g_savedWindowRect.top,
            SWP_FRAMECHANGED);
        g_fullscreen = false;
    } else {
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
    }
}

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
        if (g_inSizeMove && g_renderState != nullptr) {
            RenderOneFrame(*g_renderState);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_CLOSE:
        if (g_xr && g_xr->session != XR_NULL_HANDLE && g_xr->sessionRunning) {
            xrRequestExitSession(g_xr->session);
            return 0;
        }
        g_running = false;
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        switch (wParam) {
        case VK_ESCAPE:
            if (g_app.focusMode || g_app.selectedIndex >= 0) {
                HandleEscapeReset(g_app);
            } else {
                PostMessage(hwnd, WM_CLOSE, 0, 0);
            }
            return 0;
        case VK_TAB:
            HandleTabKey(g_app);
            return 0;
        case VK_SPACE:
            HandleSpaceKey(g_app);
            return 0;
        case '3':
            HandleDisparityToggle(g_app);
            return 0;
        }
        break;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static HWND CreateAppWindow(HINSTANCE hInstance, int width, int height) {
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
            LOG_ERROR("Failed to register window class: %lu", err);
            return nullptr;
        }
    }

    RECT rect = { 0, 0, width, height };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindowEx(0, WINDOW_CLASS, WINDOW_TITLE, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd) LOG_ERROR("Failed to create window: %lu", GetLastError());
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

struct RenderState {
    HWND hwnd;
    XrSessionManager* xr;
    D3D11Renderer* renderer;
    PanelRenderer* panelRenderer;
    ComPtr<ID3D11Texture2D> depthTexture;
    ComPtr<ID3D11DepthStencilView> depthDSV;
    std::vector<XrSwapchainImageD3D11KHR>* swapchainImages;
    PerformanceStats* perfStats;
};

static const wchar_t* GetPanelBody(PanelType type) {
    switch (type) {
    case PanelType::Participants: return PARTICIPANTS_BODY;
    case PanelType::Chat:         return CHAT_BODY;
    case PanelType::Agenda:       return AGENDA_BODY;
    case PanelType::ActionItems:  return ACTION_ITEMS_BODY;
    default: return L"";
    }
}

static void RenderOneFrame(RenderState& rs) {
    XrSessionManager& xr = *rs.xr;
    D3D11Renderer& renderer = *rs.renderer;

    UpdatePerformanceStats(*rs.perfStats);
    UpdateCameraMovement(g_inputState, rs.perfStats->deltaTime, rs.xr->displayHeightM);
    UpdatePanelAnimations(g_app, rs.perfStats->deltaTime);

    if (g_inputState.fullscreenToggleRequested) {
        ToggleFullscreen(rs.hwnd);
        g_inputState.fullscreenToggleRequested = false;
    }

    if (g_inputState.displayModeToggleRequested) {
        g_inputState.displayModeToggleRequested = false;
        if (xr.pfnRequestDisplayModeEXT && xr.session != XR_NULL_HANDLE) {
            XrDisplayModeEXT mode = g_inputState.displayMode3D ?
                XR_DISPLAY_MODE_3D_EXT : XR_DISPLAY_MODE_2D_EXT;
            xr.pfnRequestDisplayModeEXT(xr.session, mode);
        }
    }

    UpdateScene(renderer, rs.perfStats->deltaTime);
    PollEvents(xr);

    if (xr.sessionRunning) {
        XrFrameState frameState;
        if (BeginFrame(xr, frameState)) {
            XrCompositionLayerProjectionView projectionViews[2] = {};

            if (frameState.shouldRender) {
                XMMATRIX leftViewMatrix, leftProjMatrix;
                XMMATRIX rightViewMatrix, rightProjMatrix;

                if (LocateViews(xr, frameState.predictedDisplayTime,
                    leftViewMatrix, leftProjMatrix, rightViewMatrix, rightProjMatrix,
                    g_inputState.cameraPosX, g_inputState.cameraPosY, g_inputState.cameraPosZ,
                    g_inputState.yaw, g_inputState.pitch, g_inputState.stereo)) {

                    // Raw view poses for projection views
                    XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
                    locateInfo.viewConfigurationType = xr.viewConfigType;
                    locateInfo.displayTime = frameState.predictedDisplayTime;
                    locateInfo.space = (xr.displaySpace != XR_NULL_HANDLE) ? xr.displaySpace : xr.localSpace;
                    XrViewState viewState = {XR_TYPE_VIEW_STATE};
                    uint32_t viewCount = 2;
                    XrView rawViews[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
                    xrLocateViews(xr.session, &locateInfo, &viewState, 2, &viewCount, rawViews);

                    // App-side Kooima projection
                    uint32_t eyeRenderW = xr.swapchain.width / 2;
                    uint32_t eyeRenderH = xr.swapchain.height;

                    Display3DStereoView stereoViews[2];
                    bool useAppProjection = (xr.hasDisplayInfoExt && xr.displayWidthM > 0.0f);
                    if (useAppProjection) {
                        float pxSizeX = xr.displayWidthM / (float)xr.swapchain.width;
                        float pxSizeY = xr.displayHeightM / (float)xr.swapchain.height;
                        float winW_m = (float)g_windowWidth * pxSizeX;
                        float winH_m = (float)g_windowHeight * pxSizeY;
                        float minDisp = fminf(xr.displayWidthM, xr.displayHeightM);
                        float minWin = fminf(winW_m, winH_m);
                        float vs = minDisp / minWin;

                        XrVector3f rawLeft = rawViews[0].pose.position;
                        XrVector3f rawRight = rawViews[1].pose.position;
                        if (!g_inputState.displayMode3D) {
                            XrVector3f center = {
                                (rawLeft.x + rawRight.x) * 0.5f,
                                (rawLeft.y + rawRight.y) * 0.5f,
                                (rawLeft.z + rawRight.z) * 0.5f};
                            rawLeft = rawRight = center;
                        }

                        XrPosef cameraPose;
                        XMVECTOR pOri = XMQuaternionRotationRollPitchYaw(g_inputState.pitch, g_inputState.yaw, 0);
                        XMFLOAT4 q;
                        XMStoreFloat4(&q, pOri);
                        cameraPose.orientation = {q.x, q.y, q.z, q.w};
                        cameraPose.position = {g_inputState.cameraPosX, g_inputState.cameraPosY, g_inputState.cameraPosZ};

                        XrVector3f nominalViewer = {xr.nominalViewerX, xr.nominalViewerY, xr.nominalViewerZ};
                        Display3DScreen screen = {winW_m * vs, winH_m * vs};

                        if (g_inputState.cameraMode) {
                            Camera3DTunables camTunables;
                            camTunables.ipd_factor = g_inputState.stereo.ipdFactor;
                            camTunables.parallax_factor = g_inputState.stereo.parallaxFactor;
                            camTunables.inv_convergence_distance = g_inputState.stereo.invConvergenceDistance;
                            camTunables.half_tan_vfov = CAMERA_HALF_TAN_VFOV / g_inputState.stereo.zoomFactor;

                            Camera3DStereoView camViews[2];
                            camera3d_compute_stereo_views(
                                &rawLeft, &rawRight, &nominalViewer,
                                &screen, &camTunables, &cameraPose,
                                0.01f, 100.0f, &camViews[0], &camViews[1]);

                            for (int i = 0; i < 2; i++) {
                                memcpy(stereoViews[i].view_matrix, camViews[i].view_matrix, sizeof(float) * 16);
                                memcpy(stereoViews[i].projection_matrix, camViews[i].projection_matrix, sizeof(float) * 16);
                                stereoViews[i].fov = camViews[i].fov;
                                stereoViews[i].eye_world = camViews[i].eye_world;
                            }
                        } else {
                            Display3DTunables tunables;
                            tunables.ipd_factor = g_inputState.stereo.ipdFactor;
                            tunables.parallax_factor = g_inputState.stereo.parallaxFactor;
                            tunables.perspective_factor = g_inputState.stereo.perspectiveFactor;
                            tunables.virtual_display_height = g_inputState.stereo.virtualDisplayHeight / g_inputState.stereo.scaleFactor;

                            display3d_compute_stereo_views(
                                &rawLeft, &rawRight, &nominalViewer,
                                &screen, &tunables, &cameraPose,
                                0.01f, 100.0f, &stereoViews[0], &stereoViews[1]);
                        }
                    }

                    // Render 2D panels to their swapchains
                    for (int i = 0; i < NUM_PANELS; i++) {
                        Panel& panel = g_app.panels[i];
                        if (panel.type == PanelType::Scene3D) continue;
                        if (panel.swapchainInfo.swapchain == XR_NULL_HANDLE) continue;

                        uint32_t panelImageIndex;
                        if (AcquirePanelSwapchainImage(panel, panelImageIndex)) {
                            ID3D11Texture2D* panelTexture = panel.swapchainImages[panelImageIndex].texture;
                            RenderPanelContent(*rs.panelRenderer, renderer.device.Get(), panelTexture,
                                panel.title.c_str(), GetPanelBody(panel.type),
                                panel.bgColor, panel.accentColor, panel.alpha, panel.selected);
                            ReleasePanelSwapchainImage(panel);
                        }
                    }

                    // Render center 3D scene
                    bool monoMode = !g_inputState.displayMode3D;
                    int eyeCount = monoMode ? 1 : 2;

                    uint32_t imageIndex;
                    if (AcquireSwapchainImage(xr, imageIndex)) {
                        ID3D11Texture2D* swapchainTexture = (*rs.swapchainImages)[imageIndex].texture;
                        ID3D11RenderTargetView* rtv = nullptr;
                        CreateRenderTargetView(renderer, swapchainTexture, &rtv);

                        float clearColor[4] = {0.08f, 0.08f, 0.12f, 1.0f};
                        renderer.context->ClearRenderTargetView(rtv, clearColor);
                        renderer.context->ClearDepthStencilView(rs.depthDSV.Get(),
                            D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

                        uint32_t renderW, renderH;
                        if (monoMode) {
                            renderW = g_windowWidth;
                            renderH = g_windowHeight;
                            if (renderW > xr.swapchain.width) renderW = xr.swapchain.width;
                            if (renderH > xr.swapchain.height) renderH = xr.swapchain.height;
                        } else {
                            renderW = (uint32_t)(g_windowWidth * xr.recommendedViewScaleX);
                            renderH = (uint32_t)(g_windowHeight * xr.recommendedViewScaleY);
                            if (renderW > eyeRenderW) renderW = eyeRenderW;
                            if (renderH > eyeRenderH) renderH = eyeRenderH;
                        }

                        for (int eye = 0; eye < eyeCount; eye++) {
                            D3D11_VIEWPORT vp = {};
                            vp.TopLeftX = monoMode ? 0.0f : (FLOAT)(eye * renderW);
                            vp.Width = (FLOAT)renderW;
                            vp.Height = (FLOAT)renderH;
                            vp.MaxDepth = 1.0f;
                            renderer.context->RSSetViewports(1, &vp);

                            XMMATRIX viewMatrix, projMatrix;
                            if (useAppProjection) {
                                int vi = monoMode ? 0 : eye;
                                viewMatrix = ColumnMajorToXMMatrix(stereoViews[vi].view_matrix);
                                projMatrix = ColumnMajorToXMMatrix(stereoViews[vi].projection_matrix);
                            } else {
                                viewMatrix = (eye == 0) ? leftViewMatrix : rightViewMatrix;
                                projMatrix = (eye == 0) ? leftProjMatrix : rightProjMatrix;
                            }

                            RenderScene(renderer, rtv, rs.depthDSV.Get(),
                                renderW, renderH, viewMatrix, projMatrix,
                                useAppProjection ? 1.0f : g_inputState.stereo.scaleFactor, 0.03f);

                            projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                            projectionViews[eye].subImage.swapchain = xr.swapchain.swapchain;
                            projectionViews[eye].subImage.imageRect.offset = {
                                (int32_t)(monoMode ? 0 : eye * renderW), 0};
                            projectionViews[eye].subImage.imageRect.extent = {
                                (int32_t)renderW, (int32_t)renderH};
                            projectionViews[eye].subImage.imageArrayIndex = 0;
                            projectionViews[eye].pose = rawViews[monoMode ? 0 : eye].pose;
                            projectionViews[eye].fov = useAppProjection ?
                                stereoViews[monoMode ? 0 : eye].fov : rawViews[monoMode ? 0 : eye].fov;
                        }

                        if (rtv) rtv->Release();
                        ReleaseSwapchainImage(xr);
                    }
                }
            }

            // Submit all layers
            uint32_t submitViewCount = g_inputState.displayMode3D ? 2 : 1;
            EndFrameMultiLayer(xr, frameState.predictedDisplayTime, projectionViews,
                submitViewCount, g_app.panels, NUM_PANELS);
        }
    } else {
        Sleep(100);
    }
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    if (!InitializeLogging(APP_NAME)) {
        MessageBox(nullptr, L"Failed to initialize logging", L"Warning", MB_OK | MB_ICONWARNING);
    }

    LOG_INFO("=== Spatial OS - Collaboration War Room ===");

    HWND hwnd = CreateAppWindow(hInstance, g_windowWidth, g_windowHeight);
    if (!hwnd) {
        ShutdownLogging();
        return 1;
    }

    // SRMonado DLL search path
    {
        HKEY hKey;
        char installPath[MAX_PATH] = {0};
        DWORD pathSize = sizeof(installPath);
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\LeiaSR\\SRMonado", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            if (RegQueryValueExA(hKey, "InstallPath", nullptr, nullptr, (LPBYTE)installPath, &pathSize) == ERROR_SUCCESS) {
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
        MessageBox(hwnd, L"Failed to initialize OpenXR", L"Error", MB_OK | MB_ICONERROR);
        g_xr = nullptr;
        ShutdownLogging();
        return 1;
    }

    // Initialize D3D11
    LUID adapterLuid;
    if (!GetD3D11GraphicsRequirements(xr, &adapterLuid)) {
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    D3D11Renderer renderer = {};
    if (!InitializeD3D11WithLUID(renderer, adapterLuid)) {
        LOG_ERROR("D3D11 initialization failed");
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Initialize panel renderer (D2D + DirectWrite)
    PanelRenderer panelRenderer = {};
    if (!InitializePanelRenderer(panelRenderer)) {
        LOG_ERROR("Panel renderer initialization failed");
        CleanupD3D11(renderer);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create OpenXR session
    if (!CreateSession(xr, renderer.device.Get(), hwnd)) {
        LOG_ERROR("Session creation failed");
        CleanupPanelRenderer(panelRenderer);
        CleanupD3D11(renderer);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    if (!CreateSpaces(xr)) {
        CleanupOpenXR(xr);
        CleanupPanelRenderer(panelRenderer);
        CleanupD3D11(renderer);
        ShutdownLogging();
        return 1;
    }

    if (!CreateSwapchain(xr)) {
        CleanupOpenXR(xr);
        CleanupPanelRenderer(panelRenderer);
        CleanupD3D11(renderer);
        ShutdownLogging();
        return 1;
    }

    // Enumerate projection swapchain images
    std::vector<XrSwapchainImageD3D11KHR> swapchainImages;
    {
        uint32_t count = xr.swapchain.imageCount;
        swapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)swapchainImages.data());
    }

    // Initialize panels and create their swapchains
    InitializePanels(g_app);

    // Panel pixel sizes based on window dimensions
    uint32_t panelPixelSizes[][2] = {
        {320, 200},  // Participants
        {320, 200},  // Chat
        {320, 190},  // Agenda
        {800, 150},  // Action Items
        {0, 0},      // Scene3D (no swapchain needed)
    };

    for (int i = 0; i < NUM_PANELS; i++) {
        if (g_app.panels[i].type == PanelType::Scene3D) continue;
        if (!CreatePanelSwapchain(xr, g_app.panels[i], panelPixelSizes[i][0], panelPixelSizes[i][1])) {
            LOG_ERROR("Failed to create swapchain for panel '%ls'", g_app.panels[i].title.c_str());
        }
    }

    // Depth buffer
    ComPtr<ID3D11Texture2D> depthTexture;
    ComPtr<ID3D11DepthStencilView> depthDSV;
    {
        ID3D11Texture2D* depthTex = nullptr;
        ID3D11DepthStencilView* dsv = nullptr;
        if (!CreateDepthStencilView(renderer, xr.swapchain.width, xr.swapchain.height, &depthTex, &dsv)) {
            LOG_ERROR("Failed to create depth buffer");
            CleanupOpenXR(xr);
            CleanupPanelRenderer(panelRenderer);
            CleanupD3D11(renderer);
            ShutdownLogging();
            return 1;
        }
        depthTexture.Attach(depthTex);
        depthDSV.Attach(dsv);
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    LOG_INFO("=== Entering main loop ===");
    LOG_INFO("Controls: Tab=Select, Space=Focus, 3=Depth, ESC=Reset, WASD=Fly, V=2D/3D, F11=Fullscreen");

    PerformanceStats perfStats = {};
    perfStats.lastTime = std::chrono::high_resolution_clock::now();
    g_inputState.stereo.virtualDisplayHeight = 0.24f;
    g_inputState.nominalViewerZ = xr.nominalViewerZ;

    RenderState rs = {};
    rs.hwnd = hwnd;
    rs.xr = &xr;
    rs.renderer = &renderer;
    rs.panelRenderer = &panelRenderer;
    rs.depthTexture = depthTexture;
    rs.depthDSV = depthDSV;
    rs.swapchainImages = &swapchainImages;
    rs.perfStats = &perfStats;
    g_renderState = &rs;

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

    LOG_INFO("=== Shutting down ===");
    depthDSV.Reset();
    depthTexture.Reset();
    g_xr = nullptr;
    CleanupOpenXR(xr);
    CleanupPanelRenderer(panelRenderer);
    CleanupD3D11(renderer);
    DestroyWindow(hwnd);
    UnregisterClass(WINDOW_CLASS, hInstance);
    ShutdownLogging();
    return 0;
}
