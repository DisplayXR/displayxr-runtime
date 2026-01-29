// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  SR Cube OpenXR - Standard OpenXR mode (Monado creates window)
 *
 * This application demonstrates OpenXR without the XR_EXT_session_target extension.
 * Monado will create its own window for rendering.
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
static const char* APP_NAME = "sr_cube_openxr";

// Window settings (control window only - not for XR rendering)
static const wchar_t* WINDOW_CLASS = L"SRCubeOpenXRClass";
static const wchar_t* WINDOW_TITLE = L"SR Cube OpenXR - Control Window (Press ESC to exit)";

// Global state
static InputState g_inputState;
static bool g_running = true;
static UINT g_windowWidth = 640;
static UINT g_windowHeight = 480;

// Window procedure (control window only)
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    UpdateInputState(g_inputState, msg, wParam, lParam);

    switch (msg) {
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

// Create a small control window (not for XR rendering)
static HWND CreateControlWindow(HINSTANCE hInstance, int width, int height) {
    LOG_INFO("Creating control window (%dx%d) - NOT used for XR rendering", width, height);

    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(GRAY_BRUSH);
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
        100, 100,
        rect.right - rect.left,
        rect.bottom - rect.top,
        nullptr,
        nullptr,
        hInstance,
        nullptr
    );

    if (!hwnd) {
        LOG_ERROR("Failed to create control window, error: %lu", GetLastError());
        return nullptr;
    }

    LOG_INFO("Control window created: 0x%p", hwnd);
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

// Main entry point
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    // Initialize logging
    if (!InitializeLogging(APP_NAME)) {
        MessageBox(nullptr, L"Failed to initialize logging", L"Warning", MB_OK | MB_ICONWARNING);
    }

    LOG_INFO("=== SR Cube OpenXR Application ===");
    LOG_INFO("OpenXR standard mode (Monado creates window)");

    // Create control window
    HWND hwnd = CreateControlWindow(hInstance, g_windowWidth, g_windowHeight);
    if (!hwnd) {
        LOG_ERROR("Failed to create control window");
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

    // Create OpenXR session (Monado creates window)
    LOG_INFO("Creating OpenXR session...");
    if (!CreateSession(xr, renderer.device.Get())) {
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

    // Create quad layer swapchain for UI overlay (512x256 pixels)
    const uint32_t QUAD_UI_WIDTH = 512;
    const uint32_t QUAD_UI_HEIGHT = 256;
    if (!CreateQuadLayerSwapchain(xr, QUAD_UI_WIDTH, QUAD_UI_HEIGHT)) {
        LOG_WARN("Failed to create quad layer swapchain - UI will not be displayed");
    } else {
        LOG_INFO("Quad layer created for UI overlay (%ux%u)", QUAD_UI_WIDTH, QUAD_UI_HEIGHT);
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

    std::vector<XrSwapchainImageD3D11KHR> quadSwapchainImages;
    if (xr.hasQuadLayer) {
        uint32_t count = xr.quadSwapchain.imageCount;
        quadSwapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        xrEnumerateSwapchainImages(xr.quadSwapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)quadSwapchainImages.data());
        LOG_INFO("Quad layer: enumerated %u D3D11 swapchain images", count);
    }

    // Show control window
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

    // Performance tracking
    PerformanceStats perfStats = {};
    perfStats.lastTime = std::chrono::high_resolution_clock::now();

    LOG_INFO("");
    LOG_INFO("=== Entering main loop ===");
    LOG_INFO("XR rendering happens in Monado's window, not the control window");
    LOG_INFO("Controls: WASD=Fly, QE=Up/Down, Mouse=Look, Space/DblClick=Reset, P=Parallax, ESC=Quit");
    LOG_INFO("");

    // Main loop
    MSG msg = {};
    while (g_running && !xr.exitRequested) {
        // Process Windows messages
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_running = false;
                RequestExit(xr);
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        if (!g_running) break;

        // Update performance stats
        UpdatePerformanceStats(perfStats);

        // Update input-based camera movement
        UpdateCameraMovement(g_inputState, perfStats.deltaTime);

        // Update scene (cube rotation)
        UpdateScene(renderer, perfStats.deltaTime);

        // Poll OpenXR events
        PollEvents(xr);

        // Only render if session is running
        if (xr.sessionRunning) {
            XrFrameState frameState;
            if (BeginFrame(xr, frameState)) {
                XrCompositionLayerProjectionView projectionViews[2] = {};
                ConvergencePlane convPlane = {};
                XrView rawViews[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};

                if (frameState.shouldRender) {
                    XMMATRIX leftViewMatrix, leftProjMatrix;
                    XMMATRIX rightViewMatrix, rightProjMatrix;

                    if (LocateViews(xr, frameState.predictedDisplayTime,
                        leftViewMatrix, leftProjMatrix,
                        rightViewMatrix, rightProjMatrix,
                        g_inputState.cameraPosX, g_inputState.cameraPosY, g_inputState.cameraPosZ,
                        g_inputState.yaw, g_inputState.pitch)) {

                        // Get raw view poses (pre-player-transform) for projection views
                        // and convergence plane computation
                        XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
                        locateInfo.viewConfigurationType = xr.viewConfigType;
                        locateInfo.displayTime = frameState.predictedDisplayTime;
                        locateInfo.space = xr.localSpace;

                        XrViewState viewState = {XR_TYPE_VIEW_STATE};
                        uint32_t viewCount = 2;
                        rawViews[0] = {XR_TYPE_VIEW}; rawViews[1] = {XR_TYPE_VIEW};
                        xrLocateViews(xr.session, &locateInfo, &viewState, 2, &viewCount, rawViews);

                        // Compute convergence plane from raw views (physical display surface)
                        convPlane = LocateConvergencePlane(rawViews);

                        // Render each eye (3D scene only - no UI)
                        for (int eye = 0; eye < 2; eye++) {
                            uint32_t imageIndex;
                            if (AcquireSwapchainImage(xr, eye, imageIndex)) {
                                ID3D11Texture2D* swapchainTexture = swapchainImages[eye][imageIndex].texture;

                                ID3D11RenderTargetView* rtv = nullptr;
                                CreateRenderTargetView(renderer, swapchainTexture, &rtv);

                                // Set viewport to match swapchain dimensions.
                                // Each eye has its own swapchain so we must set the viewport
                                // explicitly (the D3D11 context is shared with the compositor
                                // which sets its own viewports during layer_commit).
                                D3D11_VIEWPORT vp = {};
                                vp.Width = (FLOAT)xr.swapchains[eye].width;
                                vp.Height = (FLOAT)xr.swapchains[eye].height;
                                vp.MaxDepth = 1.0f;
                                renderer.context->RSSetViewports(1, &vp);

                                // Clear render target and depth buffer for this eye.
                                // Each eye renders to its own swapchain so both must be cleared.
                                float clearColor[4] = {0.05f, 0.05f, 0.25f, 1.0f};
                                renderer.context->ClearRenderTargetView(rtv, clearColor);
                                renderer.context->ClearDepthStencilView(depthDSVs[eye].Get(),
                                    D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

                                XMMATRIX viewMatrix = (eye == 0) ? leftViewMatrix : rightViewMatrix;
                                XMMATRIX projMatrix = (eye == 0) ? leftProjMatrix : rightProjMatrix;

                                RenderScene(renderer, rtv, depthDSVs[eye].Get(),
                                    xr.swapchains[eye].width, xr.swapchains[eye].height,
                                    viewMatrix, projMatrix,
                                    g_inputState.zoomScale);

                                if (rtv) rtv->Release();

                                ReleaseSwapchainImage(xr, eye);

                                // Set up projection view for this eye
                                projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                                projectionViews[eye].subImage.swapchain = xr.swapchains[eye].swapchain;
                                projectionViews[eye].subImage.imageRect.offset = {0, 0};
                                projectionViews[eye].subImage.imageRect.extent = {
                                    (int32_t)xr.swapchains[eye].width,
                                    (int32_t)xr.swapchains[eye].height
                                };
                                projectionViews[eye].subImage.imageArrayIndex = 0;

                                projectionViews[eye].pose = rawViews[eye].pose;
                                projectionViews[eye].fov = rawViews[eye].fov;
                            }
                        }

                        // Render UI to quad layer (if available)
                        if (xr.hasQuadLayer) {
                            uint32_t quadImageIndex;
                            if (AcquireQuadSwapchainImage(xr, quadImageIndex)) {
                                ID3D11Texture2D* quadTexture = quadSwapchainImages[quadImageIndex].texture;

                                // Clear with semi-transparent background
                                ID3D11RenderTargetView* quadRtv = nullptr;
                                CreateRenderTargetView(renderer, quadTexture, &quadRtv);
                                if (quadRtv) {
                                    float clearColor[4] = {0.0f, 0.0f, 0.0f, 0.7f};
                                    renderer.context->ClearRenderTargetView(quadRtv, clearColor);
                                    quadRtv->Release();
                                }

                                // Render text to quad
                                std::wstring stateText = L"Session: ";
                                stateText += FormatSessionState((int)xr.sessionState);
                                RenderText(textOverlay, renderer.device.Get(), quadTexture,
                                    stateText, 10, 10, 300, 30);

                                std::wstring modeText = L"Mode: Standard OpenXR (Monado window)";
                                RenderText(textOverlay, renderer.device.Get(), quadTexture,
                                    modeText, 10, 45, 350, 30, true);

                                std::wstring perfText = FormatPerformanceInfo(perfStats.fps, perfStats.frameTimeMs,
                                    xr.swapchains[0].width, xr.swapchains[0].height);
                                RenderText(textOverlay, renderer.device.Get(), quadTexture,
                                    perfText, 10, 85, 300, 70, true);

                                std::wstring eyeText = FormatEyeTrackingInfo(xr.eyePosX, xr.eyePosY, xr.eyePosZ, xr.eyeTrackingActive);
                                RenderText(textOverlay, renderer.device.Get(), quadTexture,
                                    eyeText, 10, 165, 300, 70, true);

                                ReleaseQuadSwapchainImage(xr);
                            }
                        }
                    }
                }

                // Submit frame with quad layer for UI (anchored to convergence plane)
                if (convPlane.valid) {
                    float hudW, hudH;
                    XrPosef hudPose = ComputeHUDPose(convPlane, 0.2f, rawViews, hudW, hudH);
                    EndFrameWithQuadLayer(xr, frameState.predictedDisplayTime, projectionViews,
                        hudPose, hudW, hudH);
                } else {
                    // Fallback: skip quad layer, submit projection only
                    EndFrame(xr, frameState.predictedDisplayTime, projectionViews);
                }
            }
        } else {
            Sleep(100);
        }
    }

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
