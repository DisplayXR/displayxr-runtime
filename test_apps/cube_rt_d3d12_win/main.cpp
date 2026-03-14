// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  SR Cube OpenXR D3D12 - Standard OpenXR mode (DisplayXR creates window)
 *
 * This application demonstrates OpenXR with D3D12 without the XR_EXT_win32_window_binding extension.
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
#include "d3d12_renderer.h"
#include "xr_session.h"

#include <chrono>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

static const char* APP_NAME = "cube_rt_d3d12_win";

static bool g_running = true;

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

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    if (!InitializeLogging(APP_NAME)) {
        MessageBox(nullptr, L"Failed to initialize logging", L"Warning", MB_OK | MB_ICONWARNING);
    }

    LOG_INFO("=== SR Cube OpenXR D3D12 Application ===");
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
    if (!GetD3D12GraphicsRequirements(xr, &adapterLuid)) {
        LOG_ERROR("Failed to get D3D12 graphics requirements");
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Initialize D3D12 on the correct adapter
    LOG_INFO("Initializing D3D12...");
    D3D12Renderer renderer = {};
    if (!InitializeD3D12WithLUID(renderer, adapterLuid)) {
        LOG_ERROR("D3D12 initialization failed");
        MessageBox(nullptr, L"Failed to initialize D3D12", L"Error", MB_OK | MB_ICONERROR);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create OpenXR session (DisplayXR creates window)
    LOG_INFO("Creating OpenXR session...");
    if (!CreateSession(xr, renderer.device.Get(), renderer.commandQueue.Get())) {
        LOG_ERROR("OpenXR session creation failed");
        MessageBox(nullptr, L"Failed to create OpenXR session", L"Error", MB_OK | MB_ICONERROR);
        CleanupD3D12(renderer);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create reference spaces
    if (!CreateSpaces(xr)) {
        LOG_ERROR("Reference space creation failed");
        CleanupOpenXR(xr);
        CleanupD3D12(renderer);
        ShutdownLogging();
        return 1;
    }

    // Create single swapchain at native display resolution
    if (!CreateSwapchain(xr)) {
        LOG_ERROR("Swapchain creation failed");
        CleanupOpenXR(xr);
        CleanupD3D12(renderer);
        ShutdownLogging();
        return 1;
    }

    // Enumerate D3D12 swapchain images
    std::vector<XrSwapchainImageD3D12KHR> swapchainImages;
    {
        uint32_t count = xr.swapchain.imageCount;
        swapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)swapchainImages.data());
        LOG_INFO("Enumerated %u D3D12 swapchain images", count);
    }

    // Determine swapchain format for RTV creation
    DXGI_FORMAT rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (swapchainImages.size() > 0 && swapchainImages[0].texture != nullptr) {
        D3D12_RESOURCE_DESC desc = swapchainImages[0].texture->GetDesc();
        rtvFormat = desc.Format;
        if (rtvFormat == DXGI_FORMAT_R8G8B8A8_TYPELESS) rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        if (rtvFormat == DXGI_FORMAT_B8G8R8A8_TYPELESS) rtvFormat = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        LOG_INFO("Swapchain resource format: %u, RTV format: %u", (uint32_t)desc.Format, (uint32_t)rtvFormat);
    }

    // Create RTVs for swapchain images
    {
        std::vector<ID3D12Resource*> textures(swapchainImages.size());
        for (size_t i = 0; i < swapchainImages.size(); i++) {
            textures[i] = swapchainImages[i].texture;
        }
        if (!CreateSwapchainRTVs(renderer, textures.data(), (uint32_t)textures.size(),
                                  xr.swapchain.width, xr.swapchain.height, rtvFormat)) {
            LOG_ERROR("Failed to create swapchain RTVs");
            CleanupOpenXR(xr);
            CleanupD3D12(renderer);
            ShutdownLogging();
            return 1;
        }
    }

    PerformanceStats perfStats = {};
    perfStats.lastTime = std::chrono::high_resolution_clock::now();

    LOG_INFO("");
    LOG_INFO("=== Entering main loop ===");
    LOG_INFO("Rendering in DisplayXR's window (input via qwerty driver)");
    LOG_INFO("Controls: WASD=Move, QE=Up/Down, Mouse=Look, ESC=Quit");
    LOG_INFO("");

    while (g_running && !xr.exitRequested) {
        UpdatePerformanceStats(perfStats);
        UpdateScene(renderer, perfStats.deltaTime);
        PollEvents(xr);

        if (xr.sessionRunning) {
            XrFrameState frameState;
            if (BeginFrame(xr, frameState)) {
                XrCompositionLayerProjectionView projectionViews[8] = {};
                uint32_t submitViewCount = 2;

                if (frameState.shouldRender) {
                    if (LocateViews(xr, frameState.predictedDisplayTime,
                        0.0f, 0.0f, 0.0f,
                        0.0f, 0.0f)) {

                        // Use current mode's view count (not xr.viewCount which is max across all modes)
                        uint32_t modeViewCount = (xr.currentModeIndex < xr.renderingModeCount)
                            ? xr.renderingModeViewCounts[xr.currentModeIndex] : xr.viewCount;
                        submitViewCount = modeViewCount;

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
                            ID3D12Resource* swapchainTexture = swapchainImages[imageIndex].texture;

                            // Render all views with tile layout
                            for (uint32_t eye = 0; eye < modeViewCount; eye++) {
                                uint32_t tileX = eye % tileColumns;
                                uint32_t tileY = eye / tileColumns;

                                XMMATRIX viewMatrix = xr.viewMatrices[eye];
                                XMMATRIX projMatrix = xr.projMatrices[eye];

                                RenderScene(renderer, swapchainTexture, (int)imageIndex,
                                    tileX * tileW, tileY * tileH,
                                    tileW, tileH,
                                    viewMatrix, projMatrix,
                                    1.0f, eye == 0);

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

                            // Wait for GPU before releasing
                            WaitForGpu(renderer);
                            ReleaseSwapchainImage(xr);
                        }
                    }
                }

                EndFrame(xr, frameState.predictedDisplayTime, projectionViews, submitViewCount);
            }
        } else {
            Sleep(100);
        }
    }

    // Cleanup
    LOG_INFO("");
    LOG_INFO("=== Shutting down ===");

    WaitForGpu(renderer);
    CleanupOpenXR(xr);
    CleanupD3D12(renderer);

    LOG_INFO("Application shutdown complete");
    ShutdownLogging();

    return 0;
}
