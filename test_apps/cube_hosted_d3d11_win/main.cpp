// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  SR Cube OpenXR - Standard OpenXR mode (DisplayXR creates window)
 *
 * This application demonstrates OpenXR without the XR_EXT_win32_window_binding extension.
 * DisplayXR will create its own window for rendering.
 *
 * Input is handled by DisplayXR's qwerty driver:
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
static const char* APP_NAME = "cube_hosted_d3d11_win";

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
    LOG_INFO("OpenXR standard mode (DisplayXR creates window)");
    LOG_INFO("Input handled by DisplayXR's qwerty driver");

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

    // Create OpenXR session (DisplayXR creates window)
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
    LOG_INFO("Rendering in DisplayXR's window (input via qwerty driver)");
    LOG_INFO("Controls: WASD=Move, QE=Up/Down, Mouse=Look, ESC=Quit");
    LOG_INFO("");

    // Main loop - no window, just process OpenXR frames
    // Exit when OpenXR session ends (user closes DisplayXR window or presses ESC)
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
                XrCompositionLayerProjectionView projectionViews[8] = {};
                uint32_t submitViewCount = 2;

                if (frameState.shouldRender) {
                    // Camera movement is handled by DisplayXR's qwerty driver
                    // Pass zeros for player transform - XR poses already include qwerty input
                    if (LocateViews(xr, frameState.predictedDisplayTime,
                        0.0f, 0.0f, 0.0f,  // playerPos (handled by qwerty)
                        0.0f, 0.0f)) {     // playerYaw/Pitch (handled by qwerty)

                        // Use current mode's view count (not xr.viewCount which is max across all modes)
                        uint32_t modeViewCount = (xr.currentModeIndex < xr.renderingModeCount)
                            ? xr.renderingModeViewCounts[xr.currentModeIndex] : xr.viewCount;
                        submitViewCount = modeViewCount;

                        // Get raw view poses (pre-player-transform) for projection views.
                        XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
                        locateInfo.viewConfigurationType = xr.viewConfigType;
                        locateInfo.displayTime = frameState.predictedDisplayTime;
                        locateInfo.space = xr.localSpace;

                        XrViewState viewState = {XR_TYPE_VIEW_STATE};
                        uint32_t rawViewCount = 8;
                        XrView rawViews[8];
                        for (uint32_t i = 0; i < 8; i++) rawViews[i] = {XR_TYPE_VIEW};
                        xrLocateViews(xr.session, &locateInfo, &viewState, 8, &rawViewCount, rawViews);

                        // Get tile layout from rendering mode, with fallback
                        uint32_t tileColumns = (xr.currentModeIndex < xr.renderingModeCount)
                            ? xr.renderingModeTileColumns[xr.currentModeIndex] : (modeViewCount >= 2 ? 2 : 1);
                        uint32_t tileRows = (xr.currentModeIndex < xr.renderingModeCount)
                            ? xr.renderingModeTileRows[xr.currentModeIndex] : ((modeViewCount + tileColumns - 1) / tileColumns);

                        uint32_t tileW = xr.swapchain.width / tileColumns;
                        uint32_t tileH = xr.swapchain.height / tileRows;

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

                            for (uint32_t eye = 0; eye < modeViewCount; eye++) {
                                uint32_t tileX = eye % tileColumns;
                                uint32_t tileY = eye / tileColumns;

                                D3D11_VIEWPORT vp = {};
                                vp.TopLeftX = (FLOAT)(tileX * tileW);
                                vp.TopLeftY = (FLOAT)(tileY * tileH);
                                vp.Width = (FLOAT)tileW;
                                vp.Height = (FLOAT)tileH;
                                vp.MaxDepth = 1.0f;
                                renderer.context->RSSetViewports(1, &vp);

                                XMMATRIX viewMatrix = xr.viewMatrices[eye];
                                XMMATRIX projMatrix = xr.projMatrices[eye];

                                // Non-ext app: 0.3m cube at z=-2m, no zoom control
                                RenderScene(renderer, rtv, depthDSV.Get(),
                                    tileW, tileH,
                                    viewMatrix, projMatrix,
                                    1.0f, 1.6f, -2.0f, 0.3f);

                                // Set up projection view for this eye
                                projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                                projectionViews[eye].subImage.swapchain = xr.swapchain.swapchain;
                                projectionViews[eye].subImage.imageRect.offset = {
                                    (int32_t)(tileX * tileW), (int32_t)(tileY * tileH)
                                };
                                projectionViews[eye].subImage.imageRect.extent = {
                                    (int32_t)tileW,
                                    (int32_t)tileH
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
                EndFrame(xr, frameState.predictedDisplayTime, projectionViews, submitViewCount);
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
