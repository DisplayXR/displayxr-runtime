// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  SR Cube OpenXR Shared Texture — OpenGL shared texture demo
 *
 * Demonstrates zero-copy shared GPU texture via XR_EXT_win32_window_binding:
 * the app creates a shared D3D11 texture (MISC_SHARED), passes its HANDLE
 * to the runtime (windowHandle=NULL, sharedTextureHandle=handle). The runtime's
 * GL native compositor uses WGL_NV_DX_interop2 to map the D3D11 texture as a
 * GL texture and renders the composited output into it. The app then blits
 * the shared texture into its own window each frame using D3D11.
 *
 * Key difference from cube_ext_gl: no window handle is passed to the runtime.
 * The shared texture acts as the render target, and the app composites
 * the result into its own rendering pipeline.
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <wrl/client.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>

#include "logging.h"
#include "input_handler.h"
#include "gl_renderer.h"
#include "hud_renderer.h"
#include "text_overlay.h"
#include "xr_session.h"
#include "display3d_view.h"
#include "camera3d_view.h"

#include <chrono>
#include <cmath>
#include <string>
#include <sstream>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

static const char* APP_NAME = "cube_shared_gl_win";
static const wchar_t* WINDOW_CLASS = L"SRCubeSharedGLClass";
static const wchar_t* WINDOW_TITLE = L"OpenGL Cube \u2014 GL Native Compositor (Shared Texture)";

// Global state
static InputState g_inputState;
static std::mutex g_inputMutex;
static std::atomic<bool> g_running{true};
static XrSessionManager* g_xr = nullptr;
static UINT g_windowWidth = 1280;
static UINT g_windowHeight = 720;
static bool g_inSizeMove = false;
static const uint32_t HUD_PIXEL_WIDTH = 380;
static const uint32_t HUD_PIXEL_HEIGHT = 470;
static const float CAMERA_HALF_TAN_VFOV = 0.32491969623f;
static const float HUD_WIDTH_FRACTION = 0.30f;

static bool g_fullscreen = false;
static RECT g_savedWindowRect = {};
static DWORD g_savedWindowStyle = 0;

// Shared texture resources (D3D11)
static ComPtr<ID3D11Device> g_d3dDevice;
static ComPtr<ID3D11DeviceContext> g_d3dContext;
static ComPtr<IDXGIFactory2> g_dxgiFactory;
static ComPtr<ID3D11Texture2D> g_sharedTexture;
static ComPtr<ID3D11ShaderResourceView> g_sharedSRV;
static HANDLE g_sharedHandle = nullptr;
static uint32_t g_sharedWidth = 0;
static uint32_t g_sharedHeight = 0;

// Blit shader resources (D3D11)
static ComPtr<ID3D11VertexShader> g_blitVS;
static ComPtr<ID3D11PixelShader> g_blitPS;
static ComPtr<ID3D11SamplerState> g_blitSampler;

static void ToggleFullscreen(HWND hwnd) {
    if (g_fullscreen) {
        SetWindowLong(hwnd, GWL_STYLE, g_savedWindowStyle);
        SetWindowPos(hwnd, HWND_TOP,
            g_savedWindowRect.left, g_savedWindowRect.top,
            g_savedWindowRect.right - g_savedWindowRect.left,
            g_savedWindowRect.bottom - g_savedWindowRect.top,
            SWP_FRAMECHANGED);
        g_fullscreen = false;
    } else {
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
    }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    {
        std::lock_guard<std::mutex> lock(g_inputMutex);
        UpdateInputState(g_inputState, msg, wParam, lParam);
    }

    switch (msg) {
    case WM_LBUTTONDOWN: SetCapture(hwnd); return 0;
    case WM_LBUTTONUP: ReleaseCapture(); return 0;
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_windowWidth = LOWORD(lParam);
            g_windowHeight = HIWORD(lParam);
        }
        return 0;
    case WM_CLOSE:
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
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS | CS_OWNDC;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = WINDOW_CLASS;

    if (!RegisterClassEx(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            LOG_ERROR("Failed to register window class: %lu", err);
            return nullptr;
        }
    }

    RECT rect = { 0, 0, width, height };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindowEx(0, WINDOW_CLASS, WINDOW_TITLE, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, hInstance, nullptr);

    return hwnd;
}

// Create OpenGL context: temp legacy context -> core profile 3.3
static bool CreateOpenGLContext(HWND hwnd, HDC& hDC, HGLRC& hGLRC) {
    hDC = GetDC(hwnd);
    if (!hDC) {
        LOG_ERROR("GetDC failed");
        return false;
    }

    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;

    int pixelFormat = ChoosePixelFormat(hDC, &pfd);
    if (!pixelFormat || !SetPixelFormat(hDC, pixelFormat, &pfd)) {
        LOG_ERROR("Failed to set pixel format");
        return false;
    }

    HGLRC tempRC = wglCreateContext(hDC);
    if (!tempRC || !wglMakeCurrent(hDC, tempRC)) {
        LOG_ERROR("Failed to create temp GL context");
        if (tempRC) wglDeleteContext(tempRC);
        return false;
    }

    PFNWGLCREATECONTEXTATTRIBSARBPROC wglCreateContextAttribsARB =
        (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress("wglCreateContextAttribsARB");

    if (!wglCreateContextAttribsARB) {
        LOG_ERROR("wglCreateContextAttribsARB not available");
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(tempRC);
        return false;
    }

    int attribs[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
        WGL_CONTEXT_MINOR_VERSION_ARB, 3,
        WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
        0
    };

    wglMakeCurrent(nullptr, nullptr);
    hGLRC = wglCreateContextAttribsARB(hDC, nullptr, attribs);
    wglDeleteContext(tempRC);

    if (!hGLRC || !wglMakeCurrent(hDC, hGLRC)) {
        LOG_ERROR("Failed to create core profile GL context");
        if (hGLRC) { wglDeleteContext(hGLRC); hGLRC = nullptr; }
        return false;
    }

    LOG_INFO("OpenGL context: %s / %s / %s",
        (const char*)glGetString(GL_VENDOR),
        (const char*)glGetString(GL_RENDERER),
        (const char*)glGetString(GL_VERSION));

    return true;
}

// D3D11 blit shaders (for presenting shared texture to window)
static const char* g_blitVSSource = R"(
struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};
VSOut main(uint id : SV_VertexID) {
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
)";

static const char* g_blitPSSource = R"(
Texture2D    tex : register(t0);
SamplerState smp : register(s0);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return tex.Sample(smp, uv);
}
)";

static bool CreateBlitResources(ID3D11Device* device) {
    ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
    HRESULT hr = D3DCompile(g_blitVSSource, strlen(g_blitVSSource), "blitVS", nullptr, nullptr,
        "main", "vs_5_0", 0, 0, &vsBlob, &errBlob);
    if (FAILED(hr)) {
        LOG_ERROR("Blit VS compile failed: %s", errBlob ? (char*)errBlob->GetBufferPointer() : "unknown");
        return false;
    }
    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_blitVS);
    if (FAILED(hr)) return false;

    hr = D3DCompile(g_blitPSSource, strlen(g_blitPSSource), "blitPS", nullptr, nullptr,
        "main", "ps_5_0", 0, 0, &psBlob, &errBlob);
    if (FAILED(hr)) {
        LOG_ERROR("Blit PS compile failed: %s", errBlob ? (char*)errBlob->GetBufferPointer() : "unknown");
        return false;
    }
    hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_blitPS);
    if (FAILED(hr)) return false;

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    hr = device->CreateSamplerState(&sd, &g_blitSampler);
    return SUCCEEDED(hr);
}

// Blit shared texture to back buffer with aspect-ratio letterboxing
static void BlitSharedTextureToBackBuffer(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* backBufferRTV,
                                           uint32_t winW, uint32_t winH) {
    if (!g_sharedSRV) return;

    ctx->OMSetRenderTargets(1, &backBufferRTV, nullptr);

    float srcAR = (float)g_sharedWidth / (float)g_sharedHeight;
    float dstAR = (float)winW / (float)winH;

    D3D11_VIEWPORT vp = {};
    if (srcAR > dstAR) {
        vp.Width = (FLOAT)winW;
        vp.Height = (FLOAT)winW / srcAR;
        vp.TopLeftX = 0;
        vp.TopLeftY = ((FLOAT)winH - vp.Height) * 0.5f;
    } else {
        vp.Height = (FLOAT)winH;
        vp.Width = (FLOAT)winH * srcAR;
        vp.TopLeftX = ((FLOAT)winW - vp.Width) * 0.5f;
        vp.TopLeftY = 0;
    }
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);

    ctx->VSSetShader(g_blitVS.Get(), nullptr, 0);
    ctx->PSSetShader(g_blitPS.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srv = g_sharedSRV.Get();
    ctx->PSSetShaderResources(0, 1, &srv);
    ID3D11SamplerState* smp = g_blitSampler.Get();
    ctx->PSSetSamplers(0, 1, &smp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->Draw(3, 0);

    ID3D11ShaderResourceView* nullSRV = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSRV);
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
    HDC hDC,
    HGLRC hGLRC,
    XrSessionManager* xr,
    GLRenderer* renderer,
    std::vector<XrSwapchainImageOpenGLKHR>* swapchainImages,
    HudRenderer* hud,
    uint32_t hudWidth,
    uint32_t hudHeight,
    std::vector<XrSwapchainImageOpenGLKHR>* hudSwapchainImages,
    ComPtr<IDXGISwapChain1> appSwapchain,
    ComPtr<ID3D11RenderTargetView> appBackBufferRTV)
{
    LOG_INFO("[RenderThread] Started");

    if (!wglMakeCurrent(hDC, hGLRC)) {
        LOG_ERROR("[RenderThread] wglMakeCurrent failed");
        return;
    }

    PerformanceStats perfStats = {};
    perfStats.lastTime = std::chrono::high_resolution_clock::now();

    while (g_running.load() && !xr->exitRequested) {
        InputState inputSnapshot;
        bool renderingModeChanged = false;
        uint32_t windowW, windowH;
        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            inputSnapshot = g_inputState;
            g_inputState.resetViewRequested = false;
            g_inputState.fullscreenToggleRequested = false;
            g_inputState.eyeTrackingModeToggleRequested = false;
            renderingModeChanged = g_inputState.renderingModeChangeRequested;
            g_inputState.renderingModeChangeRequested = false;
            windowW = g_windowWidth;
            windowH = g_windowHeight;
        }

        // Handle rendering mode change (V=cycle, 0-8=direct)
        if (renderingModeChanged && xr->pfnRequestDisplayRenderingModeEXT && xr->session != XR_NULL_HANDLE) {
            xr->pfnRequestDisplayRenderingModeEXT(xr->session, inputSnapshot.currentRenderingMode);
        }

        UpdatePerformanceStats(perfStats);
        UpdateCameraMovement(inputSnapshot, perfStats.deltaTime, xr->displayHeightM);

        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_inputState.cameraPosX = inputSnapshot.cameraPosX;
            g_inputState.cameraPosY = inputSnapshot.cameraPosY;
            g_inputState.cameraPosZ = inputSnapshot.cameraPosZ;
        }

        if (inputSnapshot.fullscreenToggleRequested) {
            // Post to main thread
        }
        if (inputSnapshot.eyeTrackingModeToggleRequested) {
            if (xr->pfnRequestEyeTrackingModeEXT && xr->session != XR_NULL_HANDLE) {
                XrEyeTrackingModeEXT newMode = (xr->activeEyeTrackingMode == XR_EYE_TRACKING_MODE_SMOOTH_EXT)
                    ? XR_EYE_TRACKING_MODE_RAW_EXT : XR_EYE_TRACKING_MODE_SMOOTH_EXT;
                xr->pfnRequestEyeTrackingModeEXT(xr->session, newMode);
            }
        }

        UpdateScene(*renderer, perfStats.deltaTime);
        PollEvents(*xr);

        if (xr->sessionRunning) {
            XrFrameState frameState;
            if (BeginFrame(*xr, frameState)) {
                uint32_t modeViewCount = (xr->renderingModeCount > 0 && inputSnapshot.currentRenderingMode < xr->renderingModeCount)
                    ? xr->renderingModeViewCounts[inputSnapshot.currentRenderingMode] : 2;
                uint32_t tileColumns = (xr->renderingModeCount > 0 && inputSnapshot.currentRenderingMode < xr->renderingModeCount)
                    ? xr->renderingModeTileColumns[inputSnapshot.currentRenderingMode] : 2;
                uint32_t tileRows = (xr->renderingModeCount > 0 && inputSnapshot.currentRenderingMode < xr->renderingModeCount)
                    ? xr->renderingModeTileRows[inputSnapshot.currentRenderingMode] : 1;
                bool monoMode = (xr->renderingModeCount > 0 && !xr->renderingModeDisplay3D[inputSnapshot.currentRenderingMode]);
                int eyeCount = monoMode ? 1 : (int)modeViewCount;
                std::vector<XrCompositionLayerProjectionView> projectionViews(eyeCount, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
                bool rendered = false;
                bool hudSubmitted = false;

                if (frameState.shouldRender) {
                    if (LocateViews(*xr, frameState.predictedDisplayTime,
                        inputSnapshot.cameraPosX, inputSnapshot.cameraPosY, inputSnapshot.cameraPosZ,
                        inputSnapshot.yaw, inputSnapshot.pitch,
                        inputSnapshot.viewParams)) {

                        XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
                        locateInfo.viewConfigurationType = xr->viewConfigType;
                        locateInfo.displayTime = frameState.predictedDisplayTime;
                        locateInfo.space = xr->localSpace;

                        XrViewState viewState = {XR_TYPE_VIEW_STATE};
                        uint32_t viewCount = 0;
                        XrView rawViews[8] = {};
                        for (int i = 0; i < 8; i++) rawViews[i].type = XR_TYPE_VIEW;
                        xrLocateViews(xr->session, &locateInfo, &viewState, 8, &viewCount, rawViews);

                        uint32_t maxTileW = tileColumns > 0 ? xr->swapchain.width / tileColumns : xr->swapchain.width;
                        uint32_t maxTileH = tileRows > 0 ? xr->swapchain.height / tileRows : xr->swapchain.height;

                        uint32_t renderW, renderH;
                        if (monoMode) {
                            renderW = g_sharedWidth;
                            renderH = g_sharedHeight;
                            if (renderW > xr->swapchain.width) renderW = xr->swapchain.width;
                            if (renderH > xr->swapchain.height) renderH = xr->swapchain.height;
                        } else {
                            renderW = (uint32_t)(g_sharedWidth * xr->recommendedViewScaleX);
                            renderH = (uint32_t)(g_sharedHeight * xr->recommendedViewScaleY);
                            if (renderW > maxTileW) renderW = maxTileW;
                            if (renderH > maxTileH) renderH = maxTileH;
                        }

                        // App-side Kooima projection
                        std::vector<Display3DView> stereoViews(eyeCount);
                        bool useAppProjection = (xr->hasDisplayInfoExt && xr->displayWidthM > 0.0f);
                        if (useAppProjection) {
                            float pxSizeX = xr->displayWidthM / (float)xr->swapchain.width;
                            float pxSizeY = xr->displayHeightM / (float)xr->swapchain.height;
                            float winW_m = (float)g_sharedWidth * pxSizeX;
                            float winH_m = (float)g_sharedHeight * pxSizeY;
                            float minDisp = fminf(xr->displayWidthM, xr->displayHeightM);
                            float minWin  = fminf(winW_m, winH_m);
                            float vs = minDisp / minWin;

                            std::vector<XrVector3f> rawEyes(modeViewCount);
                            for (uint32_t v = 0; v < modeViewCount; v++) {
                                rawEyes[v] = (v < viewCount) ? rawViews[v].pose.position : rawViews[0].pose.position;
                            }
                            if (monoMode && modeViewCount >= 2) {
                                XrVector3f center = {0, 0, 0};
                                for (uint32_t v = 0; v < modeViewCount; v++) {
                                    center.x += rawEyes[v].x;
                                    center.y += rawEyes[v].y;
                                    center.z += rawEyes[v].z;
                                }
                                center.x /= modeViewCount;
                                center.y /= modeViewCount;
                                center.z /= modeViewCount;
                                rawEyes[0] = center;
                            }

                            XrPosef cameraPose;
                            XMVECTOR pOri = XMQuaternionRotationRollPitchYaw(
                                inputSnapshot.pitch, inputSnapshot.yaw, 0);
                            XMFLOAT4 q;
                            XMStoreFloat4(&q, pOri);
                            cameraPose.orientation = {q.x, q.y, q.z, q.w};
                            cameraPose.position = {inputSnapshot.cameraPosX, inputSnapshot.cameraPosY, inputSnapshot.cameraPosZ};

                            XrVector3f nominalViewer = {xr->nominalViewerX, xr->nominalViewerY, xr->nominalViewerZ};
                            Display3DScreen screen = {winW_m * vs, winH_m * vs};

                            if (inputSnapshot.cameraMode) {
                                Camera3DTunables camTunables;
                                camTunables.ipd_factor = inputSnapshot.viewParams.ipdFactor;
                                camTunables.parallax_factor = inputSnapshot.viewParams.parallaxFactor;
                                camTunables.inv_convergence_distance = inputSnapshot.viewParams.invConvergenceDistance;
                                camTunables.half_tan_vfov = CAMERA_HALF_TAN_VFOV / inputSnapshot.viewParams.zoomFactor;

                                std::vector<Camera3DView> camViews(eyeCount);
                                camera3d_compute_views(
                                    rawEyes.data(), eyeCount, &nominalViewer,
                                    &screen, &camTunables, &cameraPose,
                                    0.01f, 100.0f, camViews.data());

                                for (int i = 0; i < eyeCount; i++) {
                                    memcpy(stereoViews[i].view_matrix, camViews[i].view_matrix, sizeof(float) * 16);
                                    memcpy(stereoViews[i].projection_matrix, camViews[i].projection_matrix, sizeof(float) * 16);
                                    stereoViews[i].fov = camViews[i].fov;
                                    stereoViews[i].eye_world = camViews[i].eye_world;
                                }
                            } else {
                                Display3DTunables tunables;
                                tunables.ipd_factor = inputSnapshot.viewParams.ipdFactor;
                                tunables.parallax_factor = inputSnapshot.viewParams.parallaxFactor;
                                tunables.perspective_factor = inputSnapshot.viewParams.perspectiveFactor;
                                tunables.virtual_display_height = inputSnapshot.viewParams.virtualDisplayHeight / inputSnapshot.viewParams.scaleFactor;

                                display3d_compute_views(
                                    rawEyes.data(), eyeCount, &nominalViewer,
                                    &screen, &tunables, &cameraPose,
                                    0.01f, 100.0f, stereoViews.data());
                            }
                        }

                        rendered = true;

                        XMMATRIX monoViewMatrix, monoProjMatrix;
                        XrFovf monoFov = {};
                        XrPosef monoPose = rawViews[0].pose;
                        if (monoMode) {
                            XrVector3f center = {0, 0, 0};
                            for (uint32_t v = 0; v < modeViewCount && v < viewCount; v++) {
                                center.x += rawViews[v].pose.position.x;
                                center.y += rawViews[v].pose.position.y;
                                center.z += rawViews[v].pose.position.z;
                            }
                            uint32_t cnt = (modeViewCount < viewCount) ? modeViewCount : viewCount;
                            if (cnt > 0) { center.x /= cnt; center.y /= cnt; center.z /= cnt; }
                            monoPose.position = center;
                            if (!useAppProjection) {
                                monoProjMatrix = xr->projMatrices[0];
                                monoFov = rawViews[0].fov;
                                XMVECTOR centerLocalPos = XMVectorSet(
                                    monoPose.position.x, monoPose.position.y, monoPose.position.z, 0.0f);
                                XMVECTOR localOri = XMVectorSet(
                                    rawViews[0].pose.orientation.x, rawViews[0].pose.orientation.y,
                                    rawViews[0].pose.orientation.z, rawViews[0].pose.orientation.w);
                                float monoM2vView = 1.0f;
                                if (inputSnapshot.viewParams.virtualDisplayHeight > 0.0f && xr->displayHeightM > 0.0f)
                                    monoM2vView = inputSnapshot.viewParams.virtualDisplayHeight / xr->displayHeightM;
                                float eyeScale = inputSnapshot.viewParams.perspectiveFactor * monoM2vView / inputSnapshot.viewParams.scaleFactor;
                                XMVECTOR playerOri = XMQuaternionRotationRollPitchYaw(
                                    inputSnapshot.pitch, inputSnapshot.yaw, 0);
                                XMVECTOR playerPos = XMVectorSet(
                                    inputSnapshot.cameraPosX, inputSnapshot.cameraPosY,
                                    inputSnapshot.cameraPosZ, 0.0f);
                                XMVECTOR worldPos = XMVector3Rotate(centerLocalPos * eyeScale, playerOri) + playerPos;
                                XMVECTOR worldOri = XMQuaternionMultiply(localOri, playerOri);
                                XMMATRIX rot = XMMatrixTranspose(XMMatrixRotationQuaternion(worldOri));
                                XMFLOAT3 wp;
                                XMStoreFloat3(&wp, worldPos);
                                monoViewMatrix = XMMatrixTranslation(-wp.x, -wp.y, -wp.z) * rot;
                            }
                        }

                        uint32_t imageIndex;
                        if (AcquireSwapchainImage(*xr, imageIndex)) {
                            for (int eye = 0; eye < eyeCount; eye++) {
                                int vi = eye < (int)viewCount ? eye : 0;
                                XMMATRIX viewMatrix, projMatrix;
                                if (useAppProjection) {
                                    viewMatrix = ColumnMajorToXMMatrix(stereoViews[eye].view_matrix);
                                    projMatrix = ColumnMajorToXMMatrix(stereoViews[eye].projection_matrix);
                                } else if (monoMode) {
                                    viewMatrix = monoViewMatrix;
                                    projMatrix = monoProjMatrix;
                                } else {
                                    viewMatrix = xr->viewMatrices[vi];
                                    projMatrix = xr->projMatrices[vi];
                                }

                                uint32_t tileX = monoMode ? 0 : (eye % tileColumns);
                                uint32_t tileY = monoMode ? 0 : (eye / tileColumns);
                                uint32_t vpX = tileX * renderW;
                                uint32_t vpY = tileY * renderH;

                                RenderScene(*renderer, imageIndex,
                                    vpX, vpY,
                                    renderW, renderH,
                                    viewMatrix, projMatrix,
                                    useAppProjection ? 1.0f : inputSnapshot.viewParams.scaleFactor);

                                projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                                projectionViews[eye].subImage.swapchain = xr->swapchain.swapchain;
                                projectionViews[eye].subImage.imageRect.offset = {
                                    (int32_t)vpX, (int32_t)vpY
                                };
                                projectionViews[eye].subImage.imageRect.extent = {
                                    (int32_t)renderW, (int32_t)renderH
                                };
                                projectionViews[eye].subImage.imageArrayIndex = 0;
                                projectionViews[eye].pose = monoMode ? monoPose : rawViews[vi].pose;
                                projectionViews[eye].fov = useAppProjection ?
                                    stereoViews[eye].fov :
                                    (monoMode ? monoFov : rawViews[vi].fov);
                            }
                            ReleaseSwapchainImage(*xr);
                        } else {
                            rendered = false;
                        }

                        // Render HUD
                        if (rendered && inputSnapshot.hudVisible && hud && xr->hasHudSwapchain && hudSwapchainImages) {
                            uint32_t hudImageIndex;
                            if (AcquireHudSwapchainImage(*xr, hudImageIndex)) {
                                std::wstring sessionText(xr->systemName, xr->systemName + strlen(xr->systemName));
                                sessionText += L"\nSession: ";
                                sessionText += FormatSessionState((int)xr->sessionState);
                                std::wstring modeText = L"Shared Texture OpenGL (offscreen)";
                                modeText += inputSnapshot.cameraMode ?
                                    L"\nKooima: Camera-Centric [C=Toggle]" :
                                    L"\nKooima: Display-Centric [C=Toggle]";

                                uint32_t dispRenderW, dispRenderH;
                                if (monoMode) {
                                    dispRenderW = g_sharedWidth;
                                    dispRenderH = g_sharedHeight;
                                    if (dispRenderW > xr->swapchain.width) dispRenderW = xr->swapchain.width;
                                    if (dispRenderH > xr->swapchain.height) dispRenderH = xr->swapchain.height;
                                } else {
                                    dispRenderW = (uint32_t)(g_sharedWidth * xr->recommendedViewScaleX);
                                    dispRenderH = (uint32_t)(g_sharedHeight * xr->recommendedViewScaleY);
                                    if (dispRenderW > maxTileW) dispRenderW = maxTileW;
                                    if (dispRenderH > maxTileH) dispRenderH = maxTileH;
                                }
                                std::wstring perfText = FormatPerformanceInfo(perfStats.fps, perfStats.frameTimeMs,
                                    dispRenderW, dispRenderH, windowW, windowH);
                                std::wstring dispText = FormatDisplayInfo(xr->displayWidthM, xr->displayHeightM,
                                    xr->nominalViewerX, xr->nominalViewerY, xr->nominalViewerZ);
                                dispText += L"\n" + FormatScaleInfo(xr->recommendedViewScaleX, xr->recommendedViewScaleY);
                                dispText += L"\n" + FormatMode(inputSnapshot.currentRenderingMode, xr->pfnRequestDisplayRenderingModeEXT != nullptr,
                                    (xr->renderingModeCount > 0 && inputSnapshot.currentRenderingMode < xr->renderingModeCount) ? xr->renderingModeNames[inputSnapshot.currentRenderingMode] : nullptr,
                                    xr->renderingModeCount,
                                    xr->renderingModeCount > 0 ? xr->renderingModeDisplay3D[inputSnapshot.currentRenderingMode] : true);
                                std::wstring eyeText = FormatEyeTrackingInfo(
                                    xr->eyePositions, xr->viewCount,
                                    xr->eyeTrackingActive, xr->isEyeTracking,
                                    xr->activeEyeTrackingMode, xr->supportedEyeTrackingModes);

                                float fwdX = -sinf(inputSnapshot.yaw) * cosf(inputSnapshot.pitch);
                                float fwdY =  sinf(inputSnapshot.pitch);
                                float fwdZ = -cosf(inputSnapshot.yaw) * cosf(inputSnapshot.pitch);
                                std::wstring cameraText = FormatCameraInfo(
                                    inputSnapshot.cameraPosX, inputSnapshot.cameraPosY, inputSnapshot.cameraPosZ,
                                    fwdX, fwdY, fwdZ, inputSnapshot.cameraMode);
                                float dispP1 = inputSnapshot.cameraMode ? inputSnapshot.viewParams.invConvergenceDistance : inputSnapshot.viewParams.perspectiveFactor;
                                float dispP2 = inputSnapshot.cameraMode ? inputSnapshot.viewParams.zoomFactor : inputSnapshot.viewParams.scaleFactor;
                                std::wstring stereoText = FormatViewParams(
                                    inputSnapshot.viewParams.ipdFactor, inputSnapshot.viewParams.parallaxFactor,
                                    dispP1, dispP2, inputSnapshot.cameraMode);
                                {
                                    wchar_t vhBuf[64];
                                    if (inputSnapshot.cameraMode) {
                                        float tanHFOV = CAMERA_HALF_TAN_VFOV / inputSnapshot.viewParams.zoomFactor;
                                        swprintf(vhBuf, 64, L"\ntanHFOV: %.3f", tanHFOV);
                                    } else {
                                        float hudM2v = 1.0f;
                                        if (inputSnapshot.viewParams.virtualDisplayHeight > 0.0f && xr->displayHeightM > 0.0f)
                                            hudM2v = inputSnapshot.viewParams.virtualDisplayHeight / xr->displayHeightM;
                                        swprintf(vhBuf, 64, L"\nvHeight: %.3f  m2v: %.3f",
                                            inputSnapshot.viewParams.virtualDisplayHeight, hudM2v);
                                    }
                                    stereoText += vhBuf;
                                }
                                std::wstring helpText = FormatHelpText(xr->pfnRequestDisplayRenderingModeEXT != nullptr, inputSnapshot.cameraMode, xr->renderingModeCount);

                                uint32_t srcRowPitch = 0;
                                const void* pixels = RenderHudAndMap(*hud, &srcRowPitch,
                                    sessionText, modeText, perfText, dispText, eyeText,
                                    cameraText, stereoText, helpText);
                                bool uploadOk = false;
                                if (pixels) {
                                    while (glGetError() != GL_NO_ERROR) {}
                                    GLuint hudTexId = (*hudSwapchainImages)[hudImageIndex].image;
                                    glBindTexture(GL_TEXTURE_2D, hudTexId);
                                    {
                                        const uint8_t* src = (const uint8_t*)pixels;
                                        for (uint32_t row = 0; row < hudHeight; row++) {
                                            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row, hudWidth, 1,
                                                GL_RGBA, GL_UNSIGNED_BYTE,
                                                src + (hudHeight - 1 - row) * srcRowPitch);
                                        }
                                    }
                                    glFlush();
                                    GLenum glErr = glGetError();
                                    glBindTexture(GL_TEXTURE_2D, 0);
                                    UnmapHud(*hud);
                                    if (glErr == GL_NO_ERROR) {
                                        uploadOk = true;
                                    }
                                }
                                bool releaseOk = ReleaseHudSwapchainImage(*xr);
                                hudSubmitted = uploadOk && releaseOk;
                            }
                        }
                    }
                }

                if (hudSubmitted) {
                    float hudAR = (float)HUD_PIXEL_WIDTH / (float)HUD_PIXEL_HEIGHT;
                    float windowAR = (windowW > 0 && windowH > 0) ? (float)windowW / (float)windowH : 1.0f;
                    float fracW = HUD_WIDTH_FRACTION;
                    float fracH = fracW * windowAR / hudAR;
                    if (fracH > 1.0f) { fracH = 1.0f; fracW = hudAR / windowAR; }
                    EndFrameWithWindowSpaceHud(*xr, frameState.predictedDisplayTime, projectionViews.data(),
                        0.0f, 0.0f, fracW, fracH, 0.0f, (uint32_t)eyeCount);
                } else {
                    EndFrame(*xr, frameState.predictedDisplayTime, projectionViews.data(), (uint32_t)eyeCount);
                }

                // After xrEndFrame: blit shared texture to app window via D3D11
                if (frameState.shouldRender && appBackBufferRTV) {
                    // Release GL context temporarily for D3D11 blit
                    glFinish();

                    float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                    g_d3dContext->ClearRenderTargetView(appBackBufferRTV.Get(), clearColor);
                    BlitSharedTextureToBackBuffer(g_d3dContext.Get(), appBackBufferRTV.Get(),
                                                   windowW, windowH);
                    appSwapchain->Present(1, 0);
                }
            }
        } else {
            Sleep(100);
        }
    }

    wglMakeCurrent(nullptr, nullptr);

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

    LOG_INFO("=== SR Cube Shared Texture OpenGL ===");
    LOG_INFO("GL rendering + D3D11 shared texture (zero-copy GPU texture sharing)");

    HWND hwnd = CreateAppWindow(hInstance, g_windowWidth, g_windowHeight);
    if (!hwnd) {
        ShutdownLogging();
        return 1;
    }

    // Add DisplayXR to DLL search path
    {
        HKEY hKey;
        char installPath[MAX_PATH] = {0};
        DWORD pathSize = sizeof(installPath);
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\DisplayXR\\Runtime", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            if (RegQueryValueExA(hKey, "InstallPath", nullptr, nullptr, (LPBYTE)installPath, &pathSize) == ERROR_SUCCESS) {
                SetDllDirectoryA(installPath);
            }
            RegCloseKey(hKey);
        }
    }

    // Create OpenGL context
    HDC hDC = nullptr;
    HGLRC hGLRC = nullptr;
    if (!CreateOpenGLContext(hwnd, hDC, hGLRC)) {
        LOG_ERROR("OpenGL context creation failed");
        ShutdownLogging();
        return 1;
    }

    // Load GL function pointers
    if (!LoadGLFunctions()) {
        LOG_ERROR("Failed to load GL function pointers");
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(hGLRC);
        ShutdownLogging();
        return 1;
    }

    // Create D3D11 device for shared texture
    {
        D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
        D3D_FEATURE_LEVEL actualLevel;
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            featureLevels, 1, D3D11_SDK_VERSION,
            &g_d3dDevice, &actualLevel, &g_d3dContext);
        if (FAILED(hr)) {
            LOG_ERROR("Failed to create D3D11 device: 0x%08x", hr);
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(hGLRC);
            ShutdownLogging();
            return 1;
        }

        // Get DXGI factory
        ComPtr<IDXGIDevice> dxgiDevice;
        g_d3dDevice.As(&dxgiDevice);
        ComPtr<IDXGIAdapter> adapter;
        dxgiDevice->GetAdapter(&adapter);
        adapter->GetParent(__uuidof(IDXGIFactory2), &g_dxgiFactory);
    }

    // Create app-side DXGI swapchain for window presentation
    ComPtr<IDXGISwapChain1> appSwapchain;
    {
        DXGI_SWAP_CHAIN_DESC1 scd = {};
        scd.Width = g_windowWidth;
        scd.Height = g_windowHeight;
        scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd.SampleDesc.Count = 1;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount = 2;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        HRESULT hr = g_dxgiFactory->CreateSwapChainForHwnd(
            g_d3dDevice.Get(), hwnd, &scd, nullptr, nullptr, &appSwapchain);
        if (FAILED(hr)) {
            LOG_ERROR("Failed to create app swapchain: 0x%08x", hr);
            ShutdownLogging();
            return 1;
        }
    }

    ComPtr<ID3D11RenderTargetView> appBackBufferRTV;
    {
        ComPtr<ID3D11Texture2D> backBuf;
        appSwapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), &backBuf);
        g_d3dDevice->CreateRenderTargetView(backBuf.Get(), nullptr, &appBackBufferRTV);
    }

    // Initialize OpenXR
    XrSessionManager xr = {};
    g_xr = &xr;
    if (!InitializeOpenXR(xr)) {
        LOG_ERROR("OpenXR init failed");
        g_xr = nullptr;
        ShutdownLogging();
        return 1;
    }

    if (!GetOpenGLGraphicsRequirements(xr)) {
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create shared D3D11 texture
    g_sharedWidth = xr.displayPixelWidth > 0 ? xr.displayPixelWidth : 1920;
    g_sharedHeight = xr.displayPixelHeight > 0 ? xr.displayPixelHeight : 1080;
    {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = g_sharedWidth;
        desc.Height = g_sharedHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

        HRESULT hr = g_d3dDevice->CreateTexture2D(&desc, nullptr, &g_sharedTexture);
        if (FAILED(hr)) {
            LOG_ERROR("Failed to create shared texture: 0x%08x", hr);
            CleanupOpenXR(xr);
            ShutdownLogging();
            return 1;
        }

        ComPtr<IDXGIResource> dxgiResource;
        g_sharedTexture->QueryInterface(__uuidof(IDXGIResource), &dxgiResource);
        dxgiResource->GetSharedHandle(&g_sharedHandle);

        hr = g_d3dDevice->CreateShaderResourceView(g_sharedTexture.Get(), nullptr, &g_sharedSRV);
        if (FAILED(hr)) {
            LOG_ERROR("Failed to create SRV for shared texture: 0x%08x", hr);
            CleanupOpenXR(xr);
            ShutdownLogging();
            return 1;
        }

        LOG_INFO("Created shared D3D11 texture: %ux%u, handle=%p", g_sharedWidth, g_sharedHeight, g_sharedHandle);
    }

    // Create D3D11 blit resources
    if (!CreateBlitResources(g_d3dDevice.Get())) {
        LOG_ERROR("Failed to create blit resources");
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create OpenXR session with GL context + shared texture
    if (!CreateSession(xr, hDC, hGLRC, g_sharedHandle)) {
        LOG_ERROR("Session creation failed");
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    if (!CreateSpaces(xr)) {
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    if (!CreateSwapchain(xr)) {
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    std::vector<XrSwapchainImageOpenGLKHR> swapchainImages;
    {
        uint32_t count = xr.swapchain.imageCount;
        swapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
        xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)swapchainImages.data());
        LOG_INFO("Enumerated %u OpenGL swapchain images", count);
    }

    // Initialize GL renderer
    GLRenderer glRenderer = {};
    if (!InitializeGLRenderer(glRenderer)) {
        LOG_ERROR("GL renderer initialization failed");
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create FBOs for swapchain images
    {
        uint32_t count = xr.swapchain.imageCount;
        std::vector<GLuint> textures(count);
        for (uint32_t i = 0; i < count; i++) {
            textures[i] = swapchainImages[i].image;
        }
        if (!CreateSwapchainFBOs(glRenderer, textures.data(), count,
            xr.swapchain.width, xr.swapchain.height)) {
            LOG_ERROR("Failed to create FBOs for swapchain");
            CleanupGLRenderer(glRenderer);
            CleanupOpenXR(xr);
            ShutdownLogging();
            return 1;
        }
    }

    // HUD
    HudRenderer hudRenderer = {};
    bool hudOk = InitializeHudRenderer(hudRenderer, HUD_PIXEL_WIDTH, HUD_PIXEL_HEIGHT);

    std::vector<XrSwapchainImageOpenGLKHR> hudSwapImages;
    if (hudOk) {
        if (CreateHudSwapchain(xr, HUD_PIXEL_WIDTH, HUD_PIXEL_HEIGHT)) {
            uint32_t count = xr.hudSwapchain.imageCount;
            hudSwapImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
            xrEnumerateSwapchainImages(xr.hudSwapchain.swapchain, count, &count,
                (XrSwapchainImageBaseHeader*)hudSwapImages.data());
        } else {
            hudOk = false;
        }
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    LOG_INFO("");
    LOG_INFO("=== Entering main loop ===");
    LOG_INFO("Shared texture mode: GL rendering, runtime composites via WGL/DX interop, app blits D3D11 to window");
    LOG_INFO("Controls: WASD=Fly, Mouse=Look, Space=Reset, V=Mode, TAB=HUD, F11=Fullscreen, ESC=Quit");

    // Release GL context from main thread before handing to render thread
    wglMakeCurrent(nullptr, nullptr);

    g_inputState.viewParams.virtualDisplayHeight = 0.24f;
    g_inputState.nominalViewerZ = xr.nominalViewerZ;
    g_inputState.renderingModeCount = xr.renderingModeCount;

    std::thread renderThread(RenderThreadFunc, hwnd, hDC, hGLRC, &xr, &glRenderer,
        &swapchainImages,
        hudOk ? &hudRenderer : nullptr, HUD_PIXEL_WIDTH, HUD_PIXEL_HEIGHT,
        hudOk ? &hudSwapImages : nullptr,
        appSwapchain, appBackBufferRTV);

    MSG msg = {};
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    g_running.store(false);
    LOG_INFO("Main thread: waiting for render thread...");
    renderThread.join();

    LOG_INFO("");
    LOG_INFO("=== Shutting down ===");

    // Re-acquire GL context for cleanup
    wglMakeCurrent(hDC, hGLRC);

    if (hudOk) CleanupHudRenderer(hudRenderer);
    CleanupGLRenderer(glRenderer);
    g_xr = nullptr;
    CleanupOpenXR(xr);

    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(hGLRC);
    ReleaseDC(hwnd, hDC);

    g_sharedSRV.Reset();
    g_sharedTexture.Reset();
    g_blitVS.Reset();
    g_blitPS.Reset();
    g_blitSampler.Reset();
    appBackBufferRTV.Reset();
    appSwapchain.Reset();
    g_d3dContext.Reset();
    g_d3dDevice.Reset();
    g_dxgiFactory.Reset();

    DestroyWindow(hwnd);
    UnregisterClass(WINDOW_CLASS, hInstance);

    ShutdownLogging();
    return 0;
}
