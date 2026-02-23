// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  SR Cube Native - Direct SR SDK test without OpenXR
 *
 * This application demonstrates direct SR SDK usage with D3D11.
 * It uses Kooima projection for off-axis stereo rendering.
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <wrl/client.h>
#include <d3d11_1.h>
#include <dxgi1_4.h>
#include <shellscalingapi.h>

#include "logging.h"
#include "input_handler.h"
#include "d3d11_renderer.h"
#include "text_overlay.h"
#include "window_manager.h"
#include "leia_math.h"

#include <atomic>
#include <chrono>
#include <string>
#include <sstream>
#include <mutex>
#include <thread>

// Library for SetProcessDpiAwareness
#pragma comment(lib, "shcore.lib")

// LeiaSR SDK headers
#ifdef HAVE_LEIA_SR
#include "sr/utility/exception.h"
#include "sr/sense/core/inputstream.h"
#include "sr/sense/system/systemsense.h"
#include "sr/sense/eyetracker/eyetracker.h"
#include "sr/world/display/display.h"
#include "sr/weaver/dx11weaver.h"
#endif

using Microsoft::WRL::ComPtr;
using namespace DirectX;

// Application name for logging
static const char* APP_NAME = "sr_cube_native";

// Window settings
static const wchar_t* WINDOW_CLASS = L"SRCubeNativeClass";
static const wchar_t* WINDOW_TITLE = L"SR Cube (Native SDK)";

// Global state (shared between main thread and render thread)
static InputState g_inputState;              // Protected by g_mutex
static WindowInfo g_windowInfo;
static std::atomic<bool> g_running{true};    // Atomic: main thread writes, render thread reads
static UINT g_windowWidth = 2560;            // Protected by g_mutex
static UINT g_windowHeight = 1600;           // Protected by g_mutex
static bool g_windowResized = false;         // Protected by g_mutex
static std::mutex g_mutex;

// Swapchain for presentation (required for native app - weaver outputs here)
static ComPtr<IDXGISwapChain1> g_swapChain;
static ComPtr<ID3D11RenderTargetView> g_swapChainRTV;

// Stereo view texture resources (accessed by render thread)
static ComPtr<ID3D11Texture2D> g_viewTexture;
static ComPtr<ID3D11ShaderResourceView> g_viewTextureSRV;
static ComPtr<ID3D11RenderTargetView> g_viewTextureRTV;
static ComPtr<ID3D11Texture2D> g_viewDepthTexture;
static ComPtr<ID3D11DepthStencilView> g_viewDepthDSV;

#ifdef HAVE_LEIA_SR
// SR SDK global state
static SR::SRContext*        g_srContext = nullptr;
static SR::IDisplayManager*  g_displayManager = nullptr;
static SR::IDisplay*         g_display = nullptr;
static SR::IDX11Weaver1*     g_srWeaver = nullptr;

// Display properties
static float g_screenWidthMM = 0.0f;
static float g_screenHeightMM = 0.0f;
static leia::vec3f g_defaultViewingPosition = leia::vec3f(0.0f, 0.0f, 600.0f);
static int g_viewTextureWidth = 0;       // Active view texture width (may change on resize)
static int g_viewTextureHeight = 0;      // Active view texture height (may change on resize)
static int g_srRecommendedViewWidth = 0; // Original SR recommended width (never changes)
static int g_srRecommendedViewHeight = 0;// Original SR recommended height (never changes)

// Display pixel geometry (for viewport scale computation)
static int g_displayPixelWidth = 0;
static int g_displayPixelHeight = 0;
static int g_displayScreenLeft = 0;
static int g_displayScreenTop = 0;
#endif

// Window procedure (runs on main thread)
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        UpdateInputState(g_inputState, msg, wParam, lParam);
    }

    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_windowWidth = LOWORD(lParam);
            g_windowHeight = HIWORD(lParam);
            g_windowResized = true;
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

    case WM_GETMINMAXINFO:
    {
        MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        mmi->ptMinTrackSize.x = 100;
        mmi->ptMinTrackSize.y = 100;
        return 0;
    }
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
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

#ifdef HAVE_LEIA_SR

static bool CreateSRContext(double maxTimeSeconds) {
    LOG_INFO("Creating SR context (timeout: %.1fs)...", maxTimeSeconds);

    const double startTime = (double)GetTickCount64() / 1000.0;

    // Create SR context with retry loop
    while (g_srContext == nullptr) {
        try {
            g_srContext = SR::SRContext::create();
            break;
        }
        catch (SR::ServerNotAvailableException& e) {
            // SR Service may be starting up, ignore and retry
            (void)e;
        }
        catch (...) {
            LOG_ERROR("Unknown exception while creating SR context");
        }

        LOG_INFO("Waiting for SR context...");
        Sleep(100);

        double curTime = (double)GetTickCount64() / 1000.0;
        if ((curTime - startTime) > maxTimeSeconds) {
            LOG_ERROR("Timeout waiting for SR context");
            break;
        }
    }

    if (g_srContext == nullptr) {
        return false;
    }
    LOG_INFO("SR context created: 0x%p", g_srContext);

    // Get display manager using modern API
    try {
        g_displayManager = SR::GetDisplayManagerInstance(*g_srContext);
        if (g_displayManager != nullptr) {
            g_display = g_displayManager->getPrimaryActiveSRDisplay();
        }
    }
    catch (...) {
        LOG_ERROR("Failed to get DisplayManager - requires runtime version 1.34.8 or later");
        return false;
    }

    if (g_display == nullptr) {
        LOG_ERROR("No SR display found");
        return false;
    }

    // Wait for display to be ready
    bool displayReady = false;
    while (!displayReady) {
        if (g_display->isValid()) {
            SR_recti displayLocation = g_display->getLocation();
            int64_t width = displayLocation.right - displayLocation.left;
            int64_t height = displayLocation.bottom - displayLocation.top;
            if (width > 0 && height > 0) {
                displayReady = true;
                LOG_INFO("SR display ready: %lldx%lld at (%lld,%lld)",
                    width, height, displayLocation.left, displayLocation.top);
                break;
            }
        }

        LOG_INFO("Waiting for display to be ready...");
        Sleep(100);

        double curTime = (double)GetTickCount64() / 1000.0;
        if ((curTime - startTime) > maxTimeSeconds) {
            LOG_ERROR("Timeout waiting for display");
            break;
        }
    }

    return displayReady;
}

static bool InitializeLeiaSR(ID3D11DeviceContext* d3dContext, HWND hwnd, double maxTimeSeconds) {
    std::lock_guard<std::mutex> lock(g_mutex);

    LOG_INFO("Initializing LeiaSR...");

    // Create SR context
    if (!CreateSRContext(maxTimeSeconds)) {
        LOG_ERROR("Failed to create SR context");
        return false;
    }

    // Get display properties
    g_viewTextureWidth = g_display->getRecommendedViewsTextureWidth();
    g_viewTextureHeight = g_display->getRecommendedViewsTextureHeight();
    LOG_INFO("Recommended view texture size: %dx%d", g_viewTextureWidth, g_viewTextureHeight);

    // Get physical screen dimensions (cm to mm)
    g_screenWidthMM = g_display->getPhysicalSizeWidth() * 10.0f;
    g_screenHeightMM = g_display->getPhysicalSizeHeight() * 10.0f;
    LOG_INFO("Screen physical size: %.1fmm x %.1fmm", g_screenWidthMM, g_screenHeightMM);

    // Get default viewing position
    float x_mm, y_mm, z_mm;
    g_display->getDefaultViewingPosition(x_mm, y_mm, z_mm);
    g_defaultViewingPosition = leia::vec3f(x_mm, y_mm, z_mm);
    LOG_INFO("Default viewing position: (%.1f, %.1f, %.1f) mm", x_mm, y_mm, z_mm);

    // Create D3D11 weaver
    LOG_INFO("Creating D3D11 weaver...");
    WeaverErrorCode createWeaverResult = SR::CreateDX11Weaver(g_srContext, d3dContext, hwnd, &g_srWeaver);
    if (createWeaverResult != WeaverErrorCode::WeaverSuccess) {
        LOG_ERROR("Failed to create D3D11 weaver, error: %d", (int)createWeaverResult);
        return false;
    }
    LOG_INFO("D3D11 weaver created: 0x%p", g_srWeaver);

    // Initialize context after creating weaver
    g_srContext->initialize();
    LOG_INFO("SR context initialized");

    return true;
}

static void CleanupLeiaSR() {
    std::lock_guard<std::mutex> lock(g_mutex);

    LOG_INFO("Cleaning up LeiaSR...");

    if (g_srWeaver) {
        g_srWeaver->destroy();
        g_srWeaver = nullptr;
    }

    if (g_srContext) {
        SR::SRContext::deleteSRContext(g_srContext);
        g_srContext = nullptr;
    }

    g_displayManager = nullptr;
    g_display = nullptr;

    LOG_INFO("LeiaSR cleanup complete");
}

// Helper: (re)create stereo view texture resources at the given single-eye dimensions.
// Returns true on success. Releases old resources before creating new ones.
static bool CreateStereoViewTextures(ID3D11Device* device, int viewW, int viewH) {
    // Release old resources
    g_viewDepthDSV.Reset();
    g_viewDepthTexture.Reset();
    g_viewTextureRTV.Reset();
    g_viewTextureSRV.Reset();
    g_viewTexture.Reset();

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = viewW * 2;  // Side-by-side stereo
    texDesc.Height = viewH;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, &g_viewTexture);
    if (FAILED(hr)) {
        LOG_ERROR("Failed to create view texture (%dx%d)", viewW * 2, viewH);
        return false;
    }

    hr = device->CreateShaderResourceView(g_viewTexture.Get(), nullptr, &g_viewTextureSRV);
    if (FAILED(hr)) {
        LOG_ERROR("Failed to create view texture SRV");
        return false;
    }

    hr = device->CreateRenderTargetView(g_viewTexture.Get(), nullptr, &g_viewTextureRTV);
    if (FAILED(hr)) {
        LOG_ERROR("Failed to create view texture RTV");
        return false;
    }

    // Create depth texture
    texDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    texDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    hr = device->CreateTexture2D(&texDesc, nullptr, &g_viewDepthTexture);
    if (FAILED(hr)) {
        LOG_ERROR("Failed to create depth texture");
        return false;
    }

    hr = device->CreateDepthStencilView(g_viewDepthTexture.Get(), nullptr, &g_viewDepthDSV);
    if (FAILED(hr)) {
        LOG_ERROR("Failed to create depth DSV");
        return false;
    }

    return true;
}

// Render thread function — runs the render/weave loop independently of the
// Win32 message pump so that rendering continues even when the user drags or
// resizes the window (DefWindowProc enters a modal loop that blocks the main
// thread during those operations).
static void RenderThreadFunc(HWND hwnd, D3D11Renderer* renderer, TextOverlay* textOverlay) {
    LOG_INFO("[RenderThread] Started");

    PerformanceStats perfStats = {};
    perfStats.lastTime = std::chrono::high_resolution_clock::now();

    float leftEyePosX = 0.0f, leftEyePosY = 0.0f, leftEyePosZ = 0.0f;
    float rightEyePosX = 0.0f, rightEyePosY = 0.0f, rightEyePosZ = 0.0f;
    bool eyeTrackingActive = false;

    // Track current swapchain/view texture dimensions for resize detection
    UINT currentSwapW = g_windowWidth;
    UINT currentSwapH = g_windowHeight;

    while (g_running.load()) {
        // Skip rendering when minimized
        if (IsIconic(hwnd)) {
            Sleep(10);
            continue;
        }

        // Snapshot input state and window dimensions under lock
        InputState inputSnapshot;
        UINT snapWidth, snapHeight;
        bool snapResized;
        bool resetRequested = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            inputSnapshot = g_inputState;
            snapWidth = g_windowWidth;
            snapHeight = g_windowHeight;
            snapResized = g_windowResized;
            resetRequested = g_inputState.resetViewRequested;
            // Clear one-shot flags
            g_inputState.resetViewRequested = false;
            g_windowResized = false;
        }

        // Update performance stats
        UpdatePerformanceStats(perfStats);

        // Update input-based camera movement (operates on local snapshot)
        UpdateCameraMovement(inputSnapshot, perfStats.deltaTime);

        // Write back camera position under lock
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_inputState.cameraPosX = inputSnapshot.cameraPosX;
            g_inputState.cameraPosY = inputSnapshot.cameraPosY;
            g_inputState.cameraPosZ = inputSnapshot.cameraPosZ;
            if (resetRequested) {
                g_inputState.yaw = inputSnapshot.yaw;
                g_inputState.pitch = inputSnapshot.pitch;
                g_inputState.stereo = inputSnapshot.stereo;
            }
        }

        // Update scene (cube rotation)
        UpdateScene(*renderer, perfStats.deltaTime);

        // --- Handle resize (swapchain + stereo textures) ---
        if (snapResized && (snapWidth != currentSwapW || snapHeight != currentSwapH) &&
            snapWidth > 0 && snapHeight > 0) {

            LOG_INFO("[RenderThread] Resizing: %ux%u -> %ux%u", currentSwapW, currentSwapH, snapWidth, snapHeight);

            // Resize swapchain
            g_swapChainRTV.Reset();
            HRESULT hr = g_swapChain->ResizeBuffers(0, snapWidth, snapHeight, DXGI_FORMAT_UNKNOWN, 0);
            if (SUCCEEDED(hr)) {
                ComPtr<ID3D11Texture2D> backBuffer;
                hr = g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
                if (SUCCEEDED(hr)) {
                    hr = renderer->device->CreateRenderTargetView(backBuffer.Get(), nullptr, &g_swapChainRTV);
                }
            }
            if (FAILED(hr)) {
                LOG_ERROR("[RenderThread] Swapchain resize failed, hr=0x%08X", hr);
            }

            // Resize stereo textures proportionally
            float ratio = fminf((float)snapWidth / (float)g_displayPixelWidth,
                                (float)snapHeight / (float)g_displayPixelHeight);
            if (ratio > 1.0f) ratio = 1.0f;

            int newViewW = (int)((float)g_srRecommendedViewWidth * ratio);
            int newViewH = (int)((float)g_srRecommendedViewHeight * ratio);
            if (newViewW < 64) newViewW = 64;
            if (newViewH < 64) newViewH = 64;

            if (newViewW != g_viewTextureWidth || newViewH != g_viewTextureHeight) {
                LOG_INFO("[RenderThread] Resizing stereo texture: %dx%d -> %dx%d",
                    g_viewTextureWidth, g_viewTextureHeight, newViewW, newViewH);

                if (CreateStereoViewTextures(renderer->device.Get(), newViewW, newViewH)) {
                    g_viewTextureWidth = newViewW;
                    g_viewTextureHeight = newViewH;
                    g_srWeaver->setInputViewTexture(g_viewTextureSRV.Get(),
                        g_viewTextureWidth, g_viewTextureHeight, DXGI_FORMAT_R8G8B8A8_UNORM);
                }
            }

            currentSwapW = snapWidth;
            currentSwapH = snapHeight;
        }

        // --- Viewport scale and eye offset for windowed mode ---
        float pixelSizeX = g_screenWidthMM / (float)g_displayPixelWidth;
        float pixelSizeY = g_screenHeightMM / (float)g_displayPixelHeight;

        float winWidthMM  = (float)snapWidth * pixelSizeX;
        float winHeightMM = (float)snapHeight * pixelSizeY;

        float minDisp = fminf(g_screenWidthMM, g_screenHeightMM);
        float minWin  = fminf(winWidthMM, winHeightMM);
        float vs = (minWin > 0.0f) ? (minDisp / minWin) : 1.0f;

        float effectiveWidthMM  = winWidthMM * vs;
        float effectiveHeightMM = winHeightMM * vs;

        // Compute eye offset for off-center windows
        POINT clientOrigin = {0, 0};
        ClientToScreen(hwnd, &clientOrigin);

        float winCenterPxX = (float)(clientOrigin.x - g_displayScreenLeft) + (float)snapWidth / 2.0f;
        float winCenterPxY = (float)(clientOrigin.y - g_displayScreenTop) + (float)snapHeight / 2.0f;
        float dispCenterPxX = (float)g_displayPixelWidth / 2.0f;
        float dispCenterPxY = (float)g_displayPixelHeight / 2.0f;

        float offsetX_mm = (winCenterPxX - dispCenterPxX) * pixelSizeX;
        float offsetY_mm = -((winCenterPxY - dispCenterPxY) * pixelSizeY);  // negate Y

        // Get predicted eye positions from weaver
        leia::vec3f leftEye, rightEye;
        {
            float leftPos[3], rightPos[3];
            g_srWeaver->getPredictedEyePositions(leftPos, rightPos);
            leftEye = leia::vec3f(leftPos[0], leftPos[1], leftPos[2]);
            rightEye = leia::vec3f(rightPos[0], rightPos[1], rightPos[2]);
            eyeTrackingActive = true;

            leftEyePosX = leftPos[0]; leftEyePosY = leftPos[1]; leftEyePosZ = leftPos[2];
            rightEyePosX = rightPos[0]; rightEyePosY = rightPos[1]; rightEyePosZ = rightPos[2];
        }

        // Apply parallax toggle
        if (!inputSnapshot.parallaxEnabled) {
            leia::vec3f midEye = (leftEye + rightEye) * 0.5f;
            leia::vec3f translation = g_defaultViewingPosition - midEye;
            leftEye = leftEye + translation;
            rightEye = rightEye + translation;
        }

        // Apply eye offset for off-center window
        leia::vec3f eyeOffset = leia::vec3f(offsetX_mm, offsetY_mm, 0.0f);
        leia::vec3f adjLeftEye  = leftEye  - eyeOffset;
        leia::vec3f adjRightEye = rightEye - eyeOffset;

        // Clear view texture
        float clearColor[] = { 0.05f, 0.05f, 0.15f, 1.0f };
        renderer->context->ClearRenderTargetView(g_viewTextureRTV.Get(), clearColor);
        renderer->context->ClearDepthStencilView(g_viewDepthDSV.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

        // Cube parameters
        const float cubeSize = 60.0f;
        const float znear = 0.1f;
        const float zfar = 10000.0f;

        // Render stereo views (left and right)
        for (int eye = 0; eye < 2; eye++) {
            D3D11_VIEWPORT vp = {};
            vp.TopLeftX = (float)(eye * g_viewTextureWidth);
            vp.TopLeftY = 0.0f;
            vp.Width = (float)g_viewTextureWidth;
            vp.Height = (float)g_viewTextureHeight;
            vp.MinDepth = 0.0f;
            vp.MaxDepth = 1.0f;
            renderer->context->RSSetViewports(1, &vp);

            leia::vec3f eyePos = (eye == 0) ? adjLeftEye : adjRightEye;

            leia::mat4f mvp = leia::CalculateMVP(
                eyePos,
                effectiveWidthMM, effectiveHeightMM,
                renderer->cubeRotation,
                cubeSize,
                znear, zfar
            );

            RenderCubeWithMVP(*renderer, g_viewTextureRTV.Get(), g_viewDepthDSV.Get(), mvp.m);
        }

        // Render text overlay to BOTH eyes
        {
            std::wstring perfText = FormatPerformanceInfo(perfStats.fps, perfStats.frameTimeMs,
                g_viewTextureWidth, g_viewTextureHeight);
            std::wstring eyeText = FormatEyeTrackingInfo(
                leftEyePosX, leftEyePosY, leftEyePosZ,
                rightEyePosX, rightEyePosY, rightEyePosZ,
                eyeTrackingActive);
            std::wstring parallaxText = inputSnapshot.parallaxEnabled ?
                L"Parallax: ON (tracking)" : L"Parallax: OFF (fixed)";
            std::wstring helpText = L"P: Parallax | ESC: Quit";

            for (int eye = 0; eye < 2; eye++) {
                float xOffset = (float)(eye * g_viewTextureWidth);

                RenderText(*textOverlay, renderer->device.Get(), g_viewTexture.Get(),
                    perfText, xOffset + 10, 10, 200, 60, true);

                RenderText(*textOverlay, renderer->device.Get(), g_viewTexture.Get(),
                    eyeText, xOffset + 10, 80, 200, 60, true);

                RenderText(*textOverlay, renderer->device.Get(), g_viewTexture.Get(),
                    parallaxText, xOffset + 10, 150, 200, 25, true);

                RenderText(*textOverlay, renderer->device.Get(), g_viewTexture.Get(),
                    helpText, xOffset + 10, g_viewTextureHeight - 30.0f, 300, 25, true);
            }
        }

        // Set swapchain as render target for weaver output
        D3D11_VIEWPORT swapViewport = {};
        swapViewport.TopLeftX = 0.0f;
        swapViewport.TopLeftY = 0.0f;
        swapViewport.Width = (float)currentSwapW;
        swapViewport.Height = (float)currentSwapH;
        swapViewport.MinDepth = 0.0f;
        swapViewport.MaxDepth = 1.0f;
        renderer->context->RSSetViewports(1, &swapViewport);
        renderer->context->OMSetRenderTargets(1, g_swapChainRTV.GetAddressOf(), nullptr);

        // Weave (outputs interlaced image to swapchain)
        g_srWeaver->weave();

        // Present to display
        g_swapChain->Present(1, 0);
    }

    LOG_INFO("[RenderThread] Exiting");
}

#endif // HAVE_LEIA_SR

// Main entry point
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    // Ensure DPI awareness
    SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);

    // Initialize logging
    if (!InitializeLogging(APP_NAME)) {
        MessageBox(nullptr, L"Failed to initialize logging", L"Warning", MB_OK | MB_ICONWARNING);
    }

    LOG_INFO("=== SR Cube Native Application ===");
    LOG_INFO("Direct SR SDK rendering without OpenXR");

#ifndef HAVE_LEIA_SR
    LOG_ERROR("LeiaSR SDK not available - this application requires the LeiaSR SDK");
    MessageBox(nullptr,
        L"This application requires the LeiaSR SDK.\n\n"
        L"Please ensure LeiaSR SDK is installed and LEIASR_SDKROOT is set.",
        L"LeiaSR SDK Required", MB_OK | MB_ICONERROR);
    ShutdownLogging();
    return 1;
#else

    // Create SR context first to get display location
    LOG_INFO("Creating SR context to find display...");
    if (!CreateSRContext(10.0)) {
        LOG_ERROR("Failed to create SR context");
        MessageBox(nullptr,
            L"Failed to connect to SR Service.\n\n"
            L"Please ensure SR Service is running.",
            L"SR Service Required", MB_OK | MB_ICONERROR);
        ShutdownLogging();
        return 1;
    }

    // Get display location for window positioning
    SR_recti displayLocation = g_display->getLocation();
    g_displayScreenLeft = (int)displayLocation.left;
    g_displayScreenTop = (int)displayLocation.top;
    g_displayPixelWidth = (int)(displayLocation.right - displayLocation.left);
    g_displayPixelHeight = (int)(displayLocation.bottom - displayLocation.top);
    LOG_INFO("SR display at (%d,%d) size %dx%d", g_displayScreenLeft, g_displayScreenTop,
        g_displayPixelWidth, g_displayPixelHeight);

    // Use display size for window
    g_windowWidth = g_displayPixelWidth;
    g_windowHeight = g_displayPixelHeight;

    // Register window class
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
            CleanupLeiaSR();
            ShutdownLogging();
            return 1;
        }
    }

    // Create window on SR display
    LOG_INFO("Creating window on SR display...");
    HWND hwnd = CreateWindowEx(
        WS_EX_APPWINDOW,
        WINDOW_CLASS,
        WINDOW_TITLE,
        WS_POPUP,  // Borderless for fullscreen
        g_displayScreenLeft, g_displayScreenTop,
        g_displayPixelWidth, g_displayPixelHeight,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!hwnd) {
        LOG_ERROR("Failed to create window, error: %lu", GetLastError());
        CleanupLeiaSR();
        ShutdownLogging();
        return 1;
    }
    LOG_INFO("Window created: 0x%p", hwnd);
    g_windowInfo.hwnd = hwnd;

    // Initialize D3D11
    LOG_INFO("Initializing D3D11...");
    D3D11Renderer renderer = {};
    if (!InitializeD3D11(renderer)) {
        LOG_ERROR("D3D11 initialization failed");
        MessageBox(hwnd, L"Failed to initialize D3D11", L"Error", MB_OK | MB_ICONERROR);
        DestroyWindow(hwnd);
        CleanupLeiaSR();
        ShutdownLogging();
        return 1;
    }
    LOG_INFO("D3D11 initialized");

    // Create swapchain for presentation (weaver outputs to this)
    LOG_INFO("Creating swapchain...");
    {
        ComPtr<IDXGIDevice> dxgiDevice;
        renderer.device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));

        ComPtr<IDXGIAdapter> dxgiAdapter;
        dxgiDevice->GetAdapter(&dxgiAdapter);

        ComPtr<IDXGIFactory2> dxgiFactory;
        dxgiAdapter->GetParent(IID_PPV_ARGS(&dxgiFactory));

        DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
        swapChainDesc.Width = g_windowWidth;
        swapChainDesc.Height = g_windowHeight;
        swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapChainDesc.SampleDesc.Count = 1;
        swapChainDesc.SampleDesc.Quality = 0;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.BufferCount = 1;
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        swapChainDesc.Flags = 0;

        DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsDesc = {};
        fsDesc.RefreshRate = {0, 0};
        fsDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
        fsDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
        fsDesc.Windowed = TRUE;

        HRESULT hr = dxgiFactory->CreateSwapChainForHwnd(
            renderer.device.Get(),
            hwnd,
            &swapChainDesc,
            &fsDesc,
            nullptr,
            &g_swapChain
        );
        if (FAILED(hr)) {
            LOG_ERROR("Failed to create swapchain, hr=0x%08X", hr);
            MessageBox(hwnd, L"Failed to create swapchain", L"Error", MB_OK | MB_ICONERROR);
            CleanupD3D11(renderer);
            DestroyWindow(hwnd);
            CleanupLeiaSR();
            ShutdownLogging();
            return 1;
        }

        // Create render target view for swapchain backbuffer
        ComPtr<ID3D11Texture2D> backBuffer;
        hr = g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
        if (FAILED(hr)) {
            LOG_ERROR("Failed to get swapchain backbuffer");
            CleanupD3D11(renderer);
            DestroyWindow(hwnd);
            CleanupLeiaSR();
            ShutdownLogging();
            return 1;
        }

        hr = renderer.device->CreateRenderTargetView(backBuffer.Get(), nullptr, &g_swapChainRTV);
        if (FAILED(hr)) {
            LOG_ERROR("Failed to create swapchain RTV");
            CleanupD3D11(renderer);
            DestroyWindow(hwnd);
            CleanupLeiaSR();
            ShutdownLogging();
            return 1;
        }

        // Disable Alt+Enter fullscreen toggle
        dxgiFactory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    }
    LOG_INFO("Swapchain created");

    // Initialize text overlay
    LOG_INFO("Initializing text overlay...");
    TextOverlay textOverlay = {};
    if (!InitializeTextOverlay(textOverlay)) {
        LOG_ERROR("Text overlay initialization failed");
        CleanupD3D11(renderer);
        DestroyWindow(hwnd);
        CleanupLeiaSR();
        ShutdownLogging();
        return 1;
    }

    // Initialize weaver (context already created)
    LOG_INFO("Creating D3D11 weaver...");
    WeaverErrorCode createWeaverResult = SR::CreateDX11Weaver(g_srContext, renderer.context.Get(), hwnd, &g_srWeaver);
    if (createWeaverResult != WeaverErrorCode::WeaverSuccess) {
        LOG_ERROR("Failed to create D3D11 weaver, error: %d", (int)createWeaverResult);
        MessageBox(hwnd, L"Failed to create SR weaver", L"Error", MB_OK | MB_ICONERROR);
        CleanupTextOverlay(textOverlay);
        CleanupD3D11(renderer);
        DestroyWindow(hwnd);
        CleanupLeiaSR();
        ShutdownLogging();
        return 1;
    }
    LOG_INFO("D3D11 weaver created");

    // Get recommended view texture size
    g_srRecommendedViewWidth = g_display->getRecommendedViewsTextureWidth();
    g_srRecommendedViewHeight = g_display->getRecommendedViewsTextureHeight();
    g_viewTextureWidth = g_srRecommendedViewWidth;
    g_viewTextureHeight = g_srRecommendedViewHeight;
    LOG_INFO("View texture size: %dx%d", g_viewTextureWidth, g_viewTextureHeight);

    // Get physical screen dimensions (cm to mm)
    g_screenWidthMM = g_display->getPhysicalSizeWidth() * 10.0f;
    g_screenHeightMM = g_display->getPhysicalSizeHeight() * 10.0f;
    LOG_INFO("Screen physical size: %.1fmm x %.1fmm", g_screenWidthMM, g_screenHeightMM);

    // Get default viewing position
    float x_mm, y_mm, z_mm;
    g_display->getDefaultViewingPosition(x_mm, y_mm, z_mm);
    g_defaultViewingPosition = leia::vec3f(x_mm, y_mm, z_mm);
    LOG_INFO("Default viewing position: (%.1f, %.1f, %.1f) mm", x_mm, y_mm, z_mm);

    // Create stereo view texture (side-by-side)
    LOG_INFO("Creating stereo view texture (%dx%d)...", g_viewTextureWidth * 2, g_viewTextureHeight);
    if (!CreateStereoViewTextures(renderer.device.Get(), g_viewTextureWidth, g_viewTextureHeight)) {
        LOG_ERROR("Failed to create stereo view textures");
        goto cleanup;
    }
    LOG_INFO("Stereo view texture created");

    // Initialize context (must be called BEFORE setInputViewTexture,
    // otherwise initialize() resets the weaver's input texture binding)
    g_srContext->initialize();

    // Set input texture for weaver (after initialize)
    g_srWeaver->setInputViewTexture(g_viewTextureSRV.Get(), g_viewTextureWidth, g_viewTextureHeight, DXGI_FORMAT_R8G8B8A8_UNORM);

    // Show window
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    LOG_INFO("");
    LOG_INFO("=== Entering main loop ===");
    LOG_INFO("Render thread handles SR render/weave loop; main thread handles Win32 messages");
    LOG_INFO("Controls: WASD=Move, Mouse=Look, P=Toggle Parallax, ESC=Quit");
    LOG_INFO("");

    // Launch render thread — the render/weave loop runs independently of the
    // message pump so that rendering continues even when DefWindowProc blocks
    // during window drag/resize.
    {
        std::thread renderThread(RenderThreadFunc, hwnd, &renderer, &textOverlay);

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
    }

cleanup:
    LOG_INFO("");
    LOG_INFO("=== Shutting down ===");

    // Clean up swapchain
    g_swapChainRTV.Reset();
    g_swapChain.Reset();

    g_viewDepthDSV.Reset();
    g_viewDepthTexture.Reset();
    g_viewTextureRTV.Reset();
    g_viewTextureSRV.Reset();
    g_viewTexture.Reset();

    if (g_srWeaver) {
        g_srWeaver->destroy();
        g_srWeaver = nullptr;
    }

    CleanupTextOverlay(textOverlay);
    CleanupD3D11(renderer);

    DestroyWindow(hwnd);
    UnregisterClass(WINDOW_CLASS, hInstance);

    if (g_srContext) {
        SR::SRContext::deleteSRContext(g_srContext);
        g_srContext = nullptr;
    }

    LOG_INFO("Application shutdown complete");
    ShutdownLogging();

    return 0;
#endif // HAVE_LEIA_SR
}
