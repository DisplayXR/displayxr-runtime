// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  SR Cube OpenXR Ext D3D12 - OpenXR with XR_EXT_win32_window_binding (D3D12)
 *
 * D3D12 port of sr_cube_openxr_ext with window-space HUD overlay.
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>

#include "logging.h"
#include "input_handler.h"
#include "xr_session.h"
#include "d3d12_renderer.h"
#include "hud_renderer.h"
#include "text_overlay.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <wrl/client.h>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

static const char* APP_NAME = "sr_cube_openxr_ext_d3d12";

// HUD overlay: WIDTH_FRACTION anchors how wide the HUD appears on screen.
// D3D12 swapchains are imported into the Vulkan native compositor via shared handles.
// Non-power-of-2 dimensions cause a size mismatch between D3D12 and Vulkan memory layouts
// (nvidia bug - see comp_d3d12_client.cpp). Use power-of-2 to avoid this.
static const uint32_t HUD_PIXEL_WIDTH = 512;
static const uint32_t HUD_PIXEL_HEIGHT = 560;
static const float HUD_WIDTH_FRACTION = 0.30f;

// sim_display output mode switching (loaded at runtime via GetProcAddress)
typedef void (*PFN_sim_display_set_output_mode)(int mode);
static PFN_sim_display_set_output_mode g_pfnSetOutputMode = nullptr;

static const wchar_t* WINDOW_CLASS = L"SRCubeOpenXRExtD3D12Class";
static const wchar_t* WINDOW_TITLE = L"SR Cube OpenXR Ext D3D12 (Press ESC to exit)";

// Global state (shared between main thread and render thread)
static InputState g_inputState;
static std::mutex g_inputMutex;
static std::atomic<bool> g_running{true};
static XrSessionManager* g_xr = nullptr;
static UINT g_windowWidth = 1280;
static UINT g_windowHeight = 720;

// Fullscreen state
static bool g_fullscreen = false;
static RECT g_savedWindowRect = {};
static DWORD g_savedWindowStyle = 0;

// Toggle fullscreen mode for the app window
static void ToggleFullscreen(HWND hwnd) {
    if (g_fullscreen) {
        // Exit fullscreen - restore window style and position
        SetWindowLong(hwnd, GWL_STYLE, g_savedWindowStyle);
        SetWindowPos(hwnd, HWND_TOP,
            g_savedWindowRect.left, g_savedWindowRect.top,
            g_savedWindowRect.right - g_savedWindowRect.left,
            g_savedWindowRect.bottom - g_savedWindowRect.top,
            SWP_FRAMECHANGED);
        g_fullscreen = false;
        LOG_INFO("Exited fullscreen mode");
    } else {
        // Enter fullscreen - save state and go borderless
        g_savedWindowStyle = GetWindowLong(hwnd, GWL_STYLE);
        GetWindowRect(hwnd, &g_savedWindowRect);

        HMONITOR hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfo(hMonitor, &mi);

        SetWindowLong(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_TOP,
            mi.rcMonitor.left, mi.rcMonitor.top,
            mi.rcMonitor.right - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top,
            SWP_FRAMECHANGED);
        g_fullscreen = true;
        LOG_INFO("Entered fullscreen mode");
    }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    {
        std::lock_guard<std::mutex> lock(g_inputMutex);
        UpdateInputState(g_inputState, msg, wParam, lParam);
    }

    switch (msg) {
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);  // Outside mutex — safe from reentrant deadlock
        return 0;

    case WM_LBUTTONUP:
        ReleaseCapture();  // Outside mutex — WM_CAPTURECHANGED can safely re-enter
        return 0;

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_windowWidth = LOWORD(lParam);
            g_windowHeight = HIWORD(lParam);
        }
        return 0;

    case WM_CLOSE:
        // Graceful shutdown: ask OpenXR to end the session so the state machine
        // runs STOPPING -> xrEndSession -> EXITING -> exitRequested before cleanup.
        if (g_xr && g_xr->session != XR_NULL_HANDLE && g_xr->sessionRunning) {
            xrRequestExitSession(g_xr->session);
            return 0;
        }
        g_running.store(false);
        PostQuitMessage(0);
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            PostMessage(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        if (wParam == VK_F11) {
            ToggleFullscreen(hwnd);
            return 0;
        }
        break;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

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

    HWND hwnd = CreateWindowEx(0, WINDOW_CLASS, WINDOW_TITLE, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd) {
        LOG_ERROR("Failed to create window, error: %lu", GetLastError());
        return nullptr;
    }

    LOG_INFO("Window created: 0x%p", hwnd);
    return hwnd;
}

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

static void RenderThreadFunc(
    HWND hwnd,
    XrSessionManager* xr,
    D3D12Renderer* renderer,
    std::vector<XrSwapchainImageD3D12KHR>* swapchainImages,
    int rtvBaseIndex,
    HudRenderer* hud,
    std::vector<XrSwapchainImageD3D12KHR>* hudSwapchainImages,
    ID3D12Resource* hudUploadBuffer,
    uint8_t* hudUploadMapped,
    uint32_t hudUploadRowPitch,
    ID3D12CommandAllocator* hudCmdAllocator,
    ID3D12GraphicsCommandList* hudCmdList,
    ID3D12Fence* hudFence,
    HANDLE hudFenceEvent)
{
    LOG_INFO("[RenderThread] Started");

    PerformanceStats perfStats = {};
    perfStats.lastTime = std::chrono::high_resolution_clock::now();
    UINT64 hudFenceValue = 0;

    while (g_running.load() && !xr->exitRequested) {
        InputState inputSnapshot;
        bool resetRequested = false;
        uint32_t windowW, windowH;
        bool outputModeChanged = false;
        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            inputSnapshot = g_inputState;
            resetRequested = g_inputState.resetViewRequested;
            g_inputState.resetViewRequested = false;
            g_inputState.fullscreenToggleRequested = false;
            outputModeChanged = g_inputState.outputModeChangeRequested;
            g_inputState.outputModeChangeRequested = false;
            windowW = g_windowWidth;
            windowH = g_windowHeight;
        }

        if (outputModeChanged && g_pfnSetOutputMode) {
            g_pfnSetOutputMode(inputSnapshot.outputMode);
        }

        UpdatePerformanceStats(perfStats);
        UpdateCameraMovement(inputSnapshot, perfStats.deltaTime);

        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_inputState.cameraPosX = inputSnapshot.cameraPosX;
            g_inputState.cameraPosY = inputSnapshot.cameraPosY;
            g_inputState.cameraPosZ = inputSnapshot.cameraPosZ;
            if (resetRequested) {
                g_inputState.yaw = inputSnapshot.yaw;
                g_inputState.pitch = inputSnapshot.pitch;
                g_inputState.stereo = inputSnapshot.stereo;
            }
        }

        UpdateScene(*renderer, perfStats.deltaTime);
        PollEvents(*xr);

        if (xr->sessionRunning) {
            XrFrameState frameState;
            if (BeginFrame(*xr, frameState)) {
                XrCompositionLayerProjectionView projectionViews[2] = {};
                bool rendered = false;
                bool hudSubmitted = false;

                if (frameState.shouldRender) {
                    XMMATRIX leftViewMatrix, leftProjMatrix;
                    XMMATRIX rightViewMatrix, rightProjMatrix;

                    if (LocateViews(*xr, frameState.predictedDisplayTime,
                        leftViewMatrix, leftProjMatrix,
                        rightViewMatrix, rightProjMatrix,
                        inputSnapshot.cameraPosX, inputSnapshot.cameraPosY, inputSnapshot.cameraPosZ,
                        inputSnapshot.yaw, inputSnapshot.pitch,
                        inputSnapshot.stereo)) {

                        // Get raw view poses for projection views.
                        // Use DISPLAY space when available: it is physically anchored to the
                        // display center and unaffected by recentering, which is the correct
                        // reference for compositing on tracked 3D displays.
                        // Falls back to LOCAL space if XR_EXT_display_info is not enabled.
                        XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
                        locateInfo.viewConfigurationType = xr->viewConfigType;
                        locateInfo.displayTime = frameState.predictedDisplayTime;
                        locateInfo.space = (xr->displaySpace != XR_NULL_HANDLE) ? xr->displaySpace : xr->localSpace;

                        XrViewState viewState = {XR_TYPE_VIEW_STATE};
                        uint32_t viewCount = 2;
                        XrView rawViews[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
                        xrLocateViews(xr->session, &locateInfo, &viewState, 2, &viewCount, rawViews);

                        // Store raw per-eye positions in display space for HUD
                        xr->leftEyeX = rawViews[0].pose.position.x;
                        xr->leftEyeY = rawViews[0].pose.position.y;
                        xr->leftEyeZ = rawViews[0].pose.position.z;
                        xr->rightEyeX = rawViews[1].pose.position.x;
                        xr->rightEyeY = rawViews[1].pose.position.y;
                        xr->rightEyeZ = rawViews[1].pose.position.z;

                        // Max per-eye capacity from swapchain
                        uint32_t eyeRenderW = xr->swapchain.width / 2;
                        uint32_t eyeRenderH = xr->swapchain.height;

                        // Dynamic render dims based on window size, clamped to swapchain capacity
                        uint32_t renderW = (uint32_t)(windowW * xr->recommendedViewScaleX);
                        uint32_t renderH = (uint32_t)(windowH * xr->recommendedViewScaleY);
                        if (renderW > eyeRenderW) renderW = eyeRenderW;
                        if (renderH > eyeRenderH) renderH = eyeRenderH;

                        // --- App-side Kooima projection (RAW mode, app-owned camera model) ---
                        // Full display pixel dimensions for pixel-to-meter conversion.
                        float dispPxW = xr->displayPixelWidth > 0 ? (float)xr->displayPixelWidth : (float)xr->swapchain.width;
                        float dispPxH = xr->displayPixelHeight > 0 ? (float)xr->displayPixelHeight : (float)xr->swapchain.height;

                        XrFovf appFov[2];
                        bool useAppProjection = (xr->hasDisplayInfoExt && xr->displayWidthM > 0.0f);
                        if (useAppProjection) {
                            // Viewport-scale FOV (SRHydra): convert window pixels to meters,
                            // then apply isotropic scale so FOV stays consistent across window
                            // sizes on the 3D display. Matches the non-extension runtime path.
                            float pxSizeX = xr->displayWidthM / dispPxW;
                            float pxSizeY = xr->displayHeightM / dispPxH;
                            float winW_m = (float)windowW * pxSizeX;
                            float winH_m = (float)windowH * pxSizeY;
                            float minDisp = fminf(xr->displayWidthM, xr->displayHeightM);
                            float minWin  = fminf(winW_m, winH_m);
                            float vs = minDisp / minWin;
                            float screenWidthM  = winW_m * vs;
                            float screenHeightM = winH_m * vs;

                            // Alternative: content-preserving FOV — keeps rendered content at
                            // constant physical size on display regardless of window size.
                            // float screenWidthM = xr->displayWidthM * (float)renderW / (float)(xr->swapchain.width / 2);
                            // float screenHeightM = xr->displayHeightM * (float)renderH / (float)xr->swapchain.height;

                        // Apply stereo eye factors (IPD + parallax) to raw eye positions
                        XrVector3f processedEyes[2];
                        ApplyEyeFactors(
                            rawViews[0].pose.position, rawViews[1].pose.position,
                            xr->nominalViewerX, xr->nominalViewerY, xr->nominalViewerZ,
                            inputSnapshot.stereo.ipdFactor, inputSnapshot.stereo.parallaxFactor,
                            processedEyes[0], processedEyes[1]);

                        // Kooima projection with perspective + scale factors
                        float kScreenW, kScreenH;
                        KooimaScreenDim(screenWidthM, screenHeightM,
                            inputSnapshot.stereo.scaleFactor, kScreenW, kScreenH);
                        for (int e = 0; e < 2; e++) {
                            XrVector3f kooimaEye = KooimaEyePos(processedEyes[e],
                                inputSnapshot.stereo.perspectiveFactor,
                                inputSnapshot.stereo.scaleFactor);
                            if (e == 0)
                                leftProjMatrix = ComputeKooimaProjection(
                                    kooimaEye, kScreenW, kScreenH, 0.01f, 100.0f);
                            else
                                rightProjMatrix = ComputeKooimaProjection(
                                    kooimaEye, kScreenW, kScreenH, 0.01f, 100.0f);
                            appFov[e] = ComputeKooimaFov(
                                kooimaEye, kScreenW, kScreenH);
                        }
                        }

                        rendered = true;
                        uint32_t imageIndex;
                        if (AcquireSwapchainImage(*xr, imageIndex)) {
                            ID3D12Resource* swapchainTexture = (*swapchainImages)[imageIndex].texture;
                            int rtvIdx = rtvBaseIndex + (int)imageIndex;

                            for (int eye = 0; eye < 2; eye++) {
                                XMMATRIX viewMatrix = (eye == 0) ? leftViewMatrix : rightViewMatrix;
                                XMMATRIX projMatrix = (eye == 0) ? leftProjMatrix : rightProjMatrix;

                                RenderScene(*renderer, swapchainTexture, rtvIdx,
                                    eye * renderW, 0,  // viewport offset
                                    renderW, renderH,
                                    viewMatrix, projMatrix,
                                    useAppProjection ? 1.0f : inputSnapshot.stereo.scaleFactor,
                                    eye == 0);  // clear only on first eye

                                projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                                projectionViews[eye].subImage.swapchain = xr->swapchain.swapchain;
                                projectionViews[eye].subImage.imageRect.offset = {(int32_t)(eye * renderW), 0};
                                projectionViews[eye].subImage.imageRect.extent = {
                                    (int32_t)renderW, (int32_t)renderH};
                                projectionViews[eye].subImage.imageArrayIndex = 0;
                                projectionViews[eye].pose = rawViews[eye].pose;
                                // Always submit runtime's raw FOV for compositing.
                                // The Vulkan compositor re-projects layers from submitted FOV
                                // -> display distortion FOV. Submitting the runtime FOV (which
                                // matches the distortion target) makes this a no-op, so the
                                // app's Kooima rendering directly controls output size.
                                projectionViews[eye].fov = rawViews[eye].fov;
                            }
                            ReleaseSwapchainImage(*xr);
                        } else {
                            rendered = false;
                        }

                        // Render HUD to window-space layer swapchain
                        if (rendered && inputSnapshot.hudVisible && hud && xr->hasHudSwapchain && hudSwapchainImages) {
                            uint32_t hudImageIndex;
                            if (AcquireHudSwapchainImage(*xr, hudImageIndex)) {
                                std::wstring sessionText(xr->systemName, xr->systemName + strlen(xr->systemName));
                                sessionText += L"\nSession: ";
                                sessionText += FormatSessionState((int)xr->sessionState);
                                std::wstring modeText = xr->hasWin32WindowBindingExt ?
                                    L"XR_EXT_win32_window_binding: ACTIVE (D3D12)" :
                                    L"XR_EXT_win32_window_binding: NOT AVAILABLE (D3D12)";
                                std::wstring perfText = FormatPerformanceInfo(perfStats.fps, perfStats.frameTimeMs,
                                    renderW, renderH, windowW, windowH);
                                std::wstring dispText = FormatDisplayInfo(xr->displayWidthM, xr->displayHeightM,
                                    xr->nominalViewerX, xr->nominalViewerY, xr->nominalViewerZ);
                                dispText += L"\n" + FormatScaleInfo(xr->recommendedViewScaleX, xr->recommendedViewScaleY);
                                dispText += L"\n" + FormatOutputMode(inputSnapshot.outputMode);
                                std::wstring eyeText = FormatEyeTrackingInfo(
                                    xr->leftEyeX, xr->leftEyeY, xr->leftEyeZ,
                                    xr->rightEyeX, xr->rightEyeY, xr->rightEyeZ,
                                    xr->eyeTrackingActive);

                                float fwdX = -sinf(inputSnapshot.yaw) * cosf(inputSnapshot.pitch);
                                float fwdY =  sinf(inputSnapshot.pitch);
                                float fwdZ = -cosf(inputSnapshot.yaw) * cosf(inputSnapshot.pitch);
                                std::wstring cameraText = FormatCameraInfo(
                                    inputSnapshot.cameraPosX, inputSnapshot.cameraPosY, inputSnapshot.cameraPosZ,
                                    fwdX, fwdY, fwdZ);
                                std::wstring stereoText = FormatStereoParams(
                                    inputSnapshot.stereo.ipdFactor, inputSnapshot.stereo.parallaxFactor,
                                    inputSnapshot.stereo.perspectiveFactor, inputSnapshot.stereo.scaleFactor);
                                std::wstring helpText = FormatHelpText();

                                uint32_t srcRowPitch = 0;
                                const void* pixels = RenderHudAndMap(*hud, &srcRowPitch, sessionText, modeText, perfText, dispText, eyeText,
                                    cameraText, stereoText, helpText);
                                if (pixels) {
                                    // Copy pixels row-by-row to D3D12 upload buffer (256-byte aligned rows)
                                    const uint8_t* src = (const uint8_t*)pixels;
                                    for (uint32_t row = 0; row < HUD_PIXEL_HEIGHT; row++) {
                                        memcpy(hudUploadMapped + row * hudUploadRowPitch,
                                            src + row * srcRowPitch,
                                            HUD_PIXEL_WIDTH * 4);
                                    }
                                    UnmapHud(*hud);

                                    // Record D3D12 commands: copy upload buffer to HUD swapchain texture
                                    ID3D12Resource* hudTex = (*hudSwapchainImages)[hudImageIndex].texture;

                                    hudCmdAllocator->Reset();
                                    hudCmdList->Reset(hudCmdAllocator, nullptr);

                                    // Barrier: COMMON -> COPY_DEST
                                    D3D12_RESOURCE_BARRIER barrier = {};
                                    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                                    barrier.Transition.pResource = hudTex;
                                    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                                    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                                    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                                    hudCmdList->ResourceBarrier(1, &barrier);

                                    // CopyTextureRegion from upload buffer
                                    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
                                    srcLoc.pResource = hudUploadBuffer;
                                    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                                    srcLoc.PlacedFootprint.Offset = 0;
                                    srcLoc.PlacedFootprint.Footprint.Format = (DXGI_FORMAT)xr->hudSwapchain.format;
                                    srcLoc.PlacedFootprint.Footprint.Width = HUD_PIXEL_WIDTH;
                                    srcLoc.PlacedFootprint.Footprint.Height = HUD_PIXEL_HEIGHT;
                                    srcLoc.PlacedFootprint.Footprint.Depth = 1;
                                    srcLoc.PlacedFootprint.Footprint.RowPitch = hudUploadRowPitch;

                                    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
                                    dstLoc.pResource = hudTex;
                                    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                                    dstLoc.SubresourceIndex = 0;

                                    hudCmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

                                    // Barrier: COPY_DEST -> COMMON
                                    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                                    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
                                    hudCmdList->ResourceBarrier(1, &barrier);

                                    hudCmdList->Close();

                                    // Execute and wait
                                    ID3D12CommandList* lists[] = { hudCmdList };
                                    renderer->commandQueue->ExecuteCommandLists(1, lists);
                                    hudFenceValue++;
                                    renderer->commandQueue->Signal(hudFence, hudFenceValue);
                                    if (hudFence->GetCompletedValue() < hudFenceValue) {
                                        hudFence->SetEventOnCompletion(hudFenceValue, hudFenceEvent);
                                        WaitForSingleObject(hudFenceEvent, INFINITE);
                                    }

                                    hudSubmitted = true;
                                }

                                ReleaseHudSwapchainImage(*xr);
                            }
                        }
                    }
                }

                // End frame: use window-space HUD layer if available
                LOG_INFO("[FRAME] EndFrame: rendered=%d hudSubmitted=%d", rendered, hudSubmitted);
                if (rendered && hudSubmitted) {
                    float hudAR = (float)HUD_PIXEL_WIDTH / (float)HUD_PIXEL_HEIGHT;
                    float windowAR = (windowW > 0 && windowH > 0) ? (float)windowW / (float)windowH : 1.0f;
                    float fracW = HUD_WIDTH_FRACTION;
                    float fracH = fracW * windowAR / hudAR;
                    if (fracH > 1.0f) { fracH = 1.0f; fracW = hudAR / windowAR; }
                    EndFrameWithWindowSpaceHud(*xr, frameState.predictedDisplayTime, projectionViews,
                        0.0f, 0.0f, fracW, fracH, 0.0f);
                } else if (rendered) {
                    EndFrame(*xr, frameState.predictedDisplayTime, projectionViews);
                } else {
                    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
                    endInfo.displayTime = frameState.predictedDisplayTime;
                    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                    endInfo.layerCount = 0;
                    endInfo.layers = nullptr;
                    xrEndFrame(xr->session, &endInfo);
                }
            }
        } else {
            Sleep(100);
        }
    }

    if (xr->exitRequested && g_running.load()) {
        PostMessage(hwnd, WM_CLOSE, 0, 0);
    }

    LOG_INFO("[RenderThread] Exiting");
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    if (!InitializeLogging(APP_NAME)) {
        MessageBox(nullptr, L"Failed to initialize logging", L"Warning", MB_OK | MB_ICONWARNING);
    }

    LOG_INFO("=== SR Cube OpenXR Ext D3D12 Application ===");

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
    XrSessionManager xr = {};
    g_xr = &xr;
    if (!InitializeOpenXR(xr)) {
        LOG_ERROR("OpenXR initialization failed");
        g_xr = nullptr;
        ShutdownLogging();
        return 1;
    }

    // Try to load sim_display_set_output_mode from the runtime (for 1/2/3 key switching)
    {
        HMODULE rtModule = GetModuleHandleA("openxr_monado.dll");
        if (!rtModule) rtModule = GetModuleHandleA("openxr_monado");
        if (rtModule) {
            g_pfnSetOutputMode = (PFN_sim_display_set_output_mode)GetProcAddress(rtModule, "sim_display_set_output_mode");
        }
        LOG_INFO("sim_display output mode: %s", g_pfnSetOutputMode ? "available" : "not available");
    }

    // Get D3D12 graphics requirements
    LUID adapterLuid;
    if (!GetD3D12GraphicsRequirements(xr, &adapterLuid)) {
        LOG_ERROR("Failed to get D3D12 graphics requirements");
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Initialize D3D12
    D3D12Renderer renderer = {};
    if (!InitializeD3D12WithLUID(renderer, adapterLuid)) {
        LOG_ERROR("D3D12 initialization failed");
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create session
    if (!CreateSession(xr, renderer.device.Get(), renderer.commandQueue.Get(), hwnd)) {
        LOG_ERROR("OpenXR session creation failed");
        CleanupD3D12(renderer);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    if (!CreateSpaces(xr)) {
        LOG_ERROR("Reference space creation failed");
        CleanupOpenXR(xr);
        CleanupD3D12(renderer);
        ShutdownLogging();
        return 1;
    }

    if (!CreateSwapchain(xr)) {
        LOG_ERROR("Swapchain creation failed");
        CleanupOpenXR(xr);
        CleanupD3D12(renderer);
        ShutdownLogging();
        return 1;
    }

    // Enumerate D3D12 swapchain images (single SBS swapchain)
    std::vector<XrSwapchainImageD3D12KHR> swapchainImages;
    int rtvBaseIndex = 0;
    {
        uint32_t count = xr.swapchain.imageCount;
        swapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)swapchainImages.data());
        LOG_INFO("Enumerated %u D3D12 swapchain images", count);

        // Collect ID3D12Resource pointers for RTV creation
        std::vector<ID3D12Resource*> textures(count);
        for (uint32_t i = 0; i < count; i++) {
            textures[i] = swapchainImages[i].texture;
        }

        rtvBaseIndex = (int)renderer.rtvCount;
        if (!CreateSwapchainRTVs(renderer, textures.data(), count,
            xr.swapchain.width, xr.swapchain.height,
            (DXGI_FORMAT)xr.swapchain.format)) {
            LOG_ERROR("Failed to create RTVs");
            CleanupOpenXR(xr);
            CleanupD3D12(renderer);
            ShutdownLogging();
            return 1;
        }
    }

    // Initialize HUD renderer (standalone D3D11 device for text rendering)
    HudRenderer hudRenderer = {};
    bool hudOk = InitializeHudRenderer(hudRenderer, HUD_PIXEL_WIDTH, HUD_PIXEL_HEIGHT);
    if (!hudOk) {
        LOG_WARN("HUD renderer init failed - HUD will not be displayed");
    }

    // Create HUD swapchain for window-space layer submission
    std::vector<XrSwapchainImageD3D12KHR> hudSwapImages;
    if (hudOk) {
        if (CreateHudSwapchain(xr, HUD_PIXEL_WIDTH, HUD_PIXEL_HEIGHT)) {
            uint32_t count = xr.hudSwapchain.imageCount;
            hudSwapImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
            xrEnumerateSwapchainImages(xr.hudSwapchain.swapchain, count, &count,
                (XrSwapchainImageBaseHeader*)hudSwapImages.data());
            LOG_INFO("HUD swapchain: enumerated %u D3D12 images", count);
        } else {
            LOG_WARN("HUD swapchain creation failed - HUD will not be displayed");
            hudOk = false;
        }
    }

    // Create D3D12 upload resources for HUD pixel transfer
    ComPtr<ID3D12Resource> hudUploadBuffer;
    uint8_t* hudUploadMapped = nullptr;
    ComPtr<ID3D12CommandAllocator> hudCmdAllocator;
    ComPtr<ID3D12GraphicsCommandList> hudCmdList;
    ComPtr<ID3D12Fence> hudFence;
    HANDLE hudFenceEvent = nullptr;
    // Row pitch must be aligned to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256 bytes)
    uint32_t hudUploadRowPitch = (HUD_PIXEL_WIDTH * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
        & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);

    if (hudOk) {
        // Upload buffer (UPLOAD heap, persistently mapped)
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bufDesc = {};
        bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufDesc.Width = (UINT64)hudUploadRowPitch * HUD_PIXEL_HEIGHT;
        bufDesc.Height = 1;
        bufDesc.DepthOrArraySize = 1;
        bufDesc.MipLevels = 1;
        bufDesc.SampleDesc.Count = 1;
        bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        HRESULT hr = renderer.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&hudUploadBuffer));
        if (FAILED(hr)) {
            LOG_WARN("Failed to create HUD upload buffer: 0x%08X", hr);
            hudOk = false;
        }

        if (hudOk) {
            D3D12_RANGE readRange = {0, 0}; // no CPU reads
            hr = hudUploadBuffer->Map(0, &readRange, (void**)&hudUploadMapped);
            if (FAILED(hr)) {
                LOG_WARN("Failed to map HUD upload buffer: 0x%08X", hr);
                hudOk = false;
            }
        }

        if (hudOk) {
            hr = renderer.device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&hudCmdAllocator));
            if (FAILED(hr)) {
                LOG_WARN("Failed to create HUD command allocator: 0x%08X", hr);
                hudOk = false;
            }
        }

        if (hudOk) {
            hr = renderer.device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT, hudCmdAllocator.Get(), nullptr,
                IID_PPV_ARGS(&hudCmdList));
            if (FAILED(hr)) {
                LOG_WARN("Failed to create HUD command list: 0x%08X", hr);
                hudOk = false;
            } else {
                hudCmdList->Close(); // start in closed state
            }
        }

        if (hudOk) {
            hr = renderer.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&hudFence));
            if (FAILED(hr)) {
                LOG_WARN("Failed to create HUD fence: 0x%08X", hr);
                hudOk = false;
            } else {
                hudFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            }
        }

        if (hudOk) {
            LOG_INFO("HUD D3D12 resources created (%ux%u, row pitch %u)", HUD_PIXEL_WIDTH, HUD_PIXEL_HEIGHT, hudUploadRowPitch);
        }
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    LOG_INFO("");
    LOG_INFO("=== Entering main loop ===");
    LOG_INFO("Controls: WASD=Fly, QE=Up/Down, Mouse=Look, Space/DblClick=Reset, TAB=HUD, F11=Fullscreen, ESC=Quit");
    LOG_INFO("");

    std::thread renderThread(RenderThreadFunc, hwnd, &xr, &renderer,
        &swapchainImages, rtvBaseIndex,
        hudOk ? &hudRenderer : nullptr,
        hudOk ? &hudSwapImages : nullptr,
        hudUploadBuffer.Get(), hudUploadMapped, hudUploadRowPitch,
        hudCmdAllocator.Get(), hudCmdList.Get(), hudFence.Get(), hudFenceEvent);

    MSG msg = {};
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    g_running.store(false);
    LOG_INFO("Main thread: waiting for render thread...");
    renderThread.join();
    LOG_INFO("Main thread: render thread joined");

    LOG_INFO("");
    LOG_INFO("=== Shutting down ===");

    // Clean up HUD resources
    if (hudFenceEvent) CloseHandle(hudFenceEvent);
    hudFence.Reset();
    hudCmdList.Reset();
    hudCmdAllocator.Reset();
    if (hudUploadMapped && hudUploadBuffer) {
        hudUploadBuffer->Unmap(0, nullptr);
        hudUploadMapped = nullptr;
    }
    hudUploadBuffer.Reset();
    if (hudOk) CleanupHudRenderer(hudRenderer);

    g_xr = nullptr;
    CleanupOpenXR(xr);
    CleanupD3D12(renderer);

    DestroyWindow(hwnd);
    UnregisterClass(WINDOW_CLASS, hInstance);

    LOG_INFO("Application shutdown complete");
    ShutdownLogging();

    return 0;
}
