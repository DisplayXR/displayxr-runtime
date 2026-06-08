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
 * Key difference from cube_handle_d3d11: no window handle is passed to the runtime.
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
#include "projection_depth.h"

#include <chrono>
#include <cmath>
#include <string>
#include <sstream>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

static const char* APP_NAME = "cube_texture_d3d11_win";
static const wchar_t* WINDOW_CLASS = L"SRCubeSharedD3D11Class";
static const wchar_t* WINDOW_TITLE = L"D3D11 Cube \u2014 D3D11 Native Compositor (Shared Texture)";

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
// The runtime resolves the CANVAS sub-rect itself (get_window_metrics +
// u_canvas_apply_to_metrics), so the app keeps zero canvas geometry for view
// generation. Per-view staging container (matrices column-major):
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

// Shared texture resources
static ComPtr<ID3D11Texture2D> g_sharedTexture;
static ComPtr<ID3D11ShaderResourceView> g_sharedSRV;
static HANDLE g_sharedHandle = nullptr;
static uint32_t g_sharedWidth = 0;   // Shared texture size (= display pixels, worst case)
static uint32_t g_sharedHeight = 0;

// Canvas dimensions — the sub-rect of the window where 3D content is displayed.
// Computed from the letterbox viewport each frame.
static uint32_t g_canvasW = 0;
static uint32_t g_canvasH = 0;

// Surround 2D texture (spec v6 §3.6 of XR_EXT_win32_window_binding). Holds
// the app's full-window 2D content; the runtime blits non-canvas pixels of
// this into the shared swapchain each frame around the weaved canvas.
// Must match the shared multiview texture's dims + format because the
// compositor strip-blit uses CopySubresourceRegion (no format conversion).
static ComPtr<ID3D11Texture2D> g_surroundTexture;
static ComPtr<ID3D11RenderTargetView> g_surroundRTV;
static ComPtr<IDXGIKeyedMutex> g_surroundMutex;
static HANDLE g_surroundHandle = nullptr;
static bool g_surroundRegistered = false;

// Blit shader resources
static ComPtr<ID3D11VertexShader> g_blitVS;
static ComPtr<ID3D11PixelShader> g_blitPS;
static ComPtr<ID3D11SamplerState> g_blitSampler;
static ComPtr<ID3D11Buffer> g_blitParamsCB;

// Surround pattern shader resources
static ComPtr<ID3D11PixelShader> g_surroundPS;
static ComPtr<ID3D11Buffer> g_surroundParamsCB;

// #439 Phase 1 zone-mask harness ('Z' key) — set in WindowProc, consumed in
// RenderOneFrame where the XR session + D3D device are at hand.
static bool g_zoneCycleRequested = false;

// #439 Phase 3 case-1 A/B (DXR_AB_LOCAL2D=1). B-mode replaces the surround
// side-channel with the spec-§5 migration shape: an explicit Tier-2 single-rect
// mask (the canvas rect as 3D) + a full-window XrCompositionLayerLocal2DEXT
// carrying the same surround pattern. The mask activates at frame N (not
// startup) so the canvas is superseded and the runtime's view-size event fires;
// the app then renders window-sized (the mask supersedes the canvas) and the
// screen-anchored projection keeps 3D on the same screen pixels — capture diff
// vs the legacy surround path ≈ 0 in the surround region. The app pivots on its
// OWN mask activation (not the event) because the shared PollEvents drains the
// queue; the runtime still fires the event for the §8 case-5 log check.
// Determinism hooks: DXR_FREEZE=1 stops the cube animation, DXR_HUD=0 hides HUD.
static bool g_abLocal2D = false;      // B-mode toggle (DXR_AB_LOCAL2D=1)
static bool g_local2DActive = false;  // mask + layer submitted from here on
static bool g_canvasIsWindow = false; // post-pivot: window-sized rendering
static const long g_local2DActivationFrame = 60;
static long g_l2dFrameCounter = 0;
static bool g_freezeAnimation = false; // DXR_FREEZE=1
static XrSwapchain g_l2dSwapchain = XR_NULL_HANDLE;
static uint32_t g_l2dW = 0, g_l2dH = 0;

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
            // Update canvas immediately so the compositor/DP resize before next render
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
        // Prevent ALT from activating the system menu modal loop, which would
        // freeze rendering on this single-threaded app.
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            PostMessage(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        if (wParam == 'Z') {
            // #439 Phase 1: cycle the XR_EXT_local_3d_zone test states.
            g_zoneCycleRequested = true;
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
cbuffer BlitParams : register(b0) { float2 uvScale; float2 uvOffset; };
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return tex.Sample(smp, uvOffset + uv * uvScale);
}
)";

// Surround pattern pixel shader. Draws a checkerboard + soft gradient over
// the window region, with a bright border just outside the canvas hole so
// the canvas/surround boundary is visually unmistakable. Pixels inside the
// canvas are written black — the compositor blit doesn't read them (DP's
// weaved output stays in the canvas region of the shared texture), so this
// just keeps the texture tidy in case anyone samples it.
static const char* g_surroundPSSource = R"(
cbuffer SurroundParams : register(b0) {
    float2 windowSize;     // current window client area
    float2 _pad0;
    int4   canvas;         // (x, y, w, h) in window pixels
    float  time;           // seconds, for subtle motion
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
    float d  = max(dx, dy);   // negative inside canvas, positive outside
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
    // Diagonal sweep so motion is visible at a glance.
    float sweep = frac((p.x + p.y) / 256.0 - time * 0.10);
    base += (smoothstep(0.45, 0.5, sweep) - smoothstep(0.5, 0.55, sweep)) * 0.10;
    return float4(saturate(base), 1.0);
}
)";

static bool CreateSurroundShaderResources(ID3D11Device* device) {
    ComPtr<ID3DBlob> psBlob, errBlob;
    HRESULT hr = D3DCompile(g_surroundPSSource, strlen(g_surroundPSSource), "surroundPS",
        nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, &errBlob);
    if (FAILED(hr)) {
        LOG_ERROR("Surround PS compile failed: %s", errBlob ? (char*)errBlob->GetBufferPointer() : "unknown");
        return false;
    }
    hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_surroundPS);
    if (FAILED(hr)) return false;

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = 48;  // 3 × float4 = 48 bytes
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device->CreateBuffer(&cbd, nullptr, &g_surroundParamsCB);
    return SUCCEEDED(hr);
}

// Allocate the NT-shared keyed-mutex surround texture sized to match the
// multiview shared texture (spec v6 compositor enforces dim + format
// equality between the surround source and the dst it blits into).
// Returns the NT HANDLE that the runtime opens via OpenSharedResource1.
static bool CreateSurroundTexture(ID3D11Device* device,
                                   uint32_t width, uint32_t height,
                                   DXGI_FORMAT format) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE
                   | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &g_surroundTexture);
    if (FAILED(hr)) {
        LOG_ERROR("Surround texture create failed: 0x%08x", hr);
        return false;
    }

    hr = device->CreateRenderTargetView(g_surroundTexture.Get(), nullptr, &g_surroundRTV);
    if (FAILED(hr)) {
        LOG_ERROR("Surround RTV create failed: 0x%08x", hr);
        return false;
    }

    ComPtr<IDXGIResource1> dxgiRes1;
    hr = g_surroundTexture.As(&dxgiRes1);
    if (FAILED(hr) || !dxgiRes1) {
        LOG_ERROR("Surround texture has no IDXGIResource1: 0x%08x", hr);
        return false;
    }
    hr = dxgiRes1->CreateSharedHandle(nullptr,
        DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
        nullptr, &g_surroundHandle);
    if (FAILED(hr) || !g_surroundHandle) {
        LOG_ERROR("Surround texture CreateSharedHandle failed: 0x%08x", hr);
        return false;
    }

    hr = g_surroundTexture.As(&g_surroundMutex);
    if (FAILED(hr) || !g_surroundMutex) {
        LOG_ERROR("Surround texture has no IDXGIKeyedMutex: 0x%08x", hr);
        return false;
    }

    LOG_INFO("Created surround D3D11 texture: %ux%u format=%u handle=%p",
        width, height, (unsigned)format, g_surroundHandle);
    return true;
}

// Render the surround pattern into the surround texture. Holds the keyed
// mutex on key 0 while writing — runtime samples on the same key during
// xrEndFrame's submit-layers path. Renders only the current window-pixel
// region (0..winW, 0..winH); area outside is never sampled.
static void RenderSurroundPattern(D3D11Renderer& renderer,
                                   uint32_t winW, uint32_t winH,
                                   int32_t canvasX, int32_t canvasY,
                                   uint32_t canvasW, uint32_t canvasH,
                                   float timeSeconds) {
    if (!g_surroundRTV || !g_surroundPS || !g_surroundParamsCB || !g_surroundMutex) return;

    HRESULT hr = g_surroundMutex->AcquireSync(0, 16);
    if (FAILED(hr)) {
        return;  // Skip this frame; runtime's previous-frame copy remains.
    }

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(renderer.context->Map(g_surroundParamsCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        struct Params {
            float windowSize[2];
            float _pad0[2];
            int32_t canvas[4];
            float time;
            float _pad1[3];
        } p = {};
        p.windowSize[0] = (float)winW;
        p.windowSize[1] = (float)winH;
        p.canvas[0] = canvasX;
        p.canvas[1] = canvasY;
        p.canvas[2] = (int32_t)canvasW;
        p.canvas[3] = (int32_t)canvasH;
        p.time = timeSeconds;
        memcpy(mapped.pData, &p, sizeof(p));
        renderer.context->Unmap(g_surroundParamsCB.Get(), 0);
    }

    ID3D11RenderTargetView* rtv = g_surroundRTV.Get();
    renderer.context->OMSetRenderTargets(1, &rtv, nullptr);

    D3D11_VIEWPORT vp = {};
    vp.Width = (FLOAT)winW;
    vp.Height = (FLOAT)winH;
    vp.MaxDepth = 1.0f;
    renderer.context->RSSetViewports(1, &vp);

    renderer.context->VSSetShader(g_blitVS.Get(), nullptr, 0);
    renderer.context->PSSetShader(g_surroundPS.Get(), nullptr, 0);
    ID3D11Buffer* cb = g_surroundParamsCB.Get();
    renderer.context->PSSetConstantBuffers(0, 1, &cb);
    renderer.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    renderer.context->IASetInputLayout(nullptr);
    renderer.context->Draw(3, 0);

    ID3D11RenderTargetView* nullRTV = nullptr;
    renderer.context->OMSetRenderTargets(1, &nullRTV, nullptr);

    g_surroundMutex->ReleaseSync(0);
}

// #439 Phase 3 — render the SAME surround pattern shader into the Local2D
// swapchain image so the B-mode A/B is byte-comparable with the legacy surround
// in the non-canvas region (both use g_surroundPS with identical params). The
// canvas region is masked out (M=1 → weave) so its contents never show; we draw
// the full window pattern (matching the surround texture) anyway. No keyed
// mutex — the XR swapchain's acquire/wait/release provides cross-process sync.
static bool FillL2DPanelWithSurround(D3D11Renderer& renderer, ID3D11Texture2D* image,
                                     uint32_t winW, uint32_t winH,
                                     int32_t canvasX, int32_t canvasY,
                                     uint32_t canvasW, uint32_t canvasH) {
    if (!g_surroundPS || !g_surroundParamsCB || !g_blitVS || image == nullptr) {
        return false;
    }
    // The runtime allocates swapchain images TYPELESS/SRGB so they can be
    // viewed as either; CreateRenderTargetView(nullptr) fails on those. Use an
    // explicit UNORM-sibling RTV desc so the pattern is written as ENCODED
    // bytes (no sRGB encode) — matching the surround texture (format 87 =
    // B8G8R8A8_UNORM) for the A/B byte comparison.
    D3D11_TEXTURE2D_DESC td = {};
    image->GetDesc(&td);
    DXGI_FORMAT rtvFmt = td.Format;
    if (rtvFmt == DXGI_FORMAT_B8G8R8A8_TYPELESS || rtvFmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
        rtvFmt = DXGI_FORMAT_B8G8R8A8_UNORM;
    } else if (rtvFmt == DXGI_FORMAT_R8G8B8A8_TYPELESS || rtvFmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
        rtvFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = rtvFmt;
    rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    rtvDesc.Texture2D.MipSlice = 0;
    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(renderer.device->CreateRenderTargetView(image, &rtvDesc, &rtv))) {
        LOG_ERROR("Local2D fill: CreateRenderTargetView failed (fmt=%u)", (unsigned)td.Format);
        return false;
    }

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(renderer.context->Map(g_surroundParamsCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        struct Params {
            float windowSize[2];
            float _pad0[2];
            int32_t canvas[4];
            float time;
            float _pad1[3];
        } p = {};
        p.windowSize[0] = (float)winW;
        p.windowSize[1] = (float)winH;
        p.canvas[0] = canvasX;
        p.canvas[1] = canvasY;
        p.canvas[2] = (int32_t)canvasW;
        p.canvas[3] = (int32_t)canvasH;
        p.time = 0.0f; // static content — frozen so the A/B is byte-stable
        memcpy(mapped.pData, &p, sizeof(p));
        renderer.context->Unmap(g_surroundParamsCB.Get(), 0);
    }

    ID3D11RenderTargetView* rtvp = rtv.Get();
    renderer.context->OMSetRenderTargets(1, &rtvp, nullptr);
    D3D11_VIEWPORT vp = {};
    vp.Width = (FLOAT)winW;
    vp.Height = (FLOAT)winH;
    vp.MaxDepth = 1.0f;
    renderer.context->RSSetViewports(1, &vp);
    renderer.context->VSSetShader(g_blitVS.Get(), nullptr, 0);
    renderer.context->PSSetShader(g_surroundPS.Get(), nullptr, 0);
    ID3D11Buffer* cb = g_surroundParamsCB.Get();
    renderer.context->PSSetConstantBuffers(0, 1, &cb);
    renderer.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    renderer.context->IASetInputLayout(nullptr);
    renderer.context->Draw(3, 0);
    ID3D11RenderTargetView* nullRTV = nullptr;
    renderer.context->OMSetRenderTargets(1, &nullRTV, nullptr);
    return true;
}

// #439 Phase 3 — activate B-mode: create + fill the full-window Local2D
// swapchain (surround pattern), then create + submit the explicit Tier-2 mask
// (the canvas rect as 3D). Pivots the app to window-sized rendering and scales
// the virtual display height by the canvas→window ratio so the cube keeps the
// same physical size across the renegotiation (else it renders 2× — the
// #396-W7 ↔ Phase-3 reconciliation point). Returns true on success.
static bool ActivateLocal2DBMode(XrSessionManager& xr, D3D11Renderer& renderer) {
    uint32_t winW = g_windowWidth;
    uint32_t winH = g_windowHeight;
    if (winW == 0 || winH == 0 || !g_zone.available || !g_zone.pfnCreate || !g_zone.pfnSetRects ||
        !g_zone.pfnSubmit) {
        return false;
    }
    // The declared canvas sub-rect (center 50%, matching the output rect + the
    // surround pattern's canvas hole).
    int32_t mcx = (int32_t)(winW * 0.25f);
    int32_t mcy = (int32_t)(winH * 0.25f);
    uint32_t mcw = winW / 2;
    uint32_t mch = winH / 2;

    // Full-window Local2D swapchain, filled once with the surround pattern.
    XrSwapchainCreateInfo sci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    sci.format = (int64_t)DXGI_FORMAT_B8G8R8A8_UNORM;
    sci.sampleCount = 1;
    sci.width = winW;
    sci.height = winH;
    sci.faceCount = 1;
    sci.arraySize = 1;
    sci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(xr.session, &sci, &g_l2dSwapchain))) {
        LOG_ERROR("A/B B-mode: xrCreateSwapchain failed");
        return false;
    }
    g_l2dW = winW;
    g_l2dH = winH;

    uint32_t n = 0;
    xrEnumerateSwapchainImages(g_l2dSwapchain, 0, &n, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (n == 0 || XR_FAILED(xrEnumerateSwapchainImages(g_l2dSwapchain, n, &n,
                                                       (XrSwapchainImageBaseHeader*)imgs.data()))) {
        return false;
    }
    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t idx = 0;
    if (XR_FAILED(xrAcquireSwapchainImage(g_l2dSwapchain, &ai, &idx))) {
        return false;
    }
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(g_l2dSwapchain, &wi);
    bool filled = FillL2DPanelWithSurround(renderer, imgs[idx].texture, winW, winH, mcx, mcy, mcw, mch);
    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(g_l2dSwapchain, &ri);
    if (!filled) {
        LOG_ERROR("A/B B-mode: Local2D layer fill failed — surround region would be black");
        return false;
    }

    // Explicit Tier-2 mask: the canvas rect is 3D, everything else 2D.
    XrLocal3DZoneMaskCreateInfoEXT mci = {(XrStructureType)XR_TYPE_LOCAL_3D_ZONE_MASK_CREATE_INFO_EXT};
    mci.maskWidth = 0; // runtime picks the window backing size
    mci.maskHeight = 0;
    if (XR_FAILED(g_zone.pfnCreate(xr.session, &mci, &g_zone.mask))) {
        LOG_ERROR("A/B B-mode: create mask failed");
        return false;
    }
    XrRect2Di rect;
    rect.offset = {mcx, mcy};
    rect.extent = {(int32_t)mcw, (int32_t)mch};
    if (XR_FAILED(g_zone.pfnSetRects(g_zone.mask, 1, &rect)) || XR_FAILED(g_zone.pfnSubmit(g_zone.mask))) {
        LOG_ERROR("A/B B-mode: set/submit mask failed");
        return false;
    }

    // Pivot to window-sized rendering + world-scale constancy: the render canvas
    // grows canvas→window (2×), so scale the virtual display height by the same
    // ratio to keep the cube the same physical size (else it renders ~2×).
    // Leia-validated 2026-06-08: the B cube matches the A baseline within
    // eye-tracking parallax (two identical-baseline captures vary ±11% in cube
    // size from head distance alone; B falls inside that envelope). A
    // pixel-deterministic 3D A/B needs eye tracking pinned — see impl doc §9.
    g_canvasIsWindow = true;
    if (mch > 0) {
        g_inputState.viewParams.virtualDisplayHeight *= (float)winH / (float)mch;
    }
    LOG_INFO("A/B B-mode activated: Tier-2 mask (canvas %d,%d %ux%u 3D) + full-window Local2D layer %ux%u, "
             "virtualDisplayHeight*=%.3f",
             mcx, mcy, mcw, mch, winW, winH, mch > 0 ? (float)winH / (float)mch : 1.0f);
    return true;
}

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
    if (FAILED(hr)) return false;

    // Constant buffer for UV scale (canvas/shared texture ratio)
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = 16;  // float2 + padding to 16-byte alignment
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device->CreateBuffer(&cbd, nullptr, &g_blitParamsCB);
    return SUCCEEDED(hr);
}

// Blit shared texture to back buffer with aspect-ratio letterboxing.
// Two sub-modes:
//  - surround inactive: blit ONLY the canvas sub-rect of the shared texture
//    into the matching back-buffer sub-rect (legacy behavior). Non-canvas
//    back-buffer pixels stay as the clear color.
//  - surround active: blit the whole window region of the shared texture.
//    Compositor has filled the strips around the canvas with the app's 2D
//    pattern (spec v6), so a full-window blit lets the user see them.
static void BlitSharedTextureToBackBuffer(D3D11Renderer& renderer, ID3D11RenderTargetView* backBufferRTV,
                                           uint32_t winW, uint32_t winH, XrSessionManager& xr) {
    if (!g_sharedSRV) return;

    renderer.context->OMSetRenderTargets(1, &backBufferRTV, nullptr);

    // Canvas = center 50% of window (25%-75% region) to demonstrate canvas ≠ window
    float canvasX = (FLOAT)winW * 0.25f;
    float canvasY = (FLOAT)winH * 0.25f;
    float canvasW = (FLOAT)winW * 0.5f;
    float canvasH = (FLOAT)winH * 0.5f;

    // Tell the runtime where the canvas lives. Always do this — even when the
    // surround texture covers the rest of the window, the canvas rect drives
    // both the DP weave region and the compositor's surround-strip layout.
    if (xr.pfnSetSharedTextureOutputRectEXT && xr.session != XR_NULL_HANDLE) {
        xr.pfnSetSharedTextureOutputRectEXT(xr.session,
            (int32_t)canvasX, (int32_t)canvasY,
            (uint32_t)canvasW, (uint32_t)canvasH);
    }

    // Pick the viewport + UV range based on whether the shared texture's
    // surround region is populated. #439 case-1 B-mode: once the Local2D layer
    // + mask have pivoted (g_canvasIsWindow), the masked composite fills the
    // full window region of the shared texture (canvas weave + the flattened
    // Local2D surround pattern), so blit the full window — same as the legacy
    // surround path. Without this, the surround region stays at the clear
    // color (black) even though the shared texture holds the pattern.
    D3D11_VIEWPORT vp = {};
    float uvParams[4];  // (scaleX, scaleY, offsetX, offsetY)
    if (g_surroundRegistered || g_canvasIsWindow) {
        // Full window — read back canvas weave + surround strips / Local2D.
        vp.TopLeftX = 0.0f;
        vp.TopLeftY = 0.0f;
        vp.Width = (FLOAT)winW;
        vp.Height = (FLOAT)winH;
        uvParams[0] = (float)winW / (float)g_sharedWidth;
        uvParams[1] = (float)winH / (float)g_sharedHeight;
        uvParams[2] = 0.0f;
        uvParams[3] = 0.0f;
    } else {
        // Canvas-only — non-canvas back-buffer pixels stay at the clear color.
        vp.TopLeftX = canvasX;
        vp.TopLeftY = canvasY;
        vp.Width = canvasW;
        vp.Height = canvasH;
        uvParams[0] = canvasW / (float)g_sharedWidth;
        uvParams[1] = canvasH / (float)g_sharedHeight;
        uvParams[2] = canvasX / (float)g_sharedWidth;
        uvParams[3] = canvasY / (float)g_sharedHeight;
    }
    vp.MaxDepth = 1.0f;
    renderer.context->RSSetViewports(1, &vp);

    if (g_blitParamsCB && g_sharedWidth > 0 && g_sharedHeight > 0) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(renderer.context->Map(g_blitParamsCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            memcpy(mapped.pData, uvParams, sizeof(uvParams));
            renderer.context->Unmap(g_blitParamsCB.Get(), 0);
        }
    }

    renderer.context->VSSetShader(g_blitVS.Get(), nullptr, 0);
    renderer.context->PSSetShader(g_blitPS.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srv = g_sharedSRV.Get();
    renderer.context->PSSetShaderResources(0, 1, &srv);
    ID3D11SamplerState* smp = g_blitSampler.Get();
    renderer.context->PSSetSamplers(0, 1, &smp);
    ID3D11Buffer* cb = g_blitParamsCB.Get();
    renderer.context->PSSetConstantBuffers(0, 1, &cb);
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

// #439 Phase 1 — XR_EXT_local_3d_zone authoring harness. 'Z' cycles:
//   0 no mask (rect-surround behavior)
//   1 Tier-1 whole-window 3D (full weave, no 2D anywhere)
//   2 Tier-2 single rect == the canvas rect (must match the analytic
//     rect-surround output inside the window — impl doc §6 case 3)
//   3 Tier-2 multi-rect: three disconnected 3D islands
//   4 Tier-3 freeform: CPU radial gradient uploaded onto the mask RT
//     (soft 2D↔3D edge — validates the mask-lerp, impl doc §6 case 4)
static void ZoneMaskApplyNextState(XrSessionManager& xr, D3D11Renderer& renderer) {
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
        XrLocal3DZoneRenderTargetD3D11EXT binding = {XR_TYPE_LOCAL_3D_ZONE_RENDER_TARGET_D3D11_EXT};
        res = g_zone.pfnAcquireRT(g_zone.mask, &binding);
        if (XR_SUCCEEDED(res) && binding.renderTargetView != nullptr &&
            binding.width > 0 && binding.height > 0) {
            // The mask RT lives on the app's own device in-process — write a
            // CPU radial gradient straight onto the underlying resource
            // (M=1 core → soft falloff → M=0). UpdateSubresource is legal on
            // a DEFAULT-usage texture.
            ID3D11RenderTargetView* rtv =
                static_cast<ID3D11RenderTargetView*>(binding.renderTargetView);
            ComPtr<ID3D11Resource> maskRes;
            rtv->GetResource(&maskRes);
            const uint32_t mw = binding.width;
            const uint32_t mh = binding.height;
            std::vector<uint8_t> pixels((size_t)mw * mh);
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
                    pixels[(size_t)y * mw + x] = (uint8_t)(m * 255.0f + 0.5f);
                }
            }
            renderer.context->UpdateSubresource(maskRes.Get(), 0, nullptr,
                pixels.data(), mw, 0);
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
    // #439 Phase 1: 'Z' cycles the XR_EXT_local_3d_zone test states.
    if (g_zoneCycleRequested) {
        g_zoneCycleRequested = false;
        ZoneMaskApplyNextState(xr, renderer);
    }
    // #439 Phase 3: DXR_FREEZE=1 stops the cube animation for byte-stable A/B.
    if (!g_freezeAnimation) {
        UpdateScene(renderer, rs.perfStats->deltaTime);
    }
    PollEvents(xr);

    // #439 case-1 B-mode activation (frame N): create the Local2D layer + the
    // Tier-2 mask, pivot to window-sized rendering. Done before the canvas calc
    // below so this frame already renders the new canvas.
    if (g_abLocal2D && !g_local2DActive && g_l2dFrameCounter >= g_local2DActivationFrame) {
        static bool attempted = false;
        if (!attempted) {
            attempted = true;
            if (ActivateLocal2DBMode(xr, renderer)) {
                g_local2DActive = true;
            } else {
                LOG_ERROR("A/B B-mode activation failed — staying on plain projection path");
            }
        }
    }

    // Canvas = center 50% of window normally; the full window once B-mode has
    // pivoted (the mask superseded the canvas → the app renegotiates).
    if (g_windowWidth > 0 && g_windowHeight > 0) {
        if (g_canvasIsWindow) {
            g_canvasW = g_windowWidth;
            g_canvasH = g_windowHeight;
        } else {
            g_canvasW = g_windowWidth / 2;
            g_canvasH = g_windowHeight / 2;
        }
    }
    g_l2dFrameCounter++;

    // Spec v6 §3.6: refresh the surround texture each frame. Done before
    // xrBeginFrame so the runtime sees up-to-date pixels when it reads the
    // surround at submit time. The keyed-mutex acquire-write-release inside
    // RenderSurroundPattern handshakes with the compositor's read pass.
    if (g_surroundRegistered && rs.perfStats) {
        static auto surroundStartTime = std::chrono::steady_clock::now();
        // Repro knob (#439 Phase 0 A/B diff): DXR_SURROUND_FREEZE=1 pins the
        // pattern's time term so two captures from different runs are
        // byte-comparable in the surround region.
        static int surroundFreeze = -1;
        if (surroundFreeze < 0) {
            const char* e = getenv("DXR_SURROUND_FREEZE");
            surroundFreeze = (e && e[0] != '\0' && e[0] != '0') ? 1 : 0;
        }
        float t = surroundFreeze ? 0.0f
            : std::chrono::duration<float>(std::chrono::steady_clock::now() - surroundStartTime).count();
        int32_t cx = (int32_t)(g_windowWidth * 0.25f);
        int32_t cy = (int32_t)(g_windowHeight * 0.25f);
        uint32_t cw = g_windowWidth / 2;
        uint32_t ch = g_windowHeight / 2;
        RenderSurroundPattern(renderer, g_windowWidth, g_windowHeight, cx, cy, cw, ch, t);
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

                    // XR_EXT_view_rig raw channel (#396 W7): chain the raw
                    // result struct — the one-shot log below proves
                    // canvasRectPx reports this texture app's canvas SUB-RECT
                    // (xrSetSharedTextureOutputRectEXT), not the window client
                    // area. Coexists with the rig request below (independent
                    // chains: raw on XrViewState::next, rig on
                    // XrViewLocateInfo::next).
                    XrViewDisplayRawEXT viewRigRaw = {XR_TYPE_VIEW_DISPLAY_RAW_EXT};
                    if (g_hasViewRigExt) {
                        viewState.next = &viewRigRaw;
                    }

                    // XR_EXT_view_rig rig request: drive the runtime rig
                    // matching the app's current mode (C selects the rig) with
                    // the app's tunables — the runtime owns the canvas resolve
                    // and the Kooima math, and returns render-ready
                    // XrView{pose, fov}. Per-locate semantics: chain the rig
                    // on every consume locate.
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

                    if (g_hasViewRigExt) {
                        // Latch on the first locate with a real canvas rect —
                        // the earliest frames run before the window metrics /
                        // output rect exist and report the display fallback.
                        // Frame ~120 fallback: log whatever we have so a
                        // missing rect is visible in the log too.
                        static bool rawLogged = false;
                        static int rawFrames = 0;
                        rawFrames++;
                        if (!rawLogged && viewRigRaw.eyeCountOutput > 0 &&
                            (viewRigRaw.canvasRectPx.extent.width > 0 || rawFrames >= 120)) {
                            rawLogged = true;
                            LOG_INFO("ViewRig RAW (texture): eyes=%u [0]=(%.4f,%.4f,%.4f) "
                                     "canvas=(%d,%d %dx%d) %.4fx%.4fm tracking=%d",
                                     viewRigRaw.eyeCountOutput,
                                     viewRigRaw.rawEyes[0].x, viewRigRaw.rawEyes[0].y, viewRigRaw.rawEyes[0].z,
                                     viewRigRaw.canvasRectPx.offset.x, viewRigRaw.canvasRectPx.offset.y,
                                     viewRigRaw.canvasRectPx.extent.width, viewRigRaw.canvasRectPx.extent.height,
                                     viewRigRaw.canvasSizeMeters.width, viewRigRaw.canvasSizeMeters.height,
                                     (int)viewRigRaw.isTracking);
                        }
                    }

                    uint32_t maxTileW = tileColumns > 0 ? xr.swapchain.width / tileColumns : xr.swapchain.width;
                    uint32_t maxTileH = tileRows > 0 ? xr.swapchain.height / tileRows : xr.swapchain.height;

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

                    // Render HUD
                    if (g_inputState.hudVisible && xr.hasHudSwapchain && rs.hudSwapchainImages && !rs.hudSwapchainImages->empty() && rs.hudOk) {
                        uint32_t hudImageIndex;
                        if (AcquireHudSwapchainImage(xr, hudImageIndex)) {
                            std::wstring sessionText(xr.systemName, xr.systemName + strlen(xr.systemName));
                            sessionText += L"\nSession: ";
                            sessionText += FormatSessionState((int)xr.sessionState);
                            std::wstring modeText = L"Shared Texture D3D11 (offscreen)";
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
                                ID3D11Texture2D* hudTexture = (*rs.hudSwapchainImages)[hudImageIndex].texture;
                                D3D11_BOX box = {0, 0, 0, xr.hudSwapchain.width, xr.hudSwapchain.height, 1};
                                renderer.context->UpdateSubresource(hudTexture, 0, &box, pixels, srcRowPitch, 0);
                                UnmapHud(*rs.hudRenderer);
                            }
                            ReleaseHudSwapchainImage(xr);
                            hudSubmitted = true;
                        }
                    }

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

                    uint32_t imageIndex;
                    if (AcquireSwapchainImage(xr, imageIndex)) {
                        ID3D11Texture2D* swapchainTexture = (*rs.swapchainImages)[imageIndex].texture;
                        ID3D11RenderTargetView* rtv = nullptr;
                        CreateRenderTargetView(renderer, swapchainTexture,
                            static_cast<DXGI_FORMAT>(xr.swapchain.format), &rtv);

                        float clearColor[4] = {0.05f, 0.05f, 0.25f, 1.0f};
                        renderer.context->ClearRenderTargetView(rtv, clearColor);
                        renderer.context->ClearDepthStencilView(rs.depthDSV.Get(),
                            D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

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

                        for (int eye = 0; eye < eyeCount; eye++) {
                            uint32_t tileX = monoMode ? 0 : (eye % tileColumns);
                            uint32_t tileY = monoMode ? 0 : (eye / tileColumns);
                            uint32_t vpX = tileX * renderW;
                            uint32_t vpY = tileY * renderH;

                            D3D11_VIEWPORT vp = {};
                            vp.TopLeftX = (FLOAT)vpX;
                            vp.TopLeftY = (FLOAT)vpY;
                            vp.Width = (FLOAT)renderW;
                            vp.Height = (FLOAT)renderH;
                            vp.MaxDepth = 1.0f;
                            renderer.context->RSSetViewports(1, &vp);

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

                            RenderScene(renderer, rtv, rs.depthDSV.Get(),
                                renderW, renderH, viewMatrix, projMatrix,
                                useAppProjection ? 1.0f : g_inputState.viewParams.scaleFactor, 0.03f);

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

                        if (rtv) rtv->Release();
                        ReleaseSwapchainImage(xr);
                    }
                }
            }

            // #439 case-1 B-mode: submit the projection layer + the full-window
            // Local2D layer raw (the shared EndFrame helpers don't carry the
            // Local2D type). The mask makes the canvas 3D, the rest the layer's
            // surround pattern — the A/B vs the legacy surround path.
            if (g_local2DActive && g_l2dSwapchain != XR_NULL_HANDLE) {
                XrCompositionLayerProjection projLayer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
                projLayer.space = xr.localSpace;
                projLayer.viewCount = (uint32_t)eyeCount;
                projLayer.views = projectionViews.data();

                XrCompositionLayerLocal2DEXT l2dLayer = {
                    (XrStructureType)XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_EXT};
                l2dLayer.layerFlags = 0; // premultiplied; the pattern is opaque
                l2dLayer.subImage.swapchain = g_l2dSwapchain;
                l2dLayer.subImage.imageRect.offset = {0, 0};
                l2dLayer.subImage.imageRect.extent = {(int32_t)g_l2dW, (int32_t)g_l2dH};
                l2dLayer.subImage.imageArrayIndex = 0;
                l2dLayer.rect.offset = {0, 0};
                l2dLayer.rect.extent = {(int32_t)g_l2dW, (int32_t)g_l2dH};

                const XrCompositionLayerBaseHeader* layers[2] = {
                    (XrCompositionLayerBaseHeader*)&projLayer,
                    (XrCompositionLayerBaseHeader*)&l2dLayer};
                // Match the legacy surround path's blend mode (it runs through
                // the shared EndFrame, which resolves DISPLAYXR_TRANSPARENT_BG
                // via SelectEnvBlendMode) so the B-mode A/B is apples-to-apples.
                XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
                endInfo.displayTime = frameState.predictedDisplayTime;
                endInfo.environmentBlendMode = xr.runtimeSupportsAlphaBlend
                    ? XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND
                    : XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                endInfo.layerCount = 2;
                endInfo.layers = layers;
                xrEndFrame(xr.session, &endInfo);
            } else if (hudSubmitted) {
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

            // Resize app swap chain if window size changed
            if (g_resizeNeeded && rs.appSwapchain) {
                g_resizeNeeded = false;
                rs.appBackBufferRTV.Reset();
                HRESULT hr = rs.appSwapchain->ResizeBuffers(0, g_windowWidth, g_windowHeight,
                                                             DXGI_FORMAT_UNKNOWN, 0);
                if (SUCCEEDED(hr)) {
                    ComPtr<ID3D11Texture2D> backBuf;
                    rs.appSwapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), &backBuf);
                    renderer.device->CreateRenderTargetView(backBuf.Get(), nullptr, &rs.appBackBufferRTV);
                }
            }

            // After xrEndFrame: blit shared texture to app window
            if (frameState.shouldRender && rs.appBackBufferRTV) {
                float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                renderer.context->ClearRenderTargetView(rs.appBackBufferRTV.Get(), clearColor);
                BlitSharedTextureToBackBuffer(renderer, rs.appBackBufferRTV.Get(),
                                               g_windowWidth, g_windowHeight, xr);
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

    // #439 Phase 3 case-1 A/B + capture-determinism hooks.
    {
        const char* e = getenv("DXR_AB_LOCAL2D");
        if (e && *e == '1') g_abLocal2D = true;
        e = getenv("DXR_FREEZE");
        if (e && *e == '1') g_freezeAnimation = true;
        e = getenv("DXR_HUD");
        if (e && *e == '0') g_inputState.hudVisible = false;
        if (g_abLocal2D) {
            LOG_INFO("DXR_AB_LOCAL2D=1 — case-1 B-mode (Tier-2 mask + Local2D layer, no surround)");
        }
    }

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

    // Create shared D3D11 texture at worst-case swapchain atlas dims.
    // See ADR-010 for rationale.
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

        // Initialize canvas to center 50% of window (updated per-frame)
        g_canvasW = g_windowWidth / 2;
        g_canvasH = g_windowHeight / 2;
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

    // Spec v6 §3.6: full-window 2D surround texture. Sized + formatted to
    // match the multiview shared texture so the compositor strip-blit
    // (CopySubresourceRegion, no format conversion) succeeds. We resolve the
    // PFN after CreateInstance in InitializeOpenXR, so it's known here.
    bool surroundSetupOk = false;
    if (xr.pfnSetSharedTextureSurround2DEXT) {
        surroundSetupOk = CreateSurroundShaderResources(renderer.device.Get()) &&
                          CreateSurroundTexture(renderer.device.Get(),
                                                g_sharedWidth, g_sharedHeight,
                                                DXGI_FORMAT_B8G8R8A8_UNORM);
        if (!surroundSetupOk) {
            LOG_WARN("Surround 2D setup failed — continuing without surround (canvas-only blit)");
        }
    } else {
        LOG_WARN("Runtime does not expose xrSetSharedTextureSurround2DEXT (pre-spec-v6) — surround disabled");
    }

    // HUD renderer
    HudRenderer hudRenderer = {};
    bool hudOk = InitializeHudRenderer(hudRenderer, HUD_PIXEL_WIDTH, HUD_PIXEL_HEIGHT);

    // Create OpenXR session with shared texture + app HWND for weaver position tracking
    if (!CreateSession(xr, renderer.device.Get(), g_sharedHandle, hwnd)) {
        LOG_ERROR("Session creation failed");
        if (hudOk) CleanupHudRenderer(hudRenderer);
        CleanupD3D11(renderer);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Register the surround texture with the runtime. NULL pfn or handle =
    // we skip and the blit stays on the canvas-only path.
    // #439 case-1 B-mode (DXR_AB_LOCAL2D=1): skip surround registration — the
    // full-window Local2D layer supplies the 2D pixels instead (the A/B).
    if (surroundSetupOk && !g_abLocal2D && xr.pfnSetSharedTextureSurround2DEXT && g_surroundHandle) {
        XrResult sres = xr.pfnSetSharedTextureSurround2DEXT(xr.session, g_surroundHandle,
            g_sharedWidth, g_sharedHeight);
        if (XR_SUCCEEDED(sres)) {
            g_surroundRegistered = true;
            LOG_INFO("Registered surround 2D texture with runtime (%ux%u)", g_sharedWidth, g_sharedHeight);
        } else {
            LogXrResult("xrSetSharedTextureSurround2DEXT", sres);
        }
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
    rs.hudSwapchainImages = &hudSwapchainImages;
    rs.depthTexture = depthTexture;
    rs.depthDSV = depthDSV;
    rs.swapchainImages = &swapchainImages;
    rs.perfStats = &perfStats;
    rs.appSwapchain = appSwapchain;
    rs.appBackBufferRTV = appBackBufferRTV;
    // Release originals so rs holds the only reference — required for ResizeBuffers
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

    LOG_INFO("=== Shutting down ===");

    // Release RenderState ComPtrs first — these hold copies from the locals
    rs.appBackBufferRTV.Reset();
    rs.appSwapchain.Reset();
    rs.depthDSV.Reset();
    rs.depthTexture.Reset();

    // #439 Phase 1: destroy the zone mask before session teardown (the oxr
    // handle cascade would also do it; explicit keeps ordering obvious).
    if (g_zone.mask != XR_NULL_HANDLE && g_zone.pfnDestroy) {
        g_zone.pfnDestroy(g_zone.mask);
        g_zone.mask = XR_NULL_HANDLE;
        g_zone.state = 0;
    }

    // Clear the runtime's surround registration first so the compositor
    // releases its opened texture before we drop our refs.
    if (g_surroundRegistered && xr.pfnSetSharedTextureSurround2DEXT && xr.session != XR_NULL_HANDLE) {
        xr.pfnSetSharedTextureSurround2DEXT(xr.session, nullptr, 0, 0);
        g_surroundRegistered = false;
    }
    if (g_surroundHandle) {
        CloseHandle(g_surroundHandle);
        g_surroundHandle = nullptr;
    }
    g_surroundMutex.Reset();
    g_surroundRTV.Reset();
    g_surroundTexture.Reset();
    g_surroundPS.Reset();
    g_surroundParamsCB.Reset();

    depthDSV.Reset();
    depthTexture.Reset();
    g_sharedSRV.Reset();
    g_sharedTexture.Reset();
    g_blitVS.Reset();
    g_blitPS.Reset();
    g_blitSampler.Reset();
    g_blitParamsCB.Reset();
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
