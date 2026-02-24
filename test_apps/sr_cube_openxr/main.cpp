// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  SR Cube OpenXR - Standard OpenXR mode (Monado creates window)
 *
 * This application demonstrates OpenXR without the XR_EXT_win32_window_binding extension.
 * Monado will create its own window for rendering.
 *
 * Input is handled by Monado's qwerty driver:
 * - WASD: Move camera
 * - Mouse drag: Look around
 * - ESC: Close window and exit
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <wrl/client.h>

#include "logging.h"
#include "d3d11_renderer.h"
#include "xr_session.h"

#include <chrono>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

// Application name for logging
static const char* APP_NAME = "sr_cube_openxr";

// Global state
static bool g_running = true;

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
    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    // Initialize logging
    if (!InitializeLogging(APP_NAME)) {
        MessageBox(nullptr, L"Failed to initialize logging", L"Warning", MB_OK | MB_ICONWARNING);
    }

    LOG_INFO("=== SR Cube OpenXR Application ===");
    LOG_INFO("OpenXR standard mode (Monado creates window)");
    LOG_INFO("Input handled by Monado's qwerty driver");

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
        MessageBox(nullptr, L"Failed to initialize OpenXR", L"Error", MB_OK | MB_ICONERROR);
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
        MessageBox(nullptr, L"Failed to initialize D3D11", L"Error", MB_OK | MB_ICONERROR);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create OpenXR session (Monado creates window)
    LOG_INFO("Creating OpenXR session...");
    if (!CreateSession(xr, renderer.device.Get())) {
        LOG_ERROR("OpenXR session creation failed");
        MessageBox(nullptr, L"Failed to create OpenXR session", L"Error", MB_OK | MB_ICONERROR);
        CleanupD3D11(renderer);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create reference spaces
    if (!CreateSpaces(xr)) {
        LOG_ERROR("Reference space creation failed");
        CleanupOpenXR(xr);
        CleanupD3D11(renderer);
        ShutdownLogging();
        return 1;
    }

    // Create single swapchain at native display resolution
    if (!CreateSwapchain(xr)) {
        LOG_ERROR("Swapchain creation failed");
        CleanupOpenXR(xr);
        CleanupD3D11(renderer);
        ShutdownLogging();
        return 1;
    }

    // Enumerate D3D11 swapchain images (now done per-app since common is API-agnostic)
    std::vector<XrSwapchainImageD3D11KHR> swapchainImages;
    {
        uint32_t count = xr.swapchain.imageCount;
        swapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)swapchainImages.data());
        LOG_INFO("Enumerated %u D3D11 swapchain images", count);
    }

    // Create single depth buffer at full swapchain dimensions
    ComPtr<ID3D11Texture2D> depthTexture;
    ComPtr<ID3D11DepthStencilView> depthDSV;
    {
        ID3D11Texture2D* depthTex = nullptr;
        ID3D11DepthStencilView* dsv = nullptr;
        if (!CreateDepthStencilView(renderer, xr.swapchain.width, xr.swapchain.height, &depthTex, &dsv)) {
            LOG_ERROR("Failed to create depth buffer");
            CleanupOpenXR(xr);
            CleanupD3D11(renderer);
            ShutdownLogging();
            return 1;
        }
        depthTexture.Attach(depthTex);
        depthDSV.Attach(dsv);
    }

    // Performance tracking
    PerformanceStats perfStats = {};
    perfStats.lastTime = std::chrono::high_resolution_clock::now();

    LOG_INFO("");
    LOG_INFO("=== Entering main loop ===");
    LOG_INFO("Rendering in Monado's window (input via qwerty driver)");
    LOG_INFO("Controls: WASD=Move, QE=Up/Down, Mouse=Look, ESC=Quit");
    LOG_INFO("");

    // Main loop - no window, just process OpenXR frames
    // Exit when OpenXR session ends (user closes Monado window or presses ESC)
    while (g_running && !xr.exitRequested) {
        // Update performance stats
        UpdatePerformanceStats(perfStats);

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

                    // Camera movement is handled by Monado's qwerty driver
                    // Pass zeros for player transform - XR poses already include qwerty input
                    if (LocateViews(xr, frameState.predictedDisplayTime,
                        leftViewMatrix, leftProjMatrix,
                        rightViewMatrix, rightProjMatrix,
                        0.0f, 0.0f, 0.0f,  // playerPos (handled by qwerty)
                        0.0f, 0.0f)) {     // playerYaw/Pitch (handled by qwerty)

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

                        // Single swapchain: acquire once, render both eyes with SBS viewports, release once
                        uint32_t eyeRenderW = (xr.recommendedViewScaleX > 0.0f && xr.recommendedViewScaleX < 1.0f)
                            ? (uint32_t)(xr.swapchain.width * xr.recommendedViewScaleX)
                            : xr.swapchain.width / 2;
                        uint32_t eyeRenderH = xr.swapchain.height;

                        uint32_t imageIndex;
                        if (AcquireSwapchainImage(xr, imageIndex)) {
                            ID3D11Texture2D* swapchainTexture = swapchainImages[imageIndex].texture;

                            ID3D11RenderTargetView* rtv = nullptr;
                            CreateRenderTargetView(renderer, swapchainTexture, &rtv);

                            // Clear entire color+depth once before eye loop
                            float clearColor[4] = {0.05f, 0.05f, 0.25f, 1.0f};
                            renderer.context->ClearRenderTargetView(rtv, clearColor);
                            renderer.context->ClearDepthStencilView(depthDSV.Get(),
                                D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

                            for (int eye = 0; eye < 2; eye++) {
                                D3D11_VIEWPORT vp = {};
                                vp.TopLeftX = (FLOAT)(eye * eyeRenderW);
                                vp.Width = (FLOAT)eyeRenderW;
                                vp.Height = (FLOAT)eyeRenderH;
                                vp.MaxDepth = 1.0f;
                                renderer.context->RSSetViewports(1, &vp);

                                XMMATRIX viewMatrix = (eye == 0) ? leftViewMatrix : rightViewMatrix;
                                XMMATRIX projMatrix = (eye == 0) ? leftProjMatrix : rightProjMatrix;

                                // zoomScale = 1.0 (no zoom control without input handler)
                                RenderScene(renderer, rtv, depthDSV.Get(),
                                    eyeRenderW, eyeRenderH,
                                    viewMatrix, projMatrix,
                                    1.0f);

                                // Set up projection view for this eye
                                projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                                projectionViews[eye].subImage.swapchain = xr.swapchain.swapchain;
                                projectionViews[eye].subImage.imageRect.offset = {
                                    (int32_t)(eye * eyeRenderW), 0
                                };
                                projectionViews[eye].subImage.imageRect.extent = {
                                    (int32_t)eyeRenderW,
                                    (int32_t)eyeRenderH
                                };
                                projectionViews[eye].subImage.imageArrayIndex = 0;

                                projectionViews[eye].pose = rawViews[eye].pose;
                                projectionViews[eye].fov = rawViews[eye].fov;
                            }

                            if (rtv) rtv->Release();
                            ReleaseSwapchainImage(xr);
                        }
                    }
                }

                // Submit frame (projection layer only)
                EndFrame(xr, frameState.predictedDisplayTime, projectionViews);
            }
        } else {
            Sleep(100);
        }
    }

    // Cleanup
    LOG_INFO("");
    LOG_INFO("=== Shutting down ===");

    depthDSV.Reset();
    depthTexture.Reset();

    CleanupOpenXR(xr);
    CleanupD3D11(renderer);

    LOG_INFO("Application shutdown complete");
    ShutdownLogging();

    return 0;
}
