// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Cube Zones — XR_DXR_display_zones exerciser (ADR-027)
 *
 * Exercises the display-zones runtime path (N 3D zones + Local2D zones +
 * per-frame wish mask) end to end:
 *
 *  - Zone A (zoneId=1, left)  : rect {0,180,640,540}, identity display rig,
 *    spin phase 0, dark-red clear.
 *  - Zone B (zoneId=2, right) : rect {700,180,520,360}, display rig with
 *    ipdFactor 0.6 + perspectiveFactor 0.5 (visibly different framing),
 *    spin phase +1.5 rad, SEMI-TRANSPARENT dark-blue clear (alpha 0.55,
 *    premultiplied) so the O-key overlap visibly alpha-overs zone A.
 *  - Local2D strip (top)      : rect {0,0,1280,180}, always on, filled once
 *    with a CPU-generated checker + label band.
 *
 * Each zone owns ONE swapchain sized per xrGetDisplayZoneRecommendedViewSizeDXR,
 * horizontally tiled per view (width = recSize.width * viewCount); each frame
 * runs a zone-scoped locate (XrDisplayZoneDXR + XrDisplayRigDXR chained on
 * XrViewLocateInfo) and submits [projA, projB, strip] with the SAME zone
 * structs chained on the projection layers.
 *
 * Keys (zones mode):
 *  - M : cycle wish mode — 0 AUTO (no frame-end info) / 1 explicit Tier-2
 *        rects / 2 explicit Tier-3 feathered render-target mask.
 *  - O : toggle zone B between its home rect and a rect overlapping zone A
 *        (locate + submit always share the one rect variable).
 *  - DXR_ZONES_VALIDATE=1 : chain XR_DISPLAY_ZONES_FRAME_END_VALIDATE_BIT_DXR
 *        on the frame-end info in every mode (one-shot runtime WARNs).
 *
 * When the runtime doesn't advertise XR_DXR_display_zones (P2 dev gate:
 * DISPLAYXR_ZONES=1) the app logs an error once and keeps running as the
 * plain single-projection cube (graceful degrade).
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <wrl/client.h>
#include <d3dcompiler.h> // zone edge-fade pass (content-alpha feather)
#include <d3d11_1.h>

#include "logging.h"
#include "input_handler.h"
#include "d3d11_renderer.h"
#include "text_overlay.h"
#include "hud_renderer.h"
#include "xr_session.h"
#include "projection_depth.h"
#include "atlas_capture.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <sstream>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

// Application name for logging
static const char* APP_NAME = "cube_zones_d3d11_win";

// Window settings
static const wchar_t* WINDOW_CLASS = L"DXRCubeZonesClass";
static const wchar_t* WINDOW_TITLE = L"D3D11 Cube Zones — XR_DXR_display_zones";

// Global state (single-threaded — all accessed from the main thread only)
static InputState g_inputState;
static bool g_running = true;
static XrSessionManager* g_xr = nullptr;
static UINT g_windowWidth = 1280;
static UINT g_windowHeight = 720;
static bool g_inSizeMove = false;  // True while user is dragging/resizing the window
static const uint32_t HUD_PIXEL_WIDTH = 380;
static const uint32_t HUD_PIXEL_HEIGHT = 470;
static const float CAMERA_HALF_TAN_VFOV = 0.32491969623f; // tan(18°) → 36° vFOV
static const float HUD_WIDTH_FRACTION = 0.30f;

// Fullscreen state
static bool g_fullscreen = false;
static RECT g_savedWindowRect = {};
static DWORD g_savedWindowStyle = 0;

// ---------------------------------------------------------------------------
// XR_DXR_display_zones state
// ---------------------------------------------------------------------------

static const uint32_t kNumZones = 2;

// Per-zone rig framing for the test: virtual display height in app units
// (shared by both zones; the cube is 0.06 m tall).
static const float kZoneVirtualDisplayHeight = 0.30f;

struct DisplayZone {
    uint32_t zoneId = 0;
    XrRect2Di rect = {};            //!< client-window pixels; locate AND submit use this one variable
    float ipdFactor = 1.0f;
    float perspectiveFactor = 1.0f;
    float spinPhase = 0.0f;         //!< added to the shared cube rotation for this zone
    float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f}; //!< premultiplied RGBA — zone blends are
                                                    //!< expressed through content alpha (ADR-027)
    XrSwapchain swapchain = XR_NULL_HANDLE;
    int64_t format = 0;
    uint32_t tileW = 0;             //!< per-view tile width (= recommended view width)
    uint32_t tileH = 0;             //!< per-view tile height
    uint32_t tileCount = 0;         //!< view tiles in the horizontally tiled swapchain
    std::vector<XrSwapchainImageD3D11KHR> images;
    ComPtr<ID3D11Texture2D> depthTex;
    ComPtr<ID3D11DepthStencilView> depthDSV;
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

// Wish modes (M key): 0 AUTO, 1 explicit Tier-2 rects, 2 explicit Tier-3
// feathered render-target mask.
static int g_wishMode = 0;
static ID3D11RenderTargetView* g_wishRTV = nullptr; // runtime-owned (Tier-3 binding)
static uint32_t g_wishW = 0, g_wishH = 0;

// DXR_ZONES_VALIDATE=1 — chain the validate bit on every frame-end info.
static bool ZonesValidateEnabled() {
    static const bool e = []() {
        const char* v = getenv("DXR_ZONES_VALIDATE");
        return v != nullptr && *v == '1';
    }();
    return e;
}

// ---------------------------------------------------------------------------

// XR_DXR_view_rig: per-view staging container for the consumed render-ready
// views (matrices column-major).
struct RigView {
    float view_matrix[16];
    float projection_matrix[16];
    XrFovf fov;
};

// Column-major view matrix from a render-ready XrView pose:
// viewMatrix = R^T * translate(-position) — same construction as the
// displayxr::math rigs, so the runtime-rig path feeds the renderer the
// identical convention.
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
// eyeWorld). Degenerates to pose.position.z at identity rig pose (which the
// zones path uses), kept general for parity with the source app.
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

// Forward declaration — defined after PerformanceStats
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
        // During drag/resize, DefWindowProc runs a modal loop that blocks our
        // main message pump.  By leaving the window invalidated (no
        // BeginPaint/EndPaint), Windows keeps sending WM_PAINT inside that
        // modal loop, giving us a chance to keep rendering frames.
        if (g_inSizeMove && g_renderState != nullptr) {
            RenderOneFrame(*g_renderState);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;

    case WM_CLOSE:
        // Graceful shutdown: ask OpenXR to end the session so the state machine
        // runs STOPPING -> xrEndSession -> EXITING -> exitRequested before cleanup.
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

// Transparent window background — ON BY DEFAULT for this app: zones
// alpha-composite against the DESKTOP by design (translucent zone
// backgrounds, content-alpha edge fades, transparent unzoned regions), so
// the window uses WS_EX_NOREDIRECTIONBITMAP + a null brush so DComp can
// show the desktop through (mirrors cube_handle's DISPLAYXR_TRANSPARENT_BG
// plumbing, default-flipped). Set DISPLAYXR_TRANSPARENT_BG=0 to opt out
// (opaque black floor).
static bool TransparentBackgroundEnabled() {
    static const bool enabled = []() {
        const char* v = getenv("DISPLAYXR_TRANSPARENT_BG");
        return v == nullptr || *v == '\0' || *v != '0';
    }();
    return enabled;
}

// DISPLAYXR_ARRAY_LAYOUT=1 switches each zone from the default horizontally
// TILED single-slice swapchain (arraySize=1, width=tileW*viewCount, view vi at
// imageRect x=vi*tileW, imageArrayIndex=0) to the ARRAY / single-pass-instanced
// layout (arraySize=viewCount, width=tileW, view vi rendered into array slice vi
// via a TEXTURE2DARRAY RTV, submitted imageArrayIndex=vi, imageRect {0,0}).
// This exercises the runtime's per-view array-slice sampling (the D3D11 analog
// of the D3D12 #656 fix) — the same content must weave with disparity in both
// layouts. Default off (tiled), matching this app's original behavior.
static bool ArrayLayoutEnabled() {
    static const bool enabled = []() {
        const char* v = getenv("DISPLAYXR_ARRAY_LAYOUT");
        return v != nullptr && *v != '\0' && *v != '0';
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
    // Null brush in transparent mode so the redirection bitmap doesn't
    // paint an opaque floor under the DComp visual.
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

// State passed to RenderOneFrame (and accessible from WM_PAINT via g_renderState)
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
};

// ---------------------------------------------------------------------------
// Zones helpers
// ---------------------------------------------------------------------------

// Prefer an sRGB-encoded format for the Local2D strip when the session
// advertises one (INV-4.6); the CPU-authored bytes are display-referred, so
// declaring them _SRGB keeps the compositor's decode→encode round-trip honest.
static int64_t PickStripFormat(XrSessionManager& xr) {
    uint32_t n = 0;
    xrEnumerateSwapchainFormats(xr.session, 0, &n, nullptr);
    std::vector<int64_t> formats(n);
    if (n > 0) {
        xrEnumerateSwapchainFormats(xr.session, n, &n, formats.data());
    }
    for (int64_t f : formats) {
        if (f == (int64_t)DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) return f;
    }
    for (int64_t f : formats) {
        if (f == (int64_t)DXGI_FORMAT_B8G8R8A8_UNORM) return f;
    }
    return formats.empty() ? (int64_t)DXGI_FORMAT_B8G8R8A8_UNORM : formats[0];
}

// Create the always-on Local2D strip swapchain and fill it once (static
// content: acquire/fill/release once; the layer references the released image
// every frame). Checker + a solid label band; OPAQUE alpha throughout.
static bool CreateAndFillStrip(XrSessionManager& xr, ID3D11DeviceContext* context) {
    const uint32_t w = (uint32_t)kStripRect.extent.width;
    const uint32_t h = (uint32_t)kStripRect.extent.height;

    XrSwapchainCreateInfo sci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    sci.format = PickStripFormat(xr);
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
    std::vector<XrSwapchainImageD3D11KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
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
            // Label band: solid amber bar near the left so captures read
            // "this is the 2D strip" at a glance.
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
    context->UpdateSubresource(imgs[idx].texture, 0, nullptr, buf.data(), (UINT)stride, 0);

    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(g_strip.swapchain, &ri);
    return true;
}

// Create one zone's swapchain + depth, sized per
// xrGetDisplayZoneRecommendedViewSizeDXR, horizontally tiled per view.
static bool CreateZoneResources(XrSessionManager& xr, D3D11Renderer& renderer,
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
    z.format = xr.swapchain.format; // same encoding as the main projection swapchain

    const bool arrayLayout = ArrayLayoutEnabled();

    XrSwapchainCreateInfo sci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    sci.format = z.format;
    sci.sampleCount = 1;
    // ARRAY: one per-view-sized image with `viewCount` slices. TILED: one wide
    // image holding `viewCount` tiles side by side.
    sci.width = arrayLayout ? z.tileW : (z.tileW * z.tileCount);
    sci.height = z.tileH;
    sci.faceCount = 1;
    sci.arraySize = arrayLayout ? z.tileCount : 1;
    sci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(xr.session, &sci, &z.swapchain))) {
        LOG_ERROR("[zones] zone %u: xrCreateSwapchain failed (%ux%u arraySize=%u)",
                  z.zoneId, sci.width, sci.height, sci.arraySize);
        return false;
    }
    LOG_INFO("[zones] zone %u: %s swapchain %ux%u arraySize=%u (tile %ux%u, %u views)",
             z.zoneId, arrayLayout ? "ARRAY" : "TILED", sci.width, sci.height, sci.arraySize,
             z.tileW, z.tileH, z.tileCount);

    uint32_t n = 0;
    xrEnumerateSwapchainImages(z.swapchain, 0, &n, nullptr);
    z.images.resize(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (n == 0 || XR_FAILED(xrEnumerateSwapchainImages(z.swapchain, n, &n,
                                                       (XrSwapchainImageBaseHeader*)z.images.data()))) {
        LOG_ERROR("[zones] zone %u: xrEnumerateSwapchainImages failed", z.zoneId);
        return false;
    }

    // Depth is per-view-sized in ARRAY layout (each slice rendered full-viewport),
    // full-width in TILED layout (all tiles share one wide target).
    ID3D11Texture2D* depthTex = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    const uint32_t depthW = arrayLayout ? z.tileW : (z.tileW * z.tileCount);
    if (!CreateDepthStencilView(renderer, depthW, z.tileH, &depthTex, &dsv)) {
        LOG_ERROR("[zones] zone %u: depth buffer creation failed", z.zoneId);
        return false;
    }
    z.depthTex.Attach(depthTex);
    z.depthDSV.Attach(dsv);

    LOG_INFO("[zones] zone %u: rect %d,%d %dx%d -> %s (%u views of %ux%u)",
             z.zoneId, z.rect.offset.x, z.rect.offset.y, z.rect.extent.width, z.rect.extent.height,
             arrayLayout ? "ARRAY layout" : "TILED layout", z.tileCount, z.tileW, z.tileH);
    return true;
}

// One-time zones activation: capabilities check + per-zone swapchains + strip.
// On any failure the zones path is permanently disabled (plain fallback).
static void TryActivateZones(XrSessionManager& xr, D3D11Renderer& renderer) {
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

    // Zones share the session's view COUNT (display modes are session-global).
    // Allocate the zone swapchains at the MAX view count across all modes,
    // not the currently active mode's: standalone the session can start in a
    // 1-view mode (the service's tier-1 zones fallback flips to a multi-view
    // mode only at the first zones frame, AFTER these swapchains exist), and
    // a 1-tile swapchain would pin every zone to 1 view forever (#551). The
    // per-frame submit clamp to the ACTIVE mode's view count stays.
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

    // Zone B: right. Reduced view spread + flattened perspective (visibly
    // different framing), phase +1.5 rad, FULLY TRANSPARENT clear (premultiplied
    // RGBA all-zero): the cube floats over the LIVE desktop — the alpha-gate
    // punches every all-views-α==0 pixel in this zone through to DWM (#551). The
    // cube itself stays opaque; everything around it is see-through. (Contrast
    // zone A's opaque tint, which marks its boundary.)
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
        if (!CreateZoneResources(xr, renderer, g_zonesArr[zi], viewCount)) {
            g_hasDisplayZonesExt = false;
            return;
        }
    }

    if (!CreateAndFillStrip(xr, renderer.context.Get())) {
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

// Tier-3: acquire the freeform R8 render target (runtime-owned RTV).
static bool EnsureWishRenderTarget() {
    if (!g_zone.pfnAcquire || g_zone.mask == XR_NULL_HANDLE) return false;
    XrLocal3DZoneRenderTargetD3D11DXR binding = {
        (XrStructureType)XR_TYPE_LOCAL_3D_ZONE_RENDER_TARGET_D3D11_DXR};
    XrResult r = g_zone.pfnAcquire(g_zone.mask, &binding);
    if (XR_FAILED(r) || binding.renderTargetView == nullptr) {
        LOG_ERROR("[zones] xrAcquireLocal3DZoneRenderTargetDXR failed (0x%x)", (unsigned)r);
        return false;
    }
    g_wishRTV = (ID3D11RenderTargetView*)binding.renderTargetView;
    g_wishW = binding.width;
    g_wishH = binding.height;
    return true;
}

// ---- Zone edge fade (content-alpha feather) -------------------------------
//
// Per ADR-027 rule 4, zone blends are expressed through CONTENT alpha — a
// zone wanting a soft edge fades its OWN rendered alpha at the tile edges.
// Content-alpha fades composite correctly EVERYWHERE, including zone overlap
// (where the union wish mask is saturated at M=1 and cannot express per-zone
// fades). This 16-px multiplicative edge fade (dst *= f, premultiplied) is
// the recipe the avatar app's tiger zone uses later.
static ID3D11VertexShader* g_fadeVS = nullptr;
static ID3D11PixelShader* g_fadePS = nullptr;
static ID3D11Buffer* g_fadeCB = nullptr;
static ID3D11BlendState* g_fadeBlend = nullptr;
static ID3D11DepthStencilState* g_fadeDepthOff = nullptr;
static ID3D11RasterizerState* g_fadeRaster = nullptr;
static bool g_fadeInitTried = false;
// Content-alpha edge fade width. DXR_ZONES_FADE_PX overrides (validation
// runs use exaggerated widths to make the composite math obvious in
// captures); 0 disables the fade pass.
static float ZoneEdgeFadePx() {
    static const float px = []() {
        const char* v = getenv("DXR_ZONES_FADE_PX");
        if (v != nullptr && *v != '\0') {
            float f = (float)atof(v);
            if (f >= 0.0f && f <= 256.0f) return f;
        }
        return 16.0f;
    }();
    return px;
}

static const char* kFadeShaderSrc = R"(
cbuffer FadeCB : register(b0) { float2 tile_px; float feather_px; float pad; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs_main(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}
float4 ps_main(VSOut i) : SV_Target {
    float2 px = i.uv * tile_px;
    float d = min(min(px.x, tile_px.x - px.x), min(px.y, tile_px.y - px.y));
    float f = saturate(d / feather_px);
    // Blend (ZERO, INV_SRC_ALPHA): dst *= (1 - src.a) = f.
    return float4(0.0, 0.0, 0.0, 1.0 - f);
}
)";

static bool EnsureEdgeFadePass(D3D11Renderer& renderer) {
    if (g_fadeVS && g_fadePS && g_fadeCB && g_fadeBlend && g_fadeDepthOff) return true;
    if (g_fadeInitTried) return false;
    g_fadeInitTried = true;

    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3DCompile(kFadeShaderSrc, strlen(kFadeShaderSrc), nullptr, nullptr, nullptr,
                          "vs_main", "vs_5_0", 0, 0, &blob, &err)) ||
        FAILED(renderer.device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                                   nullptr, &g_fadeVS))) {
        LOG_ERROR("[zones] edge-fade VS compile/create failed%s%s", err ? ": " : "",
                  err ? (const char*)err->GetBufferPointer() : "");
        return false;
    }
    blob.Reset(); err.Reset();
    if (FAILED(D3DCompile(kFadeShaderSrc, strlen(kFadeShaderSrc), nullptr, nullptr, nullptr,
                          "ps_main", "ps_5_0", 0, 0, &blob, &err)) ||
        FAILED(renderer.device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                                  nullptr, &g_fadePS))) {
        LOG_ERROR("[zones] edge-fade PS compile/create failed%s%s", err ? ": " : "",
                  err ? (const char*)err->GetBufferPointer() : "");
        return false;
    }

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = 16;
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(renderer.device->CreateBuffer(&cbd, nullptr, &g_fadeCB))) return false;

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(renderer.device->CreateBlendState(&bd, &g_fadeBlend))) return false;

    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable = FALSE;
    if (FAILED(renderer.device->CreateDepthStencilState(&dsd, &g_fadeDepthOff))) return false;

    // The scene renderer leaves a FrontCounterClockwise + CULL_BACK
    // rasterizer state bound, which culls our clockwise fullscreen
    // triangle — the fade needs its own CULL_NONE state.
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(renderer.device->CreateRasterizerState(&rd, &g_fadeRaster))) return false;

    LOG_INFO("[zones] content-alpha edge fade pass ready (%.0f px)", ZoneEdgeFadePx());
    return true;
}

// Multiply the current tile's RGBA by the edge ramp (viewport must already
// be the tile; rtv bound without depth).
static void DrawZoneEdgeFade(D3D11Renderer& renderer, ID3D11RenderTargetView* rtv,
                             uint32_t tileW, uint32_t tileH) {
    if (ZoneEdgeFadePx() <= 0.0f) return; // DXR_ZONES_FADE_PX=0 disables
    if (!EnsureEdgeFadePass(renderer)) return;

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(renderer.context->Map(g_fadeCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        float cb[4] = {(float)tileW, (float)tileH, ZoneEdgeFadePx(), 0.0f};
        memcpy(mapped.pData, cb, sizeof(cb));
        renderer.context->Unmap(g_fadeCB, 0);
    }

    renderer.context->OMSetRenderTargets(1, &rtv, nullptr);
    renderer.context->OMSetBlendState(g_fadeBlend, nullptr, 0xFFFFFFFF);
    renderer.context->OMSetDepthStencilState(g_fadeDepthOff, 0);
    renderer.context->RSSetState(g_fadeRaster);
    renderer.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    renderer.context->IASetInputLayout(nullptr);
    renderer.context->VSSetShader(g_fadeVS, nullptr, 0);
    renderer.context->PSSetShader(g_fadePS, nullptr, 0);
    renderer.context->PSSetConstantBuffers(0, 1, &g_fadeCB);
    renderer.context->Draw(3, 0);

    ID3D11RenderTargetView* nullRtv = nullptr;
    renderer.context->OMSetRenderTargets(1, &nullRtv, nullptr);
    renderer.context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    renderer.context->OMSetDepthStencilState(nullptr, 0);
}

// Tier-3 feathered wish: clear to 0, then per zone rect paint 8 concentric
// inward-inset rects ramping M 0.125 -> 1.0 (3 px per ring, 24 px total
// feather) plus a solid-1.0 core. ID3D11DeviceContext1::ClearView only — no
// shader needed.
static void DrawFeatheredWish(D3D11Renderer& renderer) {
    if (!g_wishRTV || g_wishW == 0 || g_wishH == 0) return;
    ComPtr<ID3D11DeviceContext1> ctx1;
    if (FAILED(renderer.context.As(&ctx1)) || !ctx1) {
        LOG_ERROR("[zones] ID3D11DeviceContext1 unavailable — Tier-3 wish not drawn");
        return;
    }

    const FLOAT zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    ctx1->ClearView(g_wishRTV, zero, nullptr, 0);

    // Mask pixels are client-window pixels; scale in case the runtime's
    // backing size differs from the live client size.
    const float sx = (g_windowWidth > 0) ? (float)g_wishW / (float)g_windowWidth : 1.0f;
    const float sy = (g_windowHeight > 0) ? (float)g_wishH / (float)g_windowHeight : 1.0f;

    const int kRings = 8;
    const int kRingStep = 3;   // px per ring (window pixels)
    const int kFeather = 24;   // solid core inset

    for (uint32_t zi = 0; zi < kNumZones; zi++) {
        const XrRect2Di& zr = g_zonesArr[zi].rect;
        for (int step = 0; step <= kRings; step++) {
            const bool core = (step == kRings);
            const float m = core ? 1.0f : (float)(step + 1) / (float)kRings;
            const int inset = core ? kFeather : step * kRingStep;
            LONG l = (LONG)((zr.offset.x + inset) * sx);
            LONG t = (LONG)((zr.offset.y + inset) * sy);
            LONG rgt = (LONG)((zr.offset.x + zr.extent.width - inset) * sx);
            LONG b = (LONG)((zr.offset.y + zr.extent.height - inset) * sy);
            // (parenthesized calls — windows.h min/max macros are active in this TU)
            l = (std::max)(l, (LONG)0);
            t = (std::max)(t, (LONG)0);
            rgt = (std::min)(rgt, (LONG)g_wishW);
            b = (std::min)(b, (LONG)g_wishH);
            if (rgt <= l || b <= t) continue;
            D3D11_RECT rc = {l, t, rgt, b};
            const FLOAT mv[4] = {m, m, m, m}; // R8_UNORM: .r carries M
            ctx1->ClearView(g_wishRTV, mv, &rc, 1);
        }
    }
}

// Re-author the wish for the current mode (entering a mode, or after an O
// rect toggle while in an explicit mode). Mode 0 authors nothing (AUTO).
static void ApplyWishAuthoring(XrSessionManager& xr, D3D11Renderer& renderer) {
    if (g_wishMode == 1) {
        if (!EnsureWishMask(xr)) return;
        XrRect2Di rects[kNumZones];
        for (uint32_t zi = 0; zi < kNumZones; zi++) rects[zi] = g_zonesArr[zi].rect;
        XrResult r = g_zone.pfnSetRects(g_zone.mask, kNumZones, rects);
        if (XR_FAILED(r)) {
            LOG_ERROR("[zones] xrSetLocal3DZoneFromRectsDXR failed (0x%x)", (unsigned)r);
        }
    } else if (g_wishMode == 2) {
        if (!EnsureWishMask(xr)) return;
        if (!EnsureWishRenderTarget()) {
            LOG_ERROR("[zones] Tier-3 unavailable — staying on AUTO wish");
            g_wishMode = 0;
            return;
        }
        DrawFeatheredWish(renderer);
    }
}

static const char* WishModeName(int mode) {
    switch (mode) {
    case 1: return "explicit Tier-2 rects";
    case 2: return "explicit Tier-3 feathered";
    default: return "AUTO";
    }
}

// Edge-triggered M (wish mode cycle) + O (zone B overlap toggle) polling.
// GetAsyncKeyState keeps the InputState plumbing untouched.
static void HandleZoneKeys(XrSessionManager& xr, D3D11Renderer& renderer) {
    static bool mPrev = false, oPrev = false;

    const bool mNow = (GetAsyncKeyState('M') & 0x8000) != 0;
    if (mNow && !mPrev) {
        g_wishMode = (g_wishMode + 1) % 3;
        if (g_wishMode == 2 && (!g_zone.pfnAcquire)) {
            // Tier-3 entry point unresolved — skip to AUTO.
            LOG_WARN("[zones] Tier-3 entry point unresolved — skipping wish mode 2");
            g_wishMode = 0;
        }
        LOG_INFO("[zones] wish mode %d (%s)", g_wishMode, WishModeName(g_wishMode));
        ApplyWishAuthoring(xr, renderer);
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
        // Keep an explicit wish in sync with the moved rect.
        ApplyWishAuthoring(xr, renderer);
    }
    oPrev = oNow;
}

// Per-frame zones path: zone-scoped locate, per-zone render, submit
// [projA, projB, strip] with the zone structs chained on the projections.
static void RenderZonesFrame(RenderState& rs, const XrFrameState& frameState) {
    XrSessionManager& xr = *rs.xr;
    D3D11Renderer& renderer = *rs.renderer;

    // Per-zone locate + submit data. The zone structs are chained at BOTH
    // points (locate and xrEndFrame) — same instances within the frame.
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
        // not-yet-tracking first frame) — xrEndFrame rejects them with
        // XR_ERROR_POSE_INVALID. Skipping this zone for the frame is the
        // correct degradation; the next locate after tracking warmup is valid.
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

        // xrLocateViews always reports the MAX view count (identical centered
        // views in a mono mode) — a well-behaved extension app submits only
        // the ACTIVE mode's view count (#542: 1 tile in a 2D mode; the
        // runtime clamps over-submission to the mode regardless, this is the
        // proper app-side behavior).
        // (parenthesized calls — windows.h min/max macros are active in this TU)
        uint32_t activeViewCount = viewCountOutput;
        if (xr.renderingModeCount > 0 && xr.currentModeIndex < xr.renderingModeCount) {
            activeViewCount = xr.renderingModeViewCounts[xr.currentModeIndex];
        }
        const uint32_t n = (std::min)((std::min)(viewCountOutput, z.tileCount), activeViewCount);
        submitViewCounts[zi] = n;
        projViews[zi].assign(n, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});

        // Render-ready views -> matrices. ZDP-anchored clip: near = ez - vH,
        // far = ez + 1000*vH, ez = rig-local eye distance to the zone's
        // virtual display plane (identity rig here, so ez = pose z).
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

        const bool arrayLayout = ArrayLayoutEnabled();

        // TILED: one RTV over the whole wide image (each view drawn at a tile
        // viewport). ARRAY: one RTV per slice (TEXTURE2DARRAY, FirstArraySlice=vi),
        // each view drawn full-viewport into its slice. Clear once per RTV.
        ID3D11RenderTargetView* rtv = nullptr; // TILED shared RTV
        if (!arrayLayout) {
            CreateRenderTargetView(renderer, z.images[imageIndex].texture,
                                   static_cast<DXGI_FORMAT>(z.format), &rtv);
            renderer.context->ClearRenderTargetView(rtv, z.clearColor);
        }
        renderer.context->ClearDepthStencilView(z.depthDSV.Get(),
            D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

        // Per-zone spin phase on the shared rotation (restored after render).
        const float savedRotation = renderer.cubeRotation;
        renderer.cubeRotation += z.spinPhase;

        for (uint32_t vi = 0; vi < n; vi++) {
            // Select the render target + viewport for this view.
            ID3D11RenderTargetView* viewRtv = rtv; // TILED default
            ID3D11RenderTargetView* sliceRtv = nullptr;
            if (arrayLayout) {
                // Per-slice TEXTURE2DARRAY RTV pinned to slice vi.
                D3D11_RENDER_TARGET_VIEW_DESC rd = {};
                rd.Format = static_cast<DXGI_FORMAT>(z.format);
                rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                rd.Texture2DArray.MipSlice = 0;
                rd.Texture2DArray.FirstArraySlice = vi;
                rd.Texture2DArray.ArraySize = 1;
                if (FAILED(renderer.device->CreateRenderTargetView(
                        z.images[imageIndex].texture, &rd, &sliceRtv))) {
                    LOG_ERROR("[zones] zone %u view %u: array RTV creation failed", z.zoneId, vi);
                    continue;
                }
                renderer.context->ClearRenderTargetView(sliceRtv, z.clearColor);
                // Each slice renders full-viewport into the SAME shared depth
                // buffer, so depth MUST be cleared per slice — otherwise slice 1
                // z-tests against slice 0's depth and the other eye's cube punches
                // a shadow through (#613). TILED tiles occupy disjoint depth
                // regions and share the single pre-loop clear.
                renderer.context->ClearDepthStencilView(z.depthDSV.Get(),
                    D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
                viewRtv = sliceRtv;
            }

            D3D11_VIEWPORT vp = {};
            vp.TopLeftX = arrayLayout ? 0.0f : (FLOAT)(vi * z.tileW);
            vp.TopLeftY = 0.0f;
            vp.Width = (FLOAT)z.tileW;
            vp.Height = (FLOAT)z.tileH;
            vp.MaxDepth = 1.0f;
            renderer.context->RSSetViewports(1, &vp);

            const XMMATRIX viewMatrix = ColumnMajorToXMMatrix(rigViews[vi].view_matrix);
            const XMMATRIX projMatrix = ColumnMajorToXMMatrix(rigViews[vi].projection_matrix);
            RenderScene(renderer, viewRtv, z.depthDSV.Get(), z.tileW, z.tileH,
                        viewMatrix, projMatrix, 1.0f, 0.03f);

            projViews[zi][vi].subImage.swapchain = z.swapchain;
            // ARRAY: full image at slice vi. TILED: tile vi within the wide image.
            projViews[zi][vi].subImage.imageRect.offset = {arrayLayout ? 0 : (int32_t)(vi * z.tileW), 0};
            projViews[zi][vi].subImage.imageRect.extent = {(int32_t)z.tileW, (int32_t)z.tileH};
            projViews[zi][vi].subImage.imageArrayIndex = arrayLayout ? vi : 0;
            projViews[zi][vi].pose = zoneViews[vi].pose;
            projViews[zi][vi].fov = rigViews[vi].fov;

            if (sliceRtv) sliceRtv->Release();
        }

        renderer.cubeRotation = savedRotation;

        // Content-alpha edge feather (ADR-027 rule 4): fade THIS zone's
        // rendered RGBA at its tile edges so the zone blends softly into
        // whatever is behind it — desktop OR another zone (the union wish
        // mask cannot express per-zone fades inside an overlap). One pass
        // per view tile, after the scene render.
        //
        // Skipped in wish mode 1 (explicit Tier-2 rects): that wish is
        // M=1 out to the exact rect edge, and weave output carries no
        // alpha — content faded inside a hard-M=1 band weaves to opaque
        // black (the dark-halo mechanism), not to the desktop. Tier-2
        // content must fill its rect to the hard edge; soft outer edges
        // belong to AUTO / Tier-3, whose wish feathers M inward to 0.
        if (g_wishMode != 1) {
            for (uint32_t vi = 0; vi < n; vi++) {
                ID3D11RenderTargetView* fadeRtv = rtv; // TILED shared RTV
                ID3D11RenderTargetView* sliceRtv = nullptr;
                if (arrayLayout) {
                    D3D11_RENDER_TARGET_VIEW_DESC rd = {};
                    rd.Format = static_cast<DXGI_FORMAT>(z.format);
                    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                    rd.Texture2DArray.MipSlice = 0;
                    rd.Texture2DArray.FirstArraySlice = vi;
                    rd.Texture2DArray.ArraySize = 1;
                    if (FAILED(renderer.device->CreateRenderTargetView(
                            z.images[imageIndex].texture, &rd, &sliceRtv))) {
                        continue;
                    }
                    fadeRtv = sliceRtv;
                }
                D3D11_VIEWPORT vp = {};
                vp.TopLeftX = arrayLayout ? 0.0f : (FLOAT)(vi * z.tileW);
                vp.TopLeftY = 0.0f;
                vp.Width = (FLOAT)z.tileW;
                vp.Height = (FLOAT)z.tileH;
                vp.MaxDepth = 1.0f;
                renderer.context->RSSetViewports(1, &vp);
                DrawZoneEdgeFade(renderer, fadeRtv, z.tileW, z.tileH);
                if (sliceRtv) sliceRtv->Release();
            }
        }

        if (rtv) rtv->Release();

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
        // Content alpha is meaningful (zone B translucent bg + the edge
        // fade): declare source-alpha blending (premultiplied bytes).
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

    // Zones alpha-composite against the DESKTOP by design (translucent zone
    // backgrounds, content-alpha edge fades, transparent unzoned regions) —
    // submit ALPHA_BLEND whenever the runtime advertises it. With OPAQUE the
    // whole window floor is black and every translucent pixel reads as
    // "faded to black". (No DISPLAYXR_TRANSPARENT_BG gate here: desktop
    // show-through is the point of this demo, as it will be for the avatar.)
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
    // requested; in modes 1/2 the mask is the frame's wish, atomic with the
    // layer set.
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
}

// ---------------------------------------------------------------------------

// Render a single frame — called from the main loop and from WM_PAINT during
// drag/resize so that rendering never stalls.
static void RenderOneFrame(RenderState& rs) {
    XrSessionManager& xr = *rs.xr;
    D3D11Renderer& renderer = *rs.renderer;

    // Update performance stats
    UpdatePerformanceStats(*rs.perfStats);

    // Update input-based camera movement (clears resetViewRequested internally)
    UpdateCameraMovement(g_inputState, rs.perfStats->deltaTime, rs.xr->displayHeightM);

    // Handle fullscreen toggle (F11)
    if (g_inputState.fullscreenToggleRequested) {
        ToggleFullscreen(rs.hwnd);
        g_inputState.fullscreenToggleRequested = false;
    }

    // Handle rendering mode requests (V=cycle next, 0-8=jump absolute).
    // Single source of truth: the runtime owns the current mode; keypresses
    // are REQUESTS via xrRequestDisplayRenderingModeDXR.
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

    // Handle eye tracking mode toggle (T key)
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
    UpdateScene(renderer, rs.perfStats->deltaTime, xr.spinSpeed);

    // Poll OpenXR events
    PollEvents(xr);

    // Only render if session is running
    if (!xr.sessionRunning) {
        Sleep(100);
        return;
    }

    XrFrameState frameState;
    if (!BeginFrame(xr, frameState)) {
        return;
    }

    // ---- zones path -------------------------------------------------------
    g_zonesFrameCounter++;
    if (g_hasDisplayZonesExt && !g_zonesActive && !g_zonesAttempted &&
        g_zonesFrameCounter >= kZonesActivationFrame) {
        TryActivateZones(xr, renderer);
    }
    if (g_zonesActive && g_hasDisplayZonesExt) {
        HandleZoneKeys(xr, renderer);
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
    // activation failed, or pre-activation frames) ---------------------------
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
    // Only submit the projection layer once the locate+render below actually
    // filled it — a first-frame locate failure (tracking warmup) or a skipped
    // render otherwise submits default-constructed views whose zero quats the
    // runtime rejects with XR_ERROR_POSE_INVALID.
    bool viewsPopulated = false;

    if (frameState.shouldRender) {
        if (LocateViews(xr, frameState.predictedDisplayTime,
            g_inputState.cameraPosX, g_inputState.cameraPosY, g_inputState.cameraPosZ,
            g_inputState.yaw, g_inputState.pitch,
            g_inputState.viewParams)) {

            // Get raw view poses (pre-player-transform) for projection views.
            XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
            locateInfo.viewConfigurationType = xr.viewConfigType;
            locateInfo.displayTime = frameState.predictedDisplayTime;
            locateInfo.space = xr.localSpace;

            XrViewState viewState = {XR_TYPE_VIEW_STATE};
            uint32_t viewCount = 8;
            XrView rawViews[8];
            for (uint32_t vi = 0; vi < 8; vi++) rawViews[vi] = {XR_TYPE_VIEW};

            // XR_DXR_view_rig raw channel: chain XrViewDisplayRawDXR so the
            // runtime reports the resolved canvas size in meters — fed into
            // InputState below for the C-toggle / SPACE-reset converter.
            XrViewDisplayRawDXR rawProbe = {XR_TYPE_VIEW_DISPLAY_RAW_DXR};
            if (g_hasViewRigExt) {
                viewState.next = &rawProbe;
            }

            // XR_DXR_view_rig: drive the runtime rig matching the app's
            // current mode with the app's tunables — the runtime owns the
            // window/canvas resolve + the Kooima math and returns
            // render-ready XrView{pose, fov}. Per-locate semantics: the rig
            // must be chained on every consume locate.
            const bool useAppProjection =
                xr.hasDisplayInfoExt && xr.displayWidthM > 0.0f && g_hasViewRigExt;
            const bool rigCamera = useAppProjection && g_inputState.cameraMode;
            XrCameraRigDXR cameraRig = {XR_TYPE_CAMERA_RIG_DXR};
            XrDisplayRigDXR displayRig = {XR_TYPE_DISPLAY_RIG_DXR};
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
                    // metersToVirtual carries the eye scale the C-toggle
                    // converter derived from the display rig, so the camera
                    // rig reproduces the display rig exactly.
                    cameraRig.metersToVirtual = g_inputState.viewParams.cameraM2v;
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

            // Capture the runtime's resolved CANVAS size (the window client
            // area in meters) so the C-toggle / SPACE-reset converter runs the
            // rig math on the right physical_height_m.
            if (g_hasViewRigExt && rawProbe.canvasSizeMeters.height > 0.0f) {
                g_inputState.canvasWidthM = rawProbe.canvasSizeMeters.width;
                g_inputState.canvasHeightM = rawProbe.canvasSizeMeters.height;
            }

            // Max per-tile capacity from swapchain
            uint32_t maxTileW = tileColumns > 0 ? xr.swapchain.width / tileColumns : xr.swapchain.width;
            uint32_t maxTileH = tileRows > 0 ? xr.swapchain.height / tileRows : xr.swapchain.height;

            std::vector<RigView> rigViews(eyeCount);
            if (useAppProjection) {
                // Consume the runtime's render-ready XrView{pose, fov}. Only
                // clip policy stays app-side. Display rig: ZDP-anchored clip.
                const float rigVH =
                    g_inputState.viewParams.virtualDisplayHeight / g_inputState.viewParams.scaleFactor;
                for (int i = 0; i < eyeCount; i++) {
                    const XrView& v = rawViews[(i < (int)viewCount) ? i : 0];
                    ViewMatrixFromXrPose(v.pose, rigViews[i].view_matrix);
                    float nearZ = 0.01f, farZ = 100.0f;
                    if (!rigCamera) {
                        float ez = RigLocalEyeZ(rigPose, v.pose.position);
                        nearZ = (ez - rigVH > 0.001f) ? (ez - rigVH) : 0.001f;
                        farZ = ez + 1000.0f * rigVH;
                    }
                    ProjectionFromXrFov(v.fov, nearZ, farZ, rigViews[i].projection_matrix);
                    convert_projection_gl_to_zero_to_one(rigViews[i].projection_matrix);
                    rigViews[i].fov = v.fov;
                }
            }

            // Render HUD to window-space layer swapchain (once per frame, before view loop)
            if (g_inputState.hudVisible && xr.hasHudSwapchain && rs.hudSwapchainImages && !rs.hudSwapchainImages->empty() && rs.hudOk) {
                uint32_t hudImageIndex;
                if (AcquireHudSwapchainImage(xr, hudImageIndex)) {
                    std::wstring sessionText(xr.systemName, xr.systemName + strlen(xr.systemName));
                    sessionText += L"\nSession: ";
                    sessionText += FormatSessionState((int)xr.sessionState);
                    std::wstring modeText = g_hasDisplayZonesExt ?
                        L"XR_DXR_display_zones: ACTIVATING" :
                        L"XR_DXR_display_zones: NOT AVAILABLE (DISPLAYXR_ZONES=1?)";
                    modeText += g_inputState.cameraMode ?
                        L"\nKooima: Camera-Centric [C=Toggle]" :
                        L"\nKooima: Display-Centric [C=Toggle]";
                    modeText += g_hasViewRigExt ?
                        L"\nView rig: RUNTIME rig (XR_DXR_view_rig)" :
                        L"\nView rig: unavailable (legacy views)";

                    // Dynamic render dims matching the actual viewport computation
                    bool dispMonoMode = (xr.renderingModeCount > 0 && !xr.renderingModeDisplay3D[xr.currentModeIndex]);
                    uint32_t dispRenderW, dispRenderH;
                    if (dispMonoMode) {
                        dispRenderW = g_windowWidth;
                        dispRenderH = g_windowHeight;
                        if (dispRenderW > xr.swapchain.width) dispRenderW = xr.swapchain.width;
                        if (dispRenderH > xr.swapchain.height) dispRenderH = xr.swapchain.height;
                    } else {
                        dispRenderW = (uint32_t)(g_windowWidth * xr.recommendedViewScaleX);
                        dispRenderH = (uint32_t)(g_windowHeight * xr.recommendedViewScaleY);
                        if (dispRenderW > maxTileW) dispRenderW = maxTileW;
                        if (dispRenderH > maxTileH) dispRenderH = maxTileH;
                    }
                    std::wstring perfText = FormatPerformanceInfo(rs.perfStats->fps, rs.perfStats->frameTimeMs,
                        dispRenderW, dispRenderH,
                        g_windowWidth, g_windowHeight);
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
                    std::wstring viewParamText = FormatViewParams(
                        g_inputState.viewParams.ipdFactor, g_inputState.viewParams.parallaxFactor,
                        dispP1, dispP2, g_inputState.cameraMode);
                    std::wstring helpText = FormatHelpText(xr.pfnRequestDisplayRenderingModeEXT != nullptr, g_inputState.cameraMode, xr.renderingModeCount);

                    uint32_t srcRowPitch = 0;
                    const void* pixels = RenderHudAndMap(*rs.hudRenderer, &srcRowPitch,
                        sessionText, modeText, perfText, dispText, eyeText,
                        cameraText, viewParamText, helpText);
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

            // For mono: compute center view position and projection
            XMMATRIX monoViewMatrix, monoProjMatrix;
            XrFovf monoFov = {};
            XrPosef monoPose = rawViews[0].pose;
            if (monoMode) {
                // Center view = average of all view positions
                monoPose.position = {0.0f, 0.0f, 0.0f};
                int cnt = (int)viewCount;
                if (cnt < 1) cnt = 1;
                for (int v = 0; v < cnt; v++) {
                    monoPose.position.x += rawViews[v].pose.position.x;
                    monoPose.position.y += rawViews[v].pose.position.y;
                    monoPose.position.z += rawViews[v].pose.position.z;
                }
                monoPose.position.x /= cnt;
                monoPose.position.y /= cnt;
                monoPose.position.z /= cnt;

                // When useAppProjection, mono view+proj come from rigViews[0]
                // (the runtime centers the rig view poses in 2D mode).
                if (!useAppProjection) {
                    monoProjMatrix = xr.projMatrices[0];  // Close enough for 2D
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

            // Single swapchain: acquire once, render all views, release once
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

                // Dynamic render dims based on window size, clamped to swapchain capacity
                uint32_t renderW, renderH;
                if (monoMode) {
                    renderW = g_windowWidth;
                    renderH = g_windowHeight;
                    if (renderW > xr.swapchain.width) renderW = xr.swapchain.width;
                    if (renderH > xr.swapchain.height) renderH = xr.swapchain.height;
                } else {
                    renderW = (uint32_t)(g_windowWidth * xr.recommendedViewScaleX);
                    renderH = (uint32_t)(g_windowHeight * xr.recommendedViewScaleY);
                    if (renderW > maxTileW) renderW = maxTileW;
                    if (renderH > maxTileH) renderH = maxTileH;
                }

                for (int eye = 0; eye < eyeCount; eye++) {
                    uint32_t tileX = monoMode ? 0 : (eye % tileColumns);
                    uint32_t tileY = monoMode ? 0 : (eye / tileColumns);
                    D3D11_VIEWPORT vp = {};
                    vp.TopLeftX = (FLOAT)(tileX * renderW);
                    vp.TopLeftY = (FLOAT)(tileY * renderH);
                    vp.Width = (FLOAT)renderW;
                    vp.Height = (FLOAT)renderH;
                    vp.MaxDepth = 1.0f;
                    renderer.context->RSSetViewports(1, &vp);

                    XMMATRIX viewMatrix, projMatrix;
                    if (useAppProjection) {
                        int vi = monoMode ? 0 : eye;
                        viewMatrix = ColumnMajorToXMMatrix(rigViews[vi].view_matrix);
                        projMatrix = ColumnMajorToXMMatrix(rigViews[vi].projection_matrix);
                    } else if (monoMode) {
                        viewMatrix = monoViewMatrix;
                        projMatrix = monoProjMatrix;
                    } else {
                        int vi = (eye < (int)viewCount) ? eye : 0;
                        viewMatrix = xr.viewMatrices[vi];
                        projMatrix = xr.projMatrices[vi];
                    }

                    RenderScene(renderer, rtv, rs.depthDSV.Get(),
                        renderW, renderH,
                        viewMatrix, projMatrix,
                        useAppProjection ? 1.0f : g_inputState.viewParams.scaleFactor,
                        0.03f);

                    projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                    projectionViews[eye].subImage.swapchain = xr.swapchain.swapchain;
                    projectionViews[eye].subImage.imageRect.offset = {
                        (int32_t)(tileX * renderW), (int32_t)(tileY * renderH)
                    };
                    projectionViews[eye].subImage.imageRect.extent = {
                        (int32_t)renderW,
                        (int32_t)renderH
                    };
                    projectionViews[eye].subImage.imageArrayIndex = 0;

                    int safeIdx = (eye < (int)viewCount) ? eye : 0;
                    projectionViews[eye].pose = monoMode ? monoPose : rawViews[safeIdx].pose;
                    projectionViews[eye].fov = useAppProjection ?
                        rigViews[monoMode ? 0 : eye].fov :
                        (monoMode ? monoFov : rawViews[safeIdx].fov);
                }
                viewsPopulated = true;

                if (rtv) rtv->Release();

                // 'I' key: snapshot the multiview atlas to a PNG via the
                // runtime (XR_DXR_atlas_capture). Skipped for mono (1×1).
                if (g_inputState.captureAtlasRequested) {
                    g_inputState.captureAtlasRequested = false;
                    dxr_capture::RequestRuntimeAtlasCapture(
                        xr, APP_NAME, tileColumns, tileRows, rs.hwnd);
                }

                ReleaseSwapchainImage(xr);
            }
        }
    }

    // Submit frame (fallback path: plain projection, optionally with the HUD).
    if (!viewsPopulated) {
        XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime = frameState.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount = 0;
        endInfo.layers = nullptr;
        xrEndFrame(xr.session, &endInfo);
        return;
    }
    if (hudSubmitted) {
        float hudAR = (float)HUD_PIXEL_WIDTH / (float)HUD_PIXEL_HEIGHT;
        float windowAR = (g_windowWidth > 0 && g_windowHeight > 0) ? (float)g_windowWidth / (float)g_windowHeight : 1.0f;
        float fracW = HUD_WIDTH_FRACTION;
        float fracH = fracW * windowAR / hudAR;
        if (fracH > 1.0f) { fracH = 1.0f; fracW = hudAR / windowAR; }
        EndFrameWithWindowSpaceHud(xr, frameState.predictedDisplayTime, projectionViews.data(),
            0.0f, 0.0f, fracW, fracH, 0.0f, eyeCount);
    } else {
        EndFrame(xr, frameState.predictedDisplayTime, projectionViews.data(), eyeCount);
    }
}

// Destroy the zone/strip/wish resources (before session teardown).
static void CleanupZones() {
    for (uint32_t zi = 0; zi < kNumZones; zi++) {
        DisplayZone& z = g_zonesArr[zi];
        if (z.swapchain != XR_NULL_HANDLE) {
            xrDestroySwapchain(z.swapchain);
            z.swapchain = XR_NULL_HANDLE;
        }
        z.images.clear();
        z.depthDSV.Reset();
        z.depthTex.Reset();
    }
    if (g_strip.swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(g_strip.swapchain);
        g_strip.swapchain = XR_NULL_HANDLE;
    }
    if (g_zone.mask != XR_NULL_HANDLE && g_zone.pfnDestroy) {
        g_zone.pfnDestroy(g_zone.mask);
        g_zone.mask = XR_NULL_HANDLE;
    }
    g_wishRTV = nullptr; // runtime-owned
    g_zonesActive = false;
}

// Main entry point
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    // Initialize logging
    if (!InitializeLogging(APP_NAME)) {
        MessageBox(nullptr, L"Failed to initialize logging", L"Warning", MB_OK | MB_ICONWARNING);
    }

    LOG_INFO("=== Cube Zones (XR_DXR_display_zones) ===");
    LOG_INFO("Two 3D zones + Local2D strip + wish mask (ADR-027)");
    LOG_INFO("Runtime dev gate: DISPLAYXR_ZONES=1 must be set for the runtime to advertise the extension");

    // Create window FIRST (needed for XR_DXR_win32_window_binding)
    HWND hwnd = CreateAppWindow(hInstance, g_windowWidth, g_windowHeight);
    if (!hwnd) {
        LOG_ERROR("Failed to create window");
        ShutdownLogging();
        return 1;
    }

    // Initialize OpenXR
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

    // Check for session target extension
    if (!xr.hasWin32WindowBindingExt) {
        LOG_WARN("XR_DXR_win32_window_binding not available - runtime will create its own window");
        MessageBox(hwnd, L"XR_DXR_win32_window_binding extension not available.\nRuntime will create its own window.",
            L"Warning", MB_OK | MB_ICONWARNING);
    } else {
        LOG_INFO("XR_DXR_win32_window_binding extension is available - using app window");
    }

    // Get the required GPU adapter LUID from OpenXR
    LUID adapterLuid;
    if (!GetD3D11GraphicsRequirements(xr, &adapterLuid)) {
        LOG_ERROR("Failed to get D3D11 graphics requirements");
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Initialize D3D11 on the correct adapter
    LOG_INFO("Initializing D3D11...");
    D3D11Renderer renderer = {};
    if (!InitializeD3D11WithLUID(renderer, adapterLuid)) {
        LOG_ERROR("D3D11 initialization failed");
        MessageBox(hwnd, L"Failed to initialize D3D11", L"Error", MB_OK | MB_ICONERROR);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Initialize HUD renderer (standalone D3D11 device for text rendering)
    HudRenderer hudRenderer = {};
    bool hudOk = InitializeHudRenderer(hudRenderer, HUD_PIXEL_WIDTH, HUD_PIXEL_HEIGHT);
    if (!hudOk) {
        LOG_WARN("HUD renderer init failed - HUD will not be displayed");
    }

    // Create OpenXR session WITH window handle (XR_DXR_win32_window_binding)
    LOG_INFO("Creating OpenXR session with XR_DXR_win32_window_binding (HWND: 0x%p)...", hwnd);
    if (!CreateSession(xr, renderer.device.Get(), hwnd)) {
        LOG_ERROR("OpenXR session creation failed");
        MessageBox(hwnd, L"Failed to create OpenXR session", L"Error", MB_OK | MB_ICONERROR);
        if (hudOk) CleanupHudRenderer(hudRenderer);
        CleanupD3D11(renderer);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create reference spaces
    if (!CreateSpaces(xr)) {
        LOG_ERROR("Reference space creation failed");
        CleanupOpenXR(xr);
        if (hudOk) CleanupHudRenderer(hudRenderer);
        CleanupD3D11(renderer);
        ShutdownLogging();
        return 1;
    }

    // Create the single fallback swapchain at native display resolution. The
    // zones path creates its own per-zone swapchains at activation; this one
    // serves the pre-activation frames and the no-extension fallback.
    if (!CreateSwapchain(xr)) {
        LOG_ERROR("Swapchain creation failed");
        CleanupOpenXR(xr);
        if (hudOk) CleanupHudRenderer(hudRenderer);
        CleanupD3D11(renderer);
        ShutdownLogging();
        return 1;
    }

    // Enumerate D3D11 swapchain images
    std::vector<XrSwapchainImageD3D11KHR> swapchainImages;
    {
        uint32_t count = xr.swapchain.imageCount;
        swapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)swapchainImages.data());
        LOG_INFO("Enumerated %u D3D11 swapchain images", count);
    }

    // Create HUD swapchain for window-space layer submission (fallback path
    // only — in zones mode the layer list stays exactly [zoneA, zoneB, strip]).
    if (!CreateHudSwapchain(xr, HUD_PIXEL_WIDTH, HUD_PIXEL_HEIGHT)) {
        LOG_WARN("Failed to create HUD swapchain - HUD will not be displayed");
    }

    // Enumerate HUD swapchain images
    std::vector<XrSwapchainImageD3D11KHR> hudSwapchainImages;
    if (xr.hasHudSwapchain) {
        uint32_t count = xr.hudSwapchain.imageCount;
        hudSwapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        xrEnumerateSwapchainImages(xr.hudSwapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)hudSwapchainImages.data());
        LOG_INFO("HUD: enumerated %u D3D11 swapchain images", count);
    }

    // Show window
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // Create single depth buffer at full swapchain dimensions (fallback path)
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
    LOG_INFO("Zones: M=wish mode (AUTO/Tier-2/Tier-3), O=zone B overlap toggle");
    LOG_INFO("Controls: WASD=Fly (fallback), V=Mode, T=Eye tracking, F11=Fullscreen, ESC=Quit");
    LOG_INFO("");

    PerformanceStats perfStats = {};
    perfStats.lastTime = std::chrono::high_resolution_clock::now();

    // Set virtual display height (app units) for the fallback rig path.
    g_inputState.viewParams.virtualDisplayHeight = 0.24f;
    g_inputState.initialVirtualDisplayHeight = g_inputState.viewParams.virtualDisplayHeight; // SPACE-reset target
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
    g_renderState = &rs;

    // Single-threaded main loop: pump messages, then render one frame.
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

    // Cleanup
    LOG_INFO("");
    LOG_INFO("=== Shutting down ===");

    CleanupZones();

    depthDSV.Reset();
    depthTexture.Reset();

    g_xr = nullptr;
    CleanupOpenXR(xr);
    if (hudOk) CleanupHudRenderer(hudRenderer);
    CleanupD3D11(renderer);

    DestroyWindow(hwnd);
    UnregisterClass(WINDOW_CLASS, hInstance);

    LOG_INFO("Application shutdown complete");
    ShutdownLogging();

    return 0;
}
