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

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <sstream>
#include <thread>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

// Application name for logging
static const char* APP_NAME = "sr_cube_openxr_ext";

// Window settings
static const wchar_t* WINDOW_CLASS = L"SRCubeOpenXRExtClass";
static const wchar_t* WINDOW_TITLE = L"SR Cube OpenXR Ext - XR_EXT_session_target (Press ESC to exit)";

// Global state (shared between main thread and render thread)
static InputState g_inputState;            // Protected by g_inputMutex
static std::mutex g_inputMutex;            // Guards g_inputState + window dimensions
static std::atomic<bool> g_running{true};  // Atomic: main thread writes, render thread reads
static UINT g_windowWidth = 1280;          // Protected by g_inputMutex
static UINT g_windowHeight = 720;          // Protected by g_inputMutex
static const float HUD_WIDTH_PERCENT = 0.30f;
static const float HUD_HEIGHT_PERCENT = 0.35f;

// Window procedure (runs on main thread)
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    {
        std::lock_guard<std::mutex> lock(g_inputMutex);
        UpdateInputState(g_inputState, msg, wParam, lParam);
    }

    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_windowWidth = LOWORD(lParam);
            g_windowHeight = HIWORD(lParam);
        }
        return 0;

    case WM_CLOSE:
        g_running.store(false);
        PostQuitMessage(0);
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            g_running.store(false);
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

// Render thread function — runs the OpenXR frame loop independently of the
// Win32 message pump so that rendering continues even when the user drags or
// resizes the window (DefWindowProc enters a modal loop that blocks the main
// thread during those operations).
static void RenderThreadFunc(
    HWND hwnd,
    XrSessionManager* xr,
    D3D11Renderer* renderer,
    TextOverlay* textOverlay,
    std::vector<XrSwapchainImageD3D11KHR>* hudSwapchainImages,
    ComPtr<ID3D11Texture2D>* depthTextures,
    ComPtr<ID3D11DepthStencilView>* depthDSVs,
    std::vector<XrSwapchainImageD3D11KHR>* swapchainImages)
{
    LOG_INFO("[RenderThread] Started");

    PerformanceStats perfStats = {};
    perfStats.lastTime = std::chrono::high_resolution_clock::now();

    while (g_running.load() && !xr->exitRequested) {
        // Snapshot input state under lock — hold the lock as briefly as possible
        InputState inputSnapshot;
        bool resetRequested = false;
        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            inputSnapshot = g_inputState;
            resetRequested = g_inputState.resetViewRequested;
            // Clear one-shot flags so they don't fire again
            g_inputState.resetViewRequested = false;
            g_inputState.fullscreenToggleRequested = false;
        }

        // Update performance stats
        UpdatePerformanceStats(perfStats);

        // Update input-based camera movement (operates on local snapshot)
        UpdateCameraMovement(inputSnapshot, perfStats.deltaTime);

        // Write back camera position (only the render thread updates these via
        // WASD/QE movement). yaw/pitch/zoomScale are NOT written back because
        // they are modified by the main thread's WindowProc (mouse drag/scroll)
        // and writing them back would stomp on concurrent input.
        // Exception: on view reset, UpdateCameraMovement zeroes everything, so
        // we must also write back yaw/pitch/zoomScale in that case.
        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_inputState.cameraPosX = inputSnapshot.cameraPosX;
            g_inputState.cameraPosY = inputSnapshot.cameraPosY;
            g_inputState.cameraPosZ = inputSnapshot.cameraPosZ;
            if (resetRequested) {
                g_inputState.yaw = inputSnapshot.yaw;
                g_inputState.pitch = inputSnapshot.pitch;
                g_inputState.zoomScale = inputSnapshot.zoomScale;
            }
        }

        // Update scene (cube rotation)
        UpdateScene(*renderer, perfStats.deltaTime);

        // Poll OpenXR events
        PollEvents(*xr);

        // Only render if session is running
        if (xr->sessionRunning) {
            XrFrameState frameState;
            if (BeginFrame(*xr, frameState)) {
                XrCompositionLayerProjectionView projectionViews[2] = {};
                bool hudSubmitted = false;

                if (frameState.shouldRender) {
                    XMMATRIX leftViewMatrix, leftProjMatrix;
                    XMMATRIX rightViewMatrix, rightProjMatrix;

                    if (LocateViews(*xr, frameState.predictedDisplayTime,
                        leftViewMatrix, leftProjMatrix,
                        rightViewMatrix, rightProjMatrix,
                        inputSnapshot.cameraPosX, inputSnapshot.cameraPosY, inputSnapshot.cameraPosZ,
                        inputSnapshot.yaw, inputSnapshot.pitch)) {

                        // Get raw view poses (pre-player-transform) for projection views
                        XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
                        locateInfo.viewConfigurationType = xr->viewConfigType;
                        locateInfo.displayTime = frameState.predictedDisplayTime;
                        locateInfo.space = xr->localSpace;

                        XrViewState viewState = {XR_TYPE_VIEW_STATE};
                        uint32_t viewCount = 2;
                        XrView rawViews[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
                        xrLocateViews(xr->session, &locateInfo, &viewState, 2, &viewCount, rawViews);

                        // [Commented out — will be reused for 3D-positioned HUD later]
                        // ConvergencePlane convPlane = LocateConvergencePlane(rawViews);

                        // Render HUD to window-space layer swapchain (once per frame, before eye loop)
                        if (inputSnapshot.hudVisible && xr->hasHudSwapchain && hudSwapchainImages && !hudSwapchainImages->empty()) {
                            uint32_t hudImageIndex;
                            if (AcquireHudSwapchainImage(*xr, hudImageIndex)) {
                                ID3D11Texture2D* hudTexture = (*hudSwapchainImages)[hudImageIndex].texture;

                                ID3D11RenderTargetView* hudRtv = nullptr;
                                CreateRenderTargetView(*renderer, hudTexture, &hudRtv);
                                if (hudRtv) {
                                    float hudClear[4] = {0.0f, 0.0f, 0.0f, 0.7f};
                                    renderer->context->ClearRenderTargetView(hudRtv, hudClear);
                                    hudRtv->Release();
                                }

                                float sx = xr->hudSwapchain.width / 512.0f;
                                float sy = xr->hudSwapchain.height / 256.0f;

                                std::wstring stateText = L"Session: ";
                                stateText += FormatSessionState((int)xr->sessionState);
                                RenderText(*textOverlay, renderer->device.Get(), hudTexture,
                                    stateText, 10*sx, 10*sy, 300*sx, 30*sy);

                                std::wstring extText = xr->hasSessionTargetExt ?
                                    L"XR_EXT_session_target: ACTIVE" :
                                    L"XR_EXT_session_target: NOT AVAILABLE";
                                RenderText(*textOverlay, renderer->device.Get(), hudTexture,
                                    extText, 10*sx, 45*sy, 350*sx, 30*sy, true);

                                std::wstring perfText = FormatPerformanceInfo(perfStats.fps, perfStats.frameTimeMs,
                                    xr->swapchains[0].width, xr->swapchains[0].height);
                                RenderText(*textOverlay, renderer->device.Get(), hudTexture,
                                    perfText, 10*sx, 85*sy, 300*sx, 70*sy, true);

                                std::wstring eyeText = FormatEyeTrackingInfo(xr->eyePosX, xr->eyePosY, xr->eyePosZ, xr->eyeTrackingActive);
                                RenderText(*textOverlay, renderer->device.Get(), hudTexture,
                                    eyeText, 10*sx, 165*sy, 300*sx, 70*sy, true);

                                ReleaseHudSwapchainImage(*xr);
                                hudSubmitted = true;
                            }
                        }

                        // Render each eye
                        for (int eye = 0; eye < 2; eye++) {
                            uint32_t imageIndex;
                            if (AcquireSwapchainImage(*xr, eye, imageIndex)) {
                                ID3D11Texture2D* swapchainTexture = swapchainImages[eye][imageIndex].texture;

                                ID3D11RenderTargetView* rtv = nullptr;
                                CreateRenderTargetView(*renderer, swapchainTexture, &rtv);

                                D3D11_VIEWPORT vp = {};
                                vp.Width = (FLOAT)xr->swapchains[eye].width;
                                vp.Height = (FLOAT)xr->swapchains[eye].height;
                                vp.MaxDepth = 1.0f;
                                renderer->context->RSSetViewports(1, &vp);

                                float clearColor[4] = {0.05f, 0.05f, 0.25f, 1.0f};
                                renderer->context->ClearRenderTargetView(rtv, clearColor);
                                renderer->context->ClearDepthStencilView(depthDSVs[eye].Get(),
                                    D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

                                XMMATRIX viewMatrix = (eye == 0) ? leftViewMatrix : rightViewMatrix;
                                XMMATRIX projMatrix = (eye == 0) ? leftProjMatrix : rightProjMatrix;

                                RenderScene(*renderer, rtv, depthDSVs[eye].Get(),
                                    xr->swapchains[eye].width, xr->swapchains[eye].height,
                                    viewMatrix, projMatrix,
                                    inputSnapshot.zoomScale);

                                if (rtv) rtv->Release();

                                ReleaseSwapchainImage(*xr, eye);

                                projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                                projectionViews[eye].subImage.swapchain = xr->swapchains[eye].swapchain;
                                projectionViews[eye].subImage.imageRect.offset = {0, 0};
                                projectionViews[eye].subImage.imageRect.extent = {
                                    (int32_t)xr->swapchains[eye].width,
                                    (int32_t)xr->swapchains[eye].height
                                };
                                projectionViews[eye].subImage.imageArrayIndex = 0;

                                projectionViews[eye].pose = rawViews[eye].pose;
                                projectionViews[eye].fov = rawViews[eye].fov;
                            }
                        }
                    }
                }

                // Submit frame with window-space HUD layer if visible
                if (hudSubmitted) {
                    EndFrameWithWindowSpaceHud(*xr, frameState.predictedDisplayTime, projectionViews,
                        0.0f, 0.0f, HUD_WIDTH_PERCENT, HUD_HEIGHT_PERCENT, 0.0f);
                } else {
                    EndFrame(*xr, frameState.predictedDisplayTime, projectionViews);
                }
            }
        } else {
            Sleep(100);
        }
    }

    // If XR requested exit while the window is still open, post WM_CLOSE to
    // unblock GetMessage on the main thread.
    if (xr->exitRequested && g_running.load()) {
        PostMessage(hwnd, WM_CLOSE, 0, 0);
    }

    LOG_INFO("[RenderThread] Exiting");
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

    // Enumerate D3D11 swapchain images (now done per-app since common is API-agnostic)
    std::vector<XrSwapchainImageD3D11KHR> swapchainImages[2];
    for (int eye = 0; eye < 2; eye++) {
        uint32_t count = xr.swapchains[eye].imageCount;
        swapchainImages[eye].resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        xrEnumerateSwapchainImages(xr.swapchains[eye].swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)swapchainImages[eye].data());
        LOG_INFO("Eye %d: enumerated %u D3D11 swapchain images", eye, count);
    }

    // Create HUD swapchain for window-space layer submission
    uint32_t hudWidth = (uint32_t)(xr.swapchains[0].width * HUD_WIDTH_PERCENT);
    uint32_t hudHeight = (uint32_t)(xr.swapchains[0].height * HUD_HEIGHT_PERCENT);

    if (!CreateHudSwapchain(xr, hudWidth, hudHeight)) {
        LOG_WARN("Failed to create HUD swapchain - HUD will not be displayed");
    }

    // Enumerate HUD swapchain images
    std::vector<XrSwapchainImageD3D11KHR> hudSwapchainImages;
    if (xr.hasHudSwapchain) {
        uint32_t count = xr.hudSwapchain.imageCount;
        hudSwapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        xrEnumerateSwapchainImages(xr.hudSwapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)hudSwapchainImages.data());
        LOG_INFO("HUD: enumerated %u D3D11 swapchain images", count);
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

    LOG_INFO("");
    LOG_INFO("=== Entering main loop ===");
    LOG_INFO("XR rendering happens in the application window (XR_EXT_session_target)");
    LOG_INFO("Render thread handles OpenXR frame loop; main thread handles Win32 messages");
    LOG_INFO("Controls: WASD=Fly, QE=Up/Down, Mouse=Look, Space/DblClick=Reset, P=Parallax, TAB=HUD, ESC=Quit");
    LOG_INFO("");

    // Launch render thread — the frame loop runs independently of the message pump
    // so that rendering continues even when DefWindowProc blocks during window drag/resize.
    std::thread renderThread(RenderThreadFunc, hwnd, &xr, &renderer, &textOverlay,
        &hudSwapchainImages, depthTextures, depthDSVs, swapchainImages);

    // Main thread: blocking Win32 message pump.
    // GetMessage blocks efficiently (no CPU spin) until a message arrives.
    // When the window is being dragged/resized, DefWindowProc runs a modal loop
    // here on the main thread — but the render thread keeps submitting frames.
    MSG msg = {};
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // GetMessage returned 0 (WM_QUIT) or -1 (error) — signal render thread to stop
    g_running.store(false);

    LOG_INFO("Main thread: waiting for render thread to finish...");
    renderThread.join();
    LOG_INFO("Main thread: render thread joined");

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
