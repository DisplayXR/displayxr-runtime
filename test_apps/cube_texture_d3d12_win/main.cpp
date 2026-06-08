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
#include "projection_depth.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
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

// XR_EXT_view_rig (#396 W7 dogfood): the app chains a rig descriptor
// (XrDisplayRigEXT, or XrCameraRigEXT in camera mode) on every xrLocateViews
// and consumes the runtime's render-ready XrView{pose, fov} directly — the
// per-frame Kooima generation is deleted; only clip policy stays app-side.
// The runtime resolves the CANVAS sub-rect itself (the d3d12 compositor
// delegates window metrics to the DP). Per-view staging container (matrices
// column-major):
struct RigView {
    float view_matrix[16];
    float projection_matrix[16];
    XrFovf fov;
};

// Column-major view matrix from a render-ready XrView pose:
// viewMatrix = R^T * translate(-position) — same construction as the
// displayxr::math rigs.
static void ViewMatrixFromXrPose(const XrPosef& pose, float* out) {
    const float qx = pose.orientation.x, qy = pose.orientation.y;
    const float qz = pose.orientation.z, qw = pose.orientation.w;
    float rot[16] = {};
    rot[0] = 1.0f - 2.0f * (qy * qy + qz * qz);
    rot[1] = 2.0f * (qx * qy + qz * qw);
    rot[2] = 2.0f * (qx * qz - qy * qw);
    rot[4] = 2.0f * (qx * qy - qz * qw);
    rot[5] = 1.0f - 2.0f * (qx * qx + qz * qz);
    rot[6] = 2.0f * (qy * qz + qx * qw);
    rot[8] = 2.0f * (qx * qz + qy * qw);
    rot[9] = 2.0f * (qy * qz - qx * qw);
    rot[10] = 1.0f - 2.0f * (qx * qx + qy * qy);
    rot[15] = 1.0f;
    for (int i = 0; i < 16; i++) out[i] = 0.0f;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            out[j * 4 + i] = rot[i * 4 + j]; // R^T
    out[15] = 1.0f;
    out[12] = -(out[0] * pose.position.x + out[4] * pose.position.y + out[8] * pose.position.z);
    out[13] = -(out[1] * pose.position.x + out[5] * pose.position.y + out[9] * pose.position.z);
    out[14] = -(out[2] * pose.position.x + out[6] * pose.position.y + out[10] * pose.position.z);
}

// Column-major GL ([-1,1] clip-z) off-axis projection from a render-ready
// XrView fov + the app's own clip policy (fov is clip-independent). Pair with
// convert_projection_gl_to_zero_to_one() for D3D.
static void ProjectionFromXrFov(const XrFovf& fov, float nearZ, float farZ, float* out) {
    const float l = tanf(fov.angleLeft) * nearZ;
    const float r = tanf(fov.angleRight) * nearZ;
    const float b = tanf(fov.angleDown) * nearZ;
    const float t = tanf(fov.angleUp) * nearZ;
    for (int i = 0; i < 16; i++) out[i] = 0.0f;
    out[0] = 2.0f * nearZ / (r - l);
    out[5] = 2.0f * nearZ / (t - b);
    out[8] = (r + l) / (r - l);
    out[9] = (t + b) / (t - b);
    out[10] = -(farZ + nearZ) / (farZ - nearZ);
    out[11] = -1.0f;
    out[14] = -2.0f * farZ * nearZ / (farZ - nearZ);
}

// Display-local eye distance for the ZDP-anchored clip: z of (rigPose^-1 *
// eyeWorld). Degenerates to pose.position.z at identity rig pose.
static float RigLocalEyeZ(const XrPosef& rig, const XrVector3f& eyeWorld) {
    const float dx = eyeWorld.x - rig.position.x;
    const float dy = eyeWorld.y - rig.position.y;
    const float dz = eyeWorld.z - rig.position.z;
    const float qx = -rig.orientation.x, qy = -rig.orientation.y;
    const float qz = -rig.orientation.z, qw = rig.orientation.w;
    const float cx = qy * dz - qz * dy + qw * dx;
    const float cy = qz * dx - qx * dz + qw * dy;
    return dz + 2.0f * (qx * cy - qy * cx);
}

// Shared texture resources (D3D12)
static ComPtr<ID3D12Resource> g_sharedTexture;
static HANDLE g_sharedHandle = nullptr;
static uint32_t g_sharedWidth = 0;
static uint32_t g_sharedHeight = 0;

// Canvas dimensions — the sub-rect of the window where 3D content is displayed.
static uint32_t g_canvasW = 0;
static uint32_t g_canvasH = 0;

// Surround 2D texture (spec v7 §3.7). Same dims + format as g_sharedTexture
// because the D3D12 compositor strip-blit uses CopyTextureRegion and
// enforces format equality. Uses an ID3D12Fence (shared NT handle) for
// producer→consumer sync since IDXGIKeyedMutex is not reliably available
// on D3D12-native shared resources (E_NOINTERFACE on common drivers).
static ComPtr<ID3D12Resource> g_surroundTexture;
static HANDLE g_surroundHandle = nullptr;
static ComPtr<ID3D12Fence> g_surroundFence;
static HANDLE g_surroundFenceHandle = nullptr;
static uint64_t g_surroundFenceValue = 0;  // app-side monotonic counter
static ComPtr<ID3D12DescriptorHeap> g_surroundRtvHeap;
static ComPtr<ID3D12RootSignature> g_surroundRootSig;
static ComPtr<ID3D12PipelineState> g_surroundPSO;
static bool g_surroundRegistered = false;

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

// #439 — 'Z' cycles the zone-mask harness states (consumed in RenderOneFrame).
static bool g_zoneCycleRequested = false;

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
        if (wParam == 'Z') {
            // #439 — cycle the XR_EXT_local_3d_zone harness state.
            g_zoneCycleRequested = true;
            return 0;
        }
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

// ---- Surround 2D pipeline (spec v6 §3.6) ----

// Identical procedural pattern to cube_texture_d3d11_win: gradient +
// checkerboard everywhere outside the canvas, red highlight on the canvas
// border, slow diagonal sweep. Inside the canvas it writes black — the
// compositor copy doesn't read those pixels (the canvas region is the DP's
// weave), so the value is cosmetic only.
static const char* g_surroundPSSource = R"(
cbuffer SurroundParams : register(b0) {
    float2 windowSize;
    float2 _pad0;
    int4   canvas;
    float  time;
    float3 _pad1;
};
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 p = pos.xy;
    if (p.x >= windowSize.x || p.y >= windowSize.y) {
        return float4(0, 0, 0, 1);
    }
    float bx0 = (float)canvas.x;
    float by0 = (float)canvas.y;
    float bx1 = (float)(canvas.x + canvas.z);
    float by1 = (float)(canvas.y + canvas.w);
    float dx = max(bx0 - p.x, p.x - bx1);
    float dy = max(by0 - p.y, p.y - by1);
    float d  = max(dx, dy);
    if (d <= 0) {
        return float4(0, 0, 0, 1);
    }
    if (d <= 4.0) {
        return float4(1.0, 0.25, 0.25, 1.0);
    }
    int2 cell = int2(p / 24.0);
    bool light = ((cell.x + cell.y) & 1) == 0;
    float3 base = light ? float3(0.82, 0.84, 0.92) : float3(0.50, 0.55, 0.80);
    float2 g = saturate(p / windowSize);
    base.r += g.x * 0.18;
    base.g += g.y * 0.10;
    base.b += (1.0 - g.x) * 0.10;
    float sweep = frac((p.x + p.y) / 256.0 - time * 0.10);
    base += (smoothstep(0.45, 0.5, sweep) - smoothstep(0.5, 0.55, sweep)) * 0.10;
    return float4(saturate(base), 1.0);
}
)";

static bool CreateSurroundPipeline(ID3D12Device* device) {
    // Root signature: one root-constants param (12 × uint32 = 48 bytes).
    D3D12_ROOT_PARAMETER param = {};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.Constants.ShaderRegister = 0;
    param.Constants.RegisterSpace = 0;
    param.Constants.Num32BitValues = 12;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 1;
    rsDesc.pParameters = &param;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> serialized, error;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized, &error);
    if (FAILED(hr)) {
        LOG_ERROR("Surround root signature serialize failed: %s",
            error ? (char*)error->GetBufferPointer() : "unknown");
        return false;
    }
    hr = device->CreateRootSignature(0, serialized->GetBufferPointer(),
        serialized->GetBufferSize(), IID_PPV_ARGS(&g_surroundRootSig));
    if (FAILED(hr)) return false;

    // Reuse the blit fullscreen-triangle VS.
    ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
    hr = D3DCompile(g_blitVSSource, strlen(g_blitVSSource), "blitVS",
        nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, &errBlob);
    if (FAILED(hr)) {
        LOG_ERROR("Surround VS compile failed: %s",
            errBlob ? (char*)errBlob->GetBufferPointer() : "unknown");
        return false;
    }
    hr = D3DCompile(g_surroundPSSource, strlen(g_surroundPSSource), "surroundPS",
        nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, &errBlob);
    if (FAILED(hr)) {
        LOG_ERROR("Surround PS compile failed: %s",
            errBlob ? (char*)errBlob->GetBufferPointer() : "unknown");
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = g_surroundRootSig.Get();
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

    hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_surroundPSO));
    if (FAILED(hr)) {
        LOG_ERROR("Surround PSO creation failed: 0x%08X", hr);
        return false;
    }

    // One RTV for the surround texture.
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = 1;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    hr = device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_surroundRtvHeap));
    return SUCCEEDED(hr);
}

// Creates the surround texture (D3D12_HEAP_FLAG_SHARED, no KeyedMutex needed)
// AND a paired shared ID3D12Fence (D3D12_FENCE_FLAG_SHARED). Both NT handles
// are exported via ID3D12Device::CreateSharedHandle. The fence drives the
// runtime's `commandQueue->Wait` before its strip-blit each frame.
static bool CreateSurroundTextureD3D12(ID3D12Device* device,
                                        uint32_t width, uint32_t height,
                                        DXGI_FORMAT format) {
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = format;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    HRESULT hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_SHARED, &texDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_surroundTexture));
    if (FAILED(hr)) {
        LOG_ERROR("Surround texture create failed: 0x%08x", hr);
        return false;
    }

    hr = device->CreateSharedHandle(g_surroundTexture.Get(), nullptr, GENERIC_ALL,
        nullptr, &g_surroundHandle);
    if (FAILED(hr)) {
        LOG_ERROR("Surround texture CreateSharedHandle failed: 0x%08x", hr);
        g_surroundTexture.Reset();
        return false;
    }

    hr = device->CreateFence(0,
        D3D12_FENCE_FLAG_SHARED,
        IID_PPV_ARGS(&g_surroundFence));
    if (FAILED(hr)) {
        LOG_ERROR("Surround fence create failed: 0x%08x", hr);
        CloseHandle(g_surroundHandle);
        g_surroundHandle = nullptr;
        g_surroundTexture.Reset();
        return false;
    }

    hr = device->CreateSharedHandle(g_surroundFence.Get(), nullptr, GENERIC_ALL,
        nullptr, &g_surroundFenceHandle);
    if (FAILED(hr)) {
        LOG_ERROR("Surround fence CreateSharedHandle failed: 0x%08x", hr);
        g_surroundFence.Reset();
        CloseHandle(g_surroundHandle);
        g_surroundHandle = nullptr;
        g_surroundTexture.Reset();
        return false;
    }

    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = format;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(g_surroundTexture.Get(), &rtvDesc,
        g_surroundRtvHeap->GetCPUDescriptorHandleForHeapStart());

    LOG_INFO("Created surround D3D12 texture: %ux%u format=%u tex_handle=%p fence_handle=%p",
        width, height, (unsigned)format, g_surroundHandle, g_surroundFenceHandle);
    return true;
}

// Record (into an already-Reset cmd list) the surround render pass: barrier
// COMMON→RENDER_TARGET, fullscreen triangle, barrier back. Caller acquires
// the keyed mutex on key 0 before this and releases after ExecuteCommandLists.
static void RecordSurroundPattern(ID3D12GraphicsCommandList* cmdList,
                                    uint32_t winW, uint32_t winH,
                                    int32_t canvasX, int32_t canvasY,
                                    uint32_t canvasW, uint32_t canvasH,
                                    float timeSeconds) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = g_surroundTexture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_surroundRtvHeap->GetCPUDescriptorHandleForHeapStart();
    cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    D3D12_VIEWPORT vp = {};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = (FLOAT)winW;
    vp.Height = (FLOAT)winH;
    vp.MaxDepth = 1.0f;
    cmdList->RSSetViewports(1, &vp);

    D3D12_RECT scissor = {0, 0, (LONG)winW, (LONG)winH};
    cmdList->RSSetScissorRects(1, &scissor);

    cmdList->SetPipelineState(g_surroundPSO.Get());
    cmdList->SetGraphicsRootSignature(g_surroundRootSig.Get());

    // Match the cbuffer SurroundParams layout (48 bytes, 12 × uint32).
    struct ParamsLayout {
        float windowSize[2];
        float _pad0[2];
        int32_t canvas[4];
        float time;
        float _pad1[3];
    } params = {};
    params.windowSize[0] = (float)winW;
    params.windowSize[1] = (float)winH;
    params.canvas[0] = canvasX;
    params.canvas[1] = canvasY;
    params.canvas[2] = (int32_t)canvasW;
    params.canvas[3] = (int32_t)canvasH;
    params.time = timeSeconds;
    cmdList->SetGraphicsRoot32BitConstants(0, 12, &params, 0);

    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->DrawInstanced(3, 1, 0, 0);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    cmdList->ResourceBarrier(1, &barrier);
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

// Blit shared texture to back buffer with canvas letterboxing.
// When the surround texture is registered, this also (1) records a per-frame
// surround pattern render at the top of the cmd list (covered by a keyed
// mutex on key 0) and (2) widens the blit viewport + UV range to read back
// the full window region of the shared texture so the surround strips
// (filled by the compositor at submit time) are visible in the window.
static void BlitSharedTextureToBackBuffer(D3D12Renderer& renderer, XrSessionManager& xr,
                                           float surroundTimeSeconds) {
    if (!g_sharedTexture || !g_appSwapchain) return;

    UINT bbIndex = g_appSwapchain->GetCurrentBackBufferIndex();
    ID3D12Resource* backBuffer = g_appBackBuffers[bbIndex].Get();

    g_blitCmdAllocator->Reset();
    g_blitCmdList->Reset(g_blitCmdAllocator.Get(), g_blitPSO.Get());

    // Canvas = center 50% of window
    float canvasX = (FLOAT)g_windowWidth * 0.25f;
    float canvasY = (FLOAT)g_windowHeight * 0.25f;
    float canvasW = (FLOAT)g_windowWidth * 0.5f;
    float canvasH = (FLOAT)g_windowHeight * 0.5f;

    // Tell runtime where the canvas is. The compositor uses this to drive
    // DP weave region + surround strip layout — call every frame.
    if (xr.pfnSetSharedTextureOutputRectEXT && xr.session != XR_NULL_HANDLE) {
        xr.pfnSetSharedTextureOutputRectEXT(xr.session,
            (int32_t)canvasX, (int32_t)canvasY,
            (uint32_t)canvasW, (uint32_t)canvasH);
    }

    // (1) Surround render — sync is fence-based (spec v7). We record the
    // surround draw into the same cmd list as the blit; the GPU executes
    // them in order. After ExecuteCommandLists below we Signal(fence, N)
    // and tell the runtime to Wait(fence, N) via the OpenXR call. The
    // runtime's own command queue will block on the next ExecuteCommandLists
    // until our signal lands, so its strip-blit reads stable pixels.
    bool surroundRecorded = false;
    if (g_surroundRegistered && g_surroundFence && g_surroundPSO) {
        RecordSurroundPattern(g_blitCmdList.Get(),
            g_windowWidth, g_windowHeight,
            (int32_t)canvasX, (int32_t)canvasY,
            (uint32_t)canvasW, (uint32_t)canvasH,
            surroundTimeSeconds);
        surroundRecorded = true;
    }

    // (2) Blit setup — barriers, RTV, viewport.
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

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_appRtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += bbIndex * g_appRtvDescriptorSize;
    float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    g_blitCmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    g_blitCmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    // Viewport + UV sampling: full-window when surround is active so the
    // strip pixels become visible; canvas-only otherwise.
    D3D12_VIEWPORT vp = {};
    float uvParams[4];
    if (g_surroundRegistered) {
        vp.TopLeftX = 0.0f;
        vp.TopLeftY = 0.0f;
        vp.Width = (FLOAT)g_windowWidth;
        vp.Height = (FLOAT)g_windowHeight;
        uvParams[0] = g_sharedWidth  > 0 ? (float)g_windowWidth  / (float)g_sharedWidth  : 1.0f;
        uvParams[1] = g_sharedHeight > 0 ? (float)g_windowHeight / (float)g_sharedHeight : 1.0f;
        uvParams[2] = 0.0f;
        uvParams[3] = 0.0f;
    } else {
        vp.TopLeftX = canvasX;
        vp.TopLeftY = canvasY;
        vp.Width = canvasW;
        vp.Height = canvasH;
        uvParams[0] = g_sharedWidth  > 0 ? canvasW / (float)g_sharedWidth  : 1.0f;
        uvParams[1] = g_sharedHeight > 0 ? canvasH / (float)g_sharedHeight : 1.0f;
        uvParams[2] = g_sharedWidth  > 0 ? canvasX / (float)g_sharedWidth  : 0.0f;
        uvParams[3] = g_sharedHeight > 0 ? canvasY / (float)g_sharedHeight : 0.0f;
    }
    vp.MaxDepth = 1.0f;
    g_blitCmdList->RSSetViewports(1, &vp);

    D3D12_RECT scissor = {0, 0, (LONG)g_windowWidth, (LONG)g_windowHeight};
    g_blitCmdList->RSSetScissorRects(1, &scissor);

    // The surround pass above changed the PSO. Re-set blit PSO and root sig.
    g_blitCmdList->SetPipelineState(g_blitPSO.Get());
    g_blitCmdList->SetGraphicsRootSignature(g_blitRootSig.Get());
    ID3D12DescriptorHeap* heaps[] = {g_blitSrvHeap.Get()};
    g_blitCmdList->SetDescriptorHeaps(1, heaps);
    g_blitCmdList->SetGraphicsRootDescriptorTable(0, g_blitSrvHeap->GetGPUDescriptorHandleForHeapStart());

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

    if (surroundRecorded) {
        // Spec v7 producer signal: bump fence on our queue, then push the
        // new await value to the runtime so its next per-frame strip-blit
        // queue-waits on it. The OpenXR call also keeps the registration
        // alive (clearing it would require passing NULL handle).
        g_surroundFenceValue++;
        renderer.commandQueue->Signal(g_surroundFence.Get(), g_surroundFenceValue);
        if (xr.pfnSetSharedTextureSurround2DFenceEXT && xr.session != XR_NULL_HANDLE) {
            xr.pfnSetSharedTextureSurround2DFenceEXT(xr.session,
                g_surroundHandle, g_sharedWidth, g_sharedHeight,
                g_surroundFenceHandle, g_surroundFenceValue);
        }
    }

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

// #439 — XR_EXT_local_3d_zone authoring harness (D3D12 port of the
// cube_texture_d3d11_win Phase-1 'Z' cycle):
//   0 no mask (rect-surround behavior)
//   1 Tier-1 whole-window 3D (full weave, no 2D anywhere)
//   2 Tier-2 single rect == the canvas rect (must match the analytic
//     rect-surround output inside the window — impl doc §6 case 3)
//   3 Tier-2 multi-rect: three disconnected 3D islands
//   4 Tier-3 freeform: CPU radial gradient uploaded onto the mask resource
//     (soft 2D↔3D edge — validates the mask-lerp, impl doc §6 case 4)
static void ZoneMaskApplyNextState(XrSessionManager& xr, D3D12Renderer& renderer) {
    if (!g_zone.available || xr.session == XR_NULL_HANDLE) {
        LOG_WARN("Zone mask: XR_EXT_local_3d_zone not available on this runtime");
        return;
    }

    int next = (g_zone.state + 1) % 5;

    if (next == 0) {
        if (g_zone.mask != XR_NULL_HANDLE) {
            g_zone.pfnDestroy(g_zone.mask);
            g_zone.mask = XR_NULL_HANDLE;
        }
        g_zone.state = 0;
        LOG_INFO("Zone mask [0]: destroyed — rect-surround behavior restored");
        return;
    }

    if (g_zone.mask == XR_NULL_HANDLE) {
        XrLocal3DZoneCapabilitiesEXT caps = {XR_TYPE_LOCAL_3D_ZONE_CAPABILITIES_EXT};
        if (g_zone.pfnGetCaps && XR_SUCCEEDED(g_zone.pfnGetCaps(xr.session, &caps))) {
            LOG_INFO("Zone caps: supported=%d hwGrid=%ux%u maxMask=%ux%u", caps.supported,
                caps.hardwareZoneGridWidth, caps.hardwareZoneGridHeight,
                caps.maxMaskWidth, caps.maxMaskHeight);
        }
        XrLocal3DZoneMaskCreateInfoEXT ci = {XR_TYPE_LOCAL_3D_ZONE_MASK_CREATE_INFO_EXT};
        ci.maskWidth = 0;  // 0 = runtime chooses the client-window dims (#464)
        ci.maskHeight = 0;
        XrResult res = g_zone.pfnCreate(xr.session, &ci, &g_zone.mask);
        if (XR_FAILED(res)) {
            LogXrResult("xrCreateLocal3DZoneMaskEXT", res);
            return;
        }
    }

    XrResult res = XR_SUCCESS;
    switch (next) {
    case 1: // Tier 1 — whole window 3D
        res = g_zone.pfnSetWhole(g_zone.mask, XR_TRUE);
        LOG_INFO("Zone mask [1]: Tier-1 whole-window 3D (full weave)");
        break;
    case 2: { // Tier 2 — single rect == the canvas rect (centered 50%)
        XrRect2Di rect = {};
        rect.offset.x = (int32_t)(g_windowWidth / 4);
        rect.offset.y = (int32_t)(g_windowHeight / 4);
        rect.extent.width = (int32_t)g_canvasW;
        rect.extent.height = (int32_t)g_canvasH;
        res = g_zone.pfnSetRects(g_zone.mask, 1, &rect);
        LOG_INFO("Zone mask [2]: Tier-2 single rect == canvas (%d,%d %dx%d)",
            rect.offset.x, rect.offset.y, rect.extent.width, rect.extent.height);
        break;
    }
    case 3: { // Tier 2 — three disconnected 3D islands
        const int32_t w = (int32_t)g_windowWidth;
        const int32_t h = (int32_t)g_windowHeight;
        XrRect2Di rects[3] = {};
        rects[0].offset = {w / 8, h / 8};
        rects[0].extent = {w / 4, h / 4};
        rects[1].offset = {5 * w / 8, h / 8};
        rects[1].extent = {w / 4, h / 4};
        rects[2].offset = {3 * w / 8, 5 * h / 8};
        rects[2].extent = {w / 4, h / 4};
        res = g_zone.pfnSetRects(g_zone.mask, 3, rects);
        LOG_INFO("Zone mask [3]: Tier-2 multi-rect — 3 disconnected 3D islands");
        break;
    }
    case 4: { // Tier 3 — freeform radial gradient drawn by the app
        XrLocal3DZoneRenderTargetD3D12EXT binding = {XR_TYPE_LOCAL_3D_ZONE_RENDER_TARGET_D3D12_EXT};
        res = g_zone.pfnAcquireRT(g_zone.mask, &binding);
        if (XR_SUCCEEDED(res) && binding.resource != nullptr &&
            binding.width > 0 && binding.height > 0) {
            // The mask resource lives on the app's own device + queue
            // in-process (header v2 sync contract: same-queue submission
            // order, no fence). D3D12 has no UpdateSubresource — stage the
            // CPU radial gradient (M=1 core → soft falloff → M=0) through a
            // transient UPLOAD buffer + CopyTextureRegion, honoring the
            // RENDER_TARGET in/out state contract.
            ID3D12Resource* maskRes = static_cast<ID3D12Resource*>(binding.resource);
            const uint32_t mw = binding.width;
            const uint32_t mh = binding.height;
            const uint32_t pitch =
                (mw + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
            std::vector<uint8_t> pixels((size_t)pitch * mh, 0);
            const float cxf = mw * 0.5f;
            const float cyf = mh * 0.5f;
            const float r3d = 0.30f * (float)(mw < mh ? mw : mh);  // full-3D core
            const float feather = 0.5f * r3d;                      // soft edge
            for (uint32_t y = 0; y < mh; y++) {
                for (uint32_t x = 0; x < mw; x++) {
                    float dx = (float)x - cxf;
                    float dy = (float)y - cyf;
                    float d = sqrtf(dx * dx + dy * dy);
                    float m = 1.0f - (d - r3d) / feather;
                    m = m < 0.0f ? 0.0f : (m > 1.0f ? 1.0f : m);
                    pixels[(size_t)y * pitch + x] = (uint8_t)(m * 255.0f + 0.5f);
                }
            }

            // Transient UPLOAD buffer (alive through the WaitForGpu below).
            ComPtr<ID3D12Resource> upload;
            D3D12_HEAP_PROPERTIES up = {};
            up.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC bd = {};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = (UINT64)pitch * mh;
            bd.Height = 1;
            bd.DepthOrArraySize = 1;
            bd.MipLevels = 1;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            HRESULT hr = renderer.device->CreateCommittedResource(
                &up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr, IID_PPV_ARGS(&upload));
            if (FAILED(hr)) {
                LOG_WARN("Zone mask [4]: upload buffer creation failed (0x%08lx)", hr);
                break;
            }
            void* mapped = nullptr;
            if (FAILED(upload->Map(0, nullptr, &mapped)) || mapped == nullptr) {
                LOG_WARN("Zone mask [4]: upload buffer map failed");
                break;
            }
            memcpy(mapped, pixels.data(), pixels.size());
            upload->Unmap(0, nullptr);

            renderer.commandAllocator->Reset();
            renderer.commandList->Reset(renderer.commandAllocator.Get(), nullptr);

            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = maskRes;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            renderer.commandList->ResourceBarrier(1, &barrier);

            D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
            dstLoc.pResource = maskRes;
            dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dstLoc.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
            srcLoc.pResource = upload.Get();
            srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            srcLoc.PlacedFootprint.Offset = 0;
            srcLoc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8_UNORM;
            srcLoc.PlacedFootprint.Footprint.Width = mw;
            srcLoc.PlacedFootprint.Footprint.Height = mh;
            srcLoc.PlacedFootprint.Footprint.Depth = 1;
            srcLoc.PlacedFootprint.Footprint.RowPitch = pitch;
            renderer.commandList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

            // Back to RENDER_TARGET per the Tier-3 state contract.
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            renderer.commandList->ResourceBarrier(1, &barrier);

            renderer.commandList->Close();
            ID3D12CommandList* lists[] = {renderer.commandList.Get()};
            renderer.commandQueue->ExecuteCommandLists(1, lists);
            WaitForGpu(renderer);  // keeps `upload` alive until the copy lands

            LOG_INFO("Zone mask [4]: Tier-3 radial gradient (%ux%u, core r=%.0f, feather=%.0f)",
                mw, mh, r3d, feather);
        } else {
            LogXrResult("xrAcquireLocal3DZoneRenderTargetEXT", res);
        }
        break;
    }
    default:
        break;
    }
    if (XR_FAILED(res)) {
        LogXrResult("zone mask authoring", res);
        return;
    }

    res = g_zone.pfnSubmit(g_zone.mask);
    if (XR_FAILED(res)) {
        LogXrResult("xrSubmitLocal3DZoneEXT", res);
        return;
    }
    g_zone.state = next;
}

struct RenderState {
    HWND hwnd;
    XrSessionManager* xr;
    D3D12Renderer* renderer;
    HudRenderer* hudRenderer;
    bool hudOk;
    std::vector<XrSwapchainImageD3D12KHR>* swapchainImages;
    int rtvBaseIndex;
    // #433 DXR_TEST_PER_VIEW_SC: optional second swapchain so view 1 submits
    // from its own swapchain (Unity submission shape). XR_NULL_HANDLE when off.
    XrSwapchain swapchain2;
    std::vector<XrSwapchainImageD3D12KHR>* swapchain2Images;
    int rtv2BaseIndex;
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
    // Rendering mode requests (V=cycle, 0-8=absolute). Single source of
    // truth: the runtime owns current mode via xr.currentModeIndex.
    if (g_inputState.cycleRenderingModeRequested) {
        g_inputState.cycleRenderingModeRequested = false;
        if (xr.pfnRequestDisplayRenderingModeEXT && xr.session != XR_NULL_HANDLE &&
            xr.renderingModeCount > 0) {
            uint32_t next = (xr.currentModeIndex + 1) % xr.renderingModeCount;
            xr.pfnRequestDisplayRenderingModeEXT(xr.session, next);
        }
    }
    if (g_inputState.absoluteRenderingModeRequested >= 0) {
        uint32_t target = (uint32_t)g_inputState.absoluteRenderingModeRequested;
        g_inputState.absoluteRenderingModeRequested = -1;
        if (xr.pfnRequestDisplayRenderingModeEXT && xr.session != XR_NULL_HANDLE &&
            target < xr.renderingModeCount) {
            xr.pfnRequestDisplayRenderingModeEXT(xr.session, target);
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
    // #439 — apply a pending 'Z' zone-mask cycle (before any frame recording;
    // the Tier-3 path re-arms the renderer's command list for its upload).
    if (g_zoneCycleRequested) {
        g_zoneCycleRequested = false;
        ZoneMaskApplyNextState(xr, renderer);
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
            uint32_t modeViewCount = (xr.renderingModeCount > 0 && xr.currentModeIndex < xr.renderingModeCount)
                ? xr.renderingModeViewCounts[xr.currentModeIndex] : 2;
            uint32_t tileColumns = (xr.renderingModeCount > 0 && xr.currentModeIndex < xr.renderingModeCount)
                ? xr.renderingModeTileColumns[xr.currentModeIndex] : 2;
            uint32_t tileRows = (xr.renderingModeCount > 0 && xr.currentModeIndex < xr.renderingModeCount)
                ? xr.renderingModeTileRows[xr.currentModeIndex] : 1;
            bool monoMode = (xr.renderingModeCount > 0 && !xr.renderingModeDisplay3D[xr.currentModeIndex]);
            if (xr.renderingModeCount > 0 && xr.currentModeIndex < xr.renderingModeCount) {
                xr.recommendedViewScaleX = xr.renderingModeScaleX[xr.currentModeIndex];
                xr.recommendedViewScaleY = xr.renderingModeScaleY[xr.currentModeIndex];
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

                    // XR_EXT_view_rig (#396 W7): drive the runtime rig matching
                    // the app's current mode (C selects the rig) with the app's
                    // tunables — the runtime owns the canvas resolve and the
                    // Kooima math, and returns render-ready XrView{pose, fov}.
                    // Per-locate semantics: chain the rig on every consume
                    // locate.
                    const bool useAppProjection =
                        xr.hasDisplayInfoExt && xr.displayWidthM > 0.0f && g_hasViewRigExt;
                    const bool rigCamera = useAppProjection && g_inputState.cameraMode;
                    XrCameraRigEXT cameraRig = {XR_TYPE_CAMERA_RIG_EXT};
                    XrDisplayRigEXT displayRig = {XR_TYPE_DISPLAY_RIG_EXT};
                    XrPosef rigPose = {{0, 0, 0, 1}, {0, 0, 0}};
                    if (useAppProjection) {
                        XMVECTOR rigOri = XMQuaternionRotationRollPitchYaw(
                            g_inputState.pitch, g_inputState.yaw, 0);
                        XMFLOAT4 rq;
                        XMStoreFloat4(&rq, rigOri);
                        rigPose.orientation = {rq.x, rq.y, rq.z, rq.w};
                        rigPose.position = {g_inputState.cameraPosX, g_inputState.cameraPosY,
                                            g_inputState.cameraPosZ};
                        if (rigCamera) {
                            cameraRig.pose = rigPose;
                            cameraRig.ipdFactor = g_inputState.viewParams.ipdFactor;
                            cameraRig.parallaxFactor = g_inputState.viewParams.parallaxFactor;
                            cameraRig.convergenceDiopters = g_inputState.viewParams.invConvergenceDistance;
                            cameraRig.verticalFov =
                                2.0f * atanf(CAMERA_HALF_TAN_VFOV / g_inputState.viewParams.zoomFactor);
                            locateInfo.next = &cameraRig;
                        } else {
                            displayRig.pose = rigPose;
                            displayRig.virtualDisplayHeight =
                                g_inputState.viewParams.virtualDisplayHeight / g_inputState.viewParams.scaleFactor;
                            displayRig.ipdFactor = g_inputState.viewParams.ipdFactor;
                            displayRig.parallaxFactor = g_inputState.viewParams.parallaxFactor;
                            displayRig.perspectiveFactor = g_inputState.viewParams.perspectiveFactor;
                            locateInfo.next = &displayRig;
                        }
                    }
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

                    // #433 regression knobs: a runtime must treat any in-bounds
                    // sub-rect as a plain UV window, so the composited result
                    // must be pixel-identical with these set.
                    //   DXR_TEST_RECT_DELTA=<int>   extent = computed + N px
                    //                               (render viewport follows —
                    //                               mirrors Unity's floor()-
                    //                               rounded renderViewportScale)
                    //   DXR_TEST_RECT_ANCHOR=bottom rect anchored to the bottom
                    //                               of the swapchain instead of
                    //                               the top (Unity-style)
                    static const int testRectDelta = []() {
                        const char* e = getenv("DXR_TEST_RECT_DELTA");
                        return e != nullptr ? atoi(e) : 0;
                    }();
                    static const bool testRectBottomAnchor = []() {
                        const char* e = getenv("DXR_TEST_RECT_ANCHOR");
                        return e != nullptr && _stricmp(e, "bottom") == 0;
                    }();
                    if (testRectDelta != 0) {
                        int w = (int)renderW + testRectDelta;
                        int h = (int)renderH + testRectDelta;
                        renderW = (uint32_t)(w < 16 ? 16 : w);
                        renderH = (uint32_t)(h < 16 ? 16 : h);
                        static bool deltaLogged = false;
                        if (!deltaLogged) {
                            LOG_WARN("[#433] DXR_TEST_RECT_DELTA=%d -> submitting %ux%u rects",
                                     testRectDelta, renderW, renderH);
                            deltaLogged = true;
                        }
                    }

                    // --- Consume the runtime's render-ready XrView{pose, fov} ---
                    // Only clip policy (near/far + the GL→[0,1] depth remap)
                    // stays app-side, by design (fov is clip-independent).
                    // Camera rig: same absolute clip as the old app-side camera
                    // path. Display rig: ZDP-anchored clip (near = ez - vH,
                    // far = ez + 1000·vH; ez = rig-local z of the view pose).
                    std::vector<RigView> stereoViews(eyeCount);
                    if (useAppProjection) {
                        const float rigVH =
                            g_inputState.viewParams.virtualDisplayHeight / g_inputState.viewParams.scaleFactor;
                        for (int i = 0; i < eyeCount; i++) {
                            const XrView& v = rawViews[(i < (int)viewCount) ? i : 0];
                            ViewMatrixFromXrPose(v.pose, stereoViews[i].view_matrix);
                            float nearZ = 0.01f, farZ = 100.0f;
                            if (!rigCamera) {
                                float ez = RigLocalEyeZ(rigPose, v.pose.position);
                                nearZ = (ez - rigVH > 0.001f) ? (ez - rigVH) : 0.001f;
                                farZ = ez + 1000.0f * rigVH;
                            }
                            ProjectionFromXrFov(v.fov, nearZ, farZ, stereoViews[i].projection_matrix);
                            convert_projection_gl_to_zero_to_one(stereoViews[i].projection_matrix);
                            stereoViews[i].fov = v.fov;
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
                            modeText += g_surroundRegistered ?
                                L"\nSurround 2D: ACTIVE (spec v6)" :
                                L"\nSurround 2D: inactive";
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
                            dispText += L"\n" + FormatMode(xr.currentModeIndex, xr.pfnRequestDisplayRenderingModeEXT != nullptr,
                                (xr.renderingModeCount > 0 && xr.currentModeIndex < xr.renderingModeCount) ? xr.renderingModeNames[xr.currentModeIndex] : nullptr,
                                xr.renderingModeCount,
                                xr.renderingModeCount > 0 ? xr.renderingModeDisplay3D[xr.currentModeIndex] : true,
                                xr.renderingModeCount > 0 ? xr.renderingModeIsRequestable[xr.currentModeIndex] : true);
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
                        // #433: DXR_TEST_PER_VIEW_SC — Unity submission shape:
                        // each view in its OWN swapchain, identical rects.
                        uint32_t imageIndex2 = 0;
                        bool sc2Acquired = false;
                        if (rs.swapchain2 != XR_NULL_HANDLE && !monoMode) {
                            XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                            if (XR_SUCCEEDED(xrAcquireSwapchainImage(rs.swapchain2, &ai, &imageIndex2))) {
                                XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                                wi.timeout = XR_INFINITE_DURATION;
                                sc2Acquired = XR_SUCCEEDED(xrWaitSwapchainImage(rs.swapchain2, &wi));
                                if (!sc2Acquired) {
                                    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                                    xrReleaseSwapchainImage(rs.swapchain2, &ri);
                                }
                            }
                        }

                        for (int eye = 0; eye < eyeCount; eye++) {
                            const bool useSc2 = sc2Acquired && eye == 1;
                            uint32_t tileX = (monoMode || useSc2) ? 0 : (eye % tileColumns);
                            uint32_t tileY = (monoMode || useSc2) ? 0 : (eye / tileColumns);
                            uint32_t vpX = tileX * renderW;
                            uint32_t vpY = tileY * renderH;
                            if (testRectBottomAnchor) {
                                // #433: bottom-anchored rect, Unity-style — the
                                // render viewport moves with the submitted rect.
                                vpY = xr.swapchain.height - (tileY + 1) * renderH;
                                static bool anchorLogged = false;
                                if (!anchorLogged) {
                                    LOG_WARN("[#433] DXR_TEST_RECT_ANCHOR=bottom -> view0 rect=(%u,%u %ux%u) in %ux%u swapchain",
                                             vpX, vpY, renderW, renderH,
                                             xr.swapchain.width, xr.swapchain.height);
                                    anchorLogged = true;
                                }
                            }

                            ID3D12Resource* swapchainTexture = useSc2
                                ? (*rs.swapchain2Images)[imageIndex2].texture
                                : (*rs.swapchainImages)[imageIndex].texture;
                            int rtvIdx = useSc2
                                ? rs.rtv2BaseIndex + (int)imageIndex2
                                : rs.rtvBaseIndex + (int)imageIndex;

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
                                eye == 0 || useSc2);

                            projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                            projectionViews[eye].subImage.swapchain = useSc2
                                ? rs.swapchain2 : xr.swapchain.swapchain;
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

                        if (sc2Acquired) {
                            XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                            xrReleaseSwapchainImage(rs.swapchain2, &ri);
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

            // After xrEndFrame: blit shared texture to app window. We also
            // render the surround pattern inline (under keyed mutex) when
            // surround is registered.
            if (frameState.shouldRender) {
                static auto surroundStartTime = std::chrono::steady_clock::now();
                float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - surroundStartTime).count();
                BlitSharedTextureToBackBuffer(renderer, xr, t);
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

    // Spec v7 §3.7: full-window 2D surround texture with fence-based sync
    // (the only path that works on D3D12-native shared resources). On a
    // pre-v7 runtime the PFN lookup returns null and surround stays off —
    // the cube still weaves correctly via the canvas-only blit.
    bool surroundSetupOk = false;
    if (xr.pfnSetSharedTextureSurround2DFenceEXT) {
        surroundSetupOk = CreateSurroundPipeline(renderer.device.Get()) &&
                          CreateSurroundTextureD3D12(renderer.device.Get(),
                                                     g_sharedWidth, g_sharedHeight,
                                                     DXGI_FORMAT_B8G8R8A8_UNORM);
        if (!surroundSetupOk) {
            LOG_WARN("Surround 2D (fence) setup failed — continuing without surround");
        }
    } else {
        LOG_WARN("Runtime does not expose xrSetSharedTextureSurround2DFenceEXT (pre-spec-v7) — surround disabled");
    }

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

    // Seed the runtime with handle opens + initial await value of 0. The
    // fence's completed value is 0 at creation, so the first frame's
    // queue->Wait(fence, 0) returns immediately; subsequent per-frame
    // calls bump the await value.
    if (surroundSetupOk && xr.pfnSetSharedTextureSurround2DFenceEXT && g_surroundHandle) {
        XrResult sres = xr.pfnSetSharedTextureSurround2DFenceEXT(xr.session,
            g_surroundHandle, g_sharedWidth, g_sharedHeight,
            g_surroundFenceHandle, 0);
        if (XR_SUCCEEDED(sres)) {
            g_surroundRegistered = true;
            LOG_INFO("Registered surround 2D (fence) with runtime (%ux%u)",
                g_sharedWidth, g_sharedHeight);
        } else {
            LogXrResult("xrSetSharedTextureSurround2DFenceEXT", sres);
        }
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

    // #433 DXR_TEST_PER_VIEW_SC: create a second, identical swapchain so view 1
    // can submit from its own swapchain — mirrors Unity's per-view-swapchain
    // submission shape. The runtime must composite this identically to the
    // single-swapchain tiling. Created BEFORE the RTV pass because
    // CreateSwapchainRTVs (re)creates the RTV heap sized to its texture count —
    // both swapchains' images must go into ONE combined call.
    XrSwapchain swapchain2 = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D12KHR> swapchain2Images;
    int rtv2BaseIndex = 0;
    {
        const char* e = getenv("DXR_TEST_PER_VIEW_SC");
        if (e != nullptr && *e != '\0' && *e != '0') {
            XrSwapchainCreateInfo sci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
            sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
            sci.format = xr.swapchain.format;
            sci.sampleCount = 1;
            sci.width = xr.swapchain.width;
            sci.height = xr.swapchain.height;
            sci.faceCount = 1;
            sci.arraySize = 1;
            sci.mipCount = 1;
            XrResult r = xrCreateSwapchain(xr.session, &sci, &swapchain2);
            if (XR_SUCCEEDED(r)) {
                uint32_t count2 = 0;
                xrEnumerateSwapchainImages(swapchain2, 0, &count2, nullptr);
                swapchain2Images.resize(count2, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
                xrEnumerateSwapchainImages(swapchain2, count2, &count2,
                    (XrSwapchainImageBaseHeader*)swapchain2Images.data());
                LOG_WARN("[#433] DXR_TEST_PER_VIEW_SC=1 -> view 1 submits from its own %ux%u swapchain (%u images)",
                         xr.swapchain.width, xr.swapchain.height, count2);
            } else {
                LOG_WARN("[#433] DXR_TEST_PER_VIEW_SC: xrCreateSwapchain failed (%d), knob disabled", r);
                swapchain2 = XR_NULL_HANDLE;
            }
        }
    }

    // Enumerate D3D12 swapchain images and create RTVs (one combined heap for
    // both swapchains — see the #433 note above).
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
        rtv2BaseIndex = (int)count;
        for (const auto& img : swapchain2Images) {
            textures.push_back(img.texture);
        }

        rtvBaseIndex = (int)renderer.rtvCount;
        if (!CreateSwapchainRTVs(renderer, textures.data(), (uint32_t)textures.size(),
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
    LOG_INFO("Controls: WASD=Fly, Mouse=Look, Space=Reset, V=Mode, Z=ZoneMask, SHIFT+TAB=HUD, F11=Fullscreen, ESC=Quit");

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
    rs.swapchain2 = swapchain2;
    rs.swapchain2Images = &swapchain2Images;
    rs.rtv2BaseIndex = rtv2BaseIndex;
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

    // #439: destroy the zone mask before session teardown (the oxr handle
    // cascade would also do it; explicit keeps ordering obvious).
    if (g_zone.mask != XR_NULL_HANDLE && g_zone.pfnDestroy) {
        g_zone.pfnDestroy(g_zone.mask);
        g_zone.mask = XR_NULL_HANDLE;
        g_zone.state = 0;
    }

    // Clear surround registration before dropping our refs so the runtime
    // releases its opened ID3D12Resource + ID3D12Fence views of the shared
    // handles. Fence handle argument is ignored on the clear path.
    if (g_surroundRegistered && xr.pfnSetSharedTextureSurround2DFenceEXT && xr.session != XR_NULL_HANDLE) {
        xr.pfnSetSharedTextureSurround2DFenceEXT(xr.session, nullptr, 0, 0, nullptr, 0);
        g_surroundRegistered = false;
    }
    if (g_surroundHandle) {
        CloseHandle(g_surroundHandle);
        g_surroundHandle = nullptr;
    }
    if (g_surroundFenceHandle) {
        CloseHandle(g_surroundFenceHandle);
        g_surroundFenceHandle = nullptr;
    }
    g_surroundFence.Reset();
    g_surroundRtvHeap.Reset();
    g_surroundPSO.Reset();
    g_surroundRootSig.Reset();
    g_surroundTexture.Reset();

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
    if (swapchain2 != XR_NULL_HANDLE) {
        xrDestroySwapchain(swapchain2);
        swapchain2 = XR_NULL_HANDLE;
    }
    CleanupOpenXR(xr);
    if (hudOk) CleanupHudRenderer(hudRenderer);
    CleanupD3D12(renderer);

    DestroyWindow(hwnd);
    UnregisterClass(WINDOW_CLASS, hInstance);

    ShutdownLogging();
    return 0;
}
