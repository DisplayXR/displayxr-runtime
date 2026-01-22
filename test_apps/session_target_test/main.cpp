// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Main entry point for XR_EXT_session_target test application
 *
 * This application demonstrates the XR_EXT_session_target extension which allows
 * OpenXR applications to render into a standard Windows window that they control.
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <wrl/client.h>

#include "logging.h"
#include "xr_session.h"
#include "d3d11_renderer.h"
#include "text_overlay.h"
#include "input_state.h"

#include <chrono>
#include <string>
#include <sstream>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

// Window class name
static const wchar_t* WINDOW_CLASS = L"SessionTargetTestClass";
static const wchar_t* WINDOW_TITLE = L"XR_EXT_session_target Test";

// Global state
static InputState g_inputState;
static bool g_windowResized = false;
static UINT g_windowWidth = 1280;
static UINT g_windowHeight = 720;

// Window procedure
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Handle input
    UpdateInputState(g_inputState, msg, wParam, lParam);

    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            g_windowWidth = LOWORD(lParam);
            g_windowHeight = HIWORD(lParam);
            g_windowResized = true;
        }
        return 0;

    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
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
        LOG_ERROR("Failed to register window class, error: %lu", err);
        MessageBox(nullptr, L"Failed to register window class", L"Error", MB_OK | MB_ICONERROR);
        return nullptr;
    }
    LOG_INFO("Window class registered successfully");

    // Calculate window size to get desired client area
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
        DWORD err = GetLastError();
        LOG_ERROR("Failed to create window, error: %lu", err);
        MessageBox(nullptr, L"Failed to create window", L"Error", MB_OK | MB_ICONERROR);
        return nullptr;
    }

    LOG_INFO("Window created successfully, HWND: 0x%p", hwnd);
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

    // Initialize logging first
    if (!InitializeLogging()) {
        MessageBox(nullptr, L"Failed to initialize logging", L"Warning", MB_OK | MB_ICONWARNING);
        // Continue anyway - just won't have file logging
    }

    LOG_INFO("=== XR_EXT_session_target Test Application ===");
    LOG_INFO("Starting initialization...");

    // Create window
    HWND hwnd = CreateAppWindow(hInstance, g_windowWidth, g_windowHeight);
    if (!hwnd) {
        LOG_ERROR("Window creation failed, exiting");
        ShutdownLogging();
        return 1;
    }

    // Initialize D3D11
    LOG_INFO("Initializing D3D11...");
    D3D11Renderer renderer = {};
    if (!InitializeD3D11(renderer)) {
        LOG_ERROR("D3D11 initialization failed");
        MessageBox(hwnd, L"Failed to initialize D3D11", L"Error", MB_OK | MB_ICONERROR);
        ShutdownLogging();
        return 1;
    }
    LOG_INFO("D3D11 initialized successfully");

    // Initialize text overlay
    LOG_INFO("Initializing text overlay...");
    TextOverlay textOverlay = {};
    if (!InitializeTextOverlay(textOverlay)) {
        LOG_ERROR("Text overlay initialization failed");
        MessageBox(hwnd, L"Failed to initialize text overlay", L"Error", MB_OK | MB_ICONERROR);
        CleanupD3D11(renderer);
        ShutdownLogging();
        return 1;
    }
    LOG_INFO("Text overlay initialized successfully");

    // Initialize OpenXR
    LOG_INFO("Initializing OpenXR instance...");
    XrSessionManager xr = {};
    if (!InitializeOpenXR(xr)) {
        LOG_ERROR("OpenXR initialization failed");
        MessageBox(hwnd, L"Failed to initialize OpenXR", L"Error", MB_OK | MB_ICONERROR);
        CleanupTextOverlay(textOverlay);
        CleanupD3D11(renderer);
        ShutdownLogging();
        return 1;
    }
    LOG_INFO("OpenXR instance initialized successfully");

    // Create OpenXR session with window handle (using XR_EXT_session_target)
    LOG_INFO("Creating OpenXR session with XR_EXT_session_target (HWND: 0x%p)...", hwnd);
    if (!CreateSession(xr, renderer.device.Get(), hwnd)) {
        LOG_ERROR("OpenXR session creation failed");
        MessageBox(hwnd, L"Failed to create OpenXR session", L"Error", MB_OK | MB_ICONERROR);
        CleanupOpenXR(xr);
        CleanupTextOverlay(textOverlay);
        CleanupD3D11(renderer);
        ShutdownLogging();
        return 1;
    }
    LOG_INFO("OpenXR session created successfully");

    // Create reference spaces
    LOG_INFO("Creating reference spaces...");
    if (!CreateSpaces(xr)) {
        LOG_ERROR("Reference space creation failed");
        MessageBox(hwnd, L"Failed to create reference spaces", L"Error", MB_OK | MB_ICONERROR);
        CleanupOpenXR(xr);
        CleanupTextOverlay(textOverlay);
        CleanupD3D11(renderer);
        ShutdownLogging();
        return 1;
    }
    LOG_INFO("Reference spaces created successfully");

    // Create swapchains
    LOG_INFO("Creating swapchains...");
    if (!CreateSwapchains(xr)) {
        LOG_ERROR("Swapchain creation failed");
        MessageBox(hwnd, L"Failed to create swapchains", L"Error", MB_OK | MB_ICONERROR);
        CleanupOpenXR(xr);
        CleanupTextOverlay(textOverlay);
        CleanupD3D11(renderer);
        ShutdownLogging();
        return 1;
    }
    LOG_INFO("Swapchains created successfully (width: %u, height: %u)",
        xr.swapchains[0].width, xr.swapchains[0].height);

    // Show window
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    LOG_INFO("Window shown");

    // Per-eye render targets and depth buffers
    ComPtr<ID3D11RenderTargetView> eyeRTVs[2];
    ComPtr<ID3D11Texture2D> depthTextures[2];
    ComPtr<ID3D11DepthStencilView> depthDSVs[2];

    // Create depth buffers for each eye
    LOG_INFO("Creating depth buffers...");
    for (int eye = 0; eye < 2; eye++) {
        ID3D11Texture2D* depthTex = nullptr;
        ID3D11DepthStencilView* dsv = nullptr;
        if (!CreateDepthStencilView(renderer,
            xr.swapchains[eye].width,
            xr.swapchains[eye].height,
            &depthTex, &dsv)) {
            LOG_ERROR("Failed to create depth buffer for eye %d", eye);
            MessageBox(hwnd, L"Failed to create depth buffer", L"Error", MB_OK | MB_ICONERROR);
            CleanupOpenXR(xr);
            CleanupTextOverlay(textOverlay);
            CleanupD3D11(renderer);
            ShutdownLogging();
            return 1;
        }
        depthTextures[eye].Attach(depthTex);
        depthDSVs[eye].Attach(dsv);
    }
    LOG_INFO("Depth buffers created successfully");

    // Performance tracking
    PerformanceStats perfStats = {};
    perfStats.lastTime = std::chrono::high_resolution_clock::now();

    LOG_INFO("");
    LOG_INFO("=== Initialization complete, entering main loop ===");
    LOG_INFO("");

    // Main loop
    MSG msg = {};
    bool running = true;
    int frameNumber = 0;

    while (running && !xr.exitRequested) {
        frameNumber++;
        // Process Windows messages
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = false;
                RequestExit(xr);
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        if (!running) break;

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
                    // Get view matrices
                    XMMATRIX leftViewMatrix, leftProjMatrix;
                    XMMATRIX rightViewMatrix, rightProjMatrix;

                    if (LocateViews(xr, frameState.predictedDisplayTime,
                        leftViewMatrix, leftProjMatrix,
                        rightViewMatrix, rightProjMatrix)) {

                        // Render each eye
                        for (int eye = 0; eye < 2; eye++) {
                            uint32_t imageIndex;
                            if (AcquireSwapchainImage(xr, eye, imageIndex)) {
                                // Get swapchain texture
                                ID3D11Texture2D* swapchainTexture = xr.swapchains[eye].images[imageIndex].texture;

                                // Create RTV for this swapchain image
                                ID3D11RenderTargetView* rtv = nullptr;
                                CreateRenderTargetView(renderer, swapchainTexture, &rtv);

                                // Select view/projection matrices
                                XMMATRIX viewMatrix = (eye == 0) ? leftViewMatrix : rightViewMatrix;
                                XMMATRIX projMatrix = (eye == 0) ? leftProjMatrix : rightProjMatrix;

                                // Render scene
                                RenderScene(renderer, rtv, depthDSVs[eye].Get(),
                                    xr.swapchains[eye].width,
                                    xr.swapchains[eye].height,
                                    viewMatrix, projMatrix,
                                    g_inputState.cameraPosX,
                                    g_inputState.cameraPosY,
                                    g_inputState.cameraPosZ,
                                    g_inputState.yaw,
                                    g_inputState.pitch);

                                // Render text overlay
                                {
                                    // Session state
                                    std::wstring stateText = L"Session: ";
                                    stateText += FormatSessionState((int)xr.sessionState);
                                    RenderText(textOverlay, renderer.device.Get(), swapchainTexture,
                                        stateText, 10, 10, 200, 25);

                                    // Performance
                                    std::wstring perfText = FormatPerformanceInfo(
                                        perfStats.fps, perfStats.frameTimeMs,
                                        g_windowWidth, g_windowHeight);
                                    RenderText(textOverlay, renderer.device.Get(), swapchainTexture,
                                        perfText, 10, 40, 200, 60, true);

                                    // Input state
                                    std::wstring inputText = FormatInputInfo(
                                        g_inputState.lastKey,
                                        g_inputState.mouseX,
                                        g_inputState.mouseY,
                                        GetMouseButtonString(g_inputState));
                                    RenderText(textOverlay, renderer.device.Get(), swapchainTexture,
                                        inputText, 10, 110, 200, 80, true);

                                    // Eye tracking
                                    std::wstring eyeText = FormatEyeTrackingInfo(
                                        xr.eyePosX, xr.eyePosY, xr.eyeTrackingActive);
                                    RenderText(textOverlay, renderer.device.Get(), swapchainTexture,
                                        eyeText, 10, 200, 200, 60, true);

                                    // Help text
                                    std::wstring helpText = L"WASD: Move | Mouse drag: Look | Esc: Quit";
                                    RenderText(textOverlay, renderer.device.Get(), swapchainTexture,
                                        helpText, 10, xr.swapchains[eye].height - 30.0f, 400, 25, true);
                                }

                                // Release RTV
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

                                // Get pose from views (we need to re-query or store from LocateViews)
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
                    }
                }

                // End frame
                EndFrame(xr, frameState.predictedDisplayTime, projectionViews);
            }
        } else {
            // Not running - sleep to avoid spinning
            Sleep(100);
        }
    }

    // Cleanup
    LOG_INFO("");
    LOG_INFO("=== Shutting down ===");

    LOG_INFO("Releasing depth buffers...");
    for (int eye = 0; eye < 2; eye++) {
        depthDSVs[eye].Reset();
        depthTextures[eye].Reset();
        eyeRTVs[eye].Reset();
    }

    LOG_INFO("Cleaning up OpenXR...");
    CleanupOpenXR(xr);

    LOG_INFO("Cleaning up text overlay...");
    CleanupTextOverlay(textOverlay);

    LOG_INFO("Cleaning up D3D11...");
    CleanupD3D11(renderer);

    LOG_INFO("Destroying window...");
    DestroyWindow(hwnd);
    UnregisterClass(WINDOW_CLASS, hInstance);

    LOG_INFO("Application shutdown complete");
    ShutdownLogging();

    return 0;
}
