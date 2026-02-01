// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  SR Cube OpenXR Ext - OpenXR with XR_EXT_session_target extension
 *
 * This application demonstrates OpenXR with the XR_EXT_session_target extension.
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
static const wchar_t* WINDOW_TITLE = L"SR Cube OpenXR Ext - XR_EXT_session_target (Press ESC to exit)";

// Global state (single-threaded — all accessed from the main thread only)
static InputState g_inputState;
static bool g_running = true;
static UINT g_windowWidth = 1280;
static UINT g_windowHeight = 720;
static bool g_inSizeMove = false;  // True while user is dragging/resizing the window
static const uint32_t HUD_PIXEL_WIDTH = 380;
static const uint32_t HUD_PIXEL_HEIGHT = 280;

// Forward declaration — defined after PerformanceStats
struct RenderState;
static RenderState* g_renderState = nullptr;
static void RenderOneFrame(RenderState& rs);

// Window procedure (runs on main thread — single-threaded, no locking needed)
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    UpdateInputState(g_inputState, msg, wParam, lParam);

    switch (msg) {
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
        g_running = false;
        PostQuitMessage(0);
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            g_running = false;
            PostQuitMessage(0);
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
    TextOverlay* textOverlay;
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

    // Clear remaining one-shot flags
    g_inputState.fullscreenToggleRequested = false;

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
                    g_inputState.yaw, g_inputState.pitch)) {

                    // Get raw view poses (pre-player-transform) for projection views
                    XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
                    locateInfo.viewConfigurationType = xr.viewConfigType;
                    locateInfo.displayTime = frameState.predictedDisplayTime;
                    locateInfo.space = xr.localSpace;

                    XrViewState viewState = {XR_TYPE_VIEW_STATE};
                    uint32_t viewCount = 2;
                    XrView rawViews[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
                    xrLocateViews(xr.session, &locateInfo, &viewState, 2, &viewCount, rawViews);

                    // [Commented out — will be reused for 3D-positioned HUD later]
                    // ConvergencePlane convPlane = LocateConvergencePlane(rawViews);

                    // Render HUD to window-space layer swapchain (once per frame, before eye loop)
                    if (g_inputState.hudVisible && xr.hasHudSwapchain && rs.hudSwapchainImages && !rs.hudSwapchainImages->empty()) {
                        uint32_t hudImageIndex;
                        if (AcquireHudSwapchainImage(xr, hudImageIndex)) {
                            ID3D11Texture2D* hudTexture = (*rs.hudSwapchainImages)[hudImageIndex].texture;

                            ID3D11RenderTargetView* hudRtv = nullptr;
                            CreateRenderTargetView(renderer, hudTexture, &hudRtv);
                            if (hudRtv) {
                                float hudClear[4] = {0.0f, 0.0f, 0.0f, 0.7f};
                                renderer.context->ClearRenderTargetView(hudRtv, hudClear);
                                hudRtv->Release();
                            }

                            float px = 12.0f; // left padding
                            float tw = (float)xr.hudSwapchain.width - 2 * px; // text width

                            std::wstring stateText = L"Session: ";
                            stateText += FormatSessionState((int)xr.sessionState);
                            RenderText(*rs.textOverlay, renderer.device.Get(), hudTexture,
                                stateText, px, 12, tw, 26);

                            std::wstring extText = xr.hasSessionTargetExt ?
                                L"XR_EXT_session_target: ACTIVE" :
                                L"XR_EXT_session_target: NOT AVAILABLE";
                            RenderText(*rs.textOverlay, renderer.device.Get(), hudTexture,
                                extText, px, 42, tw, 22, true);

                            std::wstring perfText = FormatPerformanceInfo(rs.perfStats->fps, rs.perfStats->frameTimeMs,
                                xr.recommendedRenderWidth, xr.recommendedRenderHeight,
                                g_windowWidth, g_windowHeight);
                            RenderText(*rs.textOverlay, renderer.device.Get(), hudTexture,
                                perfText, px, 74, tw, 88, true);

                            std::wstring eyeText = FormatEyeTrackingInfo(xr.eyePosX, xr.eyePosY, xr.eyePosZ, xr.eyeTrackingActive);
                            RenderText(*rs.textOverlay, renderer.device.Get(), hudTexture,
                                eyeText, px, 172, tw, 88, true);

                            ReleaseHudSwapchainImage(xr);
                            hudSubmitted = true;
                        }
                    }

                    // Render each eye
                    for (int eye = 0; eye < 2; eye++) {
                        uint32_t imageIndex;
                        if (AcquireSwapchainImage(xr, eye, imageIndex)) {
                            ID3D11Texture2D* swapchainTexture = rs.swapchainImages[eye][imageIndex].texture;

                            ID3D11RenderTargetView* rtv = nullptr;
                            CreateRenderTargetView(renderer, swapchainTexture, &rtv);

                            // Use recommended render dims (may be smaller than swapchain after resize)
                            uint32_t renderW = xr.recommendedRenderWidth;
                            uint32_t renderH = xr.recommendedRenderHeight;

                            D3D11_VIEWPORT vp = {};
                            vp.Width = (FLOAT)renderW;
                            vp.Height = (FLOAT)renderH;
                            vp.MaxDepth = 1.0f;
                            renderer.context->RSSetViewports(1, &vp);

                            float clearColor[4] = {0.05f, 0.05f, 0.25f, 1.0f};
                            renderer.context->ClearRenderTargetView(rtv, clearColor);
                            renderer.context->ClearDepthStencilView(rs.depthDSVs[eye].Get(),
                                D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

                            XMMATRIX viewMatrix = (eye == 0) ? leftViewMatrix : rightViewMatrix;
                            XMMATRIX projMatrix = (eye == 0) ? leftProjMatrix : rightProjMatrix;

                            RenderScene(renderer, rtv, rs.depthDSVs[eye].Get(),
                                renderW, renderH,
                                viewMatrix, projMatrix,
                                g_inputState.zoomScale);

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

                            projectionViews[eye].pose = rawViews[eye].pose;
                            projectionViews[eye].fov = rawViews[eye].fov;
                        }
                    }
                }
            }

            // Submit frame with window-space HUD layer if visible
            if (hudSubmitted) {
                float hudWidthFrac = (float)HUD_PIXEL_WIDTH / xr.swapchains[0].width;
                float hudHeightFrac = (float)HUD_PIXEL_HEIGHT / xr.swapchains[0].height;
                EndFrameWithWindowSpaceHud(xr, frameState.predictedDisplayTime, projectionViews,
                    0.0f, 0.0f, hudWidthFrac, hudHeightFrac, 0.0f);
            } else {
                EndFrame(xr, frameState.predictedDisplayTime, projectionViews);
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
    LOG_INFO("OpenXR with XR_EXT_session_target extension");
    LOG_INFO("Application creates and controls its own window");

    // Create window FIRST (needed for XR_EXT_session_target)
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
    if (!InitializeOpenXR(xr)) {
        LOG_ERROR("OpenXR initialization failed");
        MessageBox(hwnd, L"Failed to initialize OpenXR", L"Error", MB_OK | MB_ICONERROR);
        ShutdownLogging();
        return 1;
    }

    // Check for session target extension
    if (!xr.hasSessionTargetExt) {
        LOG_WARN("XR_EXT_session_target not available - runtime will create its own window");
        MessageBox(hwnd, L"XR_EXT_session_target extension not available.\nRuntime will create its own window.",
            L"Warning", MB_OK | MB_ICONWARNING);
    } else {
        LOG_INFO("XR_EXT_session_target extension is available - using app window");
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

    // Initialize text overlay
    TextOverlay textOverlay = {};
    if (!InitializeTextOverlay(textOverlay)) {
        LOG_ERROR("Text overlay initialization failed");
        CleanupD3D11(renderer);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create OpenXR session WITH window handle (XR_EXT_session_target)
    LOG_INFO("Creating OpenXR session with XR_EXT_session_target (HWND: 0x%p)...", hwnd);
    if (!CreateSession(xr, renderer.device.Get(), hwnd)) {
        LOG_ERROR("OpenXR session creation failed");
        MessageBox(hwnd, L"Failed to create OpenXR session", L"Error", MB_OK | MB_ICONERROR);
        CleanupTextOverlay(textOverlay);
        CleanupD3D11(renderer);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create reference spaces
    if (!CreateSpaces(xr)) {
        LOG_ERROR("Reference space creation failed");
        CleanupOpenXR(xr);
        CleanupTextOverlay(textOverlay);
        CleanupD3D11(renderer);
        ShutdownLogging();
        return 1;
    }

    // Create swapchains
    if (!CreateSwapchains(xr)) {
        LOG_ERROR("Swapchain creation failed");
        CleanupOpenXR(xr);
        CleanupTextOverlay(textOverlay);
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
            CleanupTextOverlay(textOverlay);
            CleanupD3D11(renderer);
            ShutdownLogging();
            return 1;
        }
        depthTextures[eye].Attach(depthTex);
        depthDSVs[eye].Attach(dsv);
    }

    LOG_INFO("");
    LOG_INFO("=== Entering main loop ===");
    LOG_INFO("XR rendering happens in the application window (XR_EXT_session_target)");
    LOG_INFO("Single-threaded: message pump + render on the main thread (WM_PAINT during drag/resize)");
    LOG_INFO("Controls: WASD=Fly, QE=Up/Down, Mouse=Look, Space/DblClick=Reset, P=Parallax, TAB=HUD, ESC=Quit");
    LOG_INFO("");

    PerformanceStats perfStats = {};
    perfStats.lastTime = std::chrono::high_resolution_clock::now();

    RenderState rs = {};
    rs.hwnd = hwnd;
    rs.xr = &xr;
    rs.renderer = &renderer;
    rs.textOverlay = &textOverlay;
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

    CleanupOpenXR(xr);
    CleanupTextOverlay(textOverlay);
    CleanupD3D11(renderer);

    DestroyWindow(hwnd);
    UnregisterClass(WINDOW_CLASS, hInstance);

    LOG_INFO("Application shutdown complete");
    ShutdownLogging();

    return 0;
}
