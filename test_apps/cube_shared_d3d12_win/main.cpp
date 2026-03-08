// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  SR Cube OpenXR Shared Texture — D3D12 shared texture demo
 *
 * Demonstrates zero-copy shared GPU texture via XR_EXT_win32_window_binding:
 * the app creates a shared D3D12 texture (D3D12_HEAP_FLAG_SHARED), passes its
 * HANDLE to the runtime (windowHandle=NULL, sharedTextureHandle=handle). The
 * runtime renders the composited output into the shared texture. The app then
 * blits the shared texture into its own DXGI swapchain window each frame.
 *
 * Key difference from cube_ext_d3d12: no window handle is passed to the runtime.
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
#include "d3d12_renderer.h"
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

static const char* APP_NAME = "cube_shared_d3d12_win";
static const wchar_t* WINDOW_CLASS = L"SRCubeSharedD3D12Class";
static const wchar_t* WINDOW_TITLE = L"D3D12 Cube \u2014 D3D12 Native Compositor (Shared Texture)";

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

// Shared texture resources (D3D12)
static ComPtr<ID3D12Resource> g_sharedTexture;
static HANDLE g_sharedHandle = nullptr;
static uint32_t g_sharedWidth = 0;
static uint32_t g_sharedHeight = 0;

// App-side D3D11 device for window presentation (D3D11on12 pattern: simpler blit)
static ComPtr<ID3D11Device> g_d3d11Device;
static ComPtr<ID3D11DeviceContext> g_d3d11Context;
static ComPtr<IDXGISwapChain1> g_appSwapchain;
static ComPtr<ID3D11RenderTargetView> g_appBackBufferRTV;

// Blit shader resources (D3D11 for window blit)
static ComPtr<ID3D11VertexShader> g_blitVS;
static ComPtr<ID3D11PixelShader> g_blitPS;
static ComPtr<ID3D11SamplerState> g_blitSampler;

// Shared texture opened on D3D11 side for blitting
static ComPtr<ID3D11Texture2D> g_sharedTextureD3D11;
static ComPtr<ID3D11ShaderResourceView> g_sharedSRV;

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
static void BlitSharedTextureToBackBuffer(uint32_t winW, uint32_t winH) {
    if (!g_sharedSRV || !g_appBackBufferRTV) return;

    g_d3d11Context->OMSetRenderTargets(1, g_appBackBufferRTV.GetAddressOf(), nullptr);

    // Compute letterbox viewport
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
    g_d3d11Context->RSSetViewports(1, &vp);

    g_d3d11Context->VSSetShader(g_blitVS.Get(), nullptr, 0);
    g_d3d11Context->PSSetShader(g_blitPS.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srv = g_sharedSRV.Get();
    g_d3d11Context->PSSetShaderResources(0, 1, &srv);
    ID3D11SamplerState* smp = g_blitSampler.Get();
    g_d3d11Context->PSSetSamplers(0, 1, &smp);
    g_d3d11Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_d3d11Context->IASetInputLayout(nullptr);
    g_d3d11Context->Draw(3, 0);

    ID3D11ShaderResourceView* nullSRV = nullptr;
    g_d3d11Context->PSSetShaderResources(0, 1, &nullSRV);
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
    D3D12Renderer* renderer;
    HudRenderer* hudRenderer;
    bool hudOk;
    std::vector<XrSwapchainImageD3D12KHR>* hudSwapchainImages;
    std::vector<XrSwapchainImageD3D12KHR>* swapchainImages;
    PerformanceStats* perfStats;
};

static void RenderOneFrame(RenderState& rs) {
    XrSessionManager& xr = *rs.xr;
    D3D12Renderer& renderer = *rs.renderer;

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

                    // Render HUD (using D3D11 HUD renderer — writes CPU pixels into XR swapchain texture)
                    if (g_inputState.hudVisible && xr.hasHudSwapchain && rs.hudSwapchainImages && !rs.hudSwapchainImages->empty() && rs.hudOk) {
                        uint32_t hudImageIndex;
                        if (AcquireHudSwapchainImage(xr, hudImageIndex)) {
                            std::wstring sessionText(xr.systemName, xr.systemName + strlen(xr.systemName));
                            sessionText += L"\nSession: ";
                            sessionText += FormatSessionState((int)xr.sessionState);
                            std::wstring modeText = L"Shared Texture D3D12 (offscreen)";
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
                            dispText += L"\n" + FormatOutputMode(g_inputState.outputMode, xr.pfnRequestDisplayRenderingModeEXT != nullptr,
                                (xr.renderingModeCount > 0 && (uint32_t)g_inputState.outputMode < xr.renderingModeCount) ? xr.renderingModeNames[g_inputState.outputMode] : nullptr,
                                xr.renderingModeCount);
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
                            std::wstring helpText = FormatHelpText(xr.pfnRequestDisplayRenderingModeEXT != nullptr, g_inputState.cameraMode, xr.renderingModeCount);

                            uint32_t srcRowPitch = 0;
                            const void* pixels = RenderHudAndMap(*rs.hudRenderer, &srcRowPitch,
                                sessionText, modeText, perfText, dispText, eyeText,
                                cameraText, stereoText, helpText);
                            if (pixels) {
                                // HUD swapchain images are D3D12 textures — upload via D3D12
                                ID3D12Resource* hudTexture = (*rs.hudSwapchainImages)[hudImageIndex].texture;
                                // Use UpdateSubresource equivalent for D3D12: upload via staging buffer
                                // For simplicity, we'll use a mapped upload buffer
                                D3D12_RESOURCE_DESC hudDesc = hudTexture->GetDesc();
                                UINT64 uploadSize = 0;
                                D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
                                renderer.device->GetCopyableFootprints(&hudDesc, 0, 1, 0, &footprint, nullptr, nullptr, &uploadSize);

                                ComPtr<ID3D12Resource> uploadBuf;
                                D3D12_HEAP_PROPERTIES heapProps = {};
                                heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
                                D3D12_RESOURCE_DESC bufDesc = {};
                                bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                                bufDesc.Width = uploadSize;
                                bufDesc.Height = 1;
                                bufDesc.DepthOrArraySize = 1;
                                bufDesc.MipLevels = 1;
                                bufDesc.SampleDesc.Count = 1;
                                bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                                renderer.device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuf));

                                void* mapped = nullptr;
                                uploadBuf->Map(0, nullptr, &mapped);
                                const uint8_t* src = (const uint8_t*)pixels;
                                uint8_t* dst = (uint8_t*)mapped + footprint.Offset;
                                for (UINT row = 0; row < xr.hudSwapchain.height; row++) {
                                    memcpy(dst + row * footprint.Footprint.RowPitch,
                                           src + row * srcRowPitch,
                                           xr.hudSwapchain.width * 4);
                                }
                                uploadBuf->Unmap(0, nullptr);
                                UnmapHud(*rs.hudRenderer);

                                // Record copy commands
                                renderer.commandAllocator->Reset();
                                renderer.commandList->Reset(renderer.commandAllocator.Get(), nullptr);

                                D3D12_RESOURCE_BARRIER barrier = {};
                                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                                barrier.Transition.pResource = hudTexture;
                                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                                renderer.commandList->ResourceBarrier(1, &barrier);

                                D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
                                dstLoc.pResource = hudTexture;
                                dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                                dstLoc.SubresourceIndex = 0;
                                D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
                                srcLoc.pResource = uploadBuf.Get();
                                srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                                srcLoc.PlacedFootprint = footprint;
                                renderer.commandList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

                                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                                renderer.commandList->ResourceBarrier(1, &barrier);

                                renderer.commandList->Close();
                                ID3D12CommandList* lists[] = {renderer.commandList.Get()};
                                renderer.commandQueue->ExecuteCommandLists(1, lists);
                                WaitForGpu(renderer);
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

                    uint32_t imageIndex;
                    if (AcquireSwapchainImage(xr, imageIndex)) {
                        ID3D12Resource* swapchainTexture = (*rs.swapchainImages)[imageIndex].texture;

                        for (int eye = 0; eye < eyeCount; eye++) {
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

                            uint32_t vpX = monoMode ? 0 : (eye * renderW);
                            RenderScene(renderer, swapchainTexture, (int)imageIndex,
                                vpX, 0, renderW, renderH,
                                viewMatrix, projMatrix,
                                useAppProjection ? 1.0f : g_inputState.stereo.scaleFactor,
                                eye == 0);

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

                        WaitForGpu(renderer);
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
            if (frameState.shouldRender && g_appBackBufferRTV) {
                float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                g_d3d11Context->ClearRenderTargetView(g_appBackBufferRTV.Get(), clearColor);
                BlitSharedTextureToBackBuffer(g_windowWidth, g_windowHeight);
                g_appSwapchain->Present(1, 0);
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

    LOG_INFO("=== SR Cube Shared Texture D3D12 ===");
    LOG_INFO("Shared D3D12 texture (zero-copy GPU texture sharing)");

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
    if (!GetD3D12GraphicsRequirements(xr, &adapterLuid)) {
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Initialize D3D12 (for OpenXR rendering)
    D3D12Renderer renderer = {};
    if (!InitializeD3D12WithLUID(renderer, adapterLuid)) {
        LOG_ERROR("D3D12 init failed");
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create a D3D11 device on the same adapter for window presentation
    {
        ComPtr<IDXGIFactory4> dxgiFactory;
        CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory));
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; dxgiFactory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            if (desc.AdapterLuid.LowPart == adapterLuid.LowPart &&
                desc.AdapterLuid.HighPart == adapterLuid.HighPart) {
                break;
            }
            adapter.Reset();
        }
        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
        HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            0, &featureLevel, 1, D3D11_SDK_VERSION,
            &g_d3d11Device, nullptr, &g_d3d11Context);
        if (FAILED(hr)) {
            LOG_ERROR("Failed to create D3D11 device for window presentation: 0x%08x", hr);
            CleanupD3D12(renderer);
            CleanupOpenXR(xr);
            ShutdownLogging();
            return 1;
        }
    }

    // Create app-side DXGI swapchain for window presentation (D3D11)
    {
        ComPtr<IDXGIDevice> dxgiDevice;
        g_d3d11Device.As(&dxgiDevice);
        ComPtr<IDXGIAdapter> adapter;
        dxgiDevice->GetAdapter(&adapter);
        ComPtr<IDXGIFactory2> factory;
        adapter->GetParent(IID_PPV_ARGS(&factory));

        DXGI_SWAP_CHAIN_DESC1 scd = {};
        scd.Width = g_windowWidth;
        scd.Height = g_windowHeight;
        scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd.SampleDesc.Count = 1;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount = 2;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        HRESULT hr = factory->CreateSwapChainForHwnd(
            g_d3d11Device.Get(), hwnd, &scd, nullptr, nullptr, &g_appSwapchain);
        if (FAILED(hr)) {
            LOG_ERROR("Failed to create app swapchain: 0x%08x", hr);
            CleanupD3D12(renderer);
            CleanupOpenXR(xr);
            ShutdownLogging();
            return 1;
        }
    }

    // Get back buffer RTV
    {
        ComPtr<ID3D11Texture2D> backBuf;
        g_appSwapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), &backBuf);
        g_d3d11Device->CreateRenderTargetView(backBuf.Get(), nullptr, &g_appBackBufferRTV);
    }

    // Create shared D3D12 texture
    g_sharedWidth = xr.displayPixelWidth > 0 ? xr.displayPixelWidth : 1920;
    g_sharedHeight = xr.displayPixelHeight > 0 ? xr.displayPixelHeight : 1080;
    {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = g_sharedWidth;
        desc.Height = g_sharedHeight;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        HRESULT hr = renderer.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_SHARED, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&g_sharedTexture));
        if (FAILED(hr)) {
            LOG_ERROR("Failed to create shared D3D12 texture: 0x%08x", hr);
            CleanupD3D12(renderer);
            CleanupOpenXR(xr);
            ShutdownLogging();
            return 1;
        }

        // Create shared handle
        hr = renderer.device->CreateSharedHandle(g_sharedTexture.Get(), nullptr,
            GENERIC_ALL, nullptr, &g_sharedHandle);
        if (FAILED(hr)) {
            LOG_ERROR("Failed to create shared handle: 0x%08x", hr);
            CleanupD3D12(renderer);
            CleanupOpenXR(xr);
            ShutdownLogging();
            return 1;
        }

        LOG_INFO("Created shared D3D12 texture: %ux%u, handle=%p", g_sharedWidth, g_sharedHeight, g_sharedHandle);
    }

    // Open shared texture on D3D11 side for blitting to window
    {
        ComPtr<ID3D11Device1> device1;
        g_d3d11Device.As(&device1);
        HRESULT hr = device1->OpenSharedResource1(g_sharedHandle,
            IID_PPV_ARGS(&g_sharedTextureD3D11));
        if (FAILED(hr)) {
            LOG_ERROR("Failed to open shared texture on D3D11: 0x%08x", hr);
            CleanupD3D12(renderer);
            CleanupOpenXR(xr);
            ShutdownLogging();
            return 1;
        }

        hr = g_d3d11Device->CreateShaderResourceView(g_sharedTextureD3D11.Get(), nullptr, &g_sharedSRV);
        if (FAILED(hr)) {
            LOG_ERROR("Failed to create SRV for shared texture: 0x%08x", hr);
            CleanupD3D12(renderer);
            CleanupOpenXR(xr);
            ShutdownLogging();
            return 1;
        }
    }

    // Create blit shader resources (D3D11)
    if (!CreateBlitResources(g_d3d11Device.Get())) {
        LOG_ERROR("Failed to create blit resources");
        CleanupD3D12(renderer);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // HUD renderer
    HudRenderer hudRenderer = {};
    bool hudOk = InitializeHudRenderer(hudRenderer, HUD_PIXEL_WIDTH, HUD_PIXEL_HEIGHT);

    // Create OpenXR session with shared texture
    if (!CreateSession(xr, renderer.device.Get(), renderer.commandQueue.Get(), g_sharedHandle)) {
        LOG_ERROR("Session creation failed");
        if (hudOk) CleanupHudRenderer(hudRenderer);
        CleanupD3D12(renderer);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    if (!CreateSpaces(xr)) {
        CleanupOpenXR(xr);
        if (hudOk) CleanupHudRenderer(hudRenderer);
        CleanupD3D12(renderer);
        ShutdownLogging();
        return 1;
    }

    if (!CreateSwapchain(xr)) {
        CleanupOpenXR(xr);
        if (hudOk) CleanupHudRenderer(hudRenderer);
        CleanupD3D12(renderer);
        ShutdownLogging();
        return 1;
    }

    std::vector<XrSwapchainImageD3D12KHR> swapchainImages;
    {
        uint32_t count = xr.swapchain.imageCount;
        swapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)swapchainImages.data());
    }

    // Create RTVs for swapchain images
    {
        DXGI_FORMAT rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        if (swapchainImages.size() > 0 && swapchainImages[0].texture != nullptr) {
            D3D12_RESOURCE_DESC desc = swapchainImages[0].texture->GetDesc();
            rtvFormat = desc.Format;
            if (rtvFormat == DXGI_FORMAT_R8G8B8A8_TYPELESS) rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            if (rtvFormat == DXGI_FORMAT_B8G8R8A8_TYPELESS) rtvFormat = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        }
        std::vector<ID3D12Resource*> textures(swapchainImages.size());
        for (size_t i = 0; i < swapchainImages.size(); i++) {
            textures[i] = swapchainImages[i].texture;
        }
        CreateSwapchainRTVs(renderer, textures.data(), (uint32_t)textures.size(),
                            xr.swapchain.width, xr.swapchain.height, rtvFormat);
    }

    if (!CreateHudSwapchain(xr, HUD_PIXEL_WIDTH, HUD_PIXEL_HEIGHT)) {
        LOG_WARN("Failed to create HUD swapchain");
    }
    std::vector<XrSwapchainImageD3D12KHR> hudSwapchainImages;
    if (xr.hasHudSwapchain) {
        uint32_t count = xr.hudSwapchain.imageCount;
        hudSwapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        xrEnumerateSwapchainImages(xr.hudSwapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)hudSwapchainImages.data());
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    LOG_INFO("");
    LOG_INFO("=== Entering main loop ===");
    LOG_INFO("Shared texture mode: runtime renders to shared D3D12 texture, app blits to window");
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
    rs.swapchainImages = &swapchainImages;
    rs.perfStats = &perfStats;
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

    WaitForGpu(renderer);
    g_sharedSRV.Reset();
    g_sharedTextureD3D11.Reset();
    g_sharedTexture.Reset();
    g_blitVS.Reset();
    g_blitPS.Reset();
    g_blitSampler.Reset();
    g_appBackBufferRTV.Reset();
    g_appSwapchain.Reset();
    g_d3d11Context.Reset();
    g_d3d11Device.Reset();

    if (g_sharedHandle) {
        CloseHandle(g_sharedHandle);
        g_sharedHandle = nullptr;
    }

    g_xr = nullptr;
    CleanupOpenXR(xr);
    if (hudOk) CleanupHudRenderer(hudRenderer);
    CleanupD3D12(renderer);

    DestroyWindow(hwnd);
    UnregisterClass(WINDOW_CLASS, hInstance);

    ShutdownLogging();
    return 0;
}
