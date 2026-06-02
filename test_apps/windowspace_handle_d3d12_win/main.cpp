// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  windowspace_handle_d3d12_win — submit N XR_EXT window-space layers, no 3D scene.
 *
 * D3D12 handle-class app for issue #389. Flat-cleared projection layer + N
 * window-space layer swapchains, each filled with a distinct solid color via
 * the same upload-heap + CopyTextureRegion path the cube_handle_d3d12 app uses
 * for its HUD. Pass the layer count as argv[1] (default 6, clamped [1,12]).
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>

#include "logging.h"
#include "xr_session.h"
#include "d3d12_renderer.h"
#include "xr_window_space_hud.h"
#include "windowspace_layers.h"

#include <vector>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

static const char* APP_NAME = "windowspace_handle_d3d12_win";
static const wchar_t* WINDOW_CLASS = L"DXRWindowSpaceD3D12Class";
static const wchar_t* WINDOW_TITLE = L"Window-Space Layers — D3D12 Native Compositor";

static bool g_running = true;
static XrSessionManager* g_xr = nullptr;
static UINT g_windowWidth = 1280;
static UINT g_windowHeight = 720;

// Per-layer upload + copy resources for the CopyTextureRegion path.
struct LayerUpload {
    ComPtr<ID3D12Resource> uploadBuffer;
    uint8_t* mapped = nullptr;
    uint32_t rowPitch = 0;
};

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) { g_windowWidth = LOWORD(lParam); g_windowHeight = HIWORD(lParam); }
        return 0;
    case WM_CLOSE:
        if (g_xr && g_xr->session != XR_NULL_HANDLE && g_xr->sessionRunning) {
            xrRequestExitSession(g_xr->session); return 0;
        }
        g_running = false; PostQuitMessage(0); return 0;
    case WM_SYSKEYDOWN:
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) { PostMessage(hwnd, WM_CLOSE, 0, 0); return 0; }
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
        if (err != ERROR_CLASS_ALREADY_EXISTS) { LOG_ERROR("RegisterClassEx err %lu", err); return nullptr; }
    }
    RECT rect = { 0, 0, width, height };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowEx(0, WINDOW_CLASS, WINDOW_TITLE, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) { LOG_ERROR("CreateWindowEx err %lu", GetLastError()); return nullptr; }
    return hwnd;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance; (void)lpCmdLine;
    if (!InitializeLogging(APP_NAME)) {
        MessageBox(nullptr, L"Failed to initialize logging", L"Warning", MB_OK | MB_ICONWARNING);
    }

    const uint32_t layerCount = wsl::ParseLayerCount(__argc, __argv);
    LOG_INFO("=== windowspace_handle_d3d12_win === N=%u window-space layers", layerCount);

    HWND hwnd = CreateAppWindow(hInstance, g_windowWidth, g_windowHeight);
    if (!hwnd) { ShutdownLogging(); return 1; }

    XrSessionManager xr = {};
    g_xr = &xr;
    if (!InitializeOpenXR(xr)) { LOG_ERROR("OpenXR init failed"); g_xr = nullptr; ShutdownLogging(); return 1; }

    LUID adapterLuid;
    if (!GetD3D12GraphicsRequirements(xr, &adapterLuid)) {
        LOG_ERROR("D3D12 graphics requirements failed"); CleanupOpenXR(xr); ShutdownLogging(); return 1;
    }

    D3D12Renderer renderer = {};
    if (!InitializeD3D12WithLUID(renderer, adapterLuid)) {
        LOG_ERROR("D3D12 init failed"); CleanupOpenXR(xr); ShutdownLogging(); return 1;
    }

    if (!CreateSession(xr, renderer.device.Get(), renderer.commandQueue.Get(), hwnd)) {
        LOG_ERROR("Session creation failed"); CleanupD3D12(renderer); CleanupOpenXR(xr); ShutdownLogging(); return 1;
    }
    if (!CreateSpaces(xr)) { LOG_ERROR("Spaces failed"); CleanupD3D12(renderer); CleanupOpenXR(xr); ShutdownLogging(); return 1; }
    if (!CreateSwapchain(xr)) { LOG_ERROR("Swapchain failed"); CleanupD3D12(renderer); CleanupOpenXR(xr); ShutdownLogging(); return 1; }

    // Enumerate projection swapchain images + create RTVs.
    std::vector<XrSwapchainImageD3D12KHR> swapchainImages;
    int rtvBaseIndex = 0;
    {
        uint32_t count = xr.swapchain.imageCount;
        swapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)swapchainImages.data());
        std::vector<ID3D12Resource*> textures(count);
        for (uint32_t i = 0; i < count; i++) textures[i] = swapchainImages[i].texture;
        rtvBaseIndex = (int)renderer.rtvCount;
        if (!CreateSwapchainRTVs(renderer, textures.data(), count,
                xr.swapchain.width, xr.swapchain.height, (DXGI_FORMAT)xr.swapchain.format)) {
            LOG_ERROR("RTV creation failed"); CleanupD3D12(renderer); CleanupOpenXR(xr); ShutdownLogging(); return 1;
        }
        LOG_INFO("Enumerated %u D3D12 projection swapchain images", count);
    }

    // Create N window-space layer swapchains + enumerate images + solid-color buffers.
    std::vector<XrHudSwapchain> layerSC(layerCount);
    std::vector<std::vector<XrSwapchainImageD3D12KHR>> layerImages(layerCount);
    std::vector<std::vector<uint8_t>> layerPixels(layerCount);
    for (uint32_t i = 0; i < layerCount; ++i) {
        if (!CreateHudSwapchain(xr.session, wsl::kLayerPxWidth, wsl::kLayerPxHeight, layerSC[i])) {
            LOG_WARN("Window-space swapchain %u create failed", i); continue;
        }
        uint32_t cnt = layerSC[i].imageCount;
        layerImages[i].resize(cnt, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        xrEnumerateSwapchainImages(layerSC[i].swapchain, cnt, &cnt,
            (XrSwapchainImageBaseHeader*)layerImages[i].data());
        uint8_t rgba[4];
        wsl::LayerColor(i, layerCount, rgba);
        wsl::FillSolid(layerPixels[i], wsl::kLayerPxWidth, wsl::kLayerPxHeight, rgba);
    }

    // Shared D3D12 upload resources for the per-layer CopyTextureRegion path.
    const uint32_t uploadRowPitch =
        (wsl::kLayerPxWidth * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    std::vector<LayerUpload> uploads(layerCount);
    ComPtr<ID3D12CommandAllocator> cmdAllocator;
    ComPtr<ID3D12GraphicsCommandList> cmdList;
    ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent = nullptr;
    UINT64 fenceValue = 0;
    bool uploadOk = true;
    {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bufDesc = {};
        bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufDesc.Width = (UINT64)uploadRowPitch * wsl::kLayerPxHeight;
        bufDesc.Height = 1; bufDesc.DepthOrArraySize = 1; bufDesc.MipLevels = 1;
        bufDesc.SampleDesc.Count = 1; bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        for (uint32_t i = 0; i < layerCount && uploadOk; ++i) {
            if (layerSC[i].swapchain == XR_NULL_HANDLE) continue;
            HRESULT hr = renderer.device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploads[i].uploadBuffer));
            if (FAILED(hr)) { LOG_WARN("Upload buffer %u failed 0x%08X", i, hr); uploadOk = false; break; }
            D3D12_RANGE readRange = {0, 0};
            hr = uploads[i].uploadBuffer->Map(0, &readRange, (void**)&uploads[i].mapped);
            if (FAILED(hr)) { LOG_WARN("Map upload buffer %u failed 0x%08X", i, hr); uploadOk = false; break; }
            uploads[i].rowPitch = uploadRowPitch;
            // Solid color is static — populate the upload buffer once.
            const uint8_t* src = layerPixels[i].data();
            for (uint32_t row = 0; row < wsl::kLayerPxHeight; ++row)
                memcpy(uploads[i].mapped + row * uploadRowPitch, src + row * wsl::kLayerPxWidth * 4, wsl::kLayerPxWidth * 4);
        }
        if (uploadOk) {
            if (FAILED(renderer.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAllocator))) ||
                FAILED(renderer.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAllocator.Get(), nullptr, IID_PPV_ARGS(&cmdList))) ||
                FAILED(renderer.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
                LOG_WARN("D3D12 command/fence objects failed"); uploadOk = false;
            } else {
                cmdList->Close();
                fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            }
        }
    }
    if (!uploadOk) LOG_WARN("Window-space layer upload disabled (resource creation failed)");

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    XrEnvironmentBlendMode blendMode = (xr.envBlendModeCount > 0) ? xr.envBlendModes[0] : XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

    MSG msg = {};
    while (g_running && !xr.exitRequested) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_running = false; break; }
            TranslateMessage(&msg); DispatchMessage(&msg);
        }
        if (!g_running) break;

        PollEvents(xr);
        if (!xr.sessionRunning) { Sleep(50); continue; }

        XrFrameState frameState;
        if (!BeginFrame(xr, frameState)) continue;

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
                renderW = g_windowWidth; renderH = g_windowHeight;
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
                ID3D12Resource* swapchainTexture = swapchainImages[imageIndex].texture;
                int rtvIdx = rtvBaseIndex + (int)imageIndex;

                // Clear the projection image to a flat dark background (no 3D scene).
                cmdAllocator->Reset();
                cmdList->Reset(cmdAllocator.Get(), nullptr);
                D3D12_RESOURCE_BARRIER toRT = {};
                toRT.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                toRT.Transition.pResource = swapchainTexture;
                toRT.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                toRT.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                toRT.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                cmdList->ResourceBarrier(1, &toRT);
                D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = renderer.rtvHeap->GetCPUDescriptorHandleForHeapStart();
                rtvHandle.ptr += (SIZE_T)rtvIdx * renderer.rtvDescriptorSize;
                const float clearColor[4] = {
                    wsl::kBgR / 255.0f, wsl::kBgG / 255.0f, wsl::kBgB / 255.0f, wsl::kBgA / 255.0f };
                cmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
                D3D12_RESOURCE_BARRIER toCommon = toRT;
                toCommon.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                toCommon.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
                cmdList->ResourceBarrier(1, &toCommon);
                cmdList->Close();
                ID3D12CommandList* lists[] = { cmdList.Get() };
                renderer.commandQueue->ExecuteCommandLists(1, lists);
                fenceValue++;
                renderer.commandQueue->Signal(fence.Get(), fenceValue);
                if (fence->GetCompletedValue() < fenceValue) {
                    fence->SetEventOnCompletion(fenceValue, fenceEvent);
                    WaitForSingleObject(fenceEvent, INFINITE);
                }

                for (int eye = 0; eye < eyeCount; eye++) {
                    uint32_t tileX = monoMode ? 0 : (eye % tileColumns);
                    uint32_t tileY = monoMode ? 0 : (eye / tileColumns);
                    projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                    projectionViews[eye].subImage.swapchain = xr.swapchain.swapchain;
                    projectionViews[eye].subImage.imageRect.offset = {
                        (int32_t)(tileX * renderW), (int32_t)(tileY * renderH) };
                    projectionViews[eye].subImage.imageRect.extent = { (int32_t)renderW, (int32_t)renderH };
                    projectionViews[eye].subImage.imageArrayIndex = 0;
                    int safeIdx = (eye < (int)viewCount) ? eye : 0;
                    projectionViews[eye].pose = rawViews[safeIdx].pose;
                    projectionViews[eye].fov = rawViews[safeIdx].fov;
                }
                ReleaseSwapchainImage(xr);
                rendered = true;
            }

            // Fill each window-space layer via CopyTextureRegion from its upload buffer.
            if (uploadOk) {
                for (uint32_t i = 0; i < layerCount; ++i) {
                    if (layerSC[i].swapchain == XR_NULL_HANDLE || !uploads[i].uploadBuffer) continue;
                    uint32_t idx = 0;
                    if (!AcquireHudSwapchainImage(layerSC[i], idx)) continue;
                    if (idx < layerImages[i].size() && layerImages[i][idx].texture) {
                        ID3D12Resource* tex = layerImages[i][idx].texture;
                        cmdAllocator->Reset();
                        cmdList->Reset(cmdAllocator.Get(), nullptr);
                        D3D12_RESOURCE_BARRIER barrier = {};
                        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        barrier.Transition.pResource = tex;
                        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                        cmdList->ResourceBarrier(1, &barrier);
                        D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
                        srcLoc.pResource = uploads[i].uploadBuffer.Get();
                        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                        srcLoc.PlacedFootprint.Offset = 0;
                        srcLoc.PlacedFootprint.Footprint.Format = (DXGI_FORMAT)layerSC[i].format;
                        srcLoc.PlacedFootprint.Footprint.Width = wsl::kLayerPxWidth;
                        srcLoc.PlacedFootprint.Footprint.Height = wsl::kLayerPxHeight;
                        srcLoc.PlacedFootprint.Footprint.Depth = 1;
                        srcLoc.PlacedFootprint.Footprint.RowPitch = uploads[i].rowPitch;
                        D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
                        dstLoc.pResource = tex;
                        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                        dstLoc.SubresourceIndex = 0;
                        cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
                        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
                        cmdList->ResourceBarrier(1, &barrier);
                        cmdList->Close();
                        ID3D12CommandList* lists2[] = { cmdList.Get() };
                        renderer.commandQueue->ExecuteCommandLists(1, lists2);
                        fenceValue++;
                        renderer.commandQueue->Signal(fence.Get(), fenceValue);
                        if (fence->GetCompletedValue() < fenceValue) {
                            fence->SetEventOnCompletion(fenceValue, fenceEvent);
                            WaitForSingleObject(fenceEvent, INFINITE);
                        }
                    }
                    ReleaseHudSwapchainImage(layerSC[i]);
                }
            }
        }

        if (rendered) {
            std::vector<WindowSpaceLayerDesc> descs;
            descs.reserve(layerCount);
            for (uint32_t i = 0; i < layerCount; ++i) {
                if (layerSC[i].swapchain == XR_NULL_HANDLE) continue;
                float y = wsl::kLayerYStart + i * wsl::kLayerYStride;
                if (y + wsl::kLayerH > 1.0f) continue;
                WindowSpaceLayerDesc d;
                d.sc = &layerSC[i];
                d.x = wsl::kLayerX; d.y = y; d.width = wsl::kLayerW; d.height = wsl::kLayerH;
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
    if (fenceEvent) CloseHandle(fenceEvent);
    for (auto& u : uploads) {
        if (u.mapped && u.uploadBuffer) { u.uploadBuffer->Unmap(0, nullptr); u.mapped = nullptr; }
    }
    g_xr = nullptr;
    CleanupOpenXR(xr);
    CleanupD3D12(renderer);
    DestroyWindow(hwnd);
    UnregisterClass(WINDOW_CLASS, hInstance);
    ShutdownLogging();
    return 0;
}
