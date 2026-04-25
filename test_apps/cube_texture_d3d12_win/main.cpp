// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  SR Cube OpenXR Shared Texture — D3D12 shared texture demo
 *
 * Demonstrates zero-copy shared GPU texture via XR_EXT_win32_window_binding:
 * the app creates a shared D3D12 texture (D3D12_HEAP_FLAG_SHARED), passes its
 * NT HANDLE to the runtime (windowHandle=appHwnd, sharedTextureHandle=handle).
 * The runtime renders the composited output into the shared texture. The app
 * then blits the shared texture into its own window each frame.
 *
 * Key difference from cube_handle_d3d12: the compositor renders into a shared
 * texture instead of presenting to the app window directly.
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <wrl/client.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>

#include "logging.h"
#include "input_handler.h"
#include "d3d12_renderer.h"
#include "hud_renderer.h"
#include "text_overlay.h"
#include "xr_session.h"
#include "display3d_view.h"
#include "camera3d_view.h"

#include <chrono>
#include <cmath>
#include <string>
#include <sstream>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

static const char* APP_NAME = "cube_texture_d3d12_win";
static const wchar_t* WINDOW_CLASS = L"SRCubeSharedD3D12Class";
static const wchar_t* WINDOW_TITLE = L"D3D12 Cube \u2014 D3D12 Native Compositor (Shared Texture)";

// Global state
static InputState g_inputState;
static bool g_running = true;
static XrSessionManager* g_xr = nullptr;
static UINT g_windowWidth = 1280;
static UINT g_windowHeight = 720;
static bool g_inSizeMove = false;
static bool g_resizeNeeded = false;
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

// Canvas dimensions — the sub-rect of the window where 3D content is displayed.
static uint32_t g_canvasW = 0;
static uint32_t g_canvasH = 0;

// App-side DXGI swapchain for window presentation
static const UINT APP_BACK_BUFFER_COUNT = 2;
static ComPtr<IDXGISwapChain3> g_appSwapchain;
static ComPtr<ID3D12Resource> g_appBackBuffers[APP_BACK_BUFFER_COUNT];
static ComPtr<ID3D12DescriptorHeap> g_appRtvHeap;
static UINT g_appRtvDescriptorSize = 0;

// Blit pipeline resources
static ComPtr<ID3D12RootSignature> g_blitRootSig;
static ComPtr<ID3D12PipelineState> g_blitPSO;
static ComPtr<ID3D12DescriptorHeap> g_blitSrvHeap;

// Blit command resources (separate from scene rendering)
static ComPtr<ID3D12CommandAllocator> g_blitCmdAllocator;
static ComPtr<ID3D12GraphicsCommandList> g_blitCmdList;
static ComPtr<ID3D12Fence> g_blitFence;
static UINT64 g_blitFenceValue = 0;
static HANDLE g_blitFenceEvent = nullptr;

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
            g_resizeNeeded = true;
            if (g_windowWidth > 0 && g_windowHeight > 0) {
                g_canvasW = g_windowWidth / 2;
                g_canvasH = g_windowHeight / 2;
                if (g_xr && g_xr->pfnSetSharedTextureOutputRectEXT && g_xr->session != XR_NULL_HANDLE) {
                    g_xr->pfnSetSharedTextureOutputRectEXT(g_xr->session,
                        (int32_t)(g_windowWidth / 4), (int32_t)(g_windowHeight / 4),
                        g_canvasW, g_canvasH);
                }
            }
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
    case WM_SYSKEYDOWN:
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

// ---- D3D12 Blit Pipeline ----

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
cbuffer BlitParams : register(b0) { float2 uvScale; float2 uvOffset; };
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return tex.Sample(smp, uvOffset + uv * uvScale);
}
)";

static bool CreateBlitPipeline(ID3D12Device* device) {
    // Root signature: 1 descriptor table (SRV t0), 1 root constant (float2 uvScale)
    // Static sampler for s0
    D3D12_STATIC_SAMPLER_DESC staticSampler = {};
    staticSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    staticSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSampler.ShaderRegister = 0;
    staticSampler.RegisterSpace = 0;
    staticSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[2] = {};
    // Param 0: SRV descriptor table
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &srvRange;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    // Param 1: root constants (float2 uvScale + float2 uvOffset = 4 floats)
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 0;
    params[1].Constants.RegisterSpace = 0;
    params[1].Constants.Num32BitValues = 4;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 2;
    rsDesc.pParameters = params;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &staticSampler;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> serialized, error;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized, &error);
    if (FAILED(hr)) {
        LOG_ERROR("Blit root signature serialize failed: %s",
            error ? (char*)error->GetBufferPointer() : "unknown");
        return false;
    }
    hr = device->CreateRootSignature(0, serialized->GetBufferPointer(),
        serialized->GetBufferSize(), IID_PPV_ARGS(&g_blitRootSig));
    if (FAILED(hr)) return false;

    // Compile shaders
    ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
    hr = D3DCompile(g_blitVSSource, strlen(g_blitVSSource), "blitVS", nullptr, nullptr,
        "main", "vs_5_0", 0, 0, &vsBlob, &errBlob);
    if (FAILED(hr)) {
        LOG_ERROR("Blit VS compile failed: %s", errBlob ? (char*)errBlob->GetBufferPointer() : "unknown");
        return false;
    }
    hr = D3DCompile(g_blitPSSource, strlen(g_blitPSSource), "blitPS", nullptr, nullptr,
        "main", "ps_5_0", 0, 0, &psBlob, &errBlob);
    if (FAILED(hr)) {
        LOG_ERROR("Blit PS compile failed: %s", errBlob ? (char*)errBlob->GetBufferPointer() : "unknown");
        return false;
    }

    // PSO
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = g_blitRootSig.Get();
    psoDesc.VS = {vsBlob->GetBufferPointer(), vsBlob->GetBufferSize()};
    psoDesc.PS = {psBlob->GetBufferPointer(), psBlob->GetBufferSize()};
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;

    hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_blitPSO));
    if (FAILED(hr)) {
        LOG_ERROR("Blit PSO creation failed: 0x%08X", hr);
        return false;
    }

    // SRV heap for shared texture
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = 1;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&g_blitSrvHeap));
    if (FAILED(hr)) return false;

    return true;
}

static void CreateSharedTextureSRV(ID3D12Device* device) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(g_sharedTexture.Get(), &srvDesc,
        g_blitSrvHeap->GetCPUDescriptorHandleForHeapStart());
}

// Blit shared texture to back buffer with canvas letterboxing
static void BlitSharedTextureToBackBuffer(D3D12Renderer& renderer, XrSessionManager& xr) {
    if (!g_sharedTexture || !g_appSwapchain) return;

    UINT bbIndex = g_appSwapchain->GetCurrentBackBufferIndex();
    ID3D12Resource* backBuffer = g_appBackBuffers[bbIndex].Get();

    g_blitCmdAllocator->Reset();
    g_blitCmdList->Reset(g_blitCmdAllocator.Get(), g_blitPSO.Get());

    // Barriers: shared texture COMMON→SRV, back buffer PRESENT→RENDER_TARGET
    D3D12_RESOURCE_BARRIER barriers[2] = {};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource = g_sharedTexture.Get();
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition.pResource = backBuffer;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    g_blitCmdList->ResourceBarrier(2, barriers);

    // Clear back buffer to black
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_appRtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += bbIndex * g_appRtvDescriptorSize;
    float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    g_blitCmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    // Set render target
    g_blitCmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    // Canvas = center 50% of window
    D3D12_VIEWPORT vp = {};
    vp.Width = (FLOAT)g_windowWidth * 0.5f;
    vp.Height = (FLOAT)g_windowHeight * 0.5f;
    vp.TopLeftX = (FLOAT)g_windowWidth * 0.25f;
    vp.TopLeftY = (FLOAT)g_windowHeight * 0.25f;
    vp.MaxDepth = 1.0f;
    g_blitCmdList->RSSetViewports(1, &vp);

    D3D12_RECT scissor = {0, 0, (LONG)g_windowWidth, (LONG)g_windowHeight};
    g_blitCmdList->RSSetScissorRects(1, &scissor);

    // Tell runtime where canvas is in the window (weaver alignment)
    if (xr.pfnSetSharedTextureOutputRectEXT && xr.session != XR_NULL_HANDLE) {
        xr.pfnSetSharedTextureOutputRectEXT(xr.session,
            (int32_t)vp.TopLeftX, (int32_t)vp.TopLeftY,
            (uint32_t)vp.Width, (uint32_t)vp.Height);
    }

    // Set blit pipeline
    g_blitCmdList->SetGraphicsRootSignature(g_blitRootSig.Get());
    ID3D12DescriptorHeap* heaps[] = {g_blitSrvHeap.Get()};
    g_blitCmdList->SetDescriptorHeaps(1, heaps);
    g_blitCmdList->SetGraphicsRootDescriptorTable(0, g_blitSrvHeap->GetGPUDescriptorHandleForHeapStart());

    // UV scale + offset: content is at (canvasX, canvasY) in the shared texture
    // per ADR-010 — compositor writes the weaved canvas region at that offset.
    // Sample at uvOffset + uv * uvScale.
    float uvParams[4] = {
        g_sharedWidth > 0 ? (float)g_canvasW / (float)g_sharedWidth : 1.0f,
        g_sharedHeight > 0 ? (float)g_canvasH / (float)g_sharedHeight : 1.0f,
        g_sharedWidth > 0 ? vp.TopLeftX / (float)g_sharedWidth : 0.0f,
        g_sharedHeight > 0 ? vp.TopLeftY / (float)g_sharedHeight : 0.0f,
    };
    g_blitCmdList->SetGraphicsRoot32BitConstants(1, 4, uvParams, 0);

    g_blitCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_blitCmdList->DrawInstanced(3, 1, 0, 0);

    // Barriers: shared texture SRV→COMMON, back buffer RENDER_TARGET→PRESENT
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_blitCmdList->ResourceBarrier(2, barriers);

    g_blitCmdList->Close();

    ID3D12CommandList* lists[] = {g_blitCmdList.Get()};
    renderer.commandQueue->ExecuteCommandLists(1, lists);

    g_appSwapchain->Present(1, 0);

    // Fence sync
    g_blitFenceValue++;
    renderer.commandQueue->Signal(g_blitFence.Get(), g_blitFenceValue);
    if (g_blitFence->GetCompletedValue() < g_blitFenceValue) {
        g_blitFence->SetEventOnCompletion(g_blitFenceValue, g_blitFenceEvent);
        WaitForSingleObject(g_blitFenceEvent, INFINITE);
    }
}

// ---- App Swapchain Management ----

static bool CreateAppSwapchainRTVs(ID3D12Device* device) {
    for (UINT i = 0; i < APP_BACK_BUFFER_COUNT; i++) {
        HRESULT hr = g_appSwapchain->GetBuffer(i, IID_PPV_ARGS(&g_appBackBuffers[i]));
        if (FAILED(hr)) return false;

        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_appRtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtvHandle.ptr += i * g_appRtvDescriptorSize;
        device->CreateRenderTargetView(g_appBackBuffers[i].Get(), nullptr, rtvHandle);
    }
    return true;
}

static void ReleaseAppSwapchainRTVs() {
    for (UINT i = 0; i < APP_BACK_BUFFER_COUNT; i++) {
        g_appBackBuffers[i].Reset();
    }
}

static void ResizeAppSwapchain(D3D12Renderer& renderer) {
    if (!g_appSwapchain) return;

    // Wait for GPU idle before releasing back buffers
    g_blitFenceValue++;
    renderer.commandQueue->Signal(g_blitFence.Get(), g_blitFenceValue);
    if (g_blitFence->GetCompletedValue() < g_blitFenceValue) {
        g_blitFence->SetEventOnCompletion(g_blitFenceValue, g_blitFenceEvent);
        WaitForSingleObject(g_blitFenceEvent, INFINITE);
    }

    ReleaseAppSwapchainRTVs();

    HRESULT hr = g_appSwapchain->ResizeBuffers(0, g_windowWidth, g_windowHeight,
        DXGI_FORMAT_UNKNOWN, 0);
    if (SUCCEEDED(hr)) {
        CreateAppSwapchainRTVs(renderer.device.Get());
    } else {
        LOG_ERROR("App swapchain resize failed: 0x%08X", hr);
    }
}

// ---- Performance Stats ----

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

// ---- Render State & Frame Loop ----

struct RenderState {
    HWND hwnd;
    XrSessionManager* xr;
    D3D12Renderer* renderer;
    HudRenderer* hudRenderer;
    bool hudOk;
    std::vector<XrSwapchainImageD3D12KHR>* swapchainImages;
    int rtvBaseIndex;
    std::vector<XrSwapchainImageD3D12KHR>* hudSwapchainImages;
    ID3D12Resource* hudUploadBuffer;
    uint8_t* hudUploadMapped;
    uint32_t hudUploadRowPitch;
    ID3D12CommandAllocator* hudCmdAllocator;
    ID3D12GraphicsCommandList* hudCmdList;
    ID3D12Fence* hudFence;
    HANDLE hudFenceEvent;
    UINT64 hudFenceValue;
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
    if (g_inputState.renderingModeChangeRequested) {
        g_inputState.renderingModeChangeRequested = false;
        if (xr.pfnRequestDisplayRenderingModeEXT && xr.session != XR_NULL_HANDLE) {
            xr.pfnRequestDisplayRenderingModeEXT(xr.session, g_inputState.currentRenderingMode);
        }
    }
    if (g_inputState.eyeTrackingModeToggleRequested) {
        g_inputState.eyeTrackingModeToggleRequested = false;
        if (xr.pfnRequestEyeTrackingModeEXT && xr.session != XR_NULL_HANDLE) {
            XrEyeTrackingModeEXT newMode = (xr.activeEyeTrackingMode == XR_EYE_TRACKING_MODE_MANAGED_EXT)
                ? XR_EYE_TRACKING_MODE_MANUAL_EXT : XR_EYE_TRACKING_MODE_MANAGED_EXT;
            xr.pfnRequestEyeTrackingModeEXT(xr.session, newMode);
        }
    }
    UpdateScene(renderer, rs.perfStats->deltaTime);
    PollEvents(xr);

    // Canvas = center 50% of window (matches blit viewport)
    if (g_windowWidth > 0 && g_windowHeight > 0) {
        g_canvasW = g_windowWidth / 2;
        g_canvasH = g_windowHeight / 2;
    }

    if (xr.sessionRunning) {
        XrFrameState frameState;
        if (BeginFrame(xr, frameState)) {
            uint32_t modeViewCount = (xr.renderingModeCount > 0 && g_inputState.currentRenderingMode < xr.renderingModeCount)
                ? xr.renderingModeViewCounts[g_inputState.currentRenderingMode] : 2;
            uint32_t tileColumns = (xr.renderingModeCount > 0 && g_inputState.currentRenderingMode < xr.renderingModeCount)
                ? xr.renderingModeTileColumns[g_inputState.currentRenderingMode] : 2;
            uint32_t tileRows = (xr.renderingModeCount > 0 && g_inputState.currentRenderingMode < xr.renderingModeCount)
                ? xr.renderingModeTileRows[g_inputState.currentRenderingMode] : 1;
            bool monoMode = (xr.renderingModeCount > 0 && !xr.renderingModeDisplay3D[g_inputState.currentRenderingMode]);
            if (xr.renderingModeCount > 0 && g_inputState.currentRenderingMode < xr.renderingModeCount) {
                xr.recommendedViewScaleX = xr.renderingModeScaleX[g_inputState.currentRenderingMode];
                xr.recommendedViewScaleY = xr.renderingModeScaleY[g_inputState.currentRenderingMode];
            }
            int eyeCount = monoMode ? 1 : (int)modeViewCount;
            std::vector<XrCompositionLayerProjectionView> projectionViews(eyeCount, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
            bool hudSubmitted = false;

            if (frameState.shouldRender) {
                if (LocateViews(xr, frameState.predictedDisplayTime,
                    g_inputState.cameraPosX, g_inputState.cameraPosY, g_inputState.cameraPosZ,
                    g_inputState.yaw, g_inputState.pitch,
                    g_inputState.viewParams)) {

                    XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
                    locateInfo.viewConfigurationType = xr.viewConfigType;
                    locateInfo.displayTime = frameState.predictedDisplayTime;
                    locateInfo.space = xr.localSpace;

                    XrViewState viewState = {XR_TYPE_VIEW_STATE};
                    uint32_t viewCount = 0;
                    XrView rawViews[8] = {};
                    for (int i = 0; i < 8; i++) rawViews[i].type = XR_TYPE_VIEW;
                    xrLocateViews(xr.session, &locateInfo, &viewState, 8, &viewCount, rawViews);

                    uint32_t maxTileW = tileColumns > 0 ? xr.swapchain.width / tileColumns : xr.swapchain.width;
                    uint32_t maxTileH = tileRows > 0 ? xr.swapchain.height / tileRows : xr.swapchain.height;

                    // Compute render dims using canvas (not window) for texture apps
                    uint32_t renderW, renderH;
                    if (monoMode) {
                        renderW = g_canvasW;
                        renderH = g_canvasH;
                        if (renderW > xr.swapchain.width) renderW = xr.swapchain.width;
                        if (renderH > xr.swapchain.height) renderH = xr.swapchain.height;
                    } else {
                        renderW = (uint32_t)(g_canvasW * xr.recommendedViewScaleX);
                        renderH = (uint32_t)(g_canvasH * xr.recommendedViewScaleY);
                        if (renderW > maxTileW) renderW = maxTileW;
                        if (renderH > maxTileH) renderH = maxTileH;
                    }

                    // Kooima projection using canvas dims
                    std::vector<Display3DView> stereoViews(eyeCount);
                    bool useAppProjection = (xr.hasDisplayInfoExt && xr.displayWidthM > 0.0f);
                    if (useAppProjection) {
                        float pxSizeX = xr.displayWidthM / (float)xr.displayPixelWidth;
                        float pxSizeY = xr.displayHeightM / (float)xr.displayPixelHeight;
                        float canvasW_m = (float)g_canvasW * pxSizeX;
                        float canvasH_m = (float)g_canvasH * pxSizeY;

                        // Canvas-relative Kooima: physical size + eye offset both anchored
                        // on the canvas region within the window (not the window itself).
                        float canvas_off_x = (float)g_windowWidth  / 4.0f;
                        float canvas_off_y = (float)g_windowHeight / 4.0f;
                        float eyeOffsetX = 0.0f, eyeOffsetY = 0.0f;
                        {
                            POINT clientOrigin = {0, 0};
                            ClientToScreen(rs.hwnd, &clientOrigin);
                            HMONITOR hMon = MonitorFromWindow(rs.hwnd, MONITOR_DEFAULTTONEAREST);
                            MONITORINFO mi = {sizeof(mi)};
                            if (GetMonitorInfo(hMon, &mi)) {
                                float canvasCenterX = (float)(clientOrigin.x - mi.rcMonitor.left) + canvas_off_x + g_canvasW / 2.0f;
                                float canvasCenterY = (float)(clientOrigin.y - mi.rcMonitor.top)  + canvas_off_y + g_canvasH / 2.0f;
                                float dispW = (float)(mi.rcMonitor.right - mi.rcMonitor.left);
                                float dispH = (float)(mi.rcMonitor.bottom - mi.rcMonitor.top);
                                eyeOffsetX =  (canvasCenterX - dispW / 2.0f) * pxSizeX;
                                eyeOffsetY = -((canvasCenterY - dispH / 2.0f) * pxSizeY);
                            }
                        }

                        std::vector<XrVector3f> rawEyes(modeViewCount);
                        for (uint32_t v = 0; v < modeViewCount; v++) {
                            XrVector3f pos = (v < viewCount) ? rawViews[v].pose.position : rawViews[0].pose.position;
                            rawEyes[v] = {pos.x - eyeOffsetX, pos.y - eyeOffsetY, pos.z};
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
                            g_inputState.pitch, g_inputState.yaw, 0);
                        XMFLOAT4 q;
                        XMStoreFloat4(&q, pOri);
                        cameraPose.orientation = {q.x, q.y, q.z, q.w};
                        cameraPose.position = {g_inputState.cameraPosX, g_inputState.cameraPosY, g_inputState.cameraPosZ};

                        XrVector3f nominalViewer = {xr.nominalViewerX, xr.nominalViewerY, xr.nominalViewerZ};
                        Display3DScreen screen = {canvasW_m, canvasH_m};

                        if (g_inputState.cameraMode) {
                            Camera3DTunables camTunables;
                            camTunables.ipd_factor = g_inputState.viewParams.ipdFactor;
                            camTunables.parallax_factor = g_inputState.viewParams.parallaxFactor;
                            camTunables.inv_convergence_distance = g_inputState.viewParams.invConvergenceDistance;
                            camTunables.half_tan_vfov = CAMERA_HALF_TAN_VFOV / g_inputState.viewParams.zoomFactor;

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
                            tunables.ipd_factor = g_inputState.viewParams.ipdFactor;
                            tunables.parallax_factor = g_inputState.viewParams.parallaxFactor;
                            tunables.perspective_factor = g_inputState.viewParams.perspectiveFactor;
                            tunables.virtual_display_height = g_inputState.viewParams.virtualDisplayHeight / g_inputState.viewParams.scaleFactor;

                            display3d_compute_views(
                                rawEyes.data(), eyeCount, &nominalViewer,
                                &screen, &tunables, &cameraPose,
                                0.01f, 100.0f, stereoViews.data());
                        }
                    }

                    // Mono fallback view/proj
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
                            monoProjMatrix = xr.projMatrices[0];
                            monoFov = rawViews[0].fov;
                            XMVECTOR centerLocalPos = XMVectorSet(
                                monoPose.position.x, monoPose.position.y, monoPose.position.z, 0.0f);
                            XMVECTOR localOri = XMVectorSet(
                                rawViews[0].pose.orientation.x, rawViews[0].pose.orientation.y,
                                rawViews[0].pose.orientation.z, rawViews[0].pose.orientation.w);
                            float monoM2vView = 1.0f;
                            if (g_inputState.viewParams.virtualDisplayHeight > 0.0f && xr.displayHeightM > 0.0f)
                                monoM2vView = g_inputState.viewParams.virtualDisplayHeight / xr.displayHeightM;
                            float eyeScale = g_inputState.viewParams.perspectiveFactor * monoM2vView / g_inputState.viewParams.scaleFactor;
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

                    // Render HUD
                    if (g_inputState.hudVisible && xr.hasHudSwapchain && rs.hudSwapchainImages && !rs.hudSwapchainImages->empty() && rs.hudOk) {
                        uint32_t hudImageIndex;
                        if (AcquireHudSwapchainImage(xr, hudImageIndex)) {
                            std::wstring sessionText(xr.systemName, xr.systemName + strlen(xr.systemName));
                            sessionText += L"\nSession: ";
                            sessionText += FormatSessionState((int)xr.sessionState);
                            std::wstring modeText = L"Shared Texture D3D12 (offscreen)";
                            modeText += g_inputState.cameraMode ?
                                L"\nKooima: Camera-Centric [C=Toggle]" :
                                L"\nKooima: Display-Centric [C=Toggle]";

                            uint32_t dispRenderW, dispRenderH;
                            if (monoMode) {
                                dispRenderW = g_canvasW;
                                dispRenderH = g_canvasH;
                                if (dispRenderW > xr.swapchain.width) dispRenderW = xr.swapchain.width;
                                if (dispRenderH > xr.swapchain.height) dispRenderH = xr.swapchain.height;
                            } else {
                                dispRenderW = (uint32_t)(g_canvasW * xr.recommendedViewScaleX);
                                dispRenderH = (uint32_t)(g_canvasH * xr.recommendedViewScaleY);
                                if (dispRenderW > maxTileW) dispRenderW = maxTileW;
                                if (dispRenderH > maxTileH) dispRenderH = maxTileH;
                            }
                            std::wstring perfText = FormatPerformanceInfo(rs.perfStats->fps, rs.perfStats->frameTimeMs,
                                dispRenderW, dispRenderH, g_windowWidth, g_windowHeight);
                            std::wstring dispText = FormatDisplayInfo(xr.displayWidthM, xr.displayHeightM,
                                xr.nominalViewerX, xr.nominalViewerY, xr.nominalViewerZ);
                            dispText += L"\n" + FormatScaleInfo(xr.recommendedViewScaleX, xr.recommendedViewScaleY);
                            dispText += L"\n" + FormatMode(g_inputState.currentRenderingMode, xr.pfnRequestDisplayRenderingModeEXT != nullptr,
                                (xr.renderingModeCount > 0 && g_inputState.currentRenderingMode < xr.renderingModeCount) ? xr.renderingModeNames[g_inputState.currentRenderingMode] : nullptr,
                                xr.renderingModeCount,
                                xr.renderingModeCount > 0 ? xr.renderingModeDisplay3D[g_inputState.currentRenderingMode] : true);
                            std::wstring eyeText = FormatEyeTrackingInfo(
                                xr.eyePositions, (uint32_t)eyeCount,
                                xr.eyeTrackingActive, xr.isEyeTracking,
                                xr.activeEyeTrackingMode, xr.supportedEyeTrackingModes);

                            float fwdX = -sinf(g_inputState.yaw) * cosf(g_inputState.pitch);
                            float fwdY =  sinf(g_inputState.pitch);
                            float fwdZ = -cosf(g_inputState.yaw) * cosf(g_inputState.pitch);
                            std::wstring cameraText = FormatCameraInfo(
                                g_inputState.cameraPosX, g_inputState.cameraPosY, g_inputState.cameraPosZ,
                                fwdX, fwdY, fwdZ, g_inputState.cameraMode);
                            float dispP1 = g_inputState.cameraMode ? g_inputState.viewParams.invConvergenceDistance : g_inputState.viewParams.perspectiveFactor;
                            float dispP2 = g_inputState.cameraMode ? g_inputState.viewParams.zoomFactor : g_inputState.viewParams.scaleFactor;
                            std::wstring stereoText = FormatViewParams(
                                g_inputState.viewParams.ipdFactor, g_inputState.viewParams.parallaxFactor,
                                dispP1, dispP2, g_inputState.cameraMode);
                            {
                                wchar_t vhBuf[64];
                                if (g_inputState.cameraMode) {
                                    float tanHFOV = CAMERA_HALF_TAN_VFOV / g_inputState.viewParams.zoomFactor;
                                    swprintf(vhBuf, 64, L"\ntanHFOV: %.3f", tanHFOV);
                                } else {
                                    float hudM2v = 1.0f;
                                    if (g_inputState.viewParams.virtualDisplayHeight > 0.0f && xr.displayHeightM > 0.0f)
                                        hudM2v = g_inputState.viewParams.virtualDisplayHeight / xr.displayHeightM;
                                    swprintf(vhBuf, 64, L"\nvHeight: %.3f  m2v: %.3f",
                                        g_inputState.viewParams.virtualDisplayHeight, hudM2v);
                                }
                                stereoText += vhBuf;
                            }
                            std::wstring helpText = FormatHelpText(xr.pfnRequestDisplayRenderingModeEXT != nullptr, g_inputState.cameraMode, xr.renderingModeCount);

                            uint32_t srcRowPitch = 0;
                            const void* pixels = RenderHudAndMap(*rs.hudRenderer, &srcRowPitch,
                                sessionText, modeText, perfText, dispText, eyeText,
                                cameraText, stereoText, helpText);
                            if (pixels) {
                                // Copy pixels row-by-row to D3D12 upload buffer (256-byte aligned rows)
                                const uint8_t* src = (const uint8_t*)pixels;
                                for (uint32_t row = 0; row < HUD_PIXEL_HEIGHT; row++) {
                                    memcpy(rs.hudUploadMapped + row * rs.hudUploadRowPitch,
                                        src + row * srcRowPitch,
                                        HUD_PIXEL_WIDTH * 4);
                                }
                                UnmapHud(*rs.hudRenderer);

                                // Record D3D12 commands: copy upload buffer to HUD swapchain texture
                                ID3D12Resource* hudTex = (*rs.hudSwapchainImages)[hudImageIndex].texture;

                                rs.hudCmdAllocator->Reset();
                                rs.hudCmdList->Reset(rs.hudCmdAllocator, nullptr);

                                D3D12_RESOURCE_BARRIER barrier = {};
                                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                                barrier.Transition.pResource = hudTex;
                                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                                rs.hudCmdList->ResourceBarrier(1, &barrier);

                                D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
                                srcLoc.pResource = rs.hudUploadBuffer;
                                srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                                srcLoc.PlacedFootprint.Offset = 0;
                                srcLoc.PlacedFootprint.Footprint.Format = (DXGI_FORMAT)xr.hudSwapchain.format;
                                srcLoc.PlacedFootprint.Footprint.Width = HUD_PIXEL_WIDTH;
                                srcLoc.PlacedFootprint.Footprint.Height = HUD_PIXEL_HEIGHT;
                                srcLoc.PlacedFootprint.Footprint.Depth = 1;
                                srcLoc.PlacedFootprint.Footprint.RowPitch = rs.hudUploadRowPitch;

                                D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
                                dstLoc.pResource = hudTex;
                                dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                                dstLoc.SubresourceIndex = 0;

                                rs.hudCmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

                                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
                                rs.hudCmdList->ResourceBarrier(1, &barrier);

                                rs.hudCmdList->Close();

                                ID3D12CommandList* lists[] = {rs.hudCmdList};
                                renderer.commandQueue->ExecuteCommandLists(1, lists);
                                rs.hudFenceValue++;
                                renderer.commandQueue->Signal(rs.hudFence, rs.hudFenceValue);
                                if (rs.hudFence->GetCompletedValue() < rs.hudFenceValue) {
                                    rs.hudFence->SetEventOnCompletion(rs.hudFenceValue, rs.hudFenceEvent);
                                    WaitForSingleObject(rs.hudFenceEvent, INFINITE);
                                }

                                hudSubmitted = true;
                            }
                            ReleaseHudSwapchainImage(xr);
                        }
                    }

                    // Render scene into OpenXR swapchain atlas
                    uint32_t imageIndex;
                    if (AcquireSwapchainImage(xr, imageIndex)) {
                        ID3D12Resource* swapchainTexture = (*rs.swapchainImages)[imageIndex].texture;
                        int rtvIdx = rs.rtvBaseIndex + (int)imageIndex;

                        for (int eye = 0; eye < eyeCount; eye++) {
                            uint32_t tileX = monoMode ? 0 : (eye % tileColumns);
                            uint32_t tileY = monoMode ? 0 : (eye / tileColumns);
                            uint32_t vpX = tileX * renderW;
                            uint32_t vpY = tileY * renderH;

                            int vi = eye < (int)viewCount ? eye : 0;
                            XMMATRIX viewMatrix, projMatrix;
                            if (useAppProjection) {
                                viewMatrix = ColumnMajorToXMMatrix(stereoViews[eye].view_matrix);
                                projMatrix = ColumnMajorToXMMatrix(stereoViews[eye].projection_matrix);
                            } else if (monoMode) {
                                viewMatrix = monoViewMatrix;
                                projMatrix = monoProjMatrix;
                            } else {
                                viewMatrix = xr.viewMatrices[vi];
                                projMatrix = xr.projMatrices[vi];
                            }

                            RenderScene(renderer, swapchainTexture, rtvIdx,
                                vpX, vpY, renderW, renderH,
                                viewMatrix, projMatrix,
                                useAppProjection ? 1.0f : g_inputState.viewParams.scaleFactor,
                                eye == 0);

                            projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                            projectionViews[eye].subImage.swapchain = xr.swapchain.swapchain;
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

                        ReleaseSwapchainImage(xr);
                    }
                }
            }

            if (hudSubmitted) {
                float hudAR = (float)HUD_PIXEL_WIDTH / (float)HUD_PIXEL_HEIGHT;
                float windowAR = (g_windowWidth > 0 && g_windowHeight > 0) ? (float)g_windowWidth / (float)g_windowHeight : 1.0f;
                float fracW = HUD_WIDTH_FRACTION;
                float fracH = fracW * windowAR / hudAR;
                if (fracH > 1.0f) { fracH = 1.0f; fracW = hudAR / windowAR; }
                EndFrameWithWindowSpaceHud(xr, frameState.predictedDisplayTime, projectionViews.data(),
                    0.0f, 0.0f, fracW, fracH, 0.0f, (uint32_t)eyeCount);
            } else {
                EndFrame(xr, frameState.predictedDisplayTime, projectionViews.data(), (uint32_t)eyeCount);
            }

            // Resize app swapchain if needed
            if (g_resizeNeeded) {
                g_resizeNeeded = false;
                ResizeAppSwapchain(renderer);
            }

            // After xrEndFrame: blit shared texture to app window
            if (frameState.shouldRender) {
                BlitSharedTextureToBackBuffer(renderer, xr);
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

    // Initialize D3D12
    D3D12Renderer renderer = {};
    if (!InitializeD3D12WithLUID(renderer, adapterLuid)) {
        LOG_ERROR("D3D12 init failed");
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create app-side DXGI swapchain for window presentation
    {
        DXGI_SWAP_CHAIN_DESC1 scd = {};
        scd.Width = g_windowWidth;
        scd.Height = g_windowHeight;
        scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd.SampleDesc.Count = 1;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount = APP_BACK_BUFFER_COUNT;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

        ComPtr<IDXGISwapChain1> swapchain1;
        HRESULT hr = renderer.dxgiFactory->CreateSwapChainForHwnd(
            renderer.commandQueue.Get(), hwnd, &scd, nullptr, nullptr, &swapchain1);
        if (FAILED(hr)) {
            LOG_ERROR("Failed to create app swapchain: 0x%08x", hr);
            CleanupD3D12(renderer);
            CleanupOpenXR(xr);
            ShutdownLogging();
            return 1;
        }
        swapchain1.As(&g_appSwapchain);
    }

    // App swapchain RTV heap
    {
        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
        rtvDesc.NumDescriptors = APP_BACK_BUFFER_COUNT;
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        renderer.device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&g_appRtvHeap));
        g_appRtvDescriptorSize = renderer.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        CreateAppSwapchainRTVs(renderer.device.Get());
    }

    // Create blit pipeline
    if (!CreateBlitPipeline(renderer.device.Get())) {
        LOG_ERROR("Failed to create blit pipeline");
        CleanupD3D12(renderer);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Blit command resources
    {
        renderer.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_blitCmdAllocator));
        renderer.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_blitCmdAllocator.Get(), nullptr, IID_PPV_ARGS(&g_blitCmdList));
        g_blitCmdList->Close();
        renderer.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_blitFence));
        g_blitFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    }

    // Create shared D3D12 texture at worst-case swapchain atlas dims
    g_sharedWidth = 0;
    g_sharedHeight = 0;
    if (xr.renderingModeCount > 0 && xr.displayPixelWidth > 0 && xr.displayPixelHeight > 0) {
        for (uint32_t i = 0; i < xr.renderingModeCount; i++) {
            uint32_t mw = (uint32_t)(xr.renderingModeTileColumns[i] * xr.renderingModeScaleX[i] * xr.displayPixelWidth);
            uint32_t mh = (uint32_t)(xr.renderingModeTileRows[i] * xr.renderingModeScaleY[i] * xr.displayPixelHeight);
            if (mw > g_sharedWidth) g_sharedWidth = mw;
            if (mh > g_sharedHeight) g_sharedHeight = mh;
        }
    }
    if (g_sharedWidth == 0 || g_sharedHeight == 0) {
        g_sharedWidth = xr.displayPixelWidth > 0 ? xr.displayPixelWidth : 1920;
        g_sharedHeight = xr.displayPixelHeight > 0 ? xr.displayPixelHeight : 1080;
    }
    {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = g_sharedWidth;
        texDesc.Height = g_sharedHeight;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        HRESULT hr = renderer.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_SHARED, &texDesc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_sharedTexture));
        if (FAILED(hr)) {
            LOG_ERROR("Failed to create shared D3D12 texture: 0x%08x", hr);
            CleanupD3D12(renderer);
            CleanupOpenXR(xr);
            ShutdownLogging();
            return 1;
        }

        hr = renderer.device->CreateSharedHandle(g_sharedTexture.Get(), nullptr, GENERIC_ALL, nullptr, &g_sharedHandle);
        if (FAILED(hr)) {
            LOG_ERROR("Failed to create shared handle: 0x%08x", hr);
            CleanupD3D12(renderer);
            CleanupOpenXR(xr);
            ShutdownLogging();
            return 1;
        }

        g_canvasW = g_windowWidth / 2;
        g_canvasH = g_windowHeight / 2;
        LOG_INFO("Created shared D3D12 texture: %ux%u, handle=%p", g_sharedWidth, g_sharedHeight, g_sharedHandle);
    }

    // Create SRV for shared texture (for blit)
    CreateSharedTextureSRV(renderer.device.Get());

    // HUD renderer
    HudRenderer hudRenderer = {};
    bool hudOk = InitializeHudRenderer(hudRenderer, HUD_PIXEL_WIDTH, HUD_PIXEL_HEIGHT);

    // Create OpenXR session with shared texture + app HWND for weaver position tracking
    if (!CreateSession(xr, renderer.device.Get(), renderer.commandQueue.Get(), g_sharedHandle, hwnd)) {
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

    // Enumerate D3D12 swapchain images
    std::vector<XrSwapchainImageD3D12KHR> swapchainImages;
    int rtvBaseIndex = 0;
    {
        uint32_t count = xr.swapchain.imageCount;
        swapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)swapchainImages.data());

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
            if (hudOk) CleanupHudRenderer(hudRenderer);
            CleanupD3D12(renderer);
            ShutdownLogging();
            return 1;
        }
    }

    // Create HUD swapchain and upload resources
    std::vector<XrSwapchainImageD3D12KHR> hudSwapImages;
    ComPtr<ID3D12Resource> hudUploadBuffer;
    uint8_t* hudUploadMapped = nullptr;
    ComPtr<ID3D12CommandAllocator> hudCmdAllocator;
    ComPtr<ID3D12GraphicsCommandList> hudCmdList;
    ComPtr<ID3D12Fence> hudFence;
    HANDLE hudFenceEvent = nullptr;
    uint32_t hudUploadRowPitch = (HUD_PIXEL_WIDTH * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
        & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);

    if (hudOk) {
        if (CreateHudSwapchain(xr, HUD_PIXEL_WIDTH, HUD_PIXEL_HEIGHT)) {
            uint32_t count = xr.hudSwapchain.imageCount;
            hudSwapImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
            xrEnumerateSwapchainImages(xr.hudSwapchain.swapchain, count, &count,
                (XrSwapchainImageBaseHeader*)hudSwapImages.data());
        } else {
            hudOk = false;
        }
    }

    if (hudOk) {
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
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&hudUploadBuffer));
        if (SUCCEEDED(hr)) {
            D3D12_RANGE readRange = {0, 0};
            hr = hudUploadBuffer->Map(0, &readRange, (void**)&hudUploadMapped);
            if (FAILED(hr)) hudOk = false;
        } else {
            hudOk = false;
        }

        if (hudOk) {
            renderer.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&hudCmdAllocator));
            renderer.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, hudCmdAllocator.Get(), nullptr, IID_PPV_ARGS(&hudCmdList));
            hudCmdList->Close();
            renderer.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&hudFence));
            hudFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        }
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    LOG_INFO("");
    LOG_INFO("=== Entering main loop ===");
    LOG_INFO("Shared texture mode: runtime renders to shared texture, app blits to window");
    LOG_INFO("Controls: WASD=Fly, Mouse=Look, Space=Reset, V=Mode, TAB=HUD, F11=Fullscreen, ESC=Quit");

    PerformanceStats perfStats = {};
    perfStats.lastTime = std::chrono::high_resolution_clock::now();
    g_inputState.viewParams.virtualDisplayHeight = 0.24f;
    g_inputState.nominalViewerZ = xr.nominalViewerZ;
    g_inputState.renderingModeCount = xr.renderingModeCount;

    RenderState rs = {};
    rs.hwnd = hwnd;
    rs.xr = &xr;
    rs.renderer = &renderer;
    rs.hudRenderer = &hudRenderer;
    rs.hudOk = hudOk;
    rs.swapchainImages = &swapchainImages;
    rs.rtvBaseIndex = rtvBaseIndex;
    rs.hudSwapchainImages = &hudSwapImages;
    rs.hudUploadBuffer = hudUploadBuffer.Get();
    rs.hudUploadMapped = hudUploadMapped;
    rs.hudUploadRowPitch = hudUploadRowPitch;
    rs.hudCmdAllocator = hudCmdAllocator.Get();
    rs.hudCmdList = hudCmdList.Get();
    rs.hudFence = hudFence.Get();
    rs.hudFenceEvent = hudFenceEvent;
    rs.hudFenceValue = 0;
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

    // Wait for GPU idle
    if (g_blitFence && renderer.commandQueue) {
        g_blitFenceValue++;
        renderer.commandQueue->Signal(g_blitFence.Get(), g_blitFenceValue);
        if (g_blitFence->GetCompletedValue() < g_blitFenceValue) {
            g_blitFence->SetEventOnCompletion(g_blitFenceValue, g_blitFenceEvent);
            WaitForSingleObject(g_blitFenceEvent, INFINITE);
        }
    }

    // Cleanup HUD resources
    if (hudFenceEvent) CloseHandle(hudFenceEvent);
    hudFence.Reset();
    hudCmdList.Reset();
    hudCmdAllocator.Reset();
    if (hudUploadMapped && hudUploadBuffer) {
        hudUploadBuffer->Unmap(0, nullptr);
        hudUploadMapped = nullptr;
    }
    hudUploadBuffer.Reset();

    // Cleanup blit resources
    if (g_blitFenceEvent) CloseHandle(g_blitFenceEvent);
    g_blitFence.Reset();
    g_blitCmdList.Reset();
    g_blitCmdAllocator.Reset();
    g_blitPSO.Reset();
    g_blitRootSig.Reset();
    g_blitSrvHeap.Reset();

    // Cleanup shared texture
    if (g_sharedHandle) {
        CloseHandle(g_sharedHandle);
        g_sharedHandle = nullptr;
    }
    g_sharedTexture.Reset();

    // Cleanup app swapchain
    ReleaseAppSwapchainRTVs();
    g_appRtvHeap.Reset();
    g_appSwapchain.Reset();

    g_xr = nullptr;
    CleanupOpenXR(xr);
    if (hudOk) CleanupHudRenderer(hudRenderer);
    CleanupD3D12(renderer);

    DestroyWindow(hwnd);
    UnregisterClass(WINDOW_CLASS, hInstance);

    ShutdownLogging();
    return 0;
}
