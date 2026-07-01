// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Cube Zones D3D12 — XR_EXT_display_zones exerciser (ADR-027),
 *         ARRAY / single-pass-instanced (SPI) stereo layout.
 *
 * Native-D3D12 HANDLE-class parity of cube_zones_d3d11_win: creates its own
 * HWND and passes it via XR_EXT_win32_window_binding (the runtime owns
 * presentation), then submits TWO 3D display zones each frame — but, unlike
 * every existing cube_zones_* app (which tile the per-view images horizontally
 * inside a single-slice swapchain), this app uses the ARRAY layout:
 *
 *   - each zone owns ONE swapchain created with arraySize = 2, width = tileW,
 *     height = tileH (per-view size from xrGetDisplayZoneRecommendedViewSizeEXT);
 *   - each swapchain image is a single ID3D12Resource with DepthOrArraySize==2;
 *   - view vi is rendered full-viewport into array slice vi via an RTV built
 *     with D3D12_RTV_DIMENSION_TEXTURE2DARRAY (FirstArraySlice=vi, ArraySize=1);
 *   - view vi is submitted with subImage.imageArrayIndex = vi, imageRect
 *     offset {0,0} extent {tileW,tileH}.
 *
 * This is the reproduction + regression vehicle for runtime bug #672 (D3D12
 * multi-zone array layout).
 *
 * Zone geometry (identical rects/rigs to cube_zones_d3d11_win so the visuals
 * match across backends):
 *   - Zone A (zoneId=1, left)  : rect {0,180,640,540}, identity rig, spin
 *     phase 0, dark-red opaque clear.
 *   - Zone B (zoneId=2, right) : rect {700,180,520,360}, rig with ipdFactor
 *     0.6 + perspectiveFactor 0.5 (visibly different framing), spin phase
 *     +1.5 rad, fully-transparent clear (cube floats over the desktop).
 *
 * Wish mode is AUTO (XrDisplayZonesFrameEndInfoEXT chained with
 * wishMask = XR_NULL_HANDLE → the runtime auto-derives the wish from the zone
 * rects). Set DXR_ZONES_VALIDATE=1 to also chain the validate bit.
 *
 * When the runtime doesn't advertise XR_EXT_display_zones (P2 dev gate:
 * DISPLAYXR_ZONES=1) the app logs an error once and submits empty frames.
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <wrl/client.h>

#include "logging.h"
#include "xr_session.h"
#include "d3d12_renderer.h"
#include "projection_depth.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

static const char* APP_NAME = "cube_zones_d3d12_win";

static const wchar_t* WINDOW_CLASS = L"DXRCubeZonesD3D12Class";
static const wchar_t* WINDOW_TITLE = L"D3D12 Cube Zones — XR_EXT_display_zones (array/SPI)";

// Global state (main thread + render thread)
static std::atomic<bool> g_running{true};
static XrSessionManager* g_xr = nullptr;
static UINT g_windowWidth = 1280;
static UINT g_windowHeight = 720;

// Fullscreen state
static bool g_fullscreen = false;
static RECT g_savedWindowRect = {};
static DWORD g_savedWindowStyle = 0;

// ---------------------------------------------------------------------------
// XR_EXT_display_zones state
// ---------------------------------------------------------------------------

static const uint32_t kNumZones = 2;

// The ARRAY / SPI stereo layout: 2 views as array slices. A mode with more
// than 2 views under-submits (n is clamped to this) — this app targets the
// stereo (view_count <= 2) modes that #672 concerns.
static const uint32_t kZoneArraySlices = 2;

// Per-zone rig framing (shared virtual display height in app units; cube 0.06 m).
static const float kZoneVirtualDisplayHeight = 0.30f;

struct DisplayZone {
    uint32_t zoneId = 0;
    XrRect2Di rect = {};            //!< client-window pixels; locate AND submit use this one variable
    float ipdFactor = 1.0f;
    float perspectiveFactor = 1.0f;
    float spinPhase = 0.0f;         //!< added to the shared cube rotation for this zone
    float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f}; //!< premultiplied RGBA (zone blends via content alpha, ADR-027)
    XrSwapchain swapchain = XR_NULL_HANDLE;
    int64_t format = 0;
    uint32_t tileW = 0;             //!< per-view (array-slice) width  = recommended view width
    uint32_t tileH = 0;             //!< per-view (array-slice) height = recommended view height
    std::vector<XrSwapchainImageD3D12KHR> images;
    //! One RTV per (image, array slice): index [imageIndex * kZoneArraySlices + slice].
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvs;
};
static DisplayZone g_zonesArr[kNumZones];

// #672 repro geometry: two equal, ADJACENT zones spanning the full window
// width (mirrors the reporter's A=(0,565)468x1147 + B=(468,565)468x1147 —
// adjacent, filling the width — so no geometry difference vs the Unity case).
static const XrRect2Di kZoneARect = {{0, 90}, {640, 540}};
static const XrRect2Di kZoneBRect = {{640, 90}, {640, 540}};

// Zone-owned RTV descriptor heap (TEXTURE2DARRAY slice RTVs). Separate from
// renderer.rtvHeap (the main-swapchain RTVs) so zone slice views don't collide.
static ComPtr<ID3D12DescriptorHeap> g_zoneRtvHeap;
static UINT g_zoneRtvSize = 0;

// Zones activation: created a few frames in, once the session runs.
static bool g_zonesActive = false;
static bool g_zonesAttempted = false;
static long g_zonesFrameCounter = 0;
static const long kZonesActivationFrame = 10;

// DXR_ZONES_VALIDATE=1 — chain the validate bit on every frame-end info.
static bool ZonesValidateEnabled() {
    static const bool e = []() {
        const char* v = getenv("DXR_ZONES_VALIDATE");
        return v != nullptr && *v == '1';
    }();
    return e;
}

// ---------------------------------------------------------------------------
// XR_EXT_view_rig helpers (consume the runtime's render-ready XrView{pose,fov})
// ---------------------------------------------------------------------------

// Per-view staging container (matrices column-major).
struct RigView {
    float view_matrix[16];
    float projection_matrix[16];
    XrFovf fov;
};

// Column-major view matrix from a render-ready XrView pose: R^T * translate(-p).
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
// XrView fov + the app's clip policy. Pair with convert_projection_gl_to_zero_to_one.
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

// Display-local eye distance for the ZDP-anchored clip: z of (rig^-1 * eyeWorld).
// Degenerates to pose.position.z at the identity rig pose the zones path uses.
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

// ---------------------------------------------------------------------------
// Window / transparency
// ---------------------------------------------------------------------------

// Transparent window background — ON BY DEFAULT: zones alpha-composite against
// the desktop by design (translucent zone backgrounds, transparent unzoned
// regions). DISPLAYXR_TRANSPARENT_BG=0 opts out (opaque black floor).
static bool TransparentBackgroundEnabled() {
    static const bool enabled = []() {
        const char* v = getenv("DISPLAYXR_TRANSPARENT_BG");
        return v == nullptr || *v == '\0' || *v != '0';
    }();
    return enabled;
}

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

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            g_windowWidth = LOWORD(lParam);
            g_windowHeight = HIWORD(lParam);
        }
        return 0;

    case WM_CLOSE:
        // Graceful shutdown: ask OpenXR to end the session so the state machine
        // runs STOPPING -> xrEndSession -> EXITING -> exitRequested before cleanup.
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
        WINDOW_CLASS, WINDOW_TITLE, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd) {
        LOG_ERROR("Failed to create window, error: %lu", GetLastError());
        return nullptr;
    }
    LOG_INFO("Window created: 0x%p", hwnd);
    return hwnd;
}

struct PerformanceStats {
    std::chrono::high_resolution_clock::time_point lastTime;
    float deltaTime = 0.0f;
};
static void UpdatePerformanceStats(PerformanceStats& stats) {
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(now - stats.lastTime);
    stats.deltaTime = duration.count() / 1000000.0f;
    stats.lastTime = now;
}

// ---------------------------------------------------------------------------
// Zones activation (ARRAY layout)
// ---------------------------------------------------------------------------

// Create one zone's arraySize=2 swapchain + per-slice RTVs (appended into the
// shared zone RTV heap at rtvCursor). Returns false on any failure.
static bool CreateZoneResources(XrSessionManager& xr, D3D12Renderer& renderer,
                                DisplayZone& z, ID3D12Device* device, UINT& rtvCursor) {
    XrExtent2Di rec = {};
    XrResult r = g_zones.pfnGetViewSize(xr.session, &z.rect, &rec);
    if (XR_FAILED(r) || rec.width <= 0 || rec.height <= 0) {
        LOG_ERROR("[zones] zone %u: xrGetDisplayZoneRecommendedViewSizeEXT failed (0x%x, %dx%d)",
                  z.zoneId, (unsigned)r, rec.width, rec.height);
        return false;
    }
    z.tileW = (uint32_t)rec.width;
    z.tileH = (uint32_t)rec.height;
    z.format = xr.swapchain.format; // same encoding as the main projection swapchain

    // The main projection swapchain's format is an sRGB format
    // (DXGI_FORMAT_*_UNORM_SRGB) whenever the runtime advertises one, so the
    // cube shader writes linear color into a correctly display-encoding sRGB
    // target (INV-4.6) — no double-encode. Warn once if it isn't sRGB.
    const bool zoneFmtSrgb = (z.format == (int64_t)DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                              z.format == (int64_t)DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    if (!zoneFmtSrgb) {
        LOG_WARN("[zones] zone %u: swapchain format 0x%llx is not sRGB — content may double-encode",
                 z.zoneId, (unsigned long long)z.format);
    }

    // ARRAY layout: arraySize = kZoneArraySlices (2 views as array slices),
    // width/height = per-view tile size. NOT tileW*2 / arraySize=1.
    XrSwapchainCreateInfo sci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    sci.format = z.format;
    sci.sampleCount = 1;
    sci.width = z.tileW;
    sci.height = z.tileH;
    sci.faceCount = 1;
    sci.arraySize = kZoneArraySlices;
    sci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(xr.session, &sci, &z.swapchain))) {
        LOG_ERROR("[zones] zone %u: xrCreateSwapchain failed (%ux%u, arraySize=%u)",
                  z.zoneId, sci.width, sci.height, sci.arraySize);
        return false;
    }

    uint32_t n = 0;
    xrEnumerateSwapchainImages(z.swapchain, 0, &n, nullptr);
    z.images.resize(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    if (n == 0 || XR_FAILED(xrEnumerateSwapchainImages(z.swapchain, n, &n,
                                                       (XrSwapchainImageBaseHeader*)z.images.data()))) {
        LOG_ERROR("[zones] zone %u: xrEnumerateSwapchainImages failed", z.zoneId);
        return false;
    }
    if (n > 8) {
        LOG_ERROR("[zones] zone %u: unexpected image count %u (heap sized for 8)", z.zoneId, n);
        return false;
    }

    // One RTV per (image, slice), TEXTURE2DARRAY (FirstArraySlice=slice,
    // ArraySize=1). Resources are typeless (Vulkan interop) — RTV needs the
    // explicit typed format.
    z.rtvs.resize((size_t)n * kZoneArraySlices);
    for (uint32_t img = 0; img < n; img++) {
        for (uint32_t slice = 0; slice < kZoneArraySlices; slice++) {
            D3D12_RENDER_TARGET_VIEW_DESC rd = {};
            rd.Format = (DXGI_FORMAT)z.format;
            rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            rd.Texture2DArray.MipSlice = 0;
            rd.Texture2DArray.FirstArraySlice = slice;
            rd.Texture2DArray.ArraySize = 1;
            rd.Texture2DArray.PlaneSlice = 0;

            D3D12_CPU_DESCRIPTOR_HANDLE h = g_zoneRtvHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += (SIZE_T)rtvCursor * g_zoneRtvSize;
            device->CreateRenderTargetView(z.images[img].texture, &rd, h);
            z.rtvs[(size_t)img * kZoneArraySlices + slice] = h;
            rtvCursor++;
        }
    }

    LOG_INFO("[zones] zone %u: rect %d,%d %dx%d -> ARRAY swapchain %ux%u arraySize=%u (%u images)",
             z.zoneId, z.rect.offset.x, z.rect.offset.y, z.rect.extent.width, z.rect.extent.height,
             z.tileW, z.tileH, kZoneArraySlices, n);
    return true;
}

// One-time zones activation: capabilities check + per-zone array swapchains.
// On any failure the zones path is permanently disabled (empty-frame fallback).
static void TryActivateZones(XrSessionManager& xr, D3D12Renderer& renderer) {
    g_zonesAttempted = true;

    XrDisplayZoneCapabilitiesEXT caps = {XR_TYPE_DISPLAY_ZONE_CAPABILITIES_EXT};
    XrResult r = g_zones.pfnGetCaps(xr.session, &caps);
    if (XR_FAILED(r) || !caps.supported) {
        LOG_ERROR("[zones] xrGetDisplayZoneCapabilitiesEXT: rc=0x%x supported=%d — zones path disabled",
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

    // Zone A: left. Identity rig, phase 0, dark-red opaque clear.
    g_zonesArr[0].zoneId = 1;
    g_zonesArr[0].rect = kZoneARect;
    g_zonesArr[0].ipdFactor = 1.0f;
    g_zonesArr[0].perspectiveFactor = 1.0f;
    g_zonesArr[0].spinPhase = 0.0f;
    g_zonesArr[0].clearColor[0] = 0.15f;
    g_zonesArr[0].clearColor[1] = 0.03f;
    g_zonesArr[0].clearColor[2] = 0.03f;
    g_zonesArr[0].clearColor[3] = 1.0f;

    // Zone B: right. Reduced view spread + flattened perspective (visibly
    // different framing), phase +1.5 rad, FULLY TRANSPARENT clear (premultiplied
    // all-zero): the cube floats over the live desktop where alpha == 0.
    g_zonesArr[1].zoneId = 2;
    g_zonesArr[1].rect = kZoneBRect;
    g_zonesArr[1].ipdFactor = 0.6f;
    g_zonesArr[1].perspectiveFactor = 0.5f;
    g_zonesArr[1].spinPhase = 1.5f;
    g_zonesArr[1].clearColor[0] = 0.0f;
    g_zonesArr[1].clearColor[1] = 0.0f;
    g_zonesArr[1].clearColor[2] = 0.0f;
    g_zonesArr[1].clearColor[3] = 0.0f;

    // Zone RTV heap: kNumZones * (max images per zone) * kZoneArraySlices.
    // Sized generously (8 images per zone is far above any runtime's count).
    const UINT kMaxImagesPerZone = 8;
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = kNumZones * kMaxImagesPerZone * kZoneArraySlices;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (FAILED(renderer.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&g_zoneRtvHeap)))) {
        LOG_ERROR("[zones] zone RTV heap create failed");
        g_hasDisplayZonesExt = false;
        return;
    }
    g_zoneRtvSize = renderer.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    UINT rtvCursor = 0;
    for (uint32_t zi = 0; zi < kNumZones; zi++) {
        if (!CreateZoneResources(xr, renderer, g_zonesArr[zi], renderer.device.Get(), rtvCursor)) {
            g_hasDisplayZonesExt = false;
            return;
        }
    }

    g_zonesActive = true;
    LOG_INFO("[zones] ACTIVE: zone A %d,%d %dx%d + zone B %d,%d %dx%d "
             "(ARRAY layout, arraySize=%u, wish mode AUTO, validate=%d)",
             g_zonesArr[0].rect.offset.x, g_zonesArr[0].rect.offset.y,
             g_zonesArr[0].rect.extent.width, g_zonesArr[0].rect.extent.height,
             g_zonesArr[1].rect.offset.x, g_zonesArr[1].rect.offset.y,
             g_zonesArr[1].rect.extent.width, g_zonesArr[1].rect.extent.height,
             kZoneArraySlices, (int)ZonesValidateEnabled());
}

// The environment blend mode for zones submission (resolved once).
static XrEnvironmentBlendMode ZonesBlendMode() {
    static XrEnvironmentBlendMode mode = []() {
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
    return mode;
}

// ---------------------------------------------------------------------------
// Per-frame zones path (ARRAY layout)
// ---------------------------------------------------------------------------

static void RenderZonesFrame(XrSessionManager& xr, D3D12Renderer& renderer,
                             const XrFrameState& frameState) {
    // Per-zone locate + submit data. The zone structs are chained at BOTH
    // points (locate and xrEndFrame) — same instances within the frame.
    XrDisplayZoneEXT zoneStructs[kNumZones];
    XrDisplayRigEXT rigStructs[kNumZones];
    std::vector<XrCompositionLayerProjectionView> projViews[kNumZones];
    uint32_t submitViewCounts[kNumZones] = {};

    for (uint32_t zi = 0; zi < kNumZones; zi++) {
        DisplayZone& z = g_zonesArr[zi];

        rigStructs[zi] = {XR_TYPE_DISPLAY_RIG_EXT};
        rigStructs[zi].pose = {{0, 0, 0, 1}, {0, 0, 0}};
        rigStructs[zi].virtualDisplayHeight = kZoneVirtualDisplayHeight;
        rigStructs[zi].ipdFactor = z.ipdFactor;
        rigStructs[zi].parallaxFactor = 1.0f;
        rigStructs[zi].perspectiveFactor = z.perspectiveFactor;

        zoneStructs[zi] = {XR_TYPE_DISPLAY_ZONE_EXT};
        zoneStructs[zi].next = &rigStructs[zi];
        zoneStructs[zi].zoneId = z.zoneId;
        zoneStructs[zi].rect = z.rect;

        // Zone-scoped locate: chain the zone (+ rig) on XrViewLocateInfo.next.
        XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
        locateInfo.next = &zoneStructs[zi];
        locateInfo.viewConfigurationType = xr.viewConfigType;
        locateInfo.displayTime = frameState.predictedDisplayTime;
        locateInfo.space = xr.localSpace;

        // Locate into an XRT_MAX_VIEWS (8)-wide buffer (INV-3.1).
        XrViewState viewState = {XR_TYPE_VIEW_STATE};
        uint32_t viewCountOutput = 0;
        XrView zoneViews[8];
        for (uint32_t vi = 0; vi < 8; vi++) zoneViews[vi] = {XR_TYPE_VIEW};
        XrResult lr = xrLocateViews(xr.session, &locateInfo, &viewState, 8, &viewCountOutput, zoneViews);
        if (XR_FAILED(lr) || viewCountOutput == 0) {
            static bool warned = false;
            if (!warned) { warned = true;
                LOG_WARN("[zones] zone %u zone-scoped xrLocateViews failed (0x%x)", z.zoneId, (unsigned)lr); }
            submitViewCounts[zi] = 0;
            continue;
        }
        // Skip poses the runtime marked invalid (or zero quats from a
        // not-yet-tracking first frame) — xrEndFrame rejects them with
        // XR_ERROR_POSE_INVALID. The next locate after warmup is valid.
        const bool orientationValid =
            (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
        bool posesUsable = orientationValid;
        for (uint32_t vi = 0; posesUsable && vi < viewCountOutput && vi < 8; vi++) {
            const XrQuaternionf& q = zoneViews[vi].pose.orientation;
            if (q.x == 0.0f && q.y == 0.0f && q.z == 0.0f && q.w == 0.0f) posesUsable = false;
        }
        if (!posesUsable) {
            static bool warnedInvalid = false;
            if (!warnedInvalid) { warnedInvalid = true;
                LOG_WARN("[zones] zone %u locate returned invalid poses (flags=0x%llx) — "
                         "skipping submission until tracking is up",
                         z.zoneId, (unsigned long long)viewState.viewStateFlags); }
            submitViewCounts[zi] = 0;
            continue;
        }

        // xrLocateViews always reports the MAX view count; submit the ACTIVE
        // mode's view count, clamped to the array capacity (kZoneArraySlices).
        uint32_t activeViewCount = viewCountOutput;
        if (xr.renderingModeCount > 0 && xr.currentModeIndex < xr.renderingModeCount) {
            activeViewCount = xr.renderingModeViewCounts[xr.currentModeIndex];
        }
        const uint32_t n = (std::min)((std::min)(viewCountOutput, kZoneArraySlices), activeViewCount);
        submitViewCounts[zi] = n;
        projViews[zi].assign(n, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});

        // Render-ready views -> matrices. ZDP-anchored clip: near = ez - vH,
        // far = ez + 1000*vH (ez = rig-local eye z; identity rig -> pose z).
        std::vector<RigView> rigViews(n);
        for (uint32_t vi = 0; vi < n; vi++) {
            const XrView& v = zoneViews[vi];
            ViewMatrixFromXrPose(v.pose, rigViews[vi].view_matrix);
            const float ez = RigLocalEyeZ(rigStructs[zi].pose, v.pose.position);
            const float vH = kZoneVirtualDisplayHeight;
            const float nearZ = (ez - vH > 0.001f) ? (ez - vH) : 0.001f;
            const float farZ = ez + 1000.0f * vH;
            ProjectionFromXrFov(v.fov, nearZ, farZ, rigViews[vi].projection_matrix);
            convert_projection_gl_to_zero_to_one(rigViews[vi].projection_matrix);
            rigViews[vi].fov = v.fov;
        }

        // Acquire this zone's array swapchain image.
        XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        uint32_t imageIndex = 0;
        if (XR_FAILED(xrAcquireSwapchainImage(z.swapchain, &ai, &imageIndex))) {
            submitViewCounts[zi] = 0;
            continue;
        }
        XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = XR_INFINITE_DURATION;
        xrWaitSwapchainImage(z.swapchain, &wi);

        ID3D12Resource* zoneTex = z.images[imageIndex].texture;

        // Per-zone spin phase on the shared rotation (restored after render).
        const float savedRotation = renderer.cubeRotation;
        renderer.cubeRotation += z.spinPhase;

        // Render each view full-viewport into ITS array slice's RTV.
        for (uint32_t vi = 0; vi < n; vi++) {
            const D3D12_CPU_DESCRIPTOR_HANDLE rtv =
                z.rtvs[(size_t)imageIndex * kZoneArraySlices + vi];
            const XMMATRIX viewMatrix = ColumnMajorToXMMatrix(rigViews[vi].view_matrix);
            const XMMATRIX projMatrix = ColumnMajorToXMMatrix(rigViews[vi].projection_matrix);

            // rtvOverride = the array-slice RTV; dsvOverride = null (reuse the
            // shared atlas-sized depth — always >= a zone tile); per-zone clear.
            RenderScene(renderer, zoneTex, /*rtvIndex*/0,
                        /*viewportX*/0, /*viewportY*/0, z.tileW, z.tileH,
                        viewMatrix, projMatrix, /*zoomScale*/1.0f, /*clear*/true,
                        /*cubeHeight*/0.03f, /*cubeZ*/0.0f, /*cubeSize*/0.06f,
                        &rtv, /*dsvOverride*/nullptr, z.clearColor);

            projViews[zi][vi].subImage.swapchain = z.swapchain;
            projViews[zi][vi].subImage.imageRect.offset = {0, 0};
            projViews[zi][vi].subImage.imageRect.extent = {(int32_t)z.tileW, (int32_t)z.tileH};
            projViews[zi][vi].subImage.imageArrayIndex = vi; // ARRAY: view vi -> slice vi
            projViews[zi][vi].pose = zoneViews[vi].pose;
            projViews[zi][vi].fov = rigViews[vi].fov;
        }

        renderer.cubeRotation = savedRotation;

        XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(z.swapchain, &ri);
    }

    // Layer list: [projA (zone A chained), projB (zone B chained)].
    XrCompositionLayerProjection projLayers[kNumZones];
    const XrCompositionLayerBaseHeader* layers[kNumZones] = {};
    uint32_t layerCount = 0;

    for (uint32_t zi = 0; zi < kNumZones; zi++) {
        if (submitViewCounts[zi] == 0) continue;
        projLayers[zi] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        projLayers[zi].next = &zoneStructs[zi]; // SAME instance as the locate chain
        // Content alpha is meaningful (zone B transparent bg): premultiplied bytes.
        projLayers[zi].layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        projLayers[zi].space = xr.localSpace;
        projLayers[zi].viewCount = submitViewCounts[zi];
        projLayers[zi].views = projViews[zi].data();
        layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&projLayers[zi];
    }

    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = ZonesBlendMode();
    endInfo.layerCount = layerCount;
    endInfo.layers = layers;

    // Per-frame wish reference: AUTO (wishMask = XR_NULL_HANDLE → the runtime
    // auto-derives the wish from the zone rects). Chained on FrameEndInfo.next.
    XrDisplayZonesFrameEndInfoEXT zonesEnd = {(XrStructureType)XR_TYPE_DISPLAY_ZONES_FRAME_END_INFO_EXT};
    zonesEnd.flags = ZonesValidateEnabled() ? XR_DISPLAY_ZONES_FRAME_END_VALIDATE_BIT_EXT : 0;
    zonesEnd.wishMask = XR_NULL_HANDLE;
    endInfo.next = &zonesEnd;

    xrEndFrame(xr.session, &endInfo);
}

// ---------------------------------------------------------------------------
// Render thread
// ---------------------------------------------------------------------------

static void RenderThreadFunc(HWND hwnd, XrSessionManager* xr, D3D12Renderer* renderer) {
    LOG_INFO("[RenderThread] Started");
    PerformanceStats perfStats = {};
    perfStats.lastTime = std::chrono::high_resolution_clock::now();

    while (g_running.load() && !xr->exitRequested) {
        UpdatePerformanceStats(perfStats);
        UpdateScene(*renderer, perfStats.deltaTime, xr->spinSpeed);
        PollEvents(*xr);

        if (!xr->sessionRunning) {
            Sleep(100);
            continue;
        }

        XrFrameState frameState;
        if (!BeginFrame(*xr, frameState)) continue;

        // Activate zones a few frames in, once the session runs + dims settle.
        if (g_hasDisplayZonesExt && !g_zonesActive && !g_zonesAttempted &&
            g_zonesFrameCounter >= kZonesActivationFrame) {
            TryActivateZones(*xr, *renderer);
        }

        if (g_zonesActive && frameState.shouldRender) {
            RenderZonesFrame(*xr, *renderer, frameState);
        } else {
            // Fallback: submit an empty frame (no zones advertised, or warming up).
            XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
            endInfo.displayTime = frameState.predictedDisplayTime;
            endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            endInfo.layerCount = 0;
            endInfo.layers = nullptr;
            xrEndFrame(xr->session, &endInfo);
        }
        g_zonesFrameCounter++;
    }

    if (xr->exitRequested && g_running.load()) {
        PostMessage(hwnd, WM_CLOSE, 0, 0);
    }
    LOG_INFO("[RenderThread] Exiting");
}

// ---------------------------------------------------------------------------

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    if (!InitializeLogging(APP_NAME)) {
        MessageBox(nullptr, L"Failed to initialize logging", L"Warning", MB_OK | MB_ICONWARNING);
    }
    LOG_INFO("=== Cube Zones D3D12 (XR_EXT_display_zones, array/SPI) ===");

    HWND hwnd = CreateAppWindow(hInstance, g_windowWidth, g_windowHeight);
    if (!hwnd) {
        LOG_ERROR("Failed to create window");
        ShutdownLogging();
        return 1;
    }

    XrSessionManager xr = {};
    g_xr = &xr;
    if (!InitializeOpenXR(xr)) {
        LOG_ERROR("OpenXR initialization failed");
        g_xr = nullptr;
        ShutdownLogging();
        return 1;
    }

    LUID adapterLuid;
    if (!GetD3D12GraphicsRequirements(xr, &adapterLuid)) {
        LOG_ERROR("Failed to get D3D12 graphics requirements");
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    D3D12Renderer renderer = {};
    if (!InitializeD3D12WithLUID(renderer, adapterLuid)) {
        LOG_ERROR("D3D12 initialization failed");
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    if (!CreateSession(xr, renderer.device.Get(), renderer.commandQueue.Get(), hwnd)) {
        LOG_ERROR("OpenXR session creation failed");
        CleanupD3D12(renderer);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    if (!CreateSpaces(xr)) {
        LOG_ERROR("Reference space creation failed");
        CleanupOpenXR(xr);
        CleanupD3D12(renderer);
        ShutdownLogging();
        return 1;
    }

    // The main projection swapchain isn't rendered to in zones mode, but
    // creating it + its RTVs recreates the cube/grid PSOs to match the
    // swapchain format (which the zone swapchains reuse) and builds the shared
    // atlas-sized depth buffer that the zone renders reuse.
    if (!CreateSwapchain(xr)) {
        LOG_ERROR("Swapchain creation failed");
        CleanupOpenXR(xr);
        CleanupD3D12(renderer);
        ShutdownLogging();
        return 1;
    }
    {
        uint32_t count = xr.swapchain.imageCount;
        std::vector<XrSwapchainImageD3D12KHR> swapchainImages(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)swapchainImages.data());
        std::vector<ID3D12Resource*> textures(count);
        for (uint32_t i = 0; i < count; i++) textures[i] = swapchainImages[i].texture;
        if (!CreateSwapchainRTVs(renderer, textures.data(), count,
                                 xr.swapchain.width, xr.swapchain.height,
                                 (DXGI_FORMAT)xr.swapchain.format)) {
            LOG_ERROR("Failed to create RTVs / establish PSO format");
            CleanupOpenXR(xr);
            CleanupD3D12(renderer);
            ShutdownLogging();
            return 1;
        }
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    LOG_INFO("");
    LOG_INFO("=== Entering main loop === (F11=Fullscreen, ESC=Quit)");
    LOG_INFO("");

    std::thread renderThread(RenderThreadFunc, hwnd, &xr, &renderer);

    MSG msg = {};
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    g_running.store(false);
    LOG_INFO("Main thread: waiting for render thread...");
    renderThread.join();
    LOG_INFO("Main thread: render thread joined");

    LOG_INFO("=== Shutting down ===");
    g_zoneRtvHeap.Reset();
    g_xr = nullptr;
    CleanupOpenXR(xr);
    CleanupD3D12(renderer);

    DestroyWindow(hwnd);
    UnregisterClass(WINDOW_CLASS, hInstance);

    LOG_INFO("Application shutdown complete");
    ShutdownLogging();
    return 0;
}
