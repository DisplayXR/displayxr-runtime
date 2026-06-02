// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  windowspace_handle_d3d11_win — submit N XR_EXT window-space layers, no 3D scene.
 *
 * Minimal handle-class app for issue #389: the D3D11 native compositor
 * dropped/flickered window-space layers when >2 were submitted. This app
 * creates a real-HWND D3D11 handle session, a flat-cleared projection layer
 * (to define the canvas), and N independent window-space layer swapchains
 * each filled with a distinct solid color, then submits them all every frame
 * via SubmitWindowSpaceLayersFrame().
 *
 * Pass the layer count as argv[1] (default 6, clamped to [1,12]).
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <wrl/client.h>

#include "logging.h"
#include "d3d11_renderer.h"
#include "xr_session.h"
#include "xr_window_space_hud.h"
#include "windowspace_layers.h"

#include <chrono>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

static const char* APP_NAME = "windowspace_handle_d3d11_win";
static const wchar_t* WINDOW_CLASS = L"DXRWindowSpaceD3D11Class";
static const wchar_t* WINDOW_TITLE = L"Window-Space Layers — D3D11 Native Compositor";

static bool g_running = true;
static XrSessionManager* g_xr = nullptr;
static UINT g_windowWidth = 1280;
static UINT g_windowHeight = 720;

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            g_windowWidth = LOWORD(lParam);
            g_windowHeight = HIWORD(lParam);
        }
        return 0;
    case WM_CLOSE:
        if (g_xr && g_xr->session != XR_NULL_HANDLE && g_xr->sessionRunning) {
            xrRequestExitSession(g_xr->session);
            return 0;
        }
        g_running = false;
        PostQuitMessage(0);
        return 0;
    case WM_SYSKEYDOWN:
        return 0; // swallow ALT so it doesn't open the system-menu modal loop
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            PostMessage(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static HWND CreateAppWindow(HINSTANCE hInstance, int width, int height) {
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
    HWND hwnd = CreateWindowEx(0, WINDOW_CLASS, WINDOW_TITLE, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) {
        LOG_ERROR("Failed to create window, error: %lu", GetLastError());
        return nullptr;
    }
    return hwnd;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    if (!InitializeLogging(APP_NAME)) {
        MessageBox(nullptr, L"Failed to initialize logging", L"Warning", MB_OK | MB_ICONWARNING);
    }

    const uint32_t layerCount = wsl::ParseLayerCount(__argc, __argv);
    LOG_INFO("=== windowspace_handle_d3d11_win === N=%u window-space layers", layerCount);

    HWND hwnd = CreateAppWindow(hInstance, g_windowWidth, g_windowHeight);
    if (!hwnd) { ShutdownLogging(); return 1; }

    XrSessionManager xr = {};
    g_xr = &xr;
    if (!InitializeOpenXR(xr)) {
        LOG_ERROR("OpenXR initialization failed");
        g_xr = nullptr; ShutdownLogging(); return 1;
    }

    LUID adapterLuid;
    if (!GetD3D11GraphicsRequirements(xr, &adapterLuid)) {
        LOG_ERROR("Failed to get D3D11 graphics requirements");
        CleanupOpenXR(xr); ShutdownLogging(); return 1;
    }

    D3D11Renderer renderer = {};
    if (!InitializeD3D11WithLUID(renderer, adapterLuid)) {
        LOG_ERROR("D3D11 initialization failed");
        CleanupOpenXR(xr); ShutdownLogging(); return 1;
    }

    if (!CreateSession(xr, renderer.device.Get(), hwnd)) {
        LOG_ERROR("OpenXR session creation failed");
        CleanupD3D11(renderer); CleanupOpenXR(xr); ShutdownLogging(); return 1;
    }
    if (!CreateSpaces(xr)) {
        LOG_ERROR("Reference space creation failed");
        CleanupD3D11(renderer); CleanupOpenXR(xr); ShutdownLogging(); return 1;
    }
    if (!CreateSwapchain(xr)) {
        LOG_ERROR("Swapchain creation failed");
        CleanupD3D11(renderer); CleanupOpenXR(xr); ShutdownLogging(); return 1;
    }

    // Enumerate projection swapchain images.
    std::vector<XrSwapchainImageD3D11KHR> swapchainImages;
    {
        uint32_t count = xr.swapchain.imageCount;
        swapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)swapchainImages.data());
        LOG_INFO("Enumerated %u projection swapchain images", count);
    }

    // Create N window-space layer swapchains (standalone helper) + enumerate images.
    std::vector<XrHudSwapchain> layerSC(layerCount);
    std::vector<std::vector<XrSwapchainImageD3D11KHR>> layerImages(layerCount);
    std::vector<std::vector<uint8_t>> layerPixels(layerCount);
    for (uint32_t i = 0; i < layerCount; ++i) {
        if (!CreateHudSwapchain(xr.session, wsl::kLayerPxWidth, wsl::kLayerPxHeight, layerSC[i])) {
            LOG_WARN("Failed to create window-space swapchain %u", i);
            continue;
        }
        uint32_t cnt = layerSC[i].imageCount;
        layerImages[i].resize(cnt, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        xrEnumerateSwapchainImages(layerSC[i].swapchain, cnt, &cnt,
            (XrSwapchainImageBaseHeader*)layerImages[i].data());
        uint8_t rgba[4];
        wsl::LayerColor(i, layerCount, rgba);
        wsl::FillSolid(layerPixels[i], wsl::kLayerPxWidth, wsl::kLayerPxHeight, rgba);
    }
    LOG_INFO("Created %u window-space layer swapchains", layerCount);

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    XrEnvironmentBlendMode blendMode = (xr.envBlendModeCount > 0)
        ? xr.envBlendModes[0] : XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

    MSG msg = {};
    while (g_running && !xr.exitRequested) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_running = false; break; }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!g_running) break;

        PollEvents(xr);
        if (!xr.sessionRunning) { Sleep(50); continue; }

        XrFrameState frameState;
        if (!BeginFrame(xr, frameState)) continue;

        // Mode / tile info from enumerated rendering modes (same as cube app).
        bool monoMode = (xr.renderingModeCount > 0 && !xr.renderingModeDisplay3D[xr.currentModeIndex]);
        uint32_t modeViewCount = (xr.renderingModeCount > 0 && xr.currentModeIndex < xr.renderingModeCount)
            ? xr.renderingModeViewCounts[xr.currentModeIndex] : 2;
        uint32_t tileColumns = (xr.renderingModeCount > 0 && xr.currentModeIndex < xr.renderingModeCount)
            ? xr.renderingModeTileColumns[xr.currentModeIndex] : 2;
        uint32_t tileRows = (xr.renderingModeCount > 0 && xr.currentModeIndex < xr.renderingModeCount)
            ? xr.renderingModeTileRows[xr.currentModeIndex] : 1;
        if (xr.renderingModeCount > 0 && xr.currentModeIndex < xr.renderingModeCount) {
            xr.recommendedViewScaleX = xr.renderingModeScaleX[xr.currentModeIndex];
            xr.recommendedViewScaleY = xr.renderingModeScaleY[xr.currentModeIndex];
        }
        int eyeCount = monoMode ? 1 : (int)modeViewCount;
        std::vector<XrCompositionLayerProjectionView> projectionViews(eyeCount, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});

        bool rendered = false;
        if (frameState.shouldRender) {
            // Raw view poses + fov for the projection layer.
            XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
            locateInfo.viewConfigurationType = xr.viewConfigType;
            locateInfo.displayTime = frameState.predictedDisplayTime;
            locateInfo.space = xr.localSpace;
            XrViewState viewState = {XR_TYPE_VIEW_STATE};
            uint32_t viewCount = 8;
            XrView rawViews[8];
            for (uint32_t vi = 0; vi < 8; vi++) rawViews[vi] = {XR_TYPE_VIEW};
            xrLocateViews(xr.session, &locateInfo, &viewState, 8, &viewCount, rawViews);

            uint32_t maxTileW = tileColumns > 0 ? xr.swapchain.width / tileColumns : xr.swapchain.width;
            uint32_t maxTileH = tileRows > 0 ? xr.swapchain.height / tileRows : xr.swapchain.height;
            uint32_t renderW, renderH;
            if (monoMode) {
                renderW = g_windowWidth;  renderH = g_windowHeight;
                if (renderW > xr.swapchain.width) renderW = xr.swapchain.width;
                if (renderH > xr.swapchain.height) renderH = xr.swapchain.height;
            } else {
                renderW = (uint32_t)(g_windowWidth * xr.recommendedViewScaleX);
                renderH = (uint32_t)(g_windowHeight * xr.recommendedViewScaleY);
                if (renderW > maxTileW) renderW = maxTileW;
                if (renderH > maxTileH) renderH = maxTileH;
            }

            uint32_t imageIndex;
            if (AcquireSwapchainImage(xr, imageIndex)) {
                ID3D11Texture2D* swapchainTexture = swapchainImages[imageIndex].texture;
                ID3D11RenderTargetView* rtv = nullptr;
                CreateRenderTargetView(renderer, swapchainTexture,
                    static_cast<DXGI_FORMAT>(xr.swapchain.format), &rtv);
                // Flat dark background fill — no 3D scene.
                float clearColor[4] = {
                    wsl::kBgR / 255.0f, wsl::kBgG / 255.0f, wsl::kBgB / 255.0f, wsl::kBgA / 255.0f
                };
                renderer.context->ClearRenderTargetView(rtv, clearColor);
                if (rtv) rtv->Release();

                // Build per-view projection subImage rects (tile layout).
                for (int eye = 0; eye < eyeCount; eye++) {
                    uint32_t tileX = monoMode ? 0 : (eye % tileColumns);
                    uint32_t tileY = monoMode ? 0 : (eye / tileColumns);
                    projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                    projectionViews[eye].subImage.swapchain = xr.swapchain.swapchain;
                    projectionViews[eye].subImage.imageRect.offset = {
                        (int32_t)(tileX * renderW), (int32_t)(tileY * renderH) };
                    projectionViews[eye].subImage.imageRect.extent = {
                        (int32_t)renderW, (int32_t)renderH };
                    projectionViews[eye].subImage.imageArrayIndex = 0;
                    int safeIdx = (eye < (int)viewCount) ? eye : 0;
                    projectionViews[eye].pose = rawViews[safeIdx].pose;
                    projectionViews[eye].fov = rawViews[safeIdx].fov;
                }

                ReleaseSwapchainImage(xr);
                rendered = true;
            }

            // Fill + acquire/release each window-space layer image (distinct color).
            for (uint32_t i = 0; i < layerCount; ++i) {
                if (layerSC[i].swapchain == XR_NULL_HANDLE) continue;
                uint32_t idx = 0;
                if (!AcquireHudSwapchainImage(layerSC[i], idx)) continue;
                if (idx < layerImages[i].size() && layerImages[i][idx].texture && !layerPixels[i].empty()) {
                    ID3D11Texture2D* tex = layerImages[i][idx].texture;
                    D3D11_BOX box = {0, 0, 0, wsl::kLayerPxWidth, wsl::kLayerPxHeight, 1};
                    renderer.context->UpdateSubresource(tex, 0, &box,
                        layerPixels[i].data(), wsl::kLayerPxWidth * 4, 0);
                }
                ReleaseHudSwapchainImage(layerSC[i]);
            }
        }

        if (rendered) {
            // Lay out N layers as a vertical column of buttons down the left edge.
            std::vector<WindowSpaceLayerDesc> descs;
            descs.reserve(layerCount);
            for (uint32_t i = 0; i < layerCount; ++i) {
                if (layerSC[i].swapchain == XR_NULL_HANDLE) continue;
                float y = wsl::kLayerYStart + i * wsl::kLayerYStride;
                if (y + wsl::kLayerH > 1.0f) continue; // skip layers that would fall off-screen
                WindowSpaceLayerDesc d;
                d.sc = &layerSC[i];
                d.x = wsl::kLayerX;
                d.y = y;
                d.width = wsl::kLayerW;
                d.height = wsl::kLayerH;
                d.disparity = -0.01f * (float)i;
                descs.push_back(d);
            }
            SubmitWindowSpaceLayersFrame(xr.session, xr.localSpace,
                frameState.predictedDisplayTime, blendMode,
                projectionViews.data(), (uint32_t)eyeCount,
                descs.data(), (uint32_t)descs.size());
        } else {
            EndFrame(xr, frameState.predictedDisplayTime, projectionViews.data(), (uint32_t)eyeCount);
        }
    }

    LOG_INFO("=== Shutting down ===");
    g_xr = nullptr;
    CleanupOpenXR(xr);
    CleanupD3D11(renderer);
    DestroyWindow(hwnd);
    UnregisterClass(WINDOW_CLASS, hInstance);
    ShutdownLogging();
    return 0;
}
