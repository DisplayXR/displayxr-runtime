// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Cube Zones TEXTURE VK — XR_DXR_display_zones parity test (ADR-027), Windows VK leg
 *
 * VULKAN analog of cube_zones_texture_d3d11_win (issue #613 Phase 1). It is a
 * HYBRID, because the runtime's VK native compositor imports the app's shared
 * texture as a D3D11 KMT handle (VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT,
 * BGRA) — see comp_vk_native_compositor.c. So:
 *
 *  - D3D11 (kept verbatim from the D3D11 leg): the shared composite texture
 *    (D3D11 KMT, BGRA), the app's window present (DXGI swapchain + DComp
 *    transparent visual), the DXR_TEXDUMP readback, the blit-shared-to-backbuffer.
 *  - VULKAN (converted): the OpenXR graphics binding + the per-zone cube/grid
 *    rendering into the OpenXR (Vulkan) swapchains.
 *
 * ADAPTER-MATCH (the novel part): the runtime dictates the VkPhysicalDevice via
 * xrGetVulkanGraphicsDeviceKHR. The D3D11 device for the shared texture MUST be
 * created on the SAME GPU. Flow: (1) XR instance+system; (2) VK instance, get
 * the runtime VkPhysicalDevice, read its LUID; (3) create the D3D11 device on
 * the matching DXGI adapter; (4) create the D3D11 shared texture (KMT, BGRA);
 * (5) VK logical device; (6) xrCreateSession with VK binding chained to the
 * win32 window binding carrying the shared texture handle.
 *
 * The display-zones submission logic (the thing under test) is unchanged from
 * the D3D11 leg: two 3D zones + a Local2D strip + a per-frame wish mask, each
 * zone owning its own swapchain sized per xrGetDisplayZoneRecommendedViewSizeDXR.
 *
 * NOTE (no edge-fade pass): the D3D11 leg runs a content-alpha edge feather as a
 * D3D11 fullscreen pass on the zone RTV; the VK leg omits it (the SPIR-V
 * pipelines aren't a two-liner to extend, same call the macOS VK zones app
 * makes) — zone edges are hard. The fade recipe stays D3D11/Metal-validated.
 *
 * Keys (zones mode): M = cycle wish mode (AUTO / Tier-2 rects / Tier-3
 * feathered RT), O = toggle zone B overlap. DXR_ZONES_VALIDATE=1 chains the
 * validate bit. DXR_TEXDUMP dumps the shared-texture readback at frame ~150.
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <wrl/client.h>
#include <d3dcompiler.h> // blit shader compile
#include <d3d11_1.h>
#include <dxgi1_4.h>
#include <dcomp.h>       // transparent present: DComp visual over the desktop
#pragma comment(lib, "dcomp.lib")

#include "logging.h"
#include "input_handler.h"
#include "xr_session.h"
#include "vk_renderer.h"
#include "projection_depth.h"
#include "atlas_capture.h"

// stb_image_write is provided by displayxr::common's Windows impl TU. Match the
// C linkage (STBIWDEF resolves to extern "C" in C++).
extern "C" int stbi_write_png(const char* filename, int w, int h, int comp,
                              const void* data, int stride_in_bytes);

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

// Application name for logging
static const char* APP_NAME = "cube_zones_texture_vk_win";

// Window settings
static const wchar_t* WINDOW_CLASS = L"DXRCubeZonesTextureVKClass";
static const wchar_t* WINDOW_TITLE = L"VK Cube Zones TEXTURE — XR_DXR_display_zones parity test";

// Global state (single-threaded — all accessed from the main thread only)
static InputState g_inputState;
static bool g_running = true;
static XrSessionManager* g_xr = nullptr;
static UINT g_windowWidth = 1280;
static UINT g_windowHeight = 720;
static bool g_inSizeMove = false;   // True while user is dragging/resizing the window
static bool g_resizeNeeded = false; // Window size changed — resize app swapchain

// Fullscreen state
static bool g_fullscreen = false;
static RECT g_savedWindowRect = {};
static DWORD g_savedWindowStyle = 0;

// ---------------------------------------------------------------------------
// Texture-mode shared D3D11 texture (lifted from cube_texture_d3d11_win). The
// runtime composites the full-window multi-zone super-atlas DIRECTLY into this
// (the runtime's VK compositor imports it as a D3D11 KMT texture); the app
// blits it back to its own swapchain. NO output rect / 2D surround needed.
// ---------------------------------------------------------------------------
static ComPtr<ID3D11Texture2D> g_sharedTexture;
static ComPtr<ID3D11ShaderResourceView> g_sharedSRV;
static HANDLE g_sharedHandle = nullptr;
static uint32_t g_sharedWidth = 0;   // Shared texture size (= display pixels, worst case)
static uint32_t g_sharedHeight = 0;

// Blit shader resources (fullscreen-quad present of the shared texture).
static ComPtr<ID3D11VertexShader> g_blitVS;
static ComPtr<ID3D11PixelShader> g_blitPS;
static ComPtr<ID3D11SamplerState> g_blitSampler;
static ComPtr<ID3D11Buffer> g_blitParamsCB;

// DirectComposition objects for the transparent present. Must outlive the
// window: a composition swapchain alone is invisible without a visual tree.
static ComPtr<IDCompositionDevice> g_dcompDevice;
static ComPtr<IDCompositionTarget> g_dcompTarget;
static ComPtr<IDCompositionVisual> g_dcompVisual;

// Present-side D3D11 device (created on the LUID-matched adapter). Distinct
// from the VkRenderer used for cube rendering. We keep raw device/context/
// factory plus an app-side window swapchain + back-buffer RTV.
static ComPtr<ID3D11Device> g_d3dDevice;
static ComPtr<ID3D11DeviceContext> g_d3dContext;
static ComPtr<IDXGIFactory2> g_dxgiFactory;

// Autonomous-verification dump (DXR_TEXDUMP).
static std::string g_texDumpPath;
static bool g_texDumpEnabled = false;
static bool g_texDumpDone = false;
static long g_texFrameCounter = 0;
static const long kTexDumpFrame = 150;

// ---------------------------------------------------------------------------
// XR_DXR_display_zones state
// ---------------------------------------------------------------------------

static const uint32_t kNumZones = 2;

// Per-zone virtual display height in app units (shared by both zones).
static const float kZoneVirtualDisplayHeight = 0.30f;

struct DisplayZone {
    uint32_t zoneId = 0;
    XrRect2Di rect = {};            //!< client-window pixels; locate AND submit use this one variable
    float ipdFactor = 1.0f;
    float perspectiveFactor = 1.0f;
    float spinPhase = 0.0f;         //!< added to the shared cube rotation for this zone
    float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f}; //!< premultiplied RGBA
    XrSwapchain swapchain = XR_NULL_HANDLE;
    int64_t format = 0;
    uint32_t tileW = 0;             //!< per-view tile width (= recommended view width)
    uint32_t tileH = 0;             //!< per-view tile height
    uint32_t tileCount = 0;         //!< view tiles in the horizontally tiled swapchain
    std::vector<XrSwapchainImageVulkanKHR> images;
    ZoneFramebuffers fb;            //!< VK framebuffer set over the zone swapchain images
};
static DisplayZone g_zonesArr[kNumZones];

static const XrRect2Di kZoneARect        = {{0, 180}, {640, 540}};
static const XrRect2Di kZoneBRect        = {{700, 180}, {520, 360}};
static const XrRect2Di kZoneBOverlapRect = {{400, 300}, {520, 360}};
static const XrRect2Di kStripRect        = {{0, 0}, {1280, 180}};
static bool g_zoneBOverlap = false;

// Local2D strip (always on in zones mode; filled once via CPU upload).
struct StripLayer {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    uint32_t w = 0, h = 0;
};
static StripLayer g_strip;

// Zones activation: created a few frames in, once the session runs.
static bool g_zonesActive = false;
static bool g_zonesAttempted = false;
static long g_zonesFrameCounter = 0;
static const long kZonesActivationFrame = 10;

// Wish modes (M key): 0 AUTO, 1 explicit Tier-2 rects, 2 explicit Tier-3 mask.
static int g_wishMode = 0;

// DXR_ZONES_VALIDATE=1 — chain the validate bit on every frame-end info.
static bool ZonesValidateEnabled() {
    static const bool e = []() {
        const char* v = getenv("DXR_ZONES_VALIDATE");
        return v != nullptr && *v == '1';
    }();
    return e;
}

// ---------------------------------------------------------------------------

// Column-major view matrix from a render-ready XrView pose.
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

// Column-major GL ([-1,1] clip-z) off-axis projection. Pair with
// convert_projection_gl_to_zero_to_one() for Vulkan.
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

// Display-local eye distance for the ZDP-anchored clip.
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

// Forward declaration — defined after RenderState
struct RenderState;
static RenderState* g_renderState = nullptr;
static void RenderOneFrame(RenderState& rs);

// Toggle fullscreen mode for the app window
static void ToggleFullscreen(HWND hwnd) {
    if (g_fullscreen) {
        SetWindowLong(hwnd, GWL_STYLE, g_savedWindowStyle);
        SetWindowPos(hwnd, HWND_TOP,
            g_savedWindowRect.left, g_savedWindowRect.top,
            g_savedWindowRect.right - g_savedWindowRect.left,
            g_savedWindowRect.bottom - g_savedWindowRect.top,
            SWP_FRAMECHANGED);
        g_fullscreen = false;
        LOG_INFO("Exited fullscreen mode");
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
        LOG_INFO("Entered fullscreen mode");
    }
}

// Window procedure (runs on main thread — single-threaded, no locking needed)
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    UpdateInputState(g_inputState, msg, wParam, lParam);

    switch (msg) {
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        return 0;

    case WM_LBUTTONUP:
        ReleaseCapture();
        return 0;

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            g_windowWidth = LOWORD(lParam);
            g_windowHeight = HIWORD(lParam);
            g_resizeNeeded = true;
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

    case WM_TIMER:
        if (wParam == dxr_capture::kFlashTimerId) {
            dxr_capture::TickCaptureFlash(hwnd);
            return 0;
        }
        break;

    case dxr_capture::kFlashUserMsg:
        dxr_capture::TriggerCaptureFlash(hwnd);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// Transparent window background — ON BY DEFAULT for this app.
static bool TransparentBackgroundEnabled() {
    static const bool enabled = []() {
        const char* v = getenv("DISPLAYXR_TRANSPARENT_BG");
        return v == nullptr || *v == '\0' || *v != '0';
    }();
    return enabled;
}

// Create the application window
static HWND CreateAppWindow(HINSTANCE hInstance, int width, int height) {
    const bool transparent = TransparentBackgroundEnabled();
    LOG_INFO("Creating application window (%dx%d, transparent=%d)", width, height, transparent);

    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = transparent ? nullptr : (HBRUSH)GetStockObject(BLACK_BRUSH);
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
        transparent ? WS_EX_NOREDIRECTIONBITMAP : 0,
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

// ---------------------------------------------------------------------------
// LUID-matched D3D11 device creation (ADAPTER-MATCH). Creates a D3D11 device on
// the DXGI adapter whose AdapterLuid matches the runtime's VkPhysicalDevice
// LUID — required so the runtime's VK compositor can import this device's
// shared (KMT) texture.
// ---------------------------------------------------------------------------
static bool CreatePresentD3D11OnLUID(const LUID& targetLuid) {
    ComPtr<IDXGIFactory1> factory1;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), &factory1);
    if (FAILED(hr)) {
        LOG_ERROR("CreateDXGIFactory1 failed: 0x%08x", hr);
        return false;
    }

    ComPtr<IDXGIAdapter1> chosen;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory1->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 desc = {};
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            if (desc.AdapterLuid.LowPart == targetLuid.LowPart &&
                desc.AdapterLuid.HighPart == targetLuid.HighPart) {
                LOG_INFO("ADAPTER-MATCH: DXGI adapter %u (%ls) matches runtime VK LUID", i, desc.Description);
                chosen = adapter;
                break;
            }
        }
        adapter.Reset();
    }
    if (!chosen) {
        LOG_ERROR("ADAPTER-MATCH FAILED: no DXGI adapter matches the runtime VK device LUID "
                  "(0x%08lX%08lX) — KMT import would fail",
                  (unsigned long)targetLuid.HighPart, (unsigned long)targetLuid.LowPart);
        return false;
    }

    UINT createFlags = 0;
#ifdef _DEBUG
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL gotLevel;
    // DRIVER_TYPE_UNKNOWN is required when an explicit adapter is passed.
    hr = D3D11CreateDevice(chosen.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, createFlags,
        featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION,
        &g_d3dDevice, &gotLevel, &g_d3dContext);
    if (FAILED(hr)) {
        LOG_ERROR("D3D11CreateDevice on matched adapter failed: 0x%08x", hr);
        return false;
    }

    // Cache the DXGI factory for the window swapchain create.
    ComPtr<IDXGIDevice> dxgiDevice;
    hr = g_d3dDevice.As(&dxgiDevice);
    if (SUCCEEDED(hr)) {
        ComPtr<IDXGIAdapter> adp;
        if (SUCCEEDED(dxgiDevice->GetAdapter(&adp))) {
            adp->GetParent(__uuidof(IDXGIFactory2), &g_dxgiFactory);
        }
    }
    if (!g_dxgiFactory) {
        // Fall back to the enumerating factory (it is an IDXGIFactory1; QI to 2).
        factory1.As(&g_dxgiFactory);
    }

    LOG_INFO("Present D3D11 device created on matched adapter (feature level 0x%x)", gotLevel);
    return g_dxgiFactory != nullptr;
}

// ---------------------------------------------------------------------------
// Texture-mode blit (D3D11, lifted from cube_texture_d3d11_win)
// ---------------------------------------------------------------------------

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
    if (FAILED(hr)) return false;

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = 16;
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device->CreateBuffer(&cbd, nullptr, &g_blitParamsCB);
    return SUCCEEDED(hr);
}

// Blit the runtime-composited shared texture into the app's window back buffer.
static void BlitSharedTextureToBackBuffer(ID3D11RenderTargetView* backBufferRTV,
                                          uint32_t winW, uint32_t winH) {
    if (!g_sharedSRV || !backBufferRTV) return;

    g_d3dContext->OMSetRenderTargets(1, &backBufferRTV, nullptr);

    float vpW = (winW < g_sharedWidth) ? (float)winW : (float)g_sharedWidth;
    float vpH = (winH < g_sharedHeight) ? (float)winH : (float)g_sharedHeight;

    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = vpW;
    vp.Height = vpH;
    vp.MaxDepth = 1.0f;
    g_d3dContext->RSSetViewports(1, &vp);

    if (g_blitParamsCB && g_sharedWidth > 0 && g_sharedHeight > 0) {
        float uvParams[4] = {
            vpW / (float)g_sharedWidth, vpH / (float)g_sharedHeight,
            0.0f, 0.0f
        };
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(g_d3dContext->Map(g_blitParamsCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            memcpy(mapped.pData, uvParams, sizeof(uvParams));
            g_d3dContext->Unmap(g_blitParamsCB.Get(), 0);
        }
    }

    g_d3dContext->VSSetShader(g_blitVS.Get(), nullptr, 0);
    g_d3dContext->PSSetShader(g_blitPS.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srv = g_sharedSRV.Get();
    g_d3dContext->PSSetShaderResources(0, 1, &srv);
    ID3D11SamplerState* smp = g_blitSampler.Get();
    g_d3dContext->PSSetSamplers(0, 1, &smp);
    ID3D11Buffer* cb = g_blitParamsCB.Get();
    g_d3dContext->PSSetConstantBuffers(0, 1, &cb);
    g_d3dContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_d3dContext->IASetInputLayout(nullptr);
    g_d3dContext->Draw(3, 0);

    ID3D11ShaderResourceView* nullSRV = nullptr;
    g_d3dContext->PSSetShaderResources(0, 1, &nullSRV);
}

// Autonomous verification: read the shared texture back to a PNG.
static void DumpSharedTextureToPNG(const char* path) {
    if (!g_sharedTexture || path == nullptr || path[0] == '\0') return;
    if (g_sharedWidth == 0 || g_sharedHeight == 0) return;

    D3D11_TEXTURE2D_DESC desc = {};
    g_sharedTexture->GetDesc(&desc);

    D3D11_TEXTURE2D_DESC sdesc = desc;
    sdesc.Usage = D3D11_USAGE_STAGING;
    sdesc.BindFlags = 0;
    sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sdesc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = g_d3dDevice->CreateTexture2D(&sdesc, nullptr, &staging);
    if (FAILED(hr)) {
        LOG_WARN("TEXTURE READBACK: staging texture create failed (0x%08x) — skipped", hr);
        return;
    }

    g_d3dContext->CopyResource(staging.Get(), g_sharedTexture.Get());

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = g_d3dContext->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        LOG_WARN("TEXTURE READBACK: Map failed (0x%08x) — skipped", hr);
        return;
    }

    const uint32_t w = g_sharedWidth;
    const uint32_t h = g_sharedHeight;
    const uint8_t* base = (const uint8_t*)mapped.pData;
    const size_t srcStride = mapped.RowPitch;

    // Shared texture is B8G8R8A8_UNORM; swizzle B<->R to RGBA for stbi.
    std::vector<uint8_t> rgba((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t* srow = base + (size_t)y * srcStride;
        uint8_t* drow = rgba.data() + (size_t)y * w * 4;
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t* sp = srow + (size_t)x * 4; // B,G,R,A
            uint8_t* dp = drow + (size_t)x * 4;        // R,G,B,A
            dp[0] = sp[2];
            dp[1] = sp[1];
            dp[2] = sp[0];
            dp[3] = sp[3];
        }
    }
    g_d3dContext->Unmap(staging.Get(), 0);

    int ok = stbi_write_png(path, (int)w, (int)h, 4, rgba.data(), (int)(w * 4));
    if (ok) {
        LOG_WARN("TEXTURE READBACK DUMPED: %s", path);
    } else {
        LOG_WARN("TEXTURE READBACK FAILED to write: %s", path);
    }
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

// State passed to RenderOneFrame
struct RenderState {
    HWND hwnd;
    XrSessionManager* xr;
    VkRenderer* vk;
    std::vector<XrSwapchainImageVulkanKHR>* swapchainImages; // fallback single swapchain
    PerformanceStats* perfStats;
    // App-side D3D11 swapchain + back-buffer RTV for window presentation.
    ComPtr<IDXGISwapChain1> appSwapchain;
    ComPtr<ID3D11RenderTargetView> appBackBufferRTV;
    bool fallbackFbReady = false; // VK framebuffers for the fallback swapchain created
};

// Texture-mode present tail (D3D11): blit the shared texture into the app's own
// window swapchain, present, then run the autonomous-verification readback.
static void PresentAndMaybeDump(RenderState& rs) {
    if (g_resizeNeeded && rs.appSwapchain) {
        g_resizeNeeded = false;
        rs.appBackBufferRTV.Reset();
        HRESULT hr = rs.appSwapchain->ResizeBuffers(0, g_windowWidth, g_windowHeight,
                                                    DXGI_FORMAT_UNKNOWN, 0);
        if (SUCCEEDED(hr)) {
            ComPtr<ID3D11Texture2D> backBuf;
            rs.appSwapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), &backBuf);
            g_d3dDevice->CreateRenderTargetView(backBuf.Get(), nullptr, &rs.appBackBufferRTV);
        }
    }

    if (rs.appBackBufferRTV) {
        float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        g_d3dContext->ClearRenderTargetView(rs.appBackBufferRTV.Get(), clearColor);
        BlitSharedTextureToBackBuffer(rs.appBackBufferRTV.Get(), g_windowWidth, g_windowHeight);
        if (rs.appSwapchain) rs.appSwapchain->Present(1, 0);
    }

    if (g_texDumpEnabled && !g_texDumpDone && g_texFrameCounter >= kTexDumpFrame) {
        g_texDumpDone = true;
        DumpSharedTextureToPNG(g_texDumpPath.c_str());
    }
}

// ---------------------------------------------------------------------------
// Zones helpers (VK)
// ---------------------------------------------------------------------------

// Pick the swapchain format the strip + zones use. Prefer the session's main
// swapchain format (already chosen by CreateSwapchain). Falls back to BGRA8.
static int64_t PickZoneFormat(XrSessionManager& xr) {
    if (xr.swapchain.format != 0) return xr.swapchain.format;
    return (int64_t)VK_FORMAT_B8G8R8A8_UNORM;
}

// Find a memory type for the strip staging buffer.
static uint32_t FindMemTypeIdx(VkPhysicalDevice phys, uint32_t typeBits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return UINT32_MAX;
}

// Fill a VK swapchain image with CPU-built BGRA8 bytes via a host-visible
// staging buffer + vkCmdCopyBufferToImage, then transition to
// SHADER_READ_ONLY_OPTIMAL (mirrors cube_handle_vk_win's CreateAndFillL2DPanel).
static bool UploadBgraToVkImage(VkRenderer& vk, VkImage img, uint32_t w, uint32_t h,
                                const std::vector<uint8_t>& buf) {
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = buf.size();
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(vk.device, &bci, nullptr, &staging) != VK_SUCCESS) {
        LOG_ERROR("[zones] strip staging buffer create failed");
        return false;
    }
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(vk.device, staging, &mr);
    uint32_t memType = FindMemTypeIdx(vk.physicalDevice, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memType == UINT32_MAX) {
        vkDestroyBuffer(vk.device, staging, nullptr);
        LOG_ERROR("[zones] strip: no host-visible memory type");
        return false;
    }
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = memType;
    vkAllocateMemory(vk.device, &mai, nullptr, &stagingMem);
    vkBindBufferMemory(vk.device, staging, stagingMem, 0);
    void* mapped = nullptr;
    vkMapMemory(vk.device, stagingMem, 0, buf.size(), 0, &mapped);
    memcpy(mapped, buf.data(), buf.size());
    vkUnmapMemory(vk.device, stagingMem);

    VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = vk.commandPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(vk.device, &cbai, &cmd);
    VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &cbi);

    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &b);

    VkBufferImageCopy region = {};
    region.bufferRowLength = w;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd, staging, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &b);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(vk.graphicsQueue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(vk.graphicsQueue);
    vkFreeCommandBuffers(vk.device, vk.commandPool, 1, &cmd);
    vkDestroyBuffer(vk.device, staging, nullptr);
    vkFreeMemory(vk.device, stagingMem, nullptr);
    return true;
}

// Create the always-on Local2D strip swapchain and fill it once with a checker
// + label band (opaque). The layer references the released image every frame.
static bool CreateAndFillStrip(XrSessionManager& xr, VkRenderer& vk) {
    const uint32_t w = (uint32_t)kStripRect.extent.width;
    const uint32_t h = (uint32_t)kStripRect.extent.height;

    XrSwapchainCreateInfo sci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                     XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sci.format = PickZoneFormat(xr);
    sci.sampleCount = 1;
    sci.width = w;
    sci.height = h;
    sci.faceCount = 1;
    sci.arraySize = 1;
    sci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(xr.session, &sci, &g_strip.swapchain))) {
        LOG_ERROR("[zones] strip: xrCreateSwapchain failed");
        return false;
    }
    g_strip.w = w;
    g_strip.h = h;

    uint32_t n = 0;
    xrEnumerateSwapchainImages(g_strip.swapchain, 0, &n, nullptr);
    std::vector<XrSwapchainImageVulkanKHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    if (n == 0 || XR_FAILED(xrEnumerateSwapchainImages(g_strip.swapchain, n, &n,
                                                       (XrSwapchainImageBaseHeader*)imgs.data()))) {
        LOG_ERROR("[zones] strip: xrEnumerateSwapchainImages failed");
        return false;
    }

    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t idx = 0;
    if (XR_FAILED(xrAcquireSwapchainImage(g_strip.swapchain, &ai, &idx))) {
        return false;
    }
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(g_strip.swapchain, &wi);

    size_t stride = (size_t)w * 4; // BGRA8
    std::vector<uint8_t> buf(stride * h);
    for (uint32_t y = 0; y < h; y++) {
        uint8_t* row = buf.data() + (size_t)y * stride;
        for (uint32_t x = 0; x < w; x++) {
            uint8_t* px = row + (size_t)x * 4; // B,G,R,A
            bool label = (x >= 40 && x < 360 && y >= 70 && y < 110);
            if (label) {
                px[0] = 0;   // B
                px[1] = 170; // G
                px[2] = 255; // R
                px[3] = 255;
            } else {
                bool check = (((x / 24) + (y / 24)) & 1) != 0;
                uint8_t v = check ? 210 : 60;
                px[0] = v;
                px[1] = v;
                px[2] = v;
                px[3] = 255;
            }
        }
    }
    bool ok = UploadBgraToVkImage(vk, imgs[idx].image, w, h, buf);

    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(g_strip.swapchain, &ri);
    return ok;
}

// Create one zone's VK swapchain + framebuffer set, sized per
// xrGetDisplayZoneRecommendedViewSizeDXR, horizontally tiled per view.
static bool CreateZoneResources(XrSessionManager& xr, VkRenderer& vk,
                                DisplayZone& z, uint32_t viewCount) {
    XrExtent2Di rec = {};
    XrResult r = g_zones.pfnGetViewSize(xr.session, &z.rect, &rec);
    if (XR_FAILED(r) || rec.width <= 0 || rec.height <= 0) {
        LOG_ERROR("[zones] zone %u: xrGetDisplayZoneRecommendedViewSizeDXR failed (0x%x, %dx%d)",
                  z.zoneId, (unsigned)r, rec.width, rec.height);
        return false;
    }
    z.tileW = (uint32_t)rec.width;
    z.tileH = (uint32_t)rec.height;
    z.tileCount = viewCount;
    z.format = PickZoneFormat(xr);

    XrSwapchainCreateInfo sci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                     XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sci.format = z.format;
    sci.sampleCount = 1;
    sci.width = z.tileW * z.tileCount;
    sci.height = z.tileH;
    sci.faceCount = 1;
    sci.arraySize = 1;
    sci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(xr.session, &sci, &z.swapchain))) {
        LOG_ERROR("[zones] zone %u: xrCreateSwapchain failed (%ux%u)", z.zoneId, sci.width, sci.height);
        return false;
    }

    uint32_t n = 0;
    xrEnumerateSwapchainImages(z.swapchain, 0, &n, nullptr);
    z.images.resize(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    if (n == 0 || XR_FAILED(xrEnumerateSwapchainImages(z.swapchain, n, &n,
                                                       (XrSwapchainImageBaseHeader*)z.images.data()))) {
        LOG_ERROR("[zones] zone %u: xrEnumerateSwapchainImages failed", z.zoneId);
        return false;
    }

    std::vector<VkImage> vkImages(n);
    for (uint32_t i = 0; i < n; i++) vkImages[i] = z.images[i].image;

    if (!CreateZoneFramebuffers(vk, z.fb, vkImages.data(), n,
                                z.tileW * z.tileCount, z.tileH, (VkFormat)z.format)) {
        LOG_ERROR("[zones] zone %u: framebuffer creation failed", z.zoneId);
        return false;
    }

    LOG_INFO("[zones] zone %u: rect %d,%d %dx%d -> swapchain %ux%u (%u tiles of %ux%u)",
             z.zoneId, z.rect.offset.x, z.rect.offset.y, z.rect.extent.width, z.rect.extent.height,
             z.tileW * z.tileCount, z.tileH, z.tileCount, z.tileW, z.tileH);
    return true;
}

// One-time zones activation: capabilities check + per-zone swapchains + strip.
static void TryActivateZones(XrSessionManager& xr, VkRenderer& vk) {
    g_zonesAttempted = true;

    XrDisplayZoneCapabilitiesDXR caps = {XR_TYPE_DISPLAY_ZONE_CAPABILITIES_DXR};
    XrResult r = g_zones.pfnGetCaps(xr.session, &caps);
    if (XR_FAILED(r) || !caps.supported) {
        LOG_ERROR("[zones] xrGetDisplayZoneCapabilitiesDXR: rc=0x%x supported=%d — zones path disabled",
                  (unsigned)r, (int)caps.supported);
        g_hasDisplayZonesExt = false;
        return;
    }
    if (caps.maxZones3D < kNumZones) {
        LOG_ERROR("[zones] maxZones3D=%u < %u — zones path disabled", caps.maxZones3D, kNumZones);
        g_hasDisplayZonesExt = false;
        return;
    }
    LOG_INFO("[zones] capabilities: supported=1 maxZones3D=%u", caps.maxZones3D);

    // Allocate the zone swapchains at the MAX view count across all modes (see
    // the D3D11 leg's note + #551). The per-frame submit clamp to the ACTIVE
    // mode's view count stays.
    uint32_t viewCount = 2;
    if (xr.renderingModeCount > 0) {
        viewCount = 1;
        for (uint32_t mi = 0; mi < xr.renderingModeCount; mi++) {
            viewCount = (std::max)(viewCount, xr.renderingModeViewCounts[mi]);
        }
    }
    if (viewCount < 1) viewCount = 1;
    if (viewCount > 8) viewCount = 8;

    // Zone A: left, below the strip. Identity rig, phase 0, dark-red clear.
    g_zonesArr[0].zoneId = 1;
    g_zonesArr[0].rect = kZoneARect;
    g_zonesArr[0].ipdFactor = 1.0f;
    g_zonesArr[0].perspectiveFactor = 1.0f;
    g_zonesArr[0].spinPhase = 0.0f;
    g_zonesArr[0].clearColor[0] = 0.15f;
    g_zonesArr[0].clearColor[1] = 0.03f;
    g_zonesArr[0].clearColor[2] = 0.03f;
    g_zonesArr[0].clearColor[3] = 1.0f;

    // Zone B: right. Reduced view spread + flattened perspective, phase +1.5
    // rad, FULLY TRANSPARENT clear (premultiplied RGBA all-zero): the cube
    // floats over the LIVE desktop. The cube itself stays opaque.
    g_zonesArr[1].zoneId = 2;
    g_zonesArr[1].rect = g_zoneBOverlap ? kZoneBOverlapRect : kZoneBRect;
    g_zonesArr[1].ipdFactor = 0.6f;
    g_zonesArr[1].perspectiveFactor = 0.5f;
    g_zonesArr[1].spinPhase = 1.5f;
    g_zonesArr[1].clearColor[0] = 0.0f;
    g_zonesArr[1].clearColor[1] = 0.0f;
    g_zonesArr[1].clearColor[2] = 0.0f;
    g_zonesArr[1].clearColor[3] = 0.0f;

    for (uint32_t zi = 0; zi < kNumZones; zi++) {
        if (!CreateZoneResources(xr, vk, g_zonesArr[zi], viewCount)) {
            g_hasDisplayZonesExt = false;
            return;
        }
    }

    if (!CreateAndFillStrip(xr, vk)) {
        g_hasDisplayZonesExt = false;
        return;
    }

    g_zonesActive = true;
    LOG_INFO("[zones] ACTIVE: zone A %d,%d %dx%d + zone B %d,%d %dx%d + strip %d,%d %dx%d "
             "(views=%u, wish mode 0 AUTO, validate=%d) — M=wish mode, O=overlap toggle",
             kZoneARect.offset.x, kZoneARect.offset.y, kZoneARect.extent.width, kZoneARect.extent.height,
             g_zonesArr[1].rect.offset.x, g_zonesArr[1].rect.offset.y,
             g_zonesArr[1].rect.extent.width, g_zonesArr[1].rect.extent.height,
             kStripRect.offset.x, kStripRect.offset.y, kStripRect.extent.width, kStripRect.extent.height,
             viewCount, ZonesValidateEnabled() ? 1 : 0);
}

// Lazily create the one mask handle shared by wish modes 1 and 2.
static bool EnsureWishMask(XrSessionManager& xr) {
    if (g_zone.mask != XR_NULL_HANDLE) return true;
    if (!g_zone.pfnCreate) return false;
    XrLocal3DZoneMaskCreateInfoDXR mci = {(XrStructureType)XR_TYPE_LOCAL_3D_ZONE_MASK_CREATE_INFO_DXR};
    mci.maskWidth = 0; // runtime picks the window backing size
    mci.maskHeight = 0;
    XrResult r = g_zone.pfnCreate(xr.session, &mci, &g_zone.mask);
    if (XR_FAILED(r)) {
        LOG_ERROR("[zones] xrCreateLocal3DZoneMaskDXR failed (0x%x)", (unsigned)r);
        g_zone.mask = XR_NULL_HANDLE;
        return false;
    }
    LOG_INFO("[zones] wish mask created (window backing size)");
    return true;
}

// Re-author the wish for the current mode. Mode 0 authors nothing (AUTO).
// NOTE: Tier-3 (mode 2) freeform RT authoring is a D3D11 ClearView pass in the
// D3D11 leg; the VK leg only supports AUTO + Tier-2 rects (the wish mask is a
// runtime-side concept, not VK-rendered) — mode 2 falls back to creating the
// mask + Tier-2 rects so the wish is still expressed, matching the macOS VK app
// which also stops at Tier-2.
static void ApplyWishAuthoring(XrSessionManager& xr) {
    if (g_wishMode >= 1) {
        if (!EnsureWishMask(xr)) return;
        XrRect2Di rects[kNumZones];
        for (uint32_t zi = 0; zi < kNumZones; zi++) rects[zi] = g_zonesArr[zi].rect;
        XrResult r = g_zone.pfnSetRects(g_zone.mask, kNumZones, rects);
        if (XR_FAILED(r)) {
            LOG_ERROR("[zones] xrSetLocal3DZoneFromRectsDXR failed (0x%x)", (unsigned)r);
        }
    }
}

static const char* WishModeName(int mode) {
    switch (mode) {
    case 1: return "explicit Tier-2 rects";
    case 2: return "explicit Tier-2 rects (Tier-3 RT not available on VK leg)";
    default: return "AUTO";
    }
}

// Edge-triggered M (wish mode cycle) + O (zone B overlap toggle) polling.
static void HandleZoneKeys(XrSessionManager& xr) {
    static bool mPrev = false, oPrev = false;

    const bool mNow = (GetAsyncKeyState('M') & 0x8000) != 0;
    if (mNow && !mPrev) {
        g_wishMode = (g_wishMode + 1) % 3;
        LOG_INFO("[zones] wish mode %d (%s)", g_wishMode, WishModeName(g_wishMode));
        ApplyWishAuthoring(xr);
    }
    mPrev = mNow;

    const bool oNow = (GetAsyncKeyState('O') & 0x8000) != 0;
    if (oNow && !oPrev) {
        g_zoneBOverlap = !g_zoneBOverlap;
        g_zonesArr[1].rect = g_zoneBOverlap ? kZoneBOverlapRect : kZoneBRect;
        LOG_INFO("[zones] zone B rect -> %d,%d %dx%d (%s zone A)",
                 g_zonesArr[1].rect.offset.x, g_zonesArr[1].rect.offset.y,
                 g_zonesArr[1].rect.extent.width, g_zonesArr[1].rect.extent.height,
                 g_zoneBOverlap ? "OVERLAPPING" : "beside");
        ApplyWishAuthoring(xr);
    }
    oPrev = oNow;
}

// Per-frame zones path: zone-scoped locate, per-zone VK render, submit
// [projA, projB, strip] with the zone structs chained on the projections.
static void RenderZonesFrame(RenderState& rs, const XrFrameState& frameState) {
    XrSessionManager& xr = *rs.xr;
    VkRenderer& vk = *rs.vk;

    XrDisplayZoneDXR zoneStructs[kNumZones];
    XrDisplayRigDXR rigStructs[kNumZones];
    std::vector<XrCompositionLayerProjectionView> projViews[kNumZones];
    uint32_t submitViewCounts[kNumZones] = {};

    for (uint32_t zi = 0; zi < kNumZones; zi++) {
        DisplayZone& z = g_zonesArr[zi];

        rigStructs[zi] = {XR_TYPE_DISPLAY_RIG_DXR};
        rigStructs[zi].pose = {{0, 0, 0, 1}, {0, 0, 0}};
        rigStructs[zi].virtualDisplayHeight = kZoneVirtualDisplayHeight;
        rigStructs[zi].ipdFactor = z.ipdFactor;
        rigStructs[zi].parallaxFactor = 1.0f;
        rigStructs[zi].perspectiveFactor = z.perspectiveFactor;

        zoneStructs[zi] = {XR_TYPE_DISPLAY_ZONE_DXR};
        zoneStructs[zi].next = &rigStructs[zi];
        zoneStructs[zi].zoneId = z.zoneId;
        zoneStructs[zi].rect = z.rect;

        XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
        locateInfo.next = &zoneStructs[zi];
        locateInfo.viewConfigurationType = xr.viewConfigType;
        locateInfo.displayTime = frameState.predictedDisplayTime;
        locateInfo.space = xr.localSpace;

        XrViewState viewState = {XR_TYPE_VIEW_STATE};
        uint32_t viewCountOutput = 0;
        XrView zoneViews[8];
        for (uint32_t vi = 0; vi < 8; vi++) zoneViews[vi] = {XR_TYPE_VIEW};
        XrResult lr = xrLocateViews(xr.session, &locateInfo, &viewState, 8, &viewCountOutput, zoneViews);
        if (XR_FAILED(lr) || viewCountOutput == 0) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                LOG_WARN("[zones] zone %u zone-scoped xrLocateViews failed (0x%x)", z.zoneId, (unsigned)lr);
            }
            submitViewCounts[zi] = 0;
            continue;
        }
        // Don't submit poses the runtime marked invalid (or zero quats from a
        // not-yet-tracking first frame).
        const bool orientationValid =
            (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
        bool posesUsable = orientationValid;
        for (uint32_t vi = 0; posesUsable && vi < viewCountOutput && vi < 8; vi++) {
            const XrQuaternionf& q = zoneViews[vi].pose.orientation;
            if (q.x == 0.0f && q.y == 0.0f && q.z == 0.0f && q.w == 0.0f) {
                posesUsable = false;
            }
        }
        if (!posesUsable) {
            static bool warnedInvalid = false;
            if (!warnedInvalid) {
                warnedInvalid = true;
                LOG_WARN("[zones] zone %u locate returned invalid poses (flags=0x%llx) — "
                         "skipping submission until tracking is up",
                         z.zoneId, (unsigned long long)viewState.viewStateFlags);
            }
            submitViewCounts[zi] = 0;
            continue;
        }

        // Clamp the submission to the ACTIVE mode's view count (#542).
        uint32_t activeViewCount = viewCountOutput;
        if (xr.renderingModeCount > 0 && xr.currentModeIndex < xr.renderingModeCount) {
            activeViewCount = xr.renderingModeViewCounts[xr.currentModeIndex];
        }
        const uint32_t n = (std::min)((std::min)(viewCountOutput, z.tileCount), activeViewCount);
        submitViewCounts[zi] = n;
        projViews[zi].assign(n, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});

        // Render-ready views -> VK eye render params. ZDP-anchored clip.
        std::vector<EyeRenderParams> eyes(n);
        for (uint32_t vi = 0; vi < n; vi++) {
            const XrView& v = zoneViews[vi];
            float vm[16], pm[16];
            ViewMatrixFromXrPose(v.pose, vm);
            const float ez = RigLocalEyeZ(rigStructs[zi].pose, v.pose.position);
            const float vH = kZoneVirtualDisplayHeight;
            const float nearZ = (ez - vH > 0.001f) ? (ez - vH) : 0.001f;
            const float farZ = ez + 1000.0f * vH;
            ProjectionFromXrFov(v.fov, nearZ, farZ, pm);
            convert_projection_gl_to_zero_to_one(pm);
            eyes[vi].viewMatrix = ColumnMajorToXMMatrix(vm);
            eyes[vi].projMatrix = ColumnMajorToXMMatrix(pm);
            eyes[vi].viewportX = vi * z.tileW;
            eyes[vi].viewportY = 0;
            eyes[vi].width = z.tileW;
            eyes[vi].height = z.tileH;

            projViews[zi][vi].subImage.swapchain = z.swapchain;
            projViews[zi][vi].subImage.imageRect.offset = {(int32_t)(vi * z.tileW), 0};
            projViews[zi][vi].subImage.imageRect.extent = {(int32_t)z.tileW, (int32_t)z.tileH};
            projViews[zi][vi].subImage.imageArrayIndex = 0;
            projViews[zi][vi].pose = v.pose;
            projViews[zi][vi].fov = v.fov;
        }

        // Acquire this zone's swapchain image and render every view tile.
        XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        uint32_t imageIndex = 0;
        if (XR_FAILED(xrAcquireSwapchainImage(z.swapchain, &ai, &imageIndex))) {
            submitViewCounts[zi] = 0;
            continue;
        }
        XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = XR_INFINITE_DURATION;
        xrWaitSwapchainImage(z.swapchain, &wi);

        // Per-zone clear (premultiplied RGBA) + spin phase via RenderSceneToZone.
        RenderSceneToZone(vk, z.fb, imageIndex, eyes.data(), (int)n,
                          z.clearColor, z.spinPhase);

        XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(z.swapchain, &ri);
    }

    // Layer list: [projA (zone A chained), projB (zone B chained), strip].
    XrCompositionLayerProjection projLayers[kNumZones];
    XrCompositionLayerLocal2DDXR stripLayer = {(XrStructureType)XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_DXR};
    const XrCompositionLayerBaseHeader* layers[kNumZones + 1] = {};
    uint32_t layerCount = 0;

    for (uint32_t zi = 0; zi < kNumZones; zi++) {
        if (submitViewCounts[zi] == 0) continue;
        projLayers[zi] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        projLayers[zi].next = &zoneStructs[zi]; // SAME instance as the locate chain
        projLayers[zi].layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        projLayers[zi].space = xr.localSpace;
        projLayers[zi].viewCount = submitViewCounts[zi];
        projLayers[zi].views = projViews[zi].data();
        layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&projLayers[zi];
    }

    if (g_strip.swapchain != XR_NULL_HANDLE) {
        stripLayer.layerFlags = 0; // opaque content
        stripLayer.subImage.swapchain = g_strip.swapchain;
        stripLayer.subImage.imageRect.offset = {0, 0};
        stripLayer.subImage.imageRect.extent = {(int32_t)g_strip.w, (int32_t)g_strip.h};
        stripLayer.subImage.imageArrayIndex = 0;
        stripLayer.rect = kStripRect;
        layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&stripLayer;
    }

    // Zones alpha-composite against the desktop by design — submit ALPHA_BLEND
    // whenever the runtime advertises it.
    static XrEnvironmentBlendMode zonesBlendMode = []() {
        if (!TransparentBackgroundEnabled()) {
            LOG_INFO("[zones] DISPLAYXR_TRANSPARENT_BG=0 — submitting OPAQUE (black window floor)");
            return XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        }
        XrEnvironmentBlendMode modes[8];
        uint32_t count = 0;
        if (g_xr != nullptr &&
            XR_SUCCEEDED(xrEnumerateEnvironmentBlendModes(g_xr->instance, g_xr->systemId,
                                                          g_xr->viewConfigType, 8, &count, modes))) {
            for (uint32_t i = 0; i < count; i++) {
                if (modes[i] == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND) {
                    LOG_INFO("[zones] runtime advertises ALPHA_BLEND — compositing zones over the desktop");
                    return XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND;
                }
            }
        }
        LOG_WARN("[zones] ALPHA_BLEND not advertised — zones composite over an opaque black window");
        return XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    }();

    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = zonesBlendMode;
    endInfo.layerCount = layerCount;
    endInfo.layers = layers;

    // Per-frame wish reference: absent in AUTO (mode 0) unless validation is
    // requested; in modes 1/2 the mask is the frame's wish.
    XrDisplayZonesFrameEndInfoDXR zonesEnd = {(XrStructureType)XR_TYPE_DISPLAY_ZONES_FRAME_END_INFO_DXR};
    zonesEnd.flags = 0;
    zonesEnd.wishMask = XR_NULL_HANDLE;
    bool chainZonesEnd = false;
    if (g_wishMode >= 1 && g_zone.mask != XR_NULL_HANDLE) {
        zonesEnd.wishMask = g_zone.mask;
        chainZonesEnd = true;
    }
    if (ZonesValidateEnabled()) {
        zonesEnd.flags |= XR_DISPLAY_ZONES_FRAME_END_VALIDATE_BIT_DXR;
        chainZonesEnd = true;
    }
    if (chainZonesEnd) {
        endInfo.next = &zonesEnd;
    }

    xrEndFrame(xr.session, &endInfo);

    // Texture-mode present + autonomous verification.
    PresentAndMaybeDump(rs);
}

// ---------------------------------------------------------------------------

// Render a single frame.
static void RenderOneFrame(RenderState& rs) {
    XrSessionManager& xr = *rs.xr;
    VkRenderer& vk = *rs.vk;

    UpdatePerformanceStats(*rs.perfStats);
    UpdateCameraMovement(g_inputState, rs.perfStats->deltaTime, rs.xr->displayHeightM);

    if (g_inputState.fullscreenToggleRequested) {
        ToggleFullscreen(rs.hwnd);
        g_inputState.fullscreenToggleRequested = false;
    }

    // Rendering mode requests (V=cycle, 0-8=jump).
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
            XrEyeTrackingModeDXR newMode = (xr.activeEyeTrackingMode == XR_EYE_TRACKING_MODE_MANAGED_DXR)
                ? XR_EYE_TRACKING_MODE_MANUAL_DXR : XR_EYE_TRACKING_MODE_MANAGED_DXR;
            XrResult etResult = xr.pfnRequestEyeTrackingModeEXT(xr.session, newMode);
            LOG_INFO("Eye tracking mode -> %s (%s)",
                newMode == XR_EYE_TRACKING_MODE_MANUAL_DXR ? "MANUAL" : "MANAGED",
                XR_SUCCEEDED(etResult) ? "OK" : "unsupported");
        }
    }

    // Update scene (cube rotation) — both zones share the rotation, offset in phase.
    UpdateScene(vk, rs.perfStats->deltaTime, xr.spinSpeed);

    PollEvents(xr);

    if (!xr.sessionRunning) {
        Sleep(100);
        return;
    }

    XrFrameState frameState;
    if (!BeginFrame(xr, frameState)) {
        return;
    }

    g_texFrameCounter++;

    // ---- zones path -------------------------------------------------------
    g_zonesFrameCounter++;
    if (g_hasDisplayZonesExt && !g_zonesActive && !g_zonesAttempted &&
        g_zonesFrameCounter >= kZonesActivationFrame) {
        TryActivateZones(xr, vk);
    }
    if (g_zonesActive && g_hasDisplayZonesExt) {
        HandleZoneKeys(xr);
        if (frameState.shouldRender) {
            RenderZonesFrame(rs, frameState);
        } else {
            XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
            endInfo.displayTime = frameState.predictedDisplayTime;
            endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            endInfo.layerCount = 0;
            endInfo.layers = nullptr;
            xrEndFrame(xr.session, &endInfo);
        }
        return;
    }

    // ---- fallback: plain single-projection cube (extension missing, zones
    // activation failed, or pre-activation frames) --------------------------
    // NOTE: the fallback path renders the cube via VK into the single
    // xr.swapchain (no HUD layer on the VK leg — the zones path, which is the
    // focus, never submitted a HUD). It exists so the app degrades gracefully
    // when DISPLAYXR_ZONES is off.
    uint32_t modeViewCount = (xr.renderingModeCount > 0 && xr.currentModeIndex < xr.renderingModeCount)
        ? xr.renderingModeViewCounts[xr.currentModeIndex] : 2;
    uint32_t tileColumns = (xr.renderingModeCount > 0 && xr.currentModeIndex < xr.renderingModeCount)
        ? xr.renderingModeTileColumns[xr.currentModeIndex] : 2;
    bool monoMode = (xr.renderingModeCount > 0 && !xr.renderingModeDisplay3D[xr.currentModeIndex]);
    if (xr.renderingModeCount > 0 && xr.currentModeIndex < xr.renderingModeCount) {
        xr.recommendedViewScaleX = xr.renderingModeScaleX[xr.currentModeIndex];
        xr.recommendedViewScaleY = xr.renderingModeScaleY[xr.currentModeIndex];
    }
    int eyeCount = monoMode ? 1 : (int)modeViewCount;
    std::vector<XrCompositionLayerProjectionView> projectionViews(eyeCount, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
    bool viewsPopulated = false;

    if (frameState.shouldRender) {
        XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
        locateInfo.viewConfigurationType = xr.viewConfigType;
        locateInfo.displayTime = frameState.predictedDisplayTime;
        locateInfo.space = xr.localSpace;

        XrViewState viewState = {XR_TYPE_VIEW_STATE};
        uint32_t viewCount = 8;
        XrView rawViews[8];
        for (uint32_t vi = 0; vi < 8; vi++) rawViews[vi] = {XR_TYPE_VIEW};

        // Drive the runtime rig (display-centric) for a render-ready view set.
        XrDisplayRigDXR displayRig = {XR_TYPE_DISPLAY_RIG_DXR};
        XrPosef rigPose = {{0, 0, 0, 1}, {0, 0, 0}};
        const bool useAppProjection =
            xr.hasDisplayInfoExt && xr.displayWidthM > 0.0f && g_hasViewRigExt;
        if (useAppProjection) {
            XMVECTOR rigOri = XMQuaternionRotationRollPitchYaw(
                g_inputState.pitch, g_inputState.yaw, 0);
            XMFLOAT4 rq;
            XMStoreFloat4(&rq, rigOri);
            rigPose.orientation = {rq.x, rq.y, rq.z, rq.w};
            rigPose.position = {g_inputState.cameraPosX, g_inputState.cameraPosY, g_inputState.cameraPosZ};
            displayRig.pose = rigPose;
            displayRig.virtualDisplayHeight =
                g_inputState.viewParams.virtualDisplayHeight / g_inputState.viewParams.scaleFactor;
            displayRig.ipdFactor = g_inputState.viewParams.ipdFactor;
            displayRig.parallaxFactor = g_inputState.viewParams.parallaxFactor;
            displayRig.perspectiveFactor = g_inputState.viewParams.perspectiveFactor;
            locateInfo.next = &displayRig;
        }

        XrResult lr = xrLocateViews(xr.session, &locateInfo, &viewState, 8, &viewCount, rawViews);
        const bool orientationValid =
            (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
        if (XR_SUCCEEDED(lr) && viewCount > 0 && orientationValid && useAppProjection) {
            // Max per-tile capacity from swapchain
            uint32_t maxTileW = tileColumns > 0 ? xr.swapchain.width / tileColumns : xr.swapchain.width;
            uint32_t maxTileH = xr.swapchain.height;
            uint32_t renderW = (uint32_t)(g_windowWidth * xr.recommendedViewScaleX);
            uint32_t renderH = (uint32_t)(g_windowHeight * xr.recommendedViewScaleY);
            if (renderW > maxTileW) renderW = maxTileW;
            if (renderH > maxTileH) renderH = maxTileH;

            const float rigVH =
                g_inputState.viewParams.virtualDisplayHeight / g_inputState.viewParams.scaleFactor;
            std::vector<EyeRenderParams> eyes(eyeCount);
            for (int i = 0; i < eyeCount; i++) {
                const XrView& v = rawViews[(i < (int)viewCount) ? i : 0];
                float vm[16], pm[16];
                ViewMatrixFromXrPose(v.pose, vm);
                float ez = RigLocalEyeZ(rigPose, v.pose.position);
                float nearZ = (ez - rigVH > 0.001f) ? (ez - rigVH) : 0.001f;
                float farZ = ez + 1000.0f * rigVH;
                ProjectionFromXrFov(v.fov, nearZ, farZ, pm);
                convert_projection_gl_to_zero_to_one(pm);
                uint32_t tileX = monoMode ? 0 : (uint32_t)i;
                eyes[i].viewMatrix = ColumnMajorToXMMatrix(vm);
                eyes[i].projMatrix = ColumnMajorToXMMatrix(pm);
                eyes[i].viewportX = tileX * renderW;
                eyes[i].viewportY = 0;
                eyes[i].width = renderW;
                eyes[i].height = renderH;
            }

            uint32_t imageIndex;
            if (AcquireSwapchainImage(xr, imageIndex)) {
                RenderScene(vk, imageIndex, xr.swapchain.width, xr.swapchain.height,
                            eyes.data(), eyeCount, 1.0f);
                for (int eye = 0; eye < eyeCount; eye++) {
                    uint32_t tileX = monoMode ? 0 : (uint32_t)eye;
                    projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                    projectionViews[eye].subImage.swapchain = xr.swapchain.swapchain;
                    projectionViews[eye].subImage.imageRect.offset = {
                        (int32_t)(tileX * eyes[eye].width), 0};
                    projectionViews[eye].subImage.imageRect.extent = {
                        (int32_t)eyes[eye].width, (int32_t)eyes[eye].height};
                    projectionViews[eye].subImage.imageArrayIndex = 0;
                    int safeIdx = (eye < (int)viewCount) ? eye : 0;
                    projectionViews[eye].pose = rawViews[safeIdx].pose;
                    projectionViews[eye].fov = rawViews[safeIdx].fov;
                }
                viewsPopulated = true;

                if (g_inputState.captureAtlasRequested) {
                    g_inputState.captureAtlasRequested = false;
                    dxr_capture::RequestRuntimeAtlasCapture(
                        xr, APP_NAME, tileColumns, 1, rs.hwnd);
                }

                ReleaseSwapchainImage(xr);
            }
        }
    }

    if (!viewsPopulated) {
        XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime = frameState.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount = 0;
        endInfo.layers = nullptr;
        xrEndFrame(xr.session, &endInfo);
        return;
    }

    XrCompositionLayerFlags projFlags =
        TransparentBackgroundEnabled() ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
    EndFrame(xr, frameState.predictedDisplayTime, projectionViews.data(), eyeCount, projFlags);

    // Texture-mode present: even on the fallback path the runtime composited
    // into our shared texture, so present it into our own window swapchain.
    PresentAndMaybeDump(rs);
}

// Destroy the zone/strip/wish resources (before session teardown).
static void CleanupZones(VkRenderer& vk) {
    for (uint32_t zi = 0; zi < kNumZones; zi++) {
        DisplayZone& z = g_zonesArr[zi];
        DestroyZoneFramebuffers(vk, z.fb);
        if (z.swapchain != XR_NULL_HANDLE) {
            xrDestroySwapchain(z.swapchain);
            z.swapchain = XR_NULL_HANDLE;
        }
        z.images.clear();
    }
    if (g_strip.swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(g_strip.swapchain);
        g_strip.swapchain = XR_NULL_HANDLE;
    }
    if (g_zone.mask != XR_NULL_HANDLE && g_zone.pfnDestroy) {
        g_zone.pfnDestroy(g_zone.mask);
        g_zone.mask = XR_NULL_HANDLE;
    }
    g_zonesActive = false;
}

// Main entry point
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    if (!InitializeLogging(APP_NAME)) {
        MessageBox(nullptr, L"Failed to initialize logging", L"Warning", MB_OK | MB_ICONWARNING);
    }

    LOG_INFO("=== Cube Zones TEXTURE VK (XR_DXR_display_zones parity test) ===");
    LOG_INFO("Hybrid: VK renders the zones; the shared composite target is a D3D11 KMT texture (BGRA)");
    LOG_INFO("Runtime dev gate: DISPLAYXR_ZONES=1 must be set for the runtime to advertise the extension");

    // Autonomous-verification dump (DXR_TEXDUMP).
    {
        const char* e = getenv("DXR_TEXDUMP");
        if (e != nullptr) {
            g_texDumpEnabled = true;
            if (e[0] == '\0' || (e[0] == '1' && e[1] == '\0')) {
                const char* tmp = getenv("TEMP");
                if (tmp == nullptr || tmp[0] == '\0') tmp = getenv("TMP");
                std::string dir = (tmp && tmp[0]) ? tmp : ".";
                g_texDumpPath = dir + "\\zones_texture_vk_readback.png";
            } else {
                g_texDumpPath = e;
            }
            LOG_INFO("DXR_TEXDUMP set — shared-texture readback will dump to %s at frame %ld",
                     g_texDumpPath.c_str(), kTexDumpFrame);
        }
    }

    // Create window FIRST (needed for XR_DXR_win32_window_binding)
    HWND hwnd = CreateAppWindow(hInstance, g_windowWidth, g_windowHeight);
    if (!hwnd) {
        LOG_ERROR("Failed to create window");
        ShutdownLogging();
        return 1;
    }

    // (1) Initialize OpenXR instance + system + extension detection.
    LOG_INFO("Initializing OpenXR...");
    XrSessionManager xr = {};
    g_xr = &xr;
    if (!InitializeOpenXR(xr)) {
        LOG_ERROR("OpenXR initialization failed");
        MessageBox(hwnd, L"Failed to initialize OpenXR", L"Error", MB_OK | MB_ICONERROR);
        g_xr = nullptr;
        ShutdownLogging();
        return 1;
    }

    // INV-1.3: open on the 3D panel (#715) — one-shot move to the panel's
    // desktop position reported by xrGetSystemProperties (virtual-screen
    // coords, top-down; (0,0) = primary/unknown is safe), BEFORE
    // xrCreateSession so the display processor tracks the window on the
    // panel from the start.
    SetWindowPos(hwnd, nullptr, g_displayScreenLeft, g_displayScreenTop, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (!xr.hasWin32WindowBindingExt) {
        LOG_ERROR("XR_DXR_win32_window_binding not available — required for shared texture mode");
        MessageBox(hwnd, L"XR_DXR_win32_window_binding extension not available.\nRequired for shared texture mode.",
            L"Error", MB_OK | MB_ICONERROR);
        g_xr = nullptr;
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    if (!GetVulkanGraphicsRequirements(xr)) {
        LOG_ERROR("Failed to get Vulkan graphics requirements");
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // (2) Vulkan instance + the runtime's physical device + its LUID.
    VkInstance vkInstance = VK_NULL_HANDLE;
    if (!CreateVulkanInstance(xr, vkInstance)) {
        LOG_ERROR("Vulkan instance creation failed");
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }
    VkPhysicalDevice vkPhysDevice = VK_NULL_HANDLE;
    if (!GetVulkanPhysicalDevice(xr, vkInstance, vkPhysDevice)) {
        LOG_ERROR("Failed to get Vulkan physical device");
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }
    LUID adapterLuid = {};
    if (!GetVulkanPhysicalDeviceLUID(vkPhysDevice, adapterLuid)) {
        LOG_ERROR("Failed to read Vulkan device LUID — cannot adapter-match the D3D11 shared texture");
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // (3) Create the PRESENT-side D3D11 device on the LUID-matched adapter.
    if (!CreatePresentD3D11OnLUID(adapterLuid)) {
        LOG_ERROR("Failed to create LUID-matched present D3D11 device");
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // App-side DXGI window swapchain (DComp transparent visual in transparent mode).
    const bool g_transparentPresent = TransparentBackgroundEnabled();
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
        HRESULT hr;
        if (g_transparentPresent) {
            scd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
            hr = g_dxgiFactory->CreateSwapChainForComposition(g_d3dDevice.Get(), &scd, nullptr, &appSwapchain);
            if (SUCCEEDED(hr)) {
                ComPtr<IDXGIDevice> dxgiDevice;
                hr = g_d3dDevice.As(&dxgiDevice);
                if (SUCCEEDED(hr)) hr = DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&g_dcompDevice));
                if (SUCCEEDED(hr)) hr = g_dcompDevice->CreateTargetForHwnd(hwnd, TRUE, &g_dcompTarget);
                if (SUCCEEDED(hr)) hr = g_dcompDevice->CreateVisual(&g_dcompVisual);
                if (SUCCEEDED(hr)) hr = g_dcompVisual->SetContent(appSwapchain.Get());
                if (SUCCEEDED(hr)) hr = g_dcompTarget->SetRoot(g_dcompVisual.Get());
                if (SUCCEEDED(hr)) hr = g_dcompDevice->Commit();
            }
            if (FAILED(hr)) {
                LOG_ERROR("Transparent (DComp) present setup failed: 0x%08x", hr);
                vkDestroyInstance(vkInstance, nullptr);
                CleanupOpenXR(xr);
                ShutdownLogging();
                return 1;
            }
            LOG_INFO("Transparent DComp present active — alpha=0 shows the live desktop");
        } else {
            hr = g_dxgiFactory->CreateSwapChainForHwnd(g_d3dDevice.Get(), hwnd, &scd, nullptr, nullptr, &appSwapchain);
            if (FAILED(hr)) {
                LOG_ERROR("Failed to create app swapchain: 0x%08x", hr);
                vkDestroyInstance(vkInstance, nullptr);
                CleanupOpenXR(xr);
                ShutdownLogging();
                return 1;
            }
        }
    }
    ComPtr<ID3D11RenderTargetView> appBackBufferRTV;
    {
        ComPtr<ID3D11Texture2D> backBuf;
        appSwapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), &backBuf);
        g_d3dDevice->CreateRenderTargetView(backBuf.Get(), nullptr, &appBackBufferRTV);
    }

    // (4) Create the D3D11 shared texture at the worst-case atlas dims (BGRA,
    // KMT-shareable). The runtime's VK compositor imports this as the target.
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
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = g_sharedWidth;
        desc.Height = g_sharedHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        // KMT-shareable so the VK compositor can import it via
        // VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT.
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

        HRESULT hr = g_d3dDevice->CreateTexture2D(&desc, nullptr, &g_sharedTexture);
        if (FAILED(hr)) {
            LOG_ERROR("Failed to create shared texture: 0x%08x", hr);
            vkDestroyInstance(vkInstance, nullptr);
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
            vkDestroyInstance(vkInstance, nullptr);
            CleanupOpenXR(xr);
            ShutdownLogging();
            return 1;
        }
        LOG_INFO("Created shared D3D11 texture: %ux%u, handle=%p (KMT, BGRA)", g_sharedWidth, g_sharedHeight, g_sharedHandle);
    }

    if (!CreateBlitResources(g_d3dDevice.Get())) {
        LOG_ERROR("Failed to create blit resources");
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // (5) Vulkan logical device + graphics queue.
    std::vector<const char*> deviceExtensions;
    std::vector<std::string> extensionStorage;
    GetVulkanDeviceExtensions(xr, vkInstance, vkPhysDevice, deviceExtensions, extensionStorage);
    uint32_t queueFamilyIndex = 0;
    if (!FindGraphicsQueueFamily(vkPhysDevice, queueFamilyIndex)) {
        LOG_ERROR("No graphics queue family");
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }
    VkDevice vkDevice = VK_NULL_HANDLE;
    VkQueue vkGraphicsQueue = VK_NULL_HANDLE;
    if (!CreateVulkanDevice(vkPhysDevice, queueFamilyIndex, deviceExtensions, vkDevice, vkGraphicsQueue)) {
        LOG_ERROR("Vulkan device creation failed");
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // (6) Create the OpenXR session with the VK binding + shared texture handle.
    LOG_INFO("Creating OpenXR session (VK binding + shared texture handle 0x%p + HWND 0x%p)...", g_sharedHandle, hwnd);
    if (!CreateSession(xr, vkInstance, vkPhysDevice, vkDevice, queueFamilyIndex, 0, g_sharedHandle, hwnd)) {
        LOG_ERROR("OpenXR session creation failed");
        MessageBox(hwnd, L"Failed to create OpenXR session", L"Error", MB_OK | MB_ICONERROR);
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    if (!CreateSpaces(xr)) {
        LOG_ERROR("Reference space creation failed");
        CleanupOpenXR(xr);
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        ShutdownLogging();
        return 1;
    }

    // Fallback single swapchain at native display resolution (Vulkan images).
    if (!CreateSwapchain(xr)) {
        LOG_ERROR("Swapchain creation failed");
        CleanupOpenXR(xr);
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
        LOG_INFO("Enumerated %u Vulkan swapchain images", count);
    }

    // Initialize the VK renderer (pipelines + cube/grid geometry + textures).
    VkRenderer vkRenderer = {};
    if (!InitializeVkRenderer(vkRenderer, vkDevice, vkPhysDevice, vkGraphicsQueue, queueFamilyIndex,
                              (VkFormat)xr.swapchain.format)) {
        LOG_ERROR("VK renderer init failed");
        CleanupOpenXR(xr);
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        ShutdownLogging();
        return 1;
    }

    // Fallback-path framebuffers over the single swapchain (eye 0 set; the
    // fallback render uses framebuffers[0]). Zone framebuffers are created at
    // zones-activation time.
    {
        std::vector<VkImage> imgs(swapchainImages.size());
        for (size_t i = 0; i < swapchainImages.size(); i++) imgs[i] = swapchainImages[i].image;
        CreateSwapchainFramebuffers(vkRenderer, 0, imgs.data(), (uint32_t)imgs.size(),
                                    xr.swapchain.width, xr.swapchain.height, (VkFormat)xr.swapchain.format);
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    LOG_INFO("");
    LOG_INFO("=== Entering main loop ===");
    LOG_INFO("Texture mode: runtime composites zones into the shared texture, app blits it to the window");
    LOG_INFO("Zones: M=wish mode (AUTO/Tier-2), O=zone B overlap toggle");
    LOG_INFO("Controls: WASD=Fly (fallback), V=Mode, T=Eye tracking, F11=Fullscreen, ESC=Quit");
    LOG_INFO("");

    PerformanceStats perfStats = {};
    perfStats.lastTime = std::chrono::high_resolution_clock::now();

    g_inputState.viewParams.virtualDisplayHeight = 0.24f;
    g_inputState.initialVirtualDisplayHeight = g_inputState.viewParams.virtualDisplayHeight;
    g_inputState.nominalViewerZ = xr.nominalViewerZ;
    g_inputState.renderingModeCount = xr.renderingModeCount;

    RenderState rs = {};
    rs.hwnd = hwnd;
    rs.xr = &xr;
    rs.vk = &vkRenderer;
    rs.swapchainImages = &swapchainImages;
    rs.perfStats = &perfStats;
    rs.appSwapchain = appSwapchain;
    rs.appBackBufferRTV = appBackBufferRTV;
    rs.fallbackFbReady = true;
    // Release the locals so rs holds the only references (needed for ResizeBuffers).
    appBackBufferRTV.Reset();
    appSwapchain.Reset();
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

    LOG_INFO("");
    LOG_INFO("=== Shutting down ===");

    CleanupZones(vkRenderer);

    // Release RenderState ComPtrs first (they hold the swapchain refs).
    rs.appBackBufferRTV.Reset();
    rs.appSwapchain.Reset();

    g_sharedSRV.Reset();
    g_sharedTexture.Reset();
    g_blitVS.Reset();
    g_blitPS.Reset();
    g_blitSampler.Reset();
    g_blitParamsCB.Reset();
    appBackBufferRTV.Reset();
    appSwapchain.Reset();
    g_dcompVisual.Reset();
    g_dcompTarget.Reset();
    g_dcompDevice.Reset();

    g_xr = nullptr;
    CleanupOpenXR(xr);
    CleanupVkRenderer(vkRenderer);
    vkDestroyDevice(vkDevice, nullptr);
    vkDestroyInstance(vkInstance, nullptr);

    g_dxgiFactory.Reset();
    g_d3dContext.Reset();
    g_d3dDevice.Reset();

    DestroyWindow(hwnd);
    UnregisterClass(WINDOW_CLASS, hInstance);

    LOG_INFO("Application shutdown complete");
    ShutdownLogging();

    return 0;
}
