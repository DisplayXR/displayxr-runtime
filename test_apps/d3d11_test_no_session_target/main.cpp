// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D11 OpenXR test WITHOUT XR_EXT_session_target
 *
 * This application tests OpenXR with D3D11 WITHOUT using the XR_EXT_session_target
 * extension. OpenXR/Monado will create its own window for rendering.
 * This helps isolate whether issues are with the extension or base OpenXR/D3D11.
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
#include <fstream>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

// Window class name (for control window only - not used for XR rendering)
static const wchar_t* WINDOW_CLASS = L"D3D11TestNoSessionTargetClass";
static const wchar_t* WINDOW_TITLE = L"D3D11 Test (No Session Target) - Press ESC to exit";

// Global state
static InputState g_inputState;
static bool g_windowResized = false;
static UINT g_windowWidth = 640;
static UINT g_windowHeight = 480;

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

// Create a small control window (not for XR rendering)
static HWND CreateControlWindow(HINSTANCE hInstance, int width, int height) {
    LOG_INFO("Creating control window (%dx%d) - NOT used for XR rendering", width, height);

    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(GRAY_BRUSH);
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
        100, 100,  // Position near top-left so it doesn't obscure Monado's window
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

    LOG_INFO("Control window created successfully, HWND: 0x%p", hwnd);
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

    LOG_INFO("=== D3D11 OpenXR Test (NO session_target extension) ===");
    LOG_INFO("OpenXR/Monado will create its own window for rendering");
    LOG_INFO("This control window is just for input handling");
    LOG_INFO("Starting initialization...");

    // Create a small control window for input handling
    HWND hwnd = CreateControlWindow(hInstance, g_windowWidth, g_windowHeight);
    if (!hwnd) {
        LOG_ERROR("Control window creation failed, exiting");
        ShutdownLogging();
        return 1;
    }

    // WORKAROUND: Add SRMonado directory to DLL search path
    // Must do this BEFORE loading OpenXR runtime
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

    // IMPORTANT: Initialize OpenXR FIRST to get the required GPU adapter LUID
    // Then create D3D11 device on that specific adapter
    LOG_INFO("Initializing OpenXR instance (D3D11 only, NO session_target)...");
    XrSessionManager xr = {};
    if (!InitializeOpenXR(xr)) {
        LOG_ERROR("OpenXR initialization failed");
        MessageBox(hwnd, L"Failed to initialize OpenXR", L"Error", MB_OK | MB_ICONERROR);
        ShutdownLogging();
        return 1;
    }
    LOG_INFO("OpenXR instance initialized successfully");

    // Get the required GPU adapter LUID from OpenXR
    // The D3D11 device MUST be created on this specific adapter
    LUID adapterLuid;
    if (!GetD3D11GraphicsRequirements(xr, &adapterLuid)) {
        LOG_ERROR("Failed to get D3D11 graphics requirements");
        MessageBox(hwnd, L"Failed to get D3D11 graphics requirements", L"Error", MB_OK | MB_ICONERROR);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Initialize D3D11 on the CORRECT adapter (specified by OpenXR)
    LOG_INFO("Initializing D3D11 on OpenXR-specified adapter...");
    D3D11Renderer renderer = {};
    if (!InitializeD3D11WithLUID(renderer, adapterLuid)) {
        LOG_ERROR("D3D11 initialization failed");
        MessageBox(hwnd, L"Failed to initialize D3D11", L"Error", MB_OK | MB_ICONERROR);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }
    LOG_INFO("D3D11 initialized successfully on correct adapter");

    // Initialize text overlay (needs D3D11 device)
    LOG_INFO("Initializing text overlay...");
    TextOverlay textOverlay = {};
    if (!InitializeTextOverlay(textOverlay)) {
        LOG_ERROR("Text overlay initialization failed");
        MessageBox(hwnd, L"Failed to initialize text overlay", L"Error", MB_OK | MB_ICONERROR);
        CleanupD3D11(renderer);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }
    LOG_INFO("Text overlay initialized successfully");

    // Create OpenXR session with D3D11 device - NO window handle
    // OpenXR/Monado will create its own window
    LOG_INFO("Creating OpenXR session WITHOUT XR_EXT_session_target (Monado creates window)...");
    if (!CreateSession(xr, renderer.device.Get())) {
        LOG_ERROR("OpenXR session creation failed");
        MessageBox(hwnd, L"Failed to create OpenXR session", L"Error", MB_OK | MB_ICONERROR);
        CleanupTextOverlay(textOverlay);
        CleanupD3D11(renderer);
        CleanupOpenXR(xr);
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

    // Show control window
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    LOG_INFO("Control window shown");

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
    LOG_INFO("NOTE: XR rendering happens in Monado's window, not this control window");
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

                                // Render text overlay (minimal - just session state)
                                {
                                    std::wstring stateText = L"Session: ";
                                    stateText += FormatSessionState((int)xr.sessionState);
                                    stateText += L" (NO session_target)";
                                    RenderText(textOverlay, renderer.device.Get(), swapchainTexture,
                                        stateText, 10, 10, 300, 25);
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
