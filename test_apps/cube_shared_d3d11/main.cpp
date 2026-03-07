// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  SR Cube OpenXR Shared Texture — D3D11 shared texture demo
 *
 * Demonstrates zero-copy shared GPU texture via XR_EXT_win32_window_binding:
 * the app creates a shared D3D11 texture (MISC_SHARED), passes its HANDLE
 * to the runtime (windowHandle=NULL, sharedTextureHandle=handle). The runtime
 * renders the composited output into the shared texture. The app then blits
 * the shared texture into its own window each frame.
 *
 * Key difference from cube_ext_d3d11: no window handle is passed to the runtime.
 * Instead, the shared texture acts as the render target, and the app composites
 * the result into its own rendering pipeline.
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <wrl/client.h>

#include <d3dcompiler.h>

#include "logging.h"
#include "input_handler.h"
#include "d3d11_renderer.h"
#include "text_overlay.h"
#include "hud_renderer.h"
#include "xr_session.h"
#include "display3d_view.h"
#include "camera3d_view.h"

#include <chrono>
#include <cmath>
#include <string>
#include <sstream>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

static const char* APP_NAME = "cube_shared_d3d11";
static const wchar_t* WINDOW_CLASS = L"SRCubeSharedD3D11Class";
static const wchar_t* WINDOW_TITLE = L"D3D11 Cube \u2014 D3D11 Native Compositor (Shared Texture)";

// Global state
static InputState g_inputState;
static bool g_running = true;
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

// Shared texture resources
static ComPtr<ID3D11Texture2D> g_sharedTexture;
static ComPtr<ID3D11ShaderResourceView> g_sharedSRV;
static HANDLE g_sharedHandle = nullptr;
static uint32_t g_sharedWidth = 0;
static uint32_t g_sharedHeight = 0;

// Blit shader resources
static ComPtr<ID3D11VertexShader> g_blitVS;
static ComPtr<ID3D11PixelShader> g_blitPS;
static ComPtr<ID3D11SamplerState> g_blitSampler;

struct RenderState;
static RenderState* g_renderState = nullptr;
static void RenderOneFrame(RenderState& rs);

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
    UpdateInputState(g_inputState, msg, wParam, lParam);

    switch (msg) {
    case WM_LBUTTONDOWN: SetCapture(hwnd); return 0;
    case WM_LBUTTONUP: ReleaseCapture(); return 0;
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            g_windowWidth = LOWORD(lParam);
            g_windowHeight = HIWORD(lParam);
        }
        return 0;
    case WM_ENTERSIZEMOVE:
        g_inSizeMove = true;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_EXITSIZEMOVE:
        g_inSizeMove = false;
        return 0;
    case WM_PAINT:
        if (g_inSizeMove && g_renderState != nullptr) {
            RenderOneFrame(*g_renderState);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_CLOSE:
        if (g_xr && g_xr->session != XR_NULL_HANDLE && g_xr->sessionRunning) {
            xrRequestExitSession(g_xr->session);
            return 0;
        }
        g_running = false;
        PostQuitMessage(0);
        return 0;
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
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
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

// Fullscreen-quad blit shaders (hardcoded HLSL)
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
    // Compile vertex shader
    ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
    HRESULT hr = D3DCompile(g_blitVSSource, strlen(g_blitVSSource), "blitVS", nullptr, nullptr,
        "main", "vs_5_0", 0, 0, &vsBlob, &errBlob);
    if (FAILED(hr)) {
        LOG_ERROR("Blit VS compile failed: %s", errBlob ? (char*)errBlob->GetBufferPointer() : "unknown");
        return false;
    }
    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_blitVS);
    if (FAILED(hr)) return false;

    // Compile pixel shader
    hr = D3DCompile(g_blitPSSource, strlen(g_blitPSSource), "blitPS", nullptr, nullptr,
        "main", "ps_5_0", 0, 0, &psBlob, &errBlob);
    if (FAILED(hr)) {
        LOG_ERROR("Blit PS compile failed: %s", errBlob ? (char*)errBlob->GetBufferPointer() : "unknown");
        return false;
    }
    hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_blitPS);
    if (FAILED(hr)) return false;

    // Sampler
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    hr = device->CreateSamplerState(&sd, &g_blitSampler);
    return SUCCEEDED(hr);
}

// Blit shared texture to back buffer with aspect-ratio letterboxing
static void BlitSharedTextureToBackBuffer(D3D11Renderer& renderer, ID3D11RenderTargetView* backBufferRTV,
                                           uint32_t winW, uint32_t winH) {
    if (!g_sharedSRV) return;

    renderer.context->OMSetRenderTargets(1, &backBufferRTV, nullptr);

    // Compute letterbox viewport
    float srcAR = (float)g_sharedWidth / (float)g_sharedHeight;
    float dstAR = (float)winW / (float)winH;

    D3D11_VIEWPORT vp = {};
    if (srcAR > dstAR) {
        // Pillarbox (source wider)
        vp.Width = (FLOAT)winW;
        vp.Height = (FLOAT)winW / srcAR;
        vp.TopLeftX = 0;
        vp.TopLeftY = ((FLOAT)winH - vp.Height) * 0.5f;
    } else {
        // Letterbox (source taller)
        vp.Height = (FLOAT)winH;
        vp.Width = (FLOAT)winH * srcAR;
        vp.TopLeftX = ((FLOAT)winW - vp.Width) * 0.5f;
        vp.TopLeftY = 0;
    }
    vp.MaxDepth = 1.0f;
    renderer.context->RSSetViewports(1, &vp);

    renderer.context->VSSetShader(g_blitVS.Get(), nullptr, 0);
    renderer.context->PSSetShader(g_blitPS.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srv = g_sharedSRV.Get();
    renderer.context->PSSetShaderResources(0, 1, &srv);
    ID3D11SamplerState* smp = g_blitSampler.Get();
    renderer.context->PSSetSamplers(0, 1, &smp);
    renderer.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    renderer.context->IASetInputLayout(nullptr);
    renderer.context->Draw(3, 0);

    // Unbind SRV to avoid D3D11 warnings
    ID3D11ShaderResourceView* nullSRV = nullptr;
    renderer.context->PSSetShaderResources(0, 1, &nullSRV);
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

struct RenderState {
    HWND hwnd;
    XrSessionManager* xr;
    D3D11Renderer* renderer;
    HudRenderer* hudRenderer;
    bool hudOk;
    std::vector<XrSwapchainImageD3D11KHR>* hudSwapchainImages;
    ComPtr<ID3D11Texture2D> depthTexture;
    ComPtr<ID3D11DepthStencilView> depthDSV;
    std::vector<XrSwapchainImageD3D11KHR>* swapchainImages;
    PerformanceStats* perfStats;
    // App-side swapchain and RTV for window presentation
    ComPtr<IDXGISwapChain1> appSwapchain;
    ComPtr<ID3D11RenderTargetView> appBackBufferRTV;
};

static void RenderOneFrame(RenderState& rs) {
    XrSessionManager& xr = *rs.xr;
    D3D11Renderer& renderer = *rs.renderer;

    UpdatePerformanceStats(*rs.perfStats);
    UpdateCameraMovement(g_inputState, rs.perfStats->deltaTime, rs.xr->displayHeightM);

    if (g_inputState.fullscreenToggleRequested) {
        ToggleFullscreen(rs.hwnd);
        g_inputState.fullscreenToggleRequested = false;
    }
    if (g_inputState.displayModeToggleRequested) {
        g_inputState.displayModeToggleRequested = false;
        if (xr.pfnRequestDisplayModeEXT && xr.session != XR_NULL_HANDLE) {
            XrDisplayModeEXT mode = g_inputState.displayMode3D ?
                XR_DISPLAY_MODE_3D_EXT : XR_DISPLAY_MODE_2D_EXT;
            xr.pfnRequestDisplayModeEXT(xr.session, mode);
        }
    }
    if (g_inputState.eyeTrackingModeToggleRequested) {
        g_inputState.eyeTrackingModeToggleRequested = false;
        if (xr.pfnRequestEyeTrackingModeEXT && xr.session != XR_NULL_HANDLE) {
            XrEyeTrackingModeEXT newMode = (xr.activeEyeTrackingMode == XR_EYE_TRACKING_MODE_SMOOTH_EXT)
                ? XR_EYE_TRACKING_MODE_RAW_EXT : XR_EYE_TRACKING_MODE_SMOOTH_EXT;
            xr.pfnRequestEyeTrackingModeEXT(xr.session, newMode);
        }
    }
    if (g_inputState.outputModeChangeRequested) {
        g_inputState.outputModeChangeRequested = false;
        if (xr.pfnRequestDisplayRenderingModeEXT && xr.session != XR_NULL_HANDLE) {
            xr.pfnRequestDisplayRenderingModeEXT(xr.session, (uint32_t)g_inputState.outputMode);
        }
    }

    UpdateScene(renderer, rs.perfStats->deltaTime);
    PollEvents(xr);

    if (xr.sessionRunning) {
        XrFrameState frameState;
        if (BeginFrame(xr, frameState)) {
            XrCompositionLayerProjectionView projectionViews[2] = {};
            bool hudSubmitted = false;

            if (frameState.shouldRender) {
                XMMATRIX leftViewMatrix, leftProjMatrix;
                XMMATRIX rightViewMatrix, rightProjMatrix;

                if (LocateViews(xr, frameState.predictedDisplayTime,
                    leftViewMatrix, leftProjMatrix,
                    rightViewMatrix, rightProjMatrix,
                    g_inputState.cameraPosX, g_inputState.cameraPosY, g_inputState.cameraPosZ,
                    g_inputState.yaw, g_inputState.pitch,
                    g_inputState.stereo)) {

                    XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
                    locateInfo.viewConfigurationType = xr.viewConfigType;
                    locateInfo.displayTime = frameState.predictedDisplayTime;
                    locateInfo.space = xr.localSpace;

                    XrViewState viewState = {XR_TYPE_VIEW_STATE};
                    uint32_t viewCount = 2;
                    XrView rawViews[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
                    xrLocateViews(xr.session, &locateInfo, &viewState, 2, &viewCount, rawViews);

                    xr.leftEyeX = rawViews[0].pose.position.x;
                    xr.leftEyeY = rawViews[0].pose.position.y;
                    xr.leftEyeZ = rawViews[0].pose.position.z;
                    xr.rightEyeX = rawViews[1].pose.position.x;
                    xr.rightEyeY = rawViews[1].pose.position.y;
                    xr.rightEyeZ = rawViews[1].pose.position.z;

                    uint32_t eyeRenderW = xr.swapchain.width / 2;
                    uint32_t eyeRenderH = xr.swapchain.height;

                    Display3DStereoView stereoViews[2];
                    bool useAppProjection = (xr.hasDisplayInfoExt && xr.displayWidthM > 0.0f);
                    if (useAppProjection) {
                        float pxSizeX = xr.displayWidthM / (float)xr.swapchain.width;
                        float pxSizeY = xr.displayHeightM / (float)xr.swapchain.height;
                        float winW_m = (float)g_sharedWidth * pxSizeX;
                        float winH_m = (float)g_sharedHeight * pxSizeY;
                        float minDisp = fminf(xr.displayWidthM, xr.displayHeightM);
                        float minWin  = fminf(winW_m, winH_m);
                        float vs = minDisp / minWin;

                        XrVector3f rawLeft = rawViews[0].pose.position;
                        XrVector3f rawRight = rawViews[1].pose.position;
                        if (!g_inputState.displayMode3D) {
                            XrVector3f center = {
                                (rawLeft.x + rawRight.x) * 0.5f,
                                (rawLeft.y + rawRight.y) * 0.5f,
                                (rawLeft.z + rawRight.z) * 0.5f};
                            rawLeft = rawRight = center;
                        }

                        XrPosef cameraPose;
                        XMVECTOR pOri = XMQuaternionRotationRollPitchYaw(
                            g_inputState.pitch, g_inputState.yaw, 0);
                        XMFLOAT4 q;
                        XMStoreFloat4(&q, pOri);
                        cameraPose.orientation = {q.x, q.y, q.z, q.w};
                        cameraPose.position = {g_inputState.cameraPosX, g_inputState.cameraPosY, g_inputState.cameraPosZ};

                        XrVector3f nominalViewer = {xr.nominalViewerX, xr.nominalViewerY, xr.nominalViewerZ};
                        Display3DScreen screen = {winW_m * vs, winH_m * vs};

                        if (g_inputState.cameraMode) {
                            Camera3DTunables camTunables;
                            camTunables.ipd_factor = g_inputState.stereo.ipdFactor;
                            camTunables.parallax_factor = g_inputState.stereo.parallaxFactor;
                            camTunables.inv_convergence_distance = g_inputState.stereo.invConvergenceDistance;
                            camTunables.half_tan_vfov = CAMERA_HALF_TAN_VFOV / g_inputState.stereo.zoomFactor;

                            Camera3DStereoView camViews[2];
                            camera3d_compute_stereo_views(
                                &rawLeft, &rawRight, &nominalViewer,
                                &screen, &camTunables, &cameraPose,
                                0.01f, 100.0f, &camViews[0], &camViews[1]);

                            for (int i = 0; i < 2; i++) {
                                memcpy(stereoViews[i].view_matrix, camViews[i].view_matrix, sizeof(float) * 16);
                                memcpy(stereoViews[i].projection_matrix, camViews[i].projection_matrix, sizeof(float) * 16);
                                stereoViews[i].fov = camViews[i].fov;
                                stereoViews[i].eye_world = camViews[i].eye_world;
                            }
                        } else {
                            Display3DTunables tunables;
                            tunables.ipd_factor = g_inputState.stereo.ipdFactor;
                            tunables.parallax_factor = g_inputState.stereo.parallaxFactor;
                            tunables.perspective_factor = g_inputState.stereo.perspectiveFactor;
                            tunables.virtual_display_height = g_inputState.stereo.virtualDisplayHeight / g_inputState.stereo.scaleFactor;

                            display3d_compute_stereo_views(
                                &rawLeft, &rawRight, &nominalViewer,
                                &screen, &tunables, &cameraPose,
                                0.01f, 100.0f, &stereoViews[0], &stereoViews[1]);
                        }
                    }

                    // Render HUD
                    if (g_inputState.hudVisible && xr.hasHudSwapchain && rs.hudSwapchainImages && !rs.hudSwapchainImages->empty() && rs.hudOk) {
                        uint32_t hudImageIndex;
                        if (AcquireHudSwapchainImage(xr, hudImageIndex)) {
                            std::wstring sessionText(xr.systemName, xr.systemName + strlen(xr.systemName));
                            sessionText += L"\nSession: ";
                            sessionText += FormatSessionState((int)xr.sessionState);
                            std::wstring modeText = L"Shared Texture D3D11 (offscreen)";
                            if (xr.supportsDisplayModeSwitch) {
                                modeText += g_inputState.displayMode3D ?
                                    L"\nDisplay Mode: 3D Stereo [V=Toggle]" :
                                    L"\nDisplay Mode: 2D Mono [V=Toggle]";
                            }
                            modeText += g_inputState.cameraMode ?
                                L"\nKooima: Camera-Centric [C=Toggle]" :
                                L"\nKooima: Display-Centric [C=Toggle]";

                            uint32_t dispRenderW, dispRenderH;
                            if (!g_inputState.displayMode3D) {
                                dispRenderW = g_sharedWidth;
                                dispRenderH = g_sharedHeight;
                                if (dispRenderW > xr.swapchain.width) dispRenderW = xr.swapchain.width;
                                if (dispRenderH > xr.swapchain.height) dispRenderH = xr.swapchain.height;
                            } else {
                                dispRenderW = (uint32_t)(g_sharedWidth * xr.recommendedViewScaleX);
                                dispRenderH = (uint32_t)(g_sharedHeight * xr.recommendedViewScaleY);
                                if (dispRenderW > xr.swapchain.width / 2) dispRenderW = xr.swapchain.width / 2;
                                if (dispRenderH > xr.swapchain.height) dispRenderH = xr.swapchain.height;
                            }
                            std::wstring perfText = FormatPerformanceInfo(rs.perfStats->fps, rs.perfStats->frameTimeMs,
                                dispRenderW, dispRenderH, g_windowWidth, g_windowHeight);
                            std::wstring dispText = FormatDisplayInfo(xr.displayWidthM, xr.displayHeightM,
                                xr.nominalViewerX, xr.nominalViewerY, xr.nominalViewerZ);
                            dispText += L"\n" + FormatScaleInfo(xr.recommendedViewScaleX, xr.recommendedViewScaleY);
                            dispText += L"\n" + FormatOutputMode(g_inputState.outputMode, xr.pfnRequestDisplayRenderingModeEXT != nullptr);
                            std::wstring eyeText = FormatEyeTrackingInfo(
                                xr.leftEyeX, xr.leftEyeY, xr.leftEyeZ,
                                xr.rightEyeX, xr.rightEyeY, xr.rightEyeZ,
                                xr.eyeTrackingActive, xr.isEyeTracking,
                                xr.activeEyeTrackingMode, xr.supportedEyeTrackingModes);

                            float fwdX = -sinf(g_inputState.yaw) * cosf(g_inputState.pitch);
                            float fwdY =  sinf(g_inputState.pitch);
                            float fwdZ = -cosf(g_inputState.yaw) * cosf(g_inputState.pitch);
                            std::wstring cameraText = FormatCameraInfo(
                                g_inputState.cameraPosX, g_inputState.cameraPosY, g_inputState.cameraPosZ,
                                fwdX, fwdY, fwdZ, g_inputState.cameraMode);
                            float dispP1 = g_inputState.cameraMode ? g_inputState.stereo.invConvergenceDistance : g_inputState.stereo.perspectiveFactor;
                            float dispP2 = g_inputState.cameraMode ? g_inputState.stereo.zoomFactor : g_inputState.stereo.scaleFactor;
                            std::wstring stereoText = FormatStereoParams(
                                g_inputState.stereo.ipdFactor, g_inputState.stereo.parallaxFactor,
                                dispP1, dispP2, g_inputState.cameraMode);
                            {
                                wchar_t vhBuf[64];
                                if (g_inputState.cameraMode) {
                                    float tanHFOV = CAMERA_HALF_TAN_VFOV / g_inputState.stereo.zoomFactor;
                                    swprintf(vhBuf, 64, L"\ntanHFOV: %.3f", tanHFOV);
                                } else {
                                    float hudM2v = 1.0f;
                                    if (g_inputState.stereo.virtualDisplayHeight > 0.0f && xr.displayHeightM > 0.0f)
                                        hudM2v = g_inputState.stereo.virtualDisplayHeight / xr.displayHeightM;
                                    swprintf(vhBuf, 64, L"\nvHeight: %.3f  m2v: %.3f",
                                        g_inputState.stereo.virtualDisplayHeight, hudM2v);
                                }
                                stereoText += vhBuf;
                            }
                            std::wstring helpText = FormatHelpText(xr.pfnRequestDisplayRenderingModeEXT != nullptr, g_inputState.cameraMode);

                            uint32_t srcRowPitch = 0;
                            const void* pixels = RenderHudAndMap(*rs.hudRenderer, &srcRowPitch,
                                sessionText, modeText, perfText, dispText, eyeText,
                                cameraText, stereoText, helpText);
                            if (pixels) {
                                ID3D11Texture2D* hudTexture = (*rs.hudSwapchainImages)[hudImageIndex].texture;
                                D3D11_BOX box = {0, 0, 0, xr.hudSwapchain.width, xr.hudSwapchain.height, 1};
                                renderer.context->UpdateSubresource(hudTexture, 0, &box, pixels, srcRowPitch, 0);
                                UnmapHud(*rs.hudRenderer);
                            }
                            ReleaseHudSwapchainImage(xr);
                            hudSubmitted = true;
                        }
                    }

                    bool monoMode = !g_inputState.displayMode3D;
                    int eyeCount = monoMode ? 1 : 2;

                    XMMATRIX monoViewMatrix, monoProjMatrix;
                    XrFovf monoFov = {};
                    XrPosef monoPose = rawViews[0].pose;
                    if (monoMode) {
                        monoPose.position.x = (rawViews[0].pose.position.x + rawViews[1].pose.position.x) * 0.5f;
                        monoPose.position.y = (rawViews[0].pose.position.y + rawViews[1].pose.position.y) * 0.5f;
                        monoPose.position.z = (rawViews[0].pose.position.z + rawViews[1].pose.position.z) * 0.5f;
                        if (!useAppProjection) {
                            monoProjMatrix = leftProjMatrix;
                            monoFov = rawViews[0].fov;
                            XMVECTOR centerLocalPos = XMVectorSet(
                                monoPose.position.x, monoPose.position.y, monoPose.position.z, 0.0f);
                            XMVECTOR localOri = XMVectorSet(
                                rawViews[0].pose.orientation.x, rawViews[0].pose.orientation.y,
                                rawViews[0].pose.orientation.z, rawViews[0].pose.orientation.w);
                            float monoM2vView = 1.0f;
                            if (g_inputState.stereo.virtualDisplayHeight > 0.0f && xr.displayHeightM > 0.0f)
                                monoM2vView = g_inputState.stereo.virtualDisplayHeight / xr.displayHeightM;
                            float eyeScale = g_inputState.stereo.perspectiveFactor * monoM2vView / g_inputState.stereo.scaleFactor;
                            XMVECTOR playerOri = XMQuaternionRotationRollPitchYaw(
                                g_inputState.pitch, g_inputState.yaw, 0);
                            XMVECTOR playerPos = XMVectorSet(
                                g_inputState.cameraPosX, g_inputState.cameraPosY,
                                g_inputState.cameraPosZ, 0.0f);
                            XMVECTOR worldPos = XMVector3Rotate(centerLocalPos * eyeScale, playerOri) + playerPos;
                            XMVECTOR worldOri = XMQuaternionMultiply(localOri, playerOri);
                            XMMATRIX rot = XMMatrixTranspose(XMMatrixRotationQuaternion(worldOri));
                            XMFLOAT3 wp;
                            XMStoreFloat3(&wp, worldPos);
                            monoViewMatrix = XMMatrixTranslation(-wp.x, -wp.y, -wp.z) * rot;
                        }
                    }

                    uint32_t imageIndex;
                    if (AcquireSwapchainImage(xr, imageIndex)) {
                        ID3D11Texture2D* swapchainTexture = (*rs.swapchainImages)[imageIndex].texture;
                        ID3D11RenderTargetView* rtv = nullptr;
                        CreateRenderTargetView(renderer, swapchainTexture, &rtv);

                        float clearColor[4] = {0.05f, 0.05f, 0.25f, 1.0f};
                        renderer.context->ClearRenderTargetView(rtv, clearColor);
                        renderer.context->ClearDepthStencilView(rs.depthDSV.Get(),
                            D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

                        uint32_t renderW, renderH;
                        if (monoMode) {
                            renderW = g_sharedWidth;
                            renderH = g_sharedHeight;
                            if (renderW > xr.swapchain.width) renderW = xr.swapchain.width;
                            if (renderH > xr.swapchain.height) renderH = xr.swapchain.height;
                        } else {
                            renderW = (uint32_t)(g_sharedWidth * xr.recommendedViewScaleX);
                            renderH = (uint32_t)(g_sharedHeight * xr.recommendedViewScaleY);
                            if (renderW > eyeRenderW) renderW = eyeRenderW;
                            if (renderH > eyeRenderH) renderH = eyeRenderH;
                        }

                        for (int eye = 0; eye < eyeCount; eye++) {
                            D3D11_VIEWPORT vp = {};
                            vp.TopLeftX = monoMode ? 0.0f : (FLOAT)(eye * renderW);
                            vp.Width = (FLOAT)renderW;
                            vp.Height = (FLOAT)renderH;
                            vp.MaxDepth = 1.0f;
                            renderer.context->RSSetViewports(1, &vp);

                            XMMATRIX viewMatrix, projMatrix;
                            if (useAppProjection) {
                                int vi = monoMode ? 0 : eye;
                                viewMatrix = ColumnMajorToXMMatrix(stereoViews[vi].view_matrix);
                                projMatrix = ColumnMajorToXMMatrix(stereoViews[vi].projection_matrix);
                            } else if (monoMode) {
                                viewMatrix = monoViewMatrix;
                                projMatrix = monoProjMatrix;
                            } else {
                                viewMatrix = (eye == 0) ? leftViewMatrix : rightViewMatrix;
                                projMatrix = (eye == 0) ? leftProjMatrix : rightProjMatrix;
                            }

                            RenderScene(renderer, rtv, rs.depthDSV.Get(),
                                renderW, renderH, viewMatrix, projMatrix,
                                useAppProjection ? 1.0f : g_inputState.stereo.scaleFactor, 0.03f);

                            projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                            projectionViews[eye].subImage.swapchain = xr.swapchain.swapchain;
                            projectionViews[eye].subImage.imageRect.offset = {
                                (int32_t)(monoMode ? 0 : eye * renderW), 0
                            };
                            projectionViews[eye].subImage.imageRect.extent = {
                                (int32_t)renderW, (int32_t)renderH
                            };
                            projectionViews[eye].subImage.imageArrayIndex = 0;
                            projectionViews[eye].pose = monoMode ? monoPose : rawViews[eye].pose;
                            projectionViews[eye].fov = useAppProjection ?
                                stereoViews[monoMode ? 0 : eye].fov :
                                (monoMode ? monoFov : rawViews[eye].fov);
                        }

                        if (rtv) rtv->Release();
                        ReleaseSwapchainImage(xr);
                    }
                }
            }

            uint32_t submitViewCount = g_inputState.displayMode3D ? 2 : 1;
            if (hudSubmitted) {
                float hudAR = (float)HUD_PIXEL_WIDTH / (float)HUD_PIXEL_HEIGHT;
                float windowAR = (g_windowWidth > 0 && g_windowHeight > 0) ? (float)g_windowWidth / (float)g_windowHeight : 1.0f;
                float fracW = HUD_WIDTH_FRACTION;
                float fracH = fracW * windowAR / hudAR;
                if (fracH > 1.0f) { fracH = 1.0f; fracW = hudAR / windowAR; }
                EndFrameWithWindowSpaceHud(xr, frameState.predictedDisplayTime, projectionViews,
                    0.0f, 0.0f, fracW, fracH, 0.0f, submitViewCount);
            } else {
                EndFrame(xr, frameState.predictedDisplayTime, projectionViews, submitViewCount);
            }

            // After xrEndFrame: blit shared texture to app window
            if (frameState.shouldRender && rs.appBackBufferRTV) {
                float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                renderer.context->ClearRenderTargetView(rs.appBackBufferRTV.Get(), clearColor);
                BlitSharedTextureToBackBuffer(renderer, rs.appBackBufferRTV.Get(),
                                               g_windowWidth, g_windowHeight);
                rs.appSwapchain->Present(1, 0);
            }
        }
    } else {
        Sleep(100);
    }
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    if (!InitializeLogging(APP_NAME)) {
        MessageBox(nullptr, L"Failed to initialize logging", L"Warning", MB_OK | MB_ICONWARNING);
    }

    LOG_INFO("=== SR Cube Shared Texture D3D11 ===");
    LOG_INFO("Shared D3D11 texture (zero-copy GPU texture sharing)");

    HWND hwnd = CreateAppWindow(hInstance, g_windowWidth, g_windowHeight);
    if (!hwnd) {
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
                SetDllDirectoryA(installPath);
            }
            RegCloseKey(hKey);
        }
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

    LUID adapterLuid;
    if (!GetD3D11GraphicsRequirements(xr, &adapterLuid)) {
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Initialize D3D11
    D3D11Renderer renderer = {};
    if (!InitializeD3D11WithLUID(renderer, adapterLuid)) {
        LOG_ERROR("D3D11 init failed");
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
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
        HRESULT hr = renderer.dxgiFactory->CreateSwapChainForHwnd(
            renderer.device.Get(), hwnd, &scd, nullptr, nullptr, &appSwapchain);
        if (FAILED(hr)) {
            LOG_ERROR("Failed to create app swapchain: 0x%08x", hr);
            CleanupD3D11(renderer);
            CleanupOpenXR(xr);
            ShutdownLogging();
            return 1;
        }
    }

    // Get back buffer RTV
    ComPtr<ID3D11RenderTargetView> appBackBufferRTV;
    {
        ComPtr<ID3D11Texture2D> backBuf;
        appSwapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), &backBuf);
        renderer.device->CreateRenderTargetView(backBuf.Get(), nullptr, &appBackBufferRTV);
    }

    // Create shared D3D11 texture
    // Use display pixel dimensions if available, otherwise default
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

        HRESULT hr = renderer.device->CreateTexture2D(&desc, nullptr, &g_sharedTexture);
        if (FAILED(hr)) {
            LOG_ERROR("Failed to create shared texture: 0x%08x", hr);
            CleanupD3D11(renderer);
            CleanupOpenXR(xr);
            ShutdownLogging();
            return 1;
        }

        // Get shared handle
        ComPtr<IDXGIResource> dxgiResource;
        g_sharedTexture->QueryInterface(__uuidof(IDXGIResource), &dxgiResource);
        dxgiResource->GetSharedHandle(&g_sharedHandle);

        // Create SRV for blitting to window
        hr = renderer.device->CreateShaderResourceView(g_sharedTexture.Get(), nullptr, &g_sharedSRV);
        if (FAILED(hr)) {
            LOG_ERROR("Failed to create SRV for shared texture: 0x%08x", hr);
            CleanupD3D11(renderer);
            CleanupOpenXR(xr);
            ShutdownLogging();
            return 1;
        }

        LOG_INFO("Created shared D3D11 texture: %ux%u, handle=%p", g_sharedWidth, g_sharedHeight, g_sharedHandle);
    }

    // Create blit shader resources
    if (!CreateBlitResources(renderer.device.Get())) {
        LOG_ERROR("Failed to create blit resources");
        CleanupD3D11(renderer);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // HUD renderer
    HudRenderer hudRenderer = {};
    bool hudOk = InitializeHudRenderer(hudRenderer, HUD_PIXEL_WIDTH, HUD_PIXEL_HEIGHT);

    // Create OpenXR session with shared texture
    if (!CreateSession(xr, renderer.device.Get(), g_sharedHandle)) {
        LOG_ERROR("Session creation failed");
        if (hudOk) CleanupHudRenderer(hudRenderer);
        CleanupD3D11(renderer);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    if (!CreateSpaces(xr)) {
        CleanupOpenXR(xr);
        if (hudOk) CleanupHudRenderer(hudRenderer);
        CleanupD3D11(renderer);
        ShutdownLogging();
        return 1;
    }

    if (!CreateSwapchain(xr)) {
        CleanupOpenXR(xr);
        if (hudOk) CleanupHudRenderer(hudRenderer);
        CleanupD3D11(renderer);
        ShutdownLogging();
        return 1;
    }

    std::vector<XrSwapchainImageD3D11KHR> swapchainImages;
    {
        uint32_t count = xr.swapchain.imageCount;
        swapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)swapchainImages.data());
    }

    if (!CreateHudSwapchain(xr, HUD_PIXEL_WIDTH, HUD_PIXEL_HEIGHT)) {
        LOG_WARN("Failed to create HUD swapchain");
    }
    std::vector<XrSwapchainImageD3D11KHR> hudSwapchainImages;
    if (xr.hasHudSwapchain) {
        uint32_t count = xr.hudSwapchain.imageCount;
        hudSwapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        xrEnumerateSwapchainImages(xr.hudSwapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)hudSwapchainImages.data());
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    ComPtr<ID3D11Texture2D> depthTexture;
    ComPtr<ID3D11DepthStencilView> depthDSV;
    {
        ID3D11Texture2D* depthTex = nullptr;
        ID3D11DepthStencilView* dsv = nullptr;
        if (!CreateDepthStencilView(renderer, xr.swapchain.width, xr.swapchain.height, &depthTex, &dsv)) {
            LOG_ERROR("Failed to create depth buffer");
            CleanupOpenXR(xr);
            if (hudOk) CleanupHudRenderer(hudRenderer);
            CleanupD3D11(renderer);
            ShutdownLogging();
            return 1;
        }
        depthTexture.Attach(depthTex);
        depthDSV.Attach(dsv);
    }

    LOG_INFO("");
    LOG_INFO("=== Entering main loop ===");
    LOG_INFO("Shared texture mode: runtime renders to shared texture, app blits to window");
    LOG_INFO("Controls: WASD=Fly, Mouse=Look, Space=Reset, V=2D/3D, TAB=HUD, F11=Fullscreen, ESC=Quit");

    PerformanceStats perfStats = {};
    perfStats.lastTime = std::chrono::high_resolution_clock::now();
    g_inputState.stereo.virtualDisplayHeight = 0.24f;
    g_inputState.nominalViewerZ = xr.nominalViewerZ;

    RenderState rs = {};
    rs.hwnd = hwnd;
    rs.xr = &xr;
    rs.renderer = &renderer;
    rs.hudRenderer = &hudRenderer;
    rs.hudOk = hudOk;
    rs.hudSwapchainImages = &hudSwapchainImages;
    rs.depthTexture = depthTexture;
    rs.depthDSV = depthDSV;
    rs.swapchainImages = &swapchainImages;
    rs.perfStats = &perfStats;
    rs.appSwapchain = appSwapchain;
    rs.appBackBufferRTV = appBackBufferRTV;
    g_renderState = &rs;

    MSG msg = {};
    while (g_running && !xr.exitRequested) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!g_running) break;
        RenderOneFrame(rs);
    }

    g_renderState = nullptr;

    LOG_INFO("=== Shutting down ===");

    depthDSV.Reset();
    depthTexture.Reset();
    g_sharedSRV.Reset();
    g_sharedTexture.Reset();
    g_blitVS.Reset();
    g_blitPS.Reset();
    g_blitSampler.Reset();
    appBackBufferRTV.Reset();
    appSwapchain.Reset();

    g_xr = nullptr;
    CleanupOpenXR(xr);
    if (hudOk) CleanupHudRenderer(hudRenderer);
    CleanupD3D11(renderer);

    DestroyWindow(hwnd);
    UnregisterClass(WINDOW_CLASS, hInstance);

    ShutdownLogging();
    return 0;
}
