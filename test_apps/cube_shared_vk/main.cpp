// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  SR Cube OpenXR Shared Texture — Vulkan rendering + D3D11 shared texture
 *
 * Demonstrates zero-copy shared GPU texture via XR_EXT_win32_window_binding:
 * the app creates a shared D3D11 texture (MISC_SHARED), passes its HANDLE
 * to the runtime (windowHandle=NULL, sharedTextureHandle=handle). The runtime's
 * VK native compositor imports the D3D11 texture via VK_KHR_external_memory_win32,
 * renders the composited stereo output into it. The app then blits the shared
 * D3D11 texture into its own DXGI swapchain each frame.
 *
 * Key: Vulkan for OpenXR rendering, D3D11 for shared texture creation + window blit.
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <wrl/client.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>

#include "logging.h"
#include "input_handler.h"
#include "xr_session.h"
#include "vk_renderer.h"
#include "display3d_view.h"
#include "camera3d_view.h"
#include "hud_renderer.h"
#include "text_overlay.h"

#include <chrono>
#include <cmath>
#include <string>
#include <sstream>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

static const char* APP_NAME = "cube_shared_vk";
static const wchar_t* WINDOW_CLASS = L"SRCubeSharedVKClass";
static const wchar_t* WINDOW_TITLE = L"Vulkan Cube \u2014 VK Native Compositor (Shared Texture)";

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

// D3D11 resources for shared texture + window blit
static ComPtr<ID3D11Device> g_d3d11Device;
static ComPtr<ID3D11DeviceContext> g_d3d11Context;
static ComPtr<IDXGIFactory2> g_dxgiFactory;
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
static void BlitSharedTextureToBackBuffer(ID3D11DeviceContext* context, ID3D11RenderTargetView* backBufferRTV,
                                           uint32_t winW, uint32_t winH) {
    if (!g_sharedSRV) return;

    context->OMSetRenderTargets(1, &backBufferRTV, nullptr);

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
    context->RSSetViewports(1, &vp);

    context->VSSetShader(g_blitVS.Get(), nullptr, 0);
    context->PSSetShader(g_blitPS.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srv = g_sharedSRV.Get();
    context->PSSetShaderResources(0, 1, &srv);
    ID3D11SamplerState* smp = g_blitSampler.Get();
    context->PSSetSamplers(0, 1, &smp);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetInputLayout(nullptr);
    context->Draw(3, 0);

    ID3D11ShaderResourceView* nullSRV = nullptr;
    context->PSSetShaderResources(0, 1, &nullSRV);
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
    VkRenderer* vkRenderer;
    HudRenderer* hudRenderer;
    bool hudOk;
    std::vector<XrSwapchainImageVulkanKHR>* swapchainImages;
    std::vector<XrSwapchainImageVulkanKHR>* hudSwapchainImages;
    PerformanceStats* perfStats;
    // App-side D3D11 swapchain for window presentation
    ComPtr<IDXGISwapChain1> appSwapchain;
    ComPtr<ID3D11RenderTargetView> appBackBufferRTV;
};

static void RenderOneFrame(RenderState& rs) {
    XrSessionManager& xr = *rs.xr;
    VkRenderer& vkr = *rs.vkRenderer;

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

    UpdateScene(vkr, rs.perfStats->deltaTime);
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
                            std::wstring modeText = L"Shared Texture VK (offscreen)";
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

                            // HUD rendering uses D3D11-backed CPU-side rendering via HudRenderer
                            // We need to upload the pixels to the Vulkan HUD swapchain image
                            // For now, render the HUD and upload via staging
                            uint32_t srcRowPitch = 0;
                            const void* pixels = RenderHudAndMap(*rs.hudRenderer, &srcRowPitch,
                                sessionText, modeText, perfText, dispText, eyeText,
                                cameraText, stereoText, helpText);
                            if (pixels) {
                                // Upload to Vulkan HUD swapchain image via staging buffer
                                VkImage hudImage = (*rs.hudSwapchainImages)[hudImageIndex].image;
                                uint32_t hudW = xr.hudSwapchain.width;
                                uint32_t hudH = xr.hudSwapchain.height;

                                // Create staging buffer
                                VkBufferCreateInfo bufCI = {};
                                bufCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                                bufCI.size = hudW * hudH * 4;
                                bufCI.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
                                bufCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

                                VkBuffer stagingBuf = VK_NULL_HANDLE;
                                vkCreateBuffer(vkr.device, &bufCI, nullptr, &stagingBuf);

                                VkMemoryRequirements memReq;
                                vkGetBufferMemoryRequirements(vkr.device, stagingBuf, &memReq);

                                VkPhysicalDeviceMemoryProperties memProps;
                                vkGetPhysicalDeviceMemoryProperties(vkr.physicalDevice, &memProps);
                                uint32_t memIdx = UINT32_MAX;
                                for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
                                    if ((memReq.memoryTypeBits & (1u << i)) &&
                                        (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                                        memIdx = i;
                                        break;
                                    }
                                }

                                VkDeviceMemory stagingMem = VK_NULL_HANDLE;
                                if (memIdx != UINT32_MAX) {
                                    VkMemoryAllocateInfo allocI = {};
                                    allocI.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                                    allocI.allocationSize = memReq.size;
                                    allocI.memoryTypeIndex = memIdx;
                                    vkAllocateMemory(vkr.device, &allocI, nullptr, &stagingMem);
                                    vkBindBufferMemory(vkr.device, stagingBuf, stagingMem, 0);

                                    void* mapped = nullptr;
                                    vkMapMemory(vkr.device, stagingMem, 0, bufCI.size, 0, &mapped);
                                    // Copy row by row (srcRowPitch may differ from hudW*4)
                                    for (uint32_t row = 0; row < hudH; row++) {
                                        memcpy((uint8_t*)mapped + row * hudW * 4,
                                               (const uint8_t*)pixels + row * srcRowPitch,
                                               hudW * 4);
                                    }
                                    vkUnmapMemory(vkr.device, stagingMem);

                                    // Copy staging buffer to HUD image
                                    VkCommandBufferAllocateInfo cmdAI = {};
                                    cmdAI.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                                    cmdAI.commandPool = vkr.commandPool;
                                    cmdAI.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                                    cmdAI.commandBufferCount = 1;
                                    VkCommandBuffer cmd;
                                    vkAllocateCommandBuffers(vkr.device, &cmdAI, &cmd);

                                    VkCommandBufferBeginInfo beginI = {};
                                    beginI.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                                    beginI.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                                    vkBeginCommandBuffer(cmd, &beginI);

                                    // Transition HUD image to TRANSFER_DST
                                    VkImageMemoryBarrier barrier = {};
                                    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                                    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                                    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                                    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                                    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                                    barrier.image = hudImage;
                                    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                                    barrier.srcAccessMask = 0;
                                    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                                    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

                                    VkBufferImageCopy region = {};
                                    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                                    region.imageExtent = {hudW, hudH, 1};
                                    vkCmdCopyBufferToImage(cmd, stagingBuf, hudImage,
                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

                                    // Transition to COLOR_ATTACHMENT_OPTIMAL for compositor
                                    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                                    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                                    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                                    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
                                    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

                                    vkEndCommandBuffer(cmd);

                                    VkSubmitInfo submitI = {};
                                    submitI.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                                    submitI.commandBufferCount = 1;
                                    submitI.pCommandBuffers = &cmd;
                                    vkQueueSubmit(vkr.graphicsQueue, 1, &submitI, VK_NULL_HANDLE);
                                    vkQueueWaitIdle(vkr.graphicsQueue);

                                    vkFreeCommandBuffers(vkr.device, vkr.commandPool, 1, &cmd);
                                    vkFreeMemory(vkr.device, stagingMem, nullptr);
                                }
                                vkDestroyBuffer(vkr.device, stagingBuf, nullptr);

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

                        // Set up per-eye render parameters
                        EyeRenderParams eyes[2];
                        for (int eye = 0; eye < eyeCount; eye++) {
                            eyes[eye].viewportX = monoMode ? 0 : (eye * renderW);
                            eyes[eye].viewportY = 0;
                            eyes[eye].width = renderW;
                            eyes[eye].height = renderH;

                            if (useAppProjection) {
                                int vi = monoMode ? 0 : eye;
                                eyes[eye].viewMatrix = ColumnMajorToXMMatrix(stereoViews[vi].view_matrix);
                                eyes[eye].projMatrix = ColumnMajorToXMMatrix(stereoViews[vi].projection_matrix);
                            } else if (monoMode) {
                                eyes[eye].viewMatrix = monoViewMatrix;
                                eyes[eye].projMatrix = monoProjMatrix;
                            } else {
                                eyes[eye].viewMatrix = (eye == 0) ? leftViewMatrix : rightViewMatrix;
                                eyes[eye].projMatrix = (eye == 0) ? leftProjMatrix : rightProjMatrix;
                            }

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

                        // Render all eyes in a single pass
                        uint32_t fbWidth = monoMode ? renderW : (renderW * 2);
                        uint32_t fbHeight = renderH;
                        RenderScene(vkr, imageIndex, fbWidth, fbHeight,
                            eyes, eyeCount,
                            useAppProjection ? 1.0f : g_inputState.stereo.scaleFactor);

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

            // After xrEndFrame: blit shared D3D11 texture to app window
            if (frameState.shouldRender && rs.appBackBufferRTV) {
                float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                g_d3d11Context->ClearRenderTargetView(rs.appBackBufferRTV.Get(), clearColor);
                BlitSharedTextureToBackBuffer(g_d3d11Context.Get(), rs.appBackBufferRTV.Get(),
                                               g_windowWidth, g_windowHeight);
                rs.appSwapchain->Present(1, 0);
            }
        }
    } else {
        Sleep(100);
    }
}

static bool InitializeD3D11ForSharedTexture() {
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        featureLevels, _countof(featureLevels), D3D11_SDK_VERSION,
        &g_d3d11Device, nullptr, &g_d3d11Context);
    if (FAILED(hr)) {
        LOG_ERROR("D3D11CreateDevice failed: 0x%08x", hr);
        return false;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    g_d3d11Device->QueryInterface(__uuidof(IDXGIDevice), &dxgiDevice);
    ComPtr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(&adapter);
    hr = adapter->GetParent(__uuidof(IDXGIFactory2), &g_dxgiFactory);
    if (FAILED(hr)) {
        LOG_ERROR("Failed to get IDXGIFactory2: 0x%08x", hr);
        return false;
    }

    LOG_INFO("D3D11 device created for shared texture management");
    return true;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    if (!InitializeLogging(APP_NAME)) {
        MessageBox(nullptr, L"Failed to initialize logging", L"Warning", MB_OK | MB_ICONWARNING);
    }

    LOG_INFO("=== SR Cube Shared Texture VK ===");
    LOG_INFO("Vulkan rendering + D3D11 shared texture (zero-copy GPU texture sharing)");

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

    // Initialize D3D11 for shared texture creation + window blit
    if (!InitializeD3D11ForSharedTexture()) {
        ShutdownLogging();
        return 1;
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

    if (!GetVulkanGraphicsRequirements(xr)) {
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create Vulkan instance and device
    VkInstance vkInstance = VK_NULL_HANDLE;
    if (!CreateVulkanInstance(xr, vkInstance)) {
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    VkPhysicalDevice vkPhysDevice = VK_NULL_HANDLE;
    if (!GetVulkanPhysicalDevice(xr, vkInstance, vkPhysDevice)) {
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    std::vector<const char*> deviceExtensions;
    std::vector<std::string> extensionStorage;
    if (!GetVulkanDeviceExtensions(xr, vkInstance, vkPhysDevice, deviceExtensions, extensionStorage)) {
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    uint32_t queueFamilyIndex;
    if (!FindGraphicsQueueFamily(vkPhysDevice, queueFamilyIndex)) {
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    VkDevice vkDevice = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    if (!CreateVulkanDevice(vkPhysDevice, queueFamilyIndex, deviceExtensions, vkDevice, graphicsQueue)) {
        vkDestroyInstance(vkInstance, nullptr);
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
        HRESULT hr = g_dxgiFactory->CreateSwapChainForHwnd(
            g_d3d11Device.Get(), hwnd, &scd, nullptr, nullptr, &appSwapchain);
        if (FAILED(hr)) {
            LOG_ERROR("Failed to create app swapchain: 0x%08x", hr);
            vkDestroyDevice(vkDevice, nullptr);
            vkDestroyInstance(vkInstance, nullptr);
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
        g_d3d11Device->CreateRenderTargetView(backBuf.Get(), nullptr, &appBackBufferRTV);
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

        HRESULT hr = g_d3d11Device->CreateTexture2D(&desc, nullptr, &g_sharedTexture);
        if (FAILED(hr)) {
            LOG_ERROR("Failed to create shared texture: 0x%08x", hr);
            vkDestroyDevice(vkDevice, nullptr);
            vkDestroyInstance(vkInstance, nullptr);
            CleanupOpenXR(xr);
            ShutdownLogging();
            return 1;
        }

        ComPtr<IDXGIResource> dxgiResource;
        g_sharedTexture->QueryInterface(__uuidof(IDXGIResource), &dxgiResource);
        dxgiResource->GetSharedHandle(&g_sharedHandle);

        hr = g_d3d11Device->CreateShaderResourceView(g_sharedTexture.Get(), nullptr, &g_sharedSRV);
        if (FAILED(hr)) {
            LOG_ERROR("Failed to create SRV for shared texture: 0x%08x", hr);
            vkDestroyDevice(vkDevice, nullptr);
            vkDestroyInstance(vkInstance, nullptr);
            CleanupOpenXR(xr);
            ShutdownLogging();
            return 1;
        }

        LOG_INFO("Created shared D3D11 texture: %ux%u, handle=%p", g_sharedWidth, g_sharedHeight, g_sharedHandle);
    }

    // Create blit shader resources
    if (!CreateBlitResources(g_d3d11Device.Get())) {
        LOG_ERROR("Failed to create blit resources");
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // HUD renderer
    HudRenderer hudRenderer = {};
    bool hudOk = InitializeHudRenderer(hudRenderer, HUD_PIXEL_WIDTH, HUD_PIXEL_HEIGHT);

    // Create OpenXR session with Vulkan binding + shared texture
    if (!CreateSession(xr, vkInstance, vkPhysDevice, vkDevice, queueFamilyIndex, 0, g_sharedHandle)) {
        LOG_ERROR("Session creation failed");
        if (hudOk) CleanupHudRenderer(hudRenderer);
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    if (!CreateSpaces(xr)) {
        CleanupOpenXR(xr);
        if (hudOk) CleanupHudRenderer(hudRenderer);
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        ShutdownLogging();
        return 1;
    }

    if (!CreateSwapchain(xr)) {
        CleanupOpenXR(xr);
        if (hudOk) CleanupHudRenderer(hudRenderer);
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        ShutdownLogging();
        return 1;
    }

    std::vector<XrSwapchainImageVulkanKHR> swapchainImages;
    {
        uint32_t count = xr.swapchain.imageCount;
        swapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)swapchainImages.data());
    }

    if (!CreateHudSwapchain(xr, HUD_PIXEL_WIDTH, HUD_PIXEL_HEIGHT)) {
        LOG_WARN("Failed to create HUD swapchain");
    }
    std::vector<XrSwapchainImageVulkanKHR> hudSwapchainImages;
    if (xr.hasHudSwapchain) {
        uint32_t count = xr.hudSwapchain.imageCount;
        hudSwapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        xrEnumerateSwapchainImages(xr.hudSwapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)hudSwapchainImages.data());
    }

    // Initialize Vulkan renderer
    VkRenderer vkRenderer = {};
    VkFormat colorFormat = (VkFormat)xr.swapchain.format;
    if (!InitializeVkRenderer(vkRenderer, vkDevice, vkPhysDevice, graphicsQueue, queueFamilyIndex, colorFormat)) {
        LOG_ERROR("Failed to initialize Vulkan renderer");
        CleanupOpenXR(xr);
        if (hudOk) CleanupHudRenderer(hudRenderer);
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        ShutdownLogging();
        return 1;
    }

    // Create framebuffers for swapchain images (both eyes share same images with viewport offsets)
    {
        std::vector<VkImage> images(swapchainImages.size());
        for (size_t i = 0; i < swapchainImages.size(); i++) {
            images[i] = swapchainImages[i].image;
        }
        uint32_t fbWidth = xr.swapchain.width;
        uint32_t fbHeight = xr.swapchain.height;
        // Single set of framebuffers (eye 0) — both eyes render with viewport offsets
        if (!CreateSwapchainFramebuffers(vkRenderer, 0, images.data(), (uint32_t)images.size(),
                                          fbWidth, fbHeight, colorFormat)) {
            LOG_ERROR("Failed to create swapchain framebuffers");
            CleanupVkRenderer(vkRenderer);
            CleanupOpenXR(xr);
            if (hudOk) CleanupHudRenderer(hudRenderer);
            vkDestroyDevice(vkDevice, nullptr);
            vkDestroyInstance(vkInstance, nullptr);
            ShutdownLogging();
            return 1;
        }
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    LOG_INFO("");
    LOG_INFO("=== Entering main loop ===");
    LOG_INFO("Shared texture mode: VK native compositor renders to D3D11 shared texture, app blits to window");
    LOG_INFO("Controls: WASD=Fly, Mouse=Look, Space=Reset, V=2D/3D, TAB=HUD, F11=Fullscreen, ESC=Quit");

    PerformanceStats perfStats = {};
    perfStats.lastTime = std::chrono::high_resolution_clock::now();
    g_inputState.stereo.virtualDisplayHeight = 0.24f;
    g_inputState.nominalViewerZ = xr.nominalViewerZ;

    RenderState rs = {};
    rs.hwnd = hwnd;
    rs.xr = &xr;
    rs.vkRenderer = &vkRenderer;
    rs.hudRenderer = &hudRenderer;
    rs.hudOk = hudOk;
    rs.swapchainImages = &swapchainImages;
    rs.hudSwapchainImages = &hudSwapchainImages;
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

    // Wait for Vulkan to finish
    vkDeviceWaitIdle(vkDevice);

    CleanupVkRenderer(vkRenderer);

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

    vkDestroyDevice(vkDevice, nullptr);
    vkDestroyInstance(vkInstance, nullptr);

    g_d3d11Context.Reset();
    g_d3d11Device.Reset();
    g_dxgiFactory.Reset();

    DestroyWindow(hwnd);
    UnregisterClass(WINDOW_CLASS, hInstance);

    ShutdownLogging();
    return 0;
}
