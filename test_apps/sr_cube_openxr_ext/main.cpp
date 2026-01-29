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

// Global state
static InputState g_inputState;
static bool g_running = true;
static UINT g_windowWidth = 1280;
static UINT g_windowHeight = 720;

// Window procedure
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    UpdateInputState(g_inputState, msg, wParam, lParam);

    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            g_windowWidth = LOWORD(lParam);
            g_windowHeight = HIWORD(lParam);
        }
        return 0;

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
    wc.style = CS_HREDRAW | CS_VREDRAW;
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

    // Create quad layer swapchain for UI overlay (512x256 pixels)
    const uint32_t QUAD_UI_WIDTH = 512;
    const uint32_t QUAD_UI_HEIGHT = 256;
    if (!CreateQuadLayerSwapchain(xr, QUAD_UI_WIDTH, QUAD_UI_HEIGHT)) {
        LOG_WARN("Failed to create quad layer swapchain - UI will not be displayed");
    } else {
        LOG_INFO("Quad layer created for UI overlay (%ux%u)", QUAD_UI_WIDTH, QUAD_UI_HEIGHT);
    }

    // Create staging texture for D2D text rendering.
    // OpenXR swapchain textures are shared resources created by the runtime and
    // Direct2D cannot create a render target on them. We render text to this
    // app-owned staging texture, then CopyResource to the swapchain texture.
    ComPtr<ID3D11Texture2D> quadStagingTexture;
    if (xr.hasQuadLayer) {
        D3D11_TEXTURE2D_DESC stagingDesc = {};
        stagingDesc.Width = QUAD_UI_WIDTH;
        stagingDesc.Height = QUAD_UI_HEIGHT;
        stagingDesc.MipLevels = 1;
        stagingDesc.ArraySize = 1;
        stagingDesc.Format = (DXGI_FORMAT)xr.quadSwapchain.format;
        stagingDesc.SampleDesc.Count = 1;
        stagingDesc.Usage = D3D11_USAGE_DEFAULT;
        stagingDesc.BindFlags = D3D11_BIND_RENDER_TARGET;

        HRESULT hr = renderer.device->CreateTexture2D(&stagingDesc, nullptr, &quadStagingTexture);
        if (FAILED(hr)) {
            LOG_WARN("Failed to create quad staging texture: 0x%08X", hr);
        } else {
            LOG_INFO("Quad staging texture created for D2D text rendering");
        }
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

    // Performance tracking
    PerformanceStats perfStats = {};
    perfStats.lastTime = std::chrono::high_resolution_clock::now();

    LOG_INFO("");
    LOG_INFO("=== Entering main loop ===");
    LOG_INFO("XR rendering happens in the application window (XR_EXT_session_target)");
    LOG_INFO("Controls: WASD=Move, Mouse=Look, P=Toggle Parallax, ESC=Quit");
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

                if (frameState.shouldRender) {
                    XMMATRIX leftViewMatrix, leftProjMatrix;
                    XMMATRIX rightViewMatrix, rightProjMatrix;

                    if (LocateViews(xr, frameState.predictedDisplayTime,
                        leftViewMatrix, leftProjMatrix,
                        rightViewMatrix, rightProjMatrix,
                        g_inputState.cameraPosX, g_inputState.cameraPosY, g_inputState.cameraPosZ,
                        g_inputState.yaw, g_inputState.pitch)) {

                        // Render each eye (3D scene only - no UI)
                        for (int eye = 0; eye < 2; eye++) {
                            uint32_t imageIndex;
                            if (AcquireSwapchainImage(xr, eye, imageIndex)) {
                                ID3D11Texture2D* swapchainTexture = xr.swapchains[eye].images[imageIndex].texture;

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

                                // Get pose from views
                                XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
                                locateInfo.viewConfigurationType = xr.viewConfigType;
                                locateInfo.displayTime = frameState.predictedDisplayTime;
                                locateInfo.space = xr.localSpace;

                                XrViewState viewState = {XR_TYPE_VIEW_STATE};
                                uint32_t viewCount = 2;
                                XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
                                xrLocateViews(xr.session, &locateInfo, &viewState, 2, &viewCount, views);

                                projectionViews[eye].pose = views[eye].pose;
                                projectionViews[eye].fov = views[eye].fov;
                            }
                        }

                        // Render UI to quad layer (if available)
                        if (xr.hasQuadLayer) {
                            uint32_t quadImageIndex;
                            if (AcquireQuadSwapchainImage(xr, quadImageIndex)) {
                                ID3D11Texture2D* quadTexture = xr.quadSwapchain.images[quadImageIndex].texture;

                                // Use staging texture for D2D rendering (D2D cannot render
                                // to OpenXR swapchain textures which are shared resources).
                                // Fall back to direct rendering if staging wasn't created.
                                ID3D11Texture2D* textTarget = quadStagingTexture ? quadStagingTexture.Get() : quadTexture;

                                // Clear with semi-transparent black background
                                ID3D11RenderTargetView* quadRtv = nullptr;
                                CreateRenderTargetView(renderer, textTarget, &quadRtv);
                                if (quadRtv) {
                                    float clearColor[4] = {0.0f, 0.0f, 0.0f, 0.7f};
                                    renderer.context->ClearRenderTargetView(quadRtv, clearColor);
                                    quadRtv->Release();
                                }

                                // Render text to staging texture
                                std::wstring stateText = L"Session: ";
                                stateText += FormatSessionState((int)xr.sessionState);
                                RenderText(textOverlay, renderer.device.Get(), textTarget,
                                    stateText, 10, 10, 300, 30);

                                std::wstring extText = xr.hasSessionTargetExt ?
                                    L"XR_EXT_session_target: ACTIVE" :
                                    L"XR_EXT_session_target: NOT AVAILABLE";
                                RenderText(textOverlay, renderer.device.Get(), textTarget,
                                    extText, 10, 45, 350, 30, true);

                                std::wstring perfText = FormatPerformanceInfo(perfStats.fps, perfStats.frameTimeMs,
                                    xr.swapchains[0].width, xr.swapchains[0].height);
                                RenderText(textOverlay, renderer.device.Get(), textTarget,
                                    perfText, 10, 85, 300, 70, true);

                                std::wstring eyeText = FormatEyeTrackingInfo(xr.eyePosX, xr.eyePosY, xr.eyePosZ, xr.eyeTrackingActive);
                                RenderText(textOverlay, renderer.device.Get(), textTarget,
                                    eyeText, 10, 165, 300, 70, true);

                                // Copy staging texture to swapchain
                                if (quadStagingTexture) {
                                    renderer.context->CopyResource(quadTexture, quadStagingTexture.Get());
                                }

                                ReleaseQuadSwapchainImage(xr);
                            }
                        }
                    }
                }

                // Submit frame with quad layer for UI (HUD in VIEW space)
                // Quad positioned at: X=left side (-0.15m), Y=top (+0.08m), Z=0.5m in front
                // Size: 0.3m wide x 0.15m tall
                EndFrameWithQuadLayer(xr, frameState.predictedDisplayTime, projectionViews,
                    -0.15f, 0.08f, -0.65f,  // position (left, up, forward) - top-left HUD
                    0.3f, 0.15f);            // size (width, height in meters)
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
