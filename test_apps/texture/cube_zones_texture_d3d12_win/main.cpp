// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Cube Zones TEXTURE D3D12 — XR_DXR_display_zones parity test
 *         (ADR-027), native-D3D12 Windows leg.
 *
 * PARITY TEST. Native-D3D12 port of cube_zones_texture_d3d11_win. It proves
 * that a texture app rendering through the native D3D12 compositor — one that
 * provides a shared D3D12 texture and presents that texture itself — receives
 * the FULL XR_DXR_display_zones multi-zone composite written back into its
 * shared texture, byte-equivalent to what the D3D11 zones texture app gets.
 * The display-zones submission logic (the thing under test) is unchanged from
 * the D3D11 zones app; only the graphics primitives are native D3D12.
 *
 * Native-D3D12 deltas from the D3D11 zones app (everything else is identical):
 *  - Creates a shared D3D12 texture (D3D12_HEAP_FLAG_SHARED + CreateSharedHandle)
 *    sized to the worst-case atlas and passes its NT HANDLE + the app HWND via
 *    XR_DXR_win32_window_binding on xrCreateSession (device + command queue
 *    binding, XrGraphicsBindingD3D12KHR).
 *  - Per-zone swapchains use XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR; their images get
 *    RTVs in ONE combined descriptor heap (main + zoneA + zoneB), indexed by a
 *    per-zone rtvBaseIndex (mirrors the D3D12 texture app's combined RTV heap).
 *  - Per-zone clear / cube / feather each record their own command list and
 *    submit+wait on the renderer's queue (RenderScene's self-contained model);
 *    the per-zone clear color (incl. zone B's premultiplied {0,0,0,0}) is set by
 *    a dedicated clear pass since RenderScene's clear color is fixed.
 *  - Tier-3 wish authors an R8 mask via xrAcquireLocal3DZoneRenderTargetDXR +
 *    XrLocal3DZoneRenderTargetD3D12DXR: a CPU feathered-rect mask (rings at the
 *    zone rects) uploaded onto the runtime's resource via CopyTextureRegion
 *    (the freeform path lifted from cube_texture_d3d12_win's case 4).
 *  - DComp transparent present: app-owned D3D12 DXGI composition swapchain in a
 *    DirectComposition visual over the desktop (WS_EX_NOREDIRECTIONBITMAP +
 *    null brush + premultiplied alpha) — real see-through (ADR-029).
 *  - Autonomous verification: DXR_TEXDUMP reads the shared texture back to a PNG
 *    via a readback buffer at a warmup gate so the captured texture reflects
 *    what the texture app actually received.
 *
 * The surround / output-rect path (xrSetSharedTextureOutputRectDXR /
 * xrSetSharedTextureSurround2DFenceEXT) is REMOVED — zones supersede it; the
 * full window IS the composite, and the runtime writes the multi-zone
 * super-atlas directly into the shared texture.
 *
 * Exercises the display-zones runtime path (N 3D zones + Local2D zones +
 * per-frame wish mask) end to end:
 *
 *  - Zone A (zoneId=1, left)  : rect {0,180,640,540}, identity display rig,
 *    spin phase 0, OPAQUE dark-red clear {0.15,0.03,0.03,1.0}.
 *  - Zone B (zoneId=2, right) : rect {700,180,520,360}, display rig with
 *    ipdFactor 0.6 + perspectiveFactor 0.5 (visibly different framing),
 *    spin phase +1.5 rad, FULLY TRANSPARENT premultiplied clear {0,0,0,0} so
 *    the cube floats over the LIVE desktop.
 *  - Local2D strip (top)      : rect {0,0,1280,180}, always on, filled once
 *    with a CPU-generated checker + label band.
 *
 * Keys (zones mode):
 *  - M : cycle wish mode — 0 AUTO / 1 explicit Tier-2 rects / 2 explicit
 *        Tier-3 feathered render-target mask.
 *  - O : toggle zone B between its home rect and a rect overlapping zone A.
 *  - DXR_ZONES_VALIDATE=1 : chain XR_DISPLAY_ZONES_FRAME_END_VALIDATE_BIT_DXR
 *        on the frame-end info in every mode.
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
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <dcomp.h>       // #68 (A) — transparent present: DComp visual over the desktop
#pragma comment(lib, "dcomp.lib")

#include "logging.h"
#include "input_handler.h"
#include "d3d12_renderer.h"
#include "text_overlay.h"
#include "hud_renderer.h"
#include "xr_session.h"
#include "projection_depth.h"
#include "atlas_capture.h"

// stb_image_write is provided by displayxr::common's Windows impl TU. The
// shared stb header isn't pulled in here, so forward-declare the single entry
// point the shared-texture readback dump needs. STBIWDEF resolves to
// `extern "C"` in C++ — match it.
extern "C" int stbi_write_png(const char* filename, int w, int h, int comp,
                              const void* data, int stride_in_bytes);

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <sstream>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

// Application name for logging
static const char* APP_NAME = "cube_zones_texture_d3d12_win";

// Window settings
static const wchar_t* WINDOW_CLASS = L"DXRCubeZonesTextureD3D12Class";
static const wchar_t* WINDOW_TITLE = L"D3D12 Cube Zones TEXTURE — XR_DXR_display_zones parity test";
static const wchar_t* PANE_CLASS = L"DXRCubeZonesTextureD3D12PaneClass";

// #740 phase-transfer harness (DXR_PANE_OFFSET=D). Reshapes the app into the
// Unity-editor topology: a CONTAINER top-level that owns the app's DXGI
// swapchain (the thing the SR SDK's device-level window association actually
// resolves), with the bound weave surface as a WS_CHILD pane at in-container
// offset D. The DP's #740 correction then computes phase_off_x == D.
//
// The pane is pinned to a FIXED SCREEN position — the container slides left as
// D grows, and the container's non-client border cancels identically in both
// arms, so the pane's client origin lands on the same panel pixels for every D
// (and for the D-absent control). The correct weave phase is therefore
// invariant in D *by construction*: any measured phase change vs D is pure
// correction residual, with no modelling and no eye-tracking confound (measure
// with no face in view — the SDK's nominal eye fallback is bit-exact stable).
static int32_t g_paneOffsetX = -1;      // -1 = disabled -> plain top-level (control arm)
static int32_t g_paneScreenX = 900;     // pane client X relative to the panel's left edge
static HWND    g_paneHwnd = nullptr;    // bound weave surface when the harness is on

// Global state (single-threaded — all accessed from the main thread only)
static InputState g_inputState;
static bool g_running = true;
static XrSessionManager* g_xr = nullptr;
static UINT g_windowWidth = 1280;
static UINT g_windowHeight = 720;
static bool g_inSizeMove = false;  // True while user is dragging/resizing the window
static bool g_resizeNeeded = false; // Window size changed — resize app swapchain
static const uint32_t HUD_PIXEL_WIDTH = 380;
static const uint32_t HUD_PIXEL_HEIGHT = 470;
static const float CAMERA_HALF_TAN_VFOV = 0.32491969623f; // tan(18°) → 36° vFOV
static const float HUD_WIDTH_FRACTION = 0.30f;

// Fullscreen state
static bool g_fullscreen = false;
static RECT g_savedWindowRect = {};
static DWORD g_savedWindowStyle = 0;

// ---------------------------------------------------------------------------
// Texture-mode shared D3D12 texture (lifted from cube_texture_d3d12_win)
// ---------------------------------------------------------------------------
//
// The app provides a shared D3D12 texture as the runtime's composite target
// (passed as XrWin32WindowBindingCreateInfoDXR.sharedTextureHandle). For the
// zones path the runtime composites the full-window multi-zone super-atlas
// DIRECTLY into this texture (the declared output rect is superseded by zones
// → full window), so the app just blits the whole window region back to its
// own swapchain. NO output rect / 2D surround needed.
static ComPtr<ID3D12Resource> g_sharedTexture;
static HANDLE g_sharedHandle = nullptr;
static uint32_t g_sharedWidth = 0;   // Shared texture size (= display pixels, worst case)
static uint32_t g_sharedHeight = 0;

// App-side DXGI swapchain for window presentation.
static const UINT APP_BACK_BUFFER_COUNT = 2;
static ComPtr<IDXGISwapChain3> g_appSwapchain;
static ComPtr<ID3D12Resource> g_appBackBuffers[APP_BACK_BUFFER_COUNT];
static ComPtr<ID3D12DescriptorHeap> g_appRtvHeap;
static UINT g_appRtvDescriptorSize = 0;

// Blit pipeline resources (fullscreen-quad present of the shared texture).
static ComPtr<ID3D12RootSignature> g_blitRootSig;
static ComPtr<ID3D12PipelineState> g_blitPSO;
static ComPtr<ID3D12DescriptorHeap> g_blitSrvHeap;

// Blit command resources (separate from scene rendering).
static ComPtr<ID3D12CommandAllocator> g_blitCmdAllocator;
static ComPtr<ID3D12GraphicsCommandList> g_blitCmdList;
static ComPtr<ID3D12Fence> g_blitFence;
static UINT64 g_blitFenceValue = 0;
static HANDLE g_blitFenceEvent = nullptr;

// #68 (A) — DirectComposition objects for the transparent present. Must outlive
// the window: a composition swapchain alone is invisible without a visual tree.
static ComPtr<IDCompositionDevice> g_dcompDevice;
static ComPtr<IDCompositionTarget> g_dcompTarget;
static ComPtr<IDCompositionVisual> g_dcompVisual;
static bool g_transparentPresent = true;

// Autonomous-verification dump (DXR_TEXDUMP). "1" or empty-but-present →
// default %TEMP%\zones_texture_readback.png; any other value is the literal
// path; unset → no dump. The readback runs after the present so it captures
// exactly what the texture app received in its shared texture.
static std::string g_texDumpPath;
static bool g_texDumpEnabled = false;
static bool g_texDumpDone = false;          // one-shot at the warmup frame gate
static long g_texFrameCounter = 0;
static const long kTexDumpFrame = 150;      // matches the macOS sibling's gate

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
    bool sliced = false;            //!< #727: array-slice route vs horizontal-tile route
    std::vector<XrSwapchainImageD3D12KHR> images;
    int rtvBaseIndex = 0;           //!< base index of this zone's RTVs in renderer.rtvHeap
};
static DisplayZone g_zonesArr[kNumZones];
// Zones actually activated this run = min(kNumZones, caps.maxZones3D). Some DPs
// (e.g. Leia SR 1.0.6, grid 1x1) support only ONE 3D zone; rather than falling
// back to the non-zones path, run with the zones we can — a single full-window-ish
// zone matches issue #727's "one full-window display zone supplies the canvas".
static uint32_t g_activeZones = kNumZones;

// #727 experiment: choose how each zone submits its stereo pair to the runtime.
//   TILED  (default): one swapchain (arraySize=1) tileW*viewCount wide; view vi
//                      lands at imageRect.offset.x = vi*tileW, imageArrayIndex=0.
//   SLICED (DXR_ZONE_ROUTE=sliced): one array swapchain (arraySize=viewCount);
//                      view vi lands in array slice vi (imageArrayIndex=vi,
//                      imageRect.offset={0,0}) — the #727 path (Unity SPI-style).
// Both converge to an identical flat side-by-side atlas inside the runtime (the
// projection blit de-arrays slices into tiles, #656), so this toggle A/B-tests
// whether the DP weaves the two identically. Env read once.
static bool ZoneSlicedRoute() {
    static int cached = -1;
    if (cached < 0) {
        const char* v = getenv("DXR_ZONE_ROUTE");
        cached = (v != nullptr && (strcmp(v, "sliced") == 0 || strcmp(v, "SLICED") == 0 ||
                                   strcmp(v, "1") == 0)) ? 1 : 0;
    }
    return cached != 0;
}

// #727 decisive probe: DXR_SLICE_COLORS=1 paints each view a solid color
// (view 0 = BLUE, view 1 = RED) instead of the cube, so the woven readback is
// unambiguous — a correct stereo weave interlaces blue/red (with a tracked face
// in front of the panel; the Leia DP returns left-view-only = flat blue when no
// valid face is present), while a mono collapse is flat blue regardless. The
// pre-DP atlas (DP input) should always be a clean [blue|red] side-by-side.
static bool ZoneSliceColors() {
    static int cached = -1;
    if (cached < 0) {
        const char* v = getenv("DXR_SLICE_COLORS");
        cached = (v != nullptr && v[0] != '\0' && v[0] != '0') ? 1 : 0;
    }
    return cached != 0;
}

// #727 Unity-mirror config: DXR_ZONES_FULLWIN=1 runs a SINGLE zone covering the
// whole client window (no strip, no zone B) — the shape the Unity plugin submits
// (one full-window zone supplies the canvas). Combine with DXR_ZONE_ROUTE /
// DXR_SLICE_COLORS to A/B exactly that configuration.
static bool ZonesFullWindow() {
    static int cached = -1;
    if (cached < 0) {
        const char* v = getenv("DXR_ZONES_FULLWIN");
        cached = (v != nullptr && v[0] != '\0' && v[0] != '0') ? 1 : 0;
    }
    return cached != 0;
}

static const XrRect2Di kZoneARect        = {{0, 180}, {640, 540}};
static const XrRect2Di kZoneBRect        = {{700, 180}, {520, 360}};
static const XrRect2Di kZoneBOverlapRect = {{400, 300}, {520, 360}};
static const XrRect2Di kStripRect        = {{0, 0}, {1280, 180}};
static bool g_zoneBOverlap = false;

// Local2D strip (always on in zones mode; filled once via CPU upload).
struct StripLayer {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    uint32_t w = 0, h = 0;
    int64_t format = 0;
};
static StripLayer g_strip;

// Zones activation: created a few frames in, once the session runs.
// #727: DXR_ZONES_ACTIVATE_AT=N overrides the default frame-10 gate — N=1
// mirrors the Unity plugin, which publishes its zone from the very first
// frame (its mono flip lands exactly at the first zone-mask publish, so
// activation timing relative to session/weaver init is a live suspect).
static bool g_zonesActive = false;
static bool g_zonesAttempted = false;
static long g_zonesFrameCounter = 0;
static long ZonesActivationFrame() {
    static long cached = -1;
    if (cached < 0) {
        const char* v = getenv("DXR_ZONES_ACTIVATE_AT");
        long n = (v != nullptr) ? atol(v) : 0;
        cached = (n >= 1) ? n : 10;
    }
    return cached;
}

// Wish modes (M key): 0 AUTO, 1 explicit Tier-2 rects, 2 explicit Tier-3
// feathered render-target mask.
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

// XR_DXR_view_rig: per-view staging container for the consumed render-ready
// views (matrices column-major).
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
// XrView fov + the app's own clip policy. Pair with
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
        // During drag/resize, DefWindowProc runs a modal loop that blocks our
        // main message pump. By leaving the window invalidated, Windows keeps
        // sending WM_PAINT inside that modal loop, giving us a chance to keep
        // rendering frames.
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
// alpha-composite against the DESKTOP by design, so the window uses
// WS_EX_NOREDIRECTIONBITMAP + a null brush so DComp can show the desktop
// through. Set DISPLAYXR_TRANSPARENT_BG=0 to opt out (opaque black floor).
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
    // Null brush in transparent mode so the redirection bitmap doesn't paint
    // an opaque floor under the DComp visual.
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

// #740 harness: the WS_CHILD pane inside the container. Inert by design — it is
// a weave surface and a position anchor, never a render target (the DP weaves
// into the shared texture; the container presents). DefWindowProc is all it
// needs, so it deliberately does NOT share WINDOW_CLASS/WindowProc.
//
// DXR_PANE_EXSTYLE=layered reproduces the Unity plugin's overlay ex-styles
// (WS_EX_NOACTIVATE|WS_EX_LAYERED|WS_EX_TRANSPARENT, alpha 0) on the pane. The
// editor's overlay is click-through and invisible; a plain WS_CHILD is neither.
// If the SR SDK resolves its weaving window by any hit-test-like walk
// (WindowFromPoint / ChildWindowFromPointEx honour LAYERED+TRANSPARENT by
// skipping the window), a click-through child is invisible to that walk and the
// resolver lands on the container instead — which would put the container's
// origin into the phase while the DP still reports the child's rect correctly.
// That is the only remaining delta between this harness (which weaves
// correctly) and the editor (which does not). See #740.
static HWND CreatePaneWindow(HINSTANCE hInstance, HWND container, int x, int y, int w, int h) {
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = DefWindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = PANE_CLASS;
    if (!RegisterClassEx(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            LOG_ERROR("Failed to register pane class, error: %lu", err);
            return nullptr;
        }
    }

    DWORD ex = 0;
    bool layered = false;
    if (const char* es = getenv("DXR_PANE_EXSTYLE")) {
        if (_stricmp(es, "layered") == 0) {
            ex = WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT;
            layered = true;
        }
    }

    HWND pane = CreateWindowEx(ex, PANE_CLASS, L"", WS_CHILD | WS_VISIBLE,
                               x, y, w, h, container, nullptr, hInstance, nullptr);
    if (!pane) {
        LOG_ERROR("Failed to create pane window, error: %lu", GetLastError());
        return nullptr;
    }
    if (layered) {
        // alpha 0 — the editor's overlay is fully transparent; it is a position
        // anchor, not something the user ever sees.
        if (!SetLayeredWindowAttributes(pane, 0, 0, LWA_ALPHA)) {
            LOG_ERROR("SetLayeredWindowAttributes failed: %lu", GetLastError());
        }
    }
    LOG_WARN("#740 harness: pane 0x%p is WS_CHILD of container 0x%p at in-container (%d,%d) %dx%d "
             "exStyle=0x%08lX%s",
             pane, container, x, y, w, h, ex,
             layered ? " (LAYERED|TRANSPARENT|NOACTIVATE, alpha 0 — Unity overlay parity)" : "");
    return pane;
}

// ---------------------------------------------------------------------------
// Texture-mode blit pipeline (lifted from cube_texture_d3d12_win)
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

static bool CreateBlitPipeline(ID3D12Device* device) {
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
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &srvRange;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
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

// ---- App swapchain management ----

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

    // Wait for GPU idle before releasing back buffers.
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

// Blit the runtime-composited shared texture into the app's window back
// buffer. For the zones path the runtime fills the FULL WINDOW region of the
// shared texture with the multi-zone composite (output rect superseded by
// zones), so we present the whole window region (top-left anchored, 1:1,
// clamped to the worst-case shared texture).
static void BlitSharedTextureToBackBuffer(D3D12Renderer& renderer) {
    if (!g_sharedTexture || !g_appSwapchain) return;

    UINT bbIndex = g_appSwapchain->GetCurrentBackBufferIndex();
    ID3D12Resource* backBuffer = g_appBackBuffers[bbIndex].Get();

    g_blitCmdAllocator->Reset();
    g_blitCmdList->Reset(g_blitCmdAllocator.Get(), g_blitPSO.Get());

    float vpW = (g_windowWidth < g_sharedWidth) ? (float)g_windowWidth : (float)g_sharedWidth;
    float vpH = (g_windowHeight < g_sharedHeight) ? (float)g_windowHeight : (float)g_sharedHeight;

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
    float clearColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    g_blitCmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    g_blitCmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    // Present the full window region of the shared texture (top-left anchored).
    D3D12_VIEWPORT vp = {};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = vpW;
    vp.Height = vpH;
    vp.MaxDepth = 1.0f;
    g_blitCmdList->RSSetViewports(1, &vp);

    D3D12_RECT scissor = {0, 0, (LONG)g_windowWidth, (LONG)g_windowHeight};
    g_blitCmdList->RSSetScissorRects(1, &scissor);

    float uvParams[4] = {
        g_sharedWidth  > 0 ? vpW / (float)g_sharedWidth  : 1.0f,
        g_sharedHeight > 0 ? vpH / (float)g_sharedHeight : 1.0f,
        0.0f, 0.0f
    };

    g_blitCmdList->SetPipelineState(g_blitPSO.Get());
    g_blitCmdList->SetGraphicsRootSignature(g_blitRootSig.Get());
    ID3D12DescriptorHeap* heaps[] = {g_blitSrvHeap.Get()};
    g_blitCmdList->SetDescriptorHeaps(1, heaps);
    g_blitCmdList->SetGraphicsRootDescriptorTable(0, g_blitSrvHeap->GetGPUDescriptorHandleForHeapStart());
    g_blitCmdList->SetGraphicsRoot32BitConstants(1, 4, uvParams, 0);

    g_blitCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_blitCmdList->DrawInstanced(3, 1, 0, 0);

    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_blitCmdList->ResourceBarrier(2, barriers);

    g_blitCmdList->Close();

    ID3D12CommandList* lists[] = {g_blitCmdList.Get()};
    renderer.commandQueue->ExecuteCommandLists(1, lists);

    g_appSwapchain->Present(1, 0);

    // Fence sync.
    g_blitFenceValue++;
    renderer.commandQueue->Signal(g_blitFence.Get(), g_blitFenceValue);
    if (g_blitFence->GetCompletedValue() < g_blitFenceValue) {
        g_blitFence->SetEventOnCompletion(g_blitFenceValue, g_blitFenceEvent);
        WaitForSingleObject(g_blitFenceEvent, INFINITE);
    }
}

// Autonomous verification: read the shared texture back to a PNG. Captures
// exactly what the TEXTURE APP received in its shared texture (the full
// multi-zone composite the runtime wrote back). D3D12 has no Map on a DEFAULT
// texture — copy into a READBACK buffer (256-byte row aligned), Map, swizzle
// BGRA->RGBA, stbi_write_png.
static void DumpSharedTextureToPNG(D3D12Renderer& renderer, const char* path) {
    if (!g_sharedTexture || path == nullptr || path[0] == '\0') return;
    if (g_sharedWidth == 0 || g_sharedHeight == 0) return;

    const uint32_t w = g_sharedWidth;
    const uint32_t h = g_sharedHeight;
    const uint32_t rowPitch = (w * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
        & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);

    // Readback buffer (CPU-readable).
    ComPtr<ID3D12Resource> readback;
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = (UINT64)rowPitch * h;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HRESULT hr = renderer.device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&readback));
    if (FAILED(hr)) {
        LOG_WARN("TEXTURE READBACK: readback buffer create failed (0x%08x) — skipped", hr);
        return;
    }

    g_blitCmdAllocator->Reset();
    g_blitCmdList->Reset(g_blitCmdAllocator.Get(), nullptr);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = g_sharedTexture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_blitCmdList->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = g_sharedTexture.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = readback.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstLoc.PlacedFootprint.Offset = 0;
    dstLoc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    dstLoc.PlacedFootprint.Footprint.Width = w;
    dstLoc.PlacedFootprint.Footprint.Height = h;
    dstLoc.PlacedFootprint.Footprint.Depth = 1;
    dstLoc.PlacedFootprint.Footprint.RowPitch = rowPitch;
    g_blitCmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    g_blitCmdList->ResourceBarrier(1, &barrier);

    g_blitCmdList->Close();
    ID3D12CommandList* lists[] = {g_blitCmdList.Get()};
    renderer.commandQueue->ExecuteCommandLists(1, lists);

    g_blitFenceValue++;
    renderer.commandQueue->Signal(g_blitFence.Get(), g_blitFenceValue);
    if (g_blitFence->GetCompletedValue() < g_blitFenceValue) {
        g_blitFence->SetEventOnCompletion(g_blitFenceValue, g_blitFenceEvent);
        WaitForSingleObject(g_blitFenceEvent, INFINITE);
    }

    void* mapped = nullptr;
    D3D12_RANGE readRange = {0, (SIZE_T)rowPitch * h};
    if (FAILED(readback->Map(0, &readRange, &mapped)) || mapped == nullptr) {
        LOG_WARN("TEXTURE READBACK: Map failed — skipped");
        return;
    }

    // Shared texture is B8G8R8A8_UNORM; stbi_write_png wants RGBA — swizzle
    // B<->R on copy into a tightly packed RGBA buffer.
    const uint8_t* base = (const uint8_t*)mapped;
    std::vector<uint8_t> rgba((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t* srow = base + (size_t)y * rowPitch;
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
    D3D12_RANGE noWrite = {0, 0};
    readback->Unmap(0, &noWrite);

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

// State passed to RenderOneFrame (and accessible from WM_PAINT via g_renderState)
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

// Texture-mode present tail: blit the runtime-composited shared texture into
// the app's own window swapchain, present, then run autonomous verification.
static void PresentAndMaybeDump(RenderState& rs) {
    D3D12Renderer& renderer = *rs.renderer;

    if (g_resizeNeeded) {
        g_resizeNeeded = false;
        ResizeAppSwapchain(renderer);
    }

    BlitSharedTextureToBackBuffer(renderer);

    // One-shot DXR_TEXDUMP at the warmup gate (HUD/zones are active by then).
    if (g_texDumpEnabled && !g_texDumpDone && g_texFrameCounter >= kTexDumpFrame) {
        g_texDumpDone = true;
        DumpSharedTextureToPNG(renderer, g_texDumpPath.c_str());
    }

    // #727: DXR_TEXDUMP_FRAMES=N or A-B — additionally dump the woven texture
    // per-frame (…_fNNN.png): N dumps the first N frames; "A-B" dumps frames
    // A..B inclusive (to bracket a good→mono transition that lands later than
    // the warmup). Requires DXR_TEXDUMP for the path.
    if (g_texDumpEnabled) {
        static long dumpFrom = -1, dumpTo = -1;
        if (dumpFrom < 0) {
            dumpFrom = 0;
            dumpTo = 0;
            const char* v = getenv("DXR_TEXDUMP_FRAMES");
            if (v != nullptr) {
                const char* dash = strchr(v, '-');
                if (dash != nullptr) {
                    dumpFrom = atol(v);
                    dumpTo = atol(dash + 1);
                } else {
                    dumpFrom = 1;
                    dumpTo = atol(v);
                }
            }
        }
        if (g_texFrameCounter >= dumpFrom && g_texFrameCounter <= dumpTo) {
            char path[MAX_PATH];
            snprintf(path, sizeof(path), "%s_f%03ld.png",
                     g_texDumpPath.c_str(), g_texFrameCounter - 1);
            DumpSharedTextureToPNG(renderer, path);
        }
    }
}

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

// Upload `pitch`-aligned BGRA bytes into a D3D12 swapchain image (COMMON ->
// COPY_DEST -> COMMON) via a transient UPLOAD buffer + CopyTextureRegion.
static void UploadBytesToImage(D3D12Renderer& renderer, ID3D12Resource* dst,
                               DXGI_FORMAT fmt, uint32_t w, uint32_t h,
                               const uint8_t* src, uint32_t srcRowBytes) {
    const uint32_t pitch = (srcRowBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
        & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);

    ComPtr<ID3D12Resource> upload;
    D3D12_HEAP_PROPERTIES up = {};
    up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = (UINT64)pitch * h;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(renderer.device->CreateCommittedResource(
            &up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&upload)))) {
        LOG_WARN("[zones] strip/upload buffer create failed");
        return;
    }
    uint8_t* mapped = nullptr;
    if (FAILED(upload->Map(0, nullptr, (void**)&mapped)) || mapped == nullptr) {
        LOG_WARN("[zones] strip/upload buffer map failed");
        return;
    }
    for (uint32_t y = 0; y < h; y++) {
        memcpy(mapped + (size_t)y * pitch, src + (size_t)y * srcRowBytes, srcRowBytes);
    }
    upload->Unmap(0, nullptr);

    renderer.commandAllocator->Reset();
    renderer.commandList->Reset(renderer.commandAllocator.Get(), nullptr);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = dst;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    renderer.commandList->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = dst;
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = upload.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint.Offset = 0;
    srcLoc.PlacedFootprint.Footprint.Format = fmt;
    srcLoc.PlacedFootprint.Footprint.Width = w;
    srcLoc.PlacedFootprint.Footprint.Height = h;
    srcLoc.PlacedFootprint.Footprint.Depth = 1;
    srcLoc.PlacedFootprint.Footprint.RowPitch = pitch;
    renderer.commandList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    renderer.commandList->ResourceBarrier(1, &barrier);

    renderer.commandList->Close();
    ID3D12CommandList* lists[] = {renderer.commandList.Get()};
    renderer.commandQueue->ExecuteCommandLists(1, lists);
    WaitForGpu(renderer); // keeps `upload` alive until the copy lands
}

// Create the always-on Local2D strip swapchain and fill it once (static
// content: acquire/fill/release once; the layer references the released image
// every frame). Checker + a solid label band; OPAQUE alpha throughout.
static bool CreateAndFillStrip(XrSessionManager& xr, D3D12Renderer& renderer) {
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
    g_strip.format = sci.format;

    uint32_t n = 0;
    xrEnumerateSwapchainImages(g_strip.swapchain, 0, &n, nullptr);
    std::vector<XrSwapchainImageD3D12KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
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
    // The CPU bytes are BGRA8; the copy footprint format must match the
    // swapchain image's typed format (BGRA UNORM or its _SRGB sibling — same
    // byte layout, so the same bytes are correct either way).
    UploadBytesToImage(renderer, imgs[idx].texture,
                       (DXGI_FORMAT)g_strip.format, w, h,
                       buf.data(), (uint32_t)stride);

    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(g_strip.swapchain, &ri);
    return true;
}

// ---- RTV heap management ---------------------------------------------------
//
// The reused d3d12_renderer.cpp's CreateSwapchainRTVs RE-CREATES the RTV heap
// on every call (resetting renderer.rtvCount to 0). That's fine for the D3D12
// texture app which builds one combined heap in a single call, but the zones
// app creates the zone swapchains LAZILY at activation (frame 10), after the
// main-swapchain RTVs already exist. So we own a single big heap here and only
// APPEND into it — CreateSwapchainRTVs is called exactly once at startup, to
// trigger its PSO recreation + depth-buffer creation side effects, after which
// we swap in our big heap and never let it recreate again.
static ComPtr<ID3D12DescriptorHeap> g_bigRtvHeap;

// Allocate one big RTV heap (main swapchain images + headroom for the two zone
// swapchains' images) and adopt it as renderer.rtvHeap, then copy the main RTVs
// into it. Call AFTER CreateSwapchainRTVs(main) has set up PSOs + depth.
static bool AdoptBigRtvHeap(D3D12Renderer& renderer, ID3D12Resource** mainImages,
                            uint32_t mainCount, DXGI_FORMAT mainFmt, uint32_t reserveExtra) {
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = mainCount + reserveExtra;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (FAILED(renderer.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&g_bigRtvHeap))))
        return false;
    renderer.rtvHeap = g_bigRtvHeap;
    renderer.rtvDescriptorSize = renderer.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    renderer.rtvCount = 0;

    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = mainFmt;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE h = renderer.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < mainCount; i++) {
        renderer.device->CreateRenderTargetView(mainImages[i], &rtvDesc, h);
        h.ptr += renderer.rtvDescriptorSize;
    }
    renderer.rtvCount = mainCount;
    return true;
}

// Append image RTVs to the adopted big heap (does NOT recreate it). Returns the
// base index of the first appended RTV.
static int AppendImageRTVs(D3D12Renderer& renderer, ID3D12Resource** images,
                           uint32_t count, DXGI_FORMAT fmt) {
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = fmt;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    int base = (int)renderer.rtvCount;
    D3D12_CPU_DESCRIPTOR_HANDLE h = renderer.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)renderer.rtvCount * renderer.rtvDescriptorSize;
    for (uint32_t i = 0; i < count; i++) {
        renderer.device->CreateRenderTargetView(images[i], &rtvDesc, h);
        h.ptr += renderer.rtvDescriptorSize;
    }
    renderer.rtvCount += count;
    return base;
}

// #727 sliced route: append TEXTURE2DARRAY slice RTVs for an array swapchain.
// For each image, `arraySize` RTVs (one per slice, ArraySize=1). Slice s of
// image i lives at base + i*arraySize + s. Mirrors AppendImageRTVs but each RTV
// views a single array layer so RenderScene can target one eye's slice.
static int AppendArraySliceRTVs(D3D12Renderer& renderer, ID3D12Resource** images,
                                uint32_t count, uint32_t arraySize, DXGI_FORMAT fmt) {
    int base = (int)renderer.rtvCount;
    D3D12_CPU_DESCRIPTOR_HANDLE h = renderer.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)renderer.rtvCount * renderer.rtvDescriptorSize;
    for (uint32_t i = 0; i < count; i++) {
        for (uint32_t s = 0; s < arraySize; s++) {
            D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
            rtvDesc.Format = fmt;
            rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            rtvDesc.Texture2DArray.MipSlice = 0;
            rtvDesc.Texture2DArray.FirstArraySlice = s;
            rtvDesc.Texture2DArray.ArraySize = 1;
            rtvDesc.Texture2DArray.PlaneSlice = 0;
            renderer.device->CreateRenderTargetView(images[i], &rtvDesc, h);
            h.ptr += renderer.rtvDescriptorSize;
        }
    }
    renderer.rtvCount += count * arraySize;
    return base;
}

// Create one zone's swapchain, sized per xrGetDisplayZoneRecommendedViewSizeDXR,
// horizontally tiled per view, and register its image RTVs in renderer.rtvHeap.
static bool CreateZoneResources(XrSessionManager& xr, D3D12Renderer& renderer,
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
    z.sliced = ZoneSlicedRoute();       // #727: array-slice vs horizontal-tile submission
    z.format = xr.swapchain.format; // same encoding as the main projection swapchain

    XrSwapchainCreateInfo sci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    sci.format = z.format;
    sci.sampleCount = 1;
    // SLICED: single tile wide, viewCount array layers. TILED: viewCount tiles wide, 1 layer.
    sci.width = z.sliced ? z.tileW : (z.tileW * z.tileCount);
    sci.height = z.tileH;
    sci.faceCount = 1;
    sci.arraySize = z.sliced ? z.tileCount : 1;
    sci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(xr.session, &sci, &z.swapchain))) {
        LOG_ERROR("[zones] zone %u: xrCreateSwapchain failed (%ux%u arraySize=%u)",
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

    // Append this zone's RTVs to the adopted big RTV heap (does NOT recreate it —
    // the main-swapchain RTVs must survive). SLICED gets one RTV per (image,slice);
    // TILED gets one TEXTURE2D RTV per image.
    std::vector<ID3D12Resource*> textures(n);
    for (uint32_t i = 0; i < n; i++) textures[i] = z.images[i].texture;
    z.rtvBaseIndex = z.sliced
        ? AppendArraySliceRTVs(renderer, textures.data(), n, z.tileCount, (DXGI_FORMAT)z.format)
        : AppendImageRTVs(renderer, textures.data(), n, (DXGI_FORMAT)z.format);

    LOG_INFO("[zones] zone %u: rect %d,%d %dx%d -> %s swapchain %ux%u arraySize=%u "
             "(%u views of %ux%u, %u images, rtvBase=%d)",
             z.zoneId, z.rect.offset.x, z.rect.offset.y, z.rect.extent.width, z.rect.extent.height,
             z.sliced ? "SLICED" : "TILED", sci.width, sci.height, sci.arraySize,
             z.tileCount, z.tileW, z.tileH, n, z.rtvBaseIndex);
    return true;
}

// ---- Zone edge fade + clear PSOs ------------------------------------------
//
// The zone tile RTVs need (1) a per-zone-color clear that RenderScene can't do
// (its clear color is fixed) and (2) a content-alpha edge feather pass
// (ADR-027 rule 4). Both run as their own self-contained command-list submits
// against the renderer's queue — RenderScene's model. The feather uses a
// fullscreen triangle with a (ZERO, INV_SRC_ALPHA) blend so dst *= (1 - src.a)
// = the edge ramp; the clear simply ClearRenderTargetView's the zone color.

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

static ComPtr<ID3D12RootSignature> g_fadeRootSig;
static ComPtr<ID3D12PipelineState> g_fadePSO;
static ComPtr<ID3D12CommandAllocator> g_zoneCmdAlloc;
static ComPtr<ID3D12GraphicsCommandList> g_zoneCmdList;
static ComPtr<ID3D12Fence> g_zoneFence;
static UINT64 g_zoneFenceValue = 0;
static HANDLE g_zoneFenceEvent = nullptr;
static bool g_fadeInitTried = false;

// Content-alpha edge fade width. DXR_ZONES_FADE_PX overrides; 0 disables.
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

static bool EnsureZoneCmdResources(D3D12Renderer& renderer) {
    if (g_zoneCmdList) return true;
    if (FAILED(renderer.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&g_zoneCmdAlloc)))) return false;
    if (FAILED(renderer.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            g_zoneCmdAlloc.Get(), nullptr, IID_PPV_ARGS(&g_zoneCmdList)))) return false;
    g_zoneCmdList->Close();
    if (FAILED(renderer.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_zoneFence))))
        return false;
    g_zoneFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    return g_zoneFenceEvent != nullptr;
}

static void ZoneCmdSubmitAndWait(D3D12Renderer& renderer) {
    g_zoneCmdList->Close();
    ID3D12CommandList* lists[] = {g_zoneCmdList.Get()};
    renderer.commandQueue->ExecuteCommandLists(1, lists);
    g_zoneFenceValue++;
    renderer.commandQueue->Signal(g_zoneFence.Get(), g_zoneFenceValue);
    if (g_zoneFence->GetCompletedValue() < g_zoneFenceValue) {
        g_zoneFence->SetEventOnCompletion(g_zoneFenceValue, g_zoneFenceEvent);
        WaitForSingleObject(g_zoneFenceEvent, INFINITE);
    }
}

static bool EnsureEdgeFadePass(D3D12Renderer& renderer) {
    if (g_fadePSO) return true;
    if (g_fadeInitTried) return false;
    g_fadeInitTried = true;
    if (!EnsureZoneCmdResources(renderer)) return false;

    D3D12_ROOT_PARAMETER param = {};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.Constants.ShaderRegister = 0;
    param.Constants.RegisterSpace = 0;
    param.Constants.Num32BitValues = 4; // float2 tile_px + float feather + pad
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 1;
    rsDesc.pParameters = &param;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> serialized, error;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error)) ||
        FAILED(renderer.device->CreateRootSignature(0, serialized->GetBufferPointer(),
                serialized->GetBufferSize(), IID_PPV_ARGS(&g_fadeRootSig)))) {
        LOG_ERROR("[zones] edge-fade root sig failed%s%s", error ? ": " : "",
                  error ? (const char*)error->GetBufferPointer() : "");
        return false;
    }

    ComPtr<ID3DBlob> vs, ps, err;
    if (FAILED(D3DCompile(kFadeShaderSrc, strlen(kFadeShaderSrc), nullptr, nullptr, nullptr,
                          "vs_main", "vs_5_0", 0, 0, &vs, &err)) ||
        FAILED(D3DCompile(kFadeShaderSrc, strlen(kFadeShaderSrc), nullptr, nullptr, nullptr,
                          "ps_main", "ps_5_0", 0, 0, &ps, &err))) {
        LOG_ERROR("[zones] edge-fade shader compile failed%s%s", err ? ": " : "",
                  err ? (const char*)err->GetBufferPointer() : "");
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = g_fadeRootSig.Get();
    psoDesc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    psoDesc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = (DXGI_FORMAT)g_zonesArr[0].format; // zone swapchain format
    psoDesc.SampleDesc.Count = 1;
    // (ZERO, INV_SRC_ALPHA) -> dst *= (1 - src.a).
    psoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    psoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ZERO;
    psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ZERO;
    psoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    if (FAILED(renderer.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_fadePSO)))) {
        LOG_ERROR("[zones] edge-fade PSO creation failed");
        return false;
    }
    LOG_INFO("[zones] content-alpha edge fade pass ready (%.0f px)", ZoneEdgeFadePx());
    return true;
}

// Clear a zone tile's RTV to the zone's premultiplied clear color (incl alpha).
// `rtvIndex` is the per-zone image RTV in renderer.rtvHeap; clears the whole
// tiled image (all view tiles share one clear).
// Clears the single RTV at rtvIndex to the zone color. Caller passes the backing
// image resource (rtvIndex→image is not 1:1 in the sliced route) and, for a
// sliced zone, calls once per slice RTV; a tiled zone clears its whole image once.
static void ClearZoneImage(D3D12Renderer& renderer, DisplayZone& z,
                           ID3D12Resource* rt, int rtvIndex) {
    if (!EnsureZoneCmdResources(renderer)) return;

    g_zoneCmdAlloc->Reset();
    g_zoneCmdList->Reset(g_zoneCmdAlloc.Get(), nullptr);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = rt;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_zoneCmdList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = renderer.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += (SIZE_T)rtvIndex * renderer.rtvDescriptorSize;
    g_zoneCmdList->ClearRenderTargetView(rtvHandle, z.clearColor, 0, nullptr);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    g_zoneCmdList->ResourceBarrier(1, &barrier);

    ZoneCmdSubmitAndWait(renderer);
}

// #727 slice-color probe: fill one view's rectangle in `rt` with a solid color
// via a scissored ClearRenderTargetView. `vpX` is the view's x-origin (vi*tileW
// tiled, 0 for a per-slice sliced RTV); the rect scopes the clear to this view
// so tiled tile-0/tile-1 (or sliced slice-0/slice-1) get distinct colors.
static void PaintViewColor(D3D12Renderer& renderer, ID3D12Resource* rt, int rtvIndex,
                           uint32_t vpX, uint32_t tileW, uint32_t tileH, const float color[4]) {
    if (!EnsureZoneCmdResources(renderer)) return;

    g_zoneCmdAlloc->Reset();
    g_zoneCmdList->Reset(g_zoneCmdAlloc.Get(), nullptr);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = rt;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_zoneCmdList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = renderer.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += (SIZE_T)rtvIndex * renderer.rtvDescriptorSize;
    D3D12_RECT rect = {(LONG)vpX, 0, (LONG)(vpX + tileW), (LONG)tileH};
    g_zoneCmdList->ClearRenderTargetView(rtvHandle, color, 1, &rect);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    g_zoneCmdList->ResourceBarrier(1, &barrier);

    ZoneCmdSubmitAndWait(renderer);
}

// Multiply a zone view's RGBA by the edge ramp via the fade PSO (dst *= f).
// `rt` is the backing image; `vpX` is the view's x-origin within it (vi*tileW
// for tiled, 0 for a per-slice sliced RTV).
static void DrawZoneEdgeFade(D3D12Renderer& renderer, DisplayZone& z,
                             ID3D12Resource* rt, int rtvIndex,
                             uint32_t vpX, uint32_t tileW, uint32_t tileH) {
    if (ZoneEdgeFadePx() <= 0.0f) return; // DXR_ZONES_FADE_PX=0 disables
    if (!EnsureEdgeFadePass(renderer)) return;

    g_zoneCmdAlloc->Reset();
    g_zoneCmdList->Reset(g_zoneCmdAlloc.Get(), g_fadePSO.Get());

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = rt;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_zoneCmdList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = renderer.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += (SIZE_T)rtvIndex * renderer.rtvDescriptorSize;
    g_zoneCmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    D3D12_VIEWPORT vp = {};
    vp.TopLeftX = (FLOAT)vpX;
    vp.TopLeftY = 0.0f;
    vp.Width = (FLOAT)tileW;
    vp.Height = (FLOAT)tileH;
    vp.MaxDepth = 1.0f;
    g_zoneCmdList->RSSetViewports(1, &vp);
    D3D12_RECT scissor = {(LONG)vpX, 0, (LONG)(vpX + tileW), (LONG)tileH};
    g_zoneCmdList->RSSetScissorRects(1, &scissor);

    g_zoneCmdList->SetGraphicsRootSignature(g_fadeRootSig.Get());
    float cb[4] = {(float)tileW, (float)tileH, ZoneEdgeFadePx(), 0.0f};
    g_zoneCmdList->SetGraphicsRoot32BitConstants(0, 4, cb, 0);
    g_zoneCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_zoneCmdList->DrawInstanced(3, 1, 0, 0);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    g_zoneCmdList->ResourceBarrier(1, &barrier);

    ZoneCmdSubmitAndWait(renderer);
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

// Tier-3: acquire the freeform R8 D3D12 resource (runtime-owned), CPU-paint a
// feathered-rect mask at the zone rects (rings ramping M 0→1 over 24 px, solid
// core), upload it via CopyTextureRegion, then submit. The resource is handed
// out in RENDER_TARGET state and must be returned to RENDER_TARGET before
// submit (spec v2 D3D12 state contract). Mirrors cube_texture_d3d12_win case 4,
// generalized from one centered radial gradient to the zone rects.
static bool ApplyTier3FreeformWish(XrSessionManager& xr, D3D12Renderer& renderer) {
    if (!g_zone.pfnAcquire || g_zone.mask == XR_NULL_HANDLE) return false;
    XrLocal3DZoneRenderTargetD3D12DXR binding = {
        (XrStructureType)XR_TYPE_LOCAL_3D_ZONE_RENDER_TARGET_D3D12_DXR};
    XrResult r = g_zone.pfnAcquire(g_zone.mask, &binding);
    if (XR_FAILED(r) || binding.resource == nullptr || binding.width == 0 || binding.height == 0) {
        LOG_ERROR("[zones] xrAcquireLocal3DZoneRenderTargetDXR failed (0x%x)", (unsigned)r);
        return false;
    }
    ID3D12Resource* maskRes = static_cast<ID3D12Resource*>(binding.resource);
    const uint32_t mw = binding.width;
    const uint32_t mh = binding.height;
    const uint32_t pitch = (mw + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
        & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);

    // Mask pixels are client-window pixels; scale in case the runtime's backing
    // size differs from the live client size.
    const float sx = (g_windowWidth > 0) ? (float)mw / (float)g_windowWidth : 1.0f;
    const float sy = (g_windowHeight > 0) ? (float)mh / (float)g_windowHeight : 1.0f;

    std::vector<uint8_t> pixels((size_t)pitch * mh, 0);
    const int kRings = 8;
    const int kRingStep = 3;  // px per ring (window pixels)
    const int kFeather = 24;  // solid-core inset
    for (uint32_t zi = 0; zi < g_activeZones; zi++) {
        const XrRect2Di& zr = g_zonesArr[zi].rect;
        for (int step = 0; step <= kRings; step++) {
            const bool core = (step == kRings);
            const float m = core ? 1.0f : (float)(step + 1) / (float)kRings;
            const int inset = core ? kFeather : step * kRingStep;
            int l = (int)((zr.offset.x + inset) * sx);
            int t = (int)((zr.offset.y + inset) * sy);
            int rg = (int)((zr.offset.x + zr.extent.width - inset) * sx);
            int b = (int)((zr.offset.y + zr.extent.height - inset) * sy);
            l = (std::max)(l, 0);
            t = (std::max)(t, 0);
            rg = (std::min)(rg, (int)mw);
            b = (std::min)(b, (int)mh);
            if (rg <= l || b <= t) continue;
            const uint8_t mv = (uint8_t)(m * 255.0f + 0.5f);
            for (int y = t; y < b; y++) {
                uint8_t* prow = pixels.data() + (size_t)y * pitch;
                for (int x = l; x < rg; x++) prow[x] = mv;
            }
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
    if (FAILED(renderer.device->CreateCommittedResource(
            &up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&upload)))) {
        LOG_WARN("[zones] Tier-3: upload buffer creation failed");
        return false;
    }
    void* mapped = nullptr;
    if (FAILED(upload->Map(0, nullptr, &mapped)) || mapped == nullptr) {
        LOG_WARN("[zones] Tier-3: upload buffer map failed");
        return false;
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
    WaitForGpu(renderer); // keeps `upload` alive until the copy lands

    LOG_INFO("[zones] Tier-3 freeform mask painted at zone rects (%ux%u)", mw, mh);
    return true;
}

// Re-author the wish for the current mode (entering a mode, or after an O rect
// toggle while in an explicit mode). Mode 0 authors nothing (AUTO).
static void ApplyWishAuthoring(XrSessionManager& xr, D3D12Renderer& renderer) {
    if (g_wishMode == 1) {
        if (!EnsureWishMask(xr)) return;
        XrRect2Di rects[kNumZones];
        for (uint32_t zi = 0; zi < g_activeZones; zi++) rects[zi] = g_zonesArr[zi].rect;
        XrResult r = g_zone.pfnSetRects(g_zone.mask, g_activeZones, rects);
        if (XR_FAILED(r)) {
            LOG_ERROR("[zones] xrSetLocal3DZoneFromRectsDXR failed (0x%x)", (unsigned)r);
        }
    } else if (g_wishMode == 2) {
        if (!EnsureWishMask(xr)) return;
        if (!ApplyTier3FreeformWish(xr, renderer)) {
            LOG_ERROR("[zones] Tier-3 unavailable — staying on AUTO wish");
            g_wishMode = 0;
        }
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
static void HandleZoneKeys(XrSessionManager& xr, D3D12Renderer& renderer) {
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

// One-time zones activation: capabilities check + per-zone swapchains + strip.
// On any failure the zones path is permanently disabled (plain fallback).
static void TryActivateZones(XrSessionManager& xr, D3D12Renderer& renderer) {
    g_zonesAttempted = true;

    XrDisplayZoneCapabilitiesDXR caps = {XR_TYPE_DISPLAY_ZONE_CAPABILITIES_DXR};
    XrResult r = g_zones.pfnGetCaps(xr.session, &caps);
    if (XR_FAILED(r) || !caps.supported) {
        LOG_ERROR("[zones] xrGetDisplayZoneCapabilitiesDXR: rc=0x%x supported=%d — zones path disabled",
                  (unsigned)r, (int)caps.supported);
        g_hasDisplayZonesExt = false;
        return;
    }
    if (caps.maxZones3D < 1) {
        LOG_ERROR("[zones] maxZones3D=%u < 1 — zones path disabled", caps.maxZones3D);
        g_hasDisplayZonesExt = false;
        return;
    }
    // Run with what the DP supports (clamp to kNumZones). One zone is enough to
    // exercise the shared-texture 3D weave for the #727 tiled-vs-sliced A/B.
    g_activeZones = (caps.maxZones3D < kNumZones) ? caps.maxZones3D : kNumZones;
    LOG_INFO("[zones] capabilities: supported=1 maxZones3D=%u -> activeZones=%u",
             caps.maxZones3D, g_activeZones);

    // Zones share the session's view COUNT (display modes are session-global).
    // Allocate the zone swapchains at the MAX view count across all modes, not
    // the currently active mode's: standalone the session can start in a 1-view
    // mode (the service's tier-1 zones fallback flips to a multi-view mode only
    // at the first zones frame, AFTER these swapchains exist), and a 1-tile
    // swapchain would pin every zone to 1 view forever (#551). The per-frame
    // submit clamp to the ACTIVE mode's view count stays.
    uint32_t viewCount = 2;
    if (xr.renderingModeCount > 0) {
        viewCount = 1;
        for (uint32_t mi = 0; mi < xr.renderingModeCount; mi++) {
            viewCount = (std::max)(viewCount, xr.renderingModeViewCounts[mi]);
        }
    }
    if (viewCount < 1) viewCount = 1;
    if (viewCount > 8) viewCount = 8;

    // Zone A: left, below the strip. Identity rig, phase 0, dark-red OPAQUE clear.
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
    // punches every all-views-α==0 pixel in this zone through to DWM (#551).
    g_zonesArr[1].zoneId = 2;
    g_zonesArr[1].rect = g_zoneBOverlap ? kZoneBOverlapRect : kZoneBRect;
    g_zonesArr[1].ipdFactor = 0.6f;
    g_zonesArr[1].perspectiveFactor = 0.5f;
    g_zonesArr[1].spinPhase = 1.5f;
    g_zonesArr[1].clearColor[0] = 0.0f;
    g_zonesArr[1].clearColor[1] = 0.0f;
    g_zonesArr[1].clearColor[2] = 0.0f;
    g_zonesArr[1].clearColor[3] = 0.0f;

    // #727 Unity-mirror: one zone spanning the whole client window, no strip,
    // no zone B — matches the Unity plugin's forced full-window zone so the
    // runtime/DP see the identical zones shape (full-window canvas, wish
    // evaluates 3D everywhere).
    const bool fullwin = ZonesFullWindow();
    if (fullwin) {
        g_activeZones = 1;
        RECT cr = {};
        if (xr.windowHandle != nullptr && GetClientRect(xr.windowHandle, &cr) &&
            cr.right > 0 && cr.bottom > 0) {
            g_zonesArr[0].rect = {{0, 0}, {(int32_t)cr.right, (int32_t)cr.bottom}};
        } else {
            g_zonesArr[0].rect = {{0, 0}, {(int32_t)g_windowWidth, (int32_t)g_windowHeight}};
        }
        LOG_INFO("[zones] DXR_ZONES_FULLWIN=1 — single full-window zone %dx%d, no strip",
                 g_zonesArr[0].rect.extent.width, g_zonesArr[0].rect.extent.height);
    }

    for (uint32_t zi = 0; zi < g_activeZones; zi++) {
        if (!CreateZoneResources(xr, renderer, g_zonesArr[zi], viewCount)) {
            g_hasDisplayZonesExt = false;
            return;
        }
    }

    if (!fullwin && !CreateAndFillStrip(xr, renderer)) {
        g_hasDisplayZonesExt = false;
        return;
    }

    g_zonesActive = true;
    LOG_INFO("[zones] ACTIVE: zone A %d,%d %dx%d + zone B %d,%d %dx%d + strip %d,%d %dx%d "
             "(views=%u, activeZones=%u, fullwin=%d, wish mode 0 AUTO, validate=%d) — M=wish mode, O=overlap toggle",
             g_zonesArr[0].rect.offset.x, g_zonesArr[0].rect.offset.y,
             g_zonesArr[0].rect.extent.width, g_zonesArr[0].rect.extent.height,
             g_zonesArr[1].rect.offset.x, g_zonesArr[1].rect.offset.y,
             g_zonesArr[1].rect.extent.width, g_zonesArr[1].rect.extent.height,
             kStripRect.offset.x, kStripRect.offset.y, kStripRect.extent.width, kStripRect.extent.height,
             viewCount, g_activeZones, (int)fullwin, ZonesValidateEnabled() ? 1 : 0);
}

// Per-frame zones path: zone-scoped locate, per-zone render, submit
// [projA, projB, strip] with the zone structs chained on the projections.
static void RenderZonesFrame(RenderState& rs, const XrFrameState& frameState) {
    XrSessionManager& xr = *rs.xr;
    D3D12Renderer& renderer = *rs.renderer;

    // Per-zone locate + submit data. The zone structs are chained at BOTH
    // points (locate and xrEndFrame) — same instances within the frame.
    XrDisplayZoneDXR zoneStructs[kNumZones];
    XrDisplayRigDXR rigStructs[kNumZones];
    std::vector<XrCompositionLayerProjectionView> projViews[kNumZones];
    uint32_t submitViewCounts[kNumZones] = {};

    for (uint32_t zi = 0; zi < g_activeZones; zi++) {
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
        // views in a mono mode) — a well-behaved extension app submits only the
        // ACTIVE mode's view count (#542: 1 tile in a 2D mode; the runtime
        // clamps over-submission to the mode regardless, this is the proper
        // app-side behavior).
        uint32_t activeViewCount = viewCountOutput;
        if (xr.renderingModeCount > 0 && xr.currentModeIndex < xr.renderingModeCount) {
            activeViewCount = xr.renderingModeViewCounts[xr.currentModeIndex];
        }
        const uint32_t n = (std::min)((std::min)(viewCountOutput, z.tileCount), activeViewCount);
        submitViewCounts[zi] = n;
        projViews[zi].assign(n, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});

        // Render-ready views -> matrices. ZDP-anchored clip: near = ez - vH,
        // far = ez + 1000*vH, ez = rig-local eye distance to the zone's virtual
        // display plane (identity rig here, so ez = pose z).
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

        ID3D12Resource* zoneTex = z.images[imageIndex].texture;
        // #727: per-view RTV + x-origin differ by route. SLICED renders each
        // view into array slice vi (own RTV, x-origin 0); TILED renders every
        // view into one RTV at x-origin vi*tileW.
        auto viewRtv = [&](uint32_t vi) -> int {
            return z.sliced ? z.rtvBaseIndex + (int)(imageIndex * z.tileCount + vi)
                            : z.rtvBaseIndex + (int)imageIndex;
        };
        auto viewVpX = [&](uint32_t vi) -> uint32_t {
            return z.sliced ? 0u : (vi * z.tileW);
        };

        const bool sliceColors = ZoneSliceColors();

        // Per-zone clear (premultiplied RGBA, incl alpha). Zone A is opaque;
        // zone B's transparent {0,0,0,0} clear punches the unzoned-around-cube
        // pixels through to the desktop. RenderScene can't do this (fixed clear
        // color), so a dedicated clear pass authors it; the cube is then drawn
        // with clear=false. TILED clears the whole image via its one RTV; SLICED
        // must clear each slice's RTV. The slice-color probe fills each view
        // solidly below, so it needs no pre-clear.
        if (!sliceColors) {
            if (z.sliced) {
                for (uint32_t vi = 0; vi < n; vi++)
                    ClearZoneImage(renderer, z, zoneTex, viewRtv(vi));
            } else {
                ClearZoneImage(renderer, z, zoneTex, viewRtv(0));
            }
        }

        // Per-zone spin phase on the shared rotation (restored after render).
        const float savedRotation = renderer.cubeRotation;
        renderer.cubeRotation += z.spinPhase;

        for (uint32_t vi = 0; vi < n; vi++) {
            if (sliceColors) {
                // #727 probe: view 0 = BLUE, view 1 = RED (opaque). A correct
                // stereo weave interlaces the two; a mono collapse is flat blue.
                static const float kBlue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
                static const float kRed[4]  = {1.0f, 0.0f, 0.0f, 1.0f};
                PaintViewColor(renderer, zoneTex, viewRtv(vi), viewVpX(vi),
                               z.tileW, z.tileH, (vi == 0) ? kBlue : kRed);
            } else {
                const XMMATRIX viewMatrix = ColumnMajorToXMMatrix(rigViews[vi].view_matrix);
                const XMMATRIX projMatrix = ColumnMajorToXMMatrix(rigViews[vi].projection_matrix);
                RenderScene(renderer, zoneTex, viewRtv(vi),
                            viewVpX(vi), 0, z.tileW, z.tileH,
                            viewMatrix, projMatrix, 1.0f, /*clear=*/false, 0.03f);
            }

            projViews[zi][vi].subImage.swapchain = z.swapchain;
            projViews[zi][vi].subImage.imageRect.offset = {(int32_t)viewVpX(vi), 0};
            projViews[zi][vi].subImage.imageRect.extent = {(int32_t)z.tileW, (int32_t)z.tileH};
            projViews[zi][vi].subImage.imageArrayIndex = z.sliced ? vi : 0;
            projViews[zi][vi].pose = zoneViews[vi].pose;
            projViews[zi][vi].fov = rigViews[vi].fov;
        }

        renderer.cubeRotation = savedRotation;

        // Content-alpha edge feather (ADR-027 rule 4): fade THIS zone's
        // rendered RGBA at its tile edges so the zone blends softly into
        // whatever is behind it — desktop OR another zone. Skipped in wish
        // mode 1 (Tier-2 hard rects): content faded inside a hard-M=1 band
        // weaves to opaque black, not the desktop. Also skipped for the
        // slice-color probe (want solid, un-feathered fields).
        if (!sliceColors && g_wishMode != 1) {
            for (uint32_t vi = 0; vi < n; vi++) {
                DrawZoneEdgeFade(renderer, z, zoneTex, viewRtv(vi), viewVpX(vi), z.tileW, z.tileH);
            }
        }

        XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(z.swapchain, &ri);
    }

    // Layer list: [projA (zone A chained), projB (zone B chained), strip].
    XrCompositionLayerProjection projLayers[kNumZones];
    XrCompositionLayerLocal2DDXR stripLayer = {(XrStructureType)XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_DXR};
    const XrCompositionLayerBaseHeader* layers[kNumZones + 1] = {};
    uint32_t layerCount = 0;

    for (uint32_t zi = 0; zi < g_activeZones; zi++) {
        if (submitViewCounts[zi] == 0) continue;
        projLayers[zi] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        projLayers[zi].next = &zoneStructs[zi]; // SAME instance as the locate chain
        // Content alpha is meaningful (zone B transparent bg + the edge fade):
        // declare source-alpha blending (premultiplied bytes).
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

    // Zones alpha-composite against the DESKTOP by design — submit ALPHA_BLEND
    // whenever the runtime advertises it. With OPAQUE the whole window floor is
    // black and every translucent pixel reads as "faded to black".
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

    // #727: one-shot capture of the runtime's PROJECTION-ONLY atlas — the DP
    // INPUT (post per-tile blit, pre-weave), which is face-independent. Fired at
    // the same gate as the woven DXR_TEXDUMP so both artifacts reflect the same
    // frame. Lands in Pictures\DisplayXR\<app>-<N>_atlas_2_2x1.png. Manual 'I'
    // still works too. Uses the max submitted view count as the atlas columns.
    {
        static long zoneFrames = 0;
        static bool dpInputCaptured = false;
        zoneFrames++;
        uint32_t cols = 0;
        for (uint32_t zi = 0; zi < g_activeZones; zi++)
            cols = (submitViewCounts[zi] > cols) ? submitViewCounts[zi] : cols;
        if (!dpInputCaptured && zoneFrames >= kTexDumpFrame && cols > 1) {
            dpInputCaptured = true;
            dxr_capture::RequestRuntimeAtlasCapture(xr, APP_NAME, cols, 1, rs.hwnd);
        }
    }

    // Texture-mode present + autonomous verification: the runtime composited the
    // full-window multi-zone super-atlas into our shared texture (output rect
    // superseded by zones), so blit it into our own window swapchain and (when
    // DXR_TEXDUMP is set) read it back to a PNG.
    PresentAndMaybeDump(rs);
}

// ---------------------------------------------------------------------------

// Render a single frame — called from the main loop and from WM_PAINT during
// drag/resize so that rendering never stalls.
static void RenderOneFrame(RenderState& rs) {
    XrSessionManager& xr = *rs.xr;
    D3D12Renderer& renderer = *rs.renderer;

    UpdatePerformanceStats(*rs.perfStats);
    UpdateCameraMovement(g_inputState, rs.perfStats->deltaTime, rs.xr->displayHeightM);

    if (g_inputState.fullscreenToggleRequested) {
        ToggleFullscreen(rs.hwnd);
        g_inputState.fullscreenToggleRequested = false;
    }

    // Rendering mode requests (V=cycle next, 0-8=jump absolute).
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
    UpdateScene(renderer, rs.perfStats->deltaTime);

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
        g_zonesFrameCounter >= ZonesActivationFrame()) {
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
    bool viewsPopulated = false;

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
            uint32_t viewCount = 8;
            XrView rawViews[8];
            for (uint32_t vi = 0; vi < 8; vi++) rawViews[vi] = {XR_TYPE_VIEW};

            XrViewDisplayRawDXR rawProbe = {XR_TYPE_VIEW_DISPLAY_RAW_DXR};
            if (g_hasViewRigExt) {
                viewState.next = &rawProbe;
            }

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

            if (g_hasViewRigExt && rawProbe.canvasSizeMeters.height > 0.0f) {
                g_inputState.canvasWidthM = rawProbe.canvasSizeMeters.width;
                g_inputState.canvasHeightM = rawProbe.canvasSizeMeters.height;
            }

            uint32_t maxTileW = tileColumns > 0 ? xr.swapchain.width / tileColumns : xr.swapchain.width;
            uint32_t maxTileH = tileRows > 0 ? xr.swapchain.height / tileRows : xr.swapchain.height;

            std::vector<RigView> rigViews(eyeCount);
            if (useAppProjection) {
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

            // Render HUD to window-space layer swapchain.
            if (g_inputState.hudVisible && xr.hasHudSwapchain && rs.hudSwapchainImages && !rs.hudSwapchainImages->empty() && rs.hudOk) {
                uint32_t hudImageIndex;
                if (AcquireHudSwapchainImage(xr, hudImageIndex)) {
                    std::wstring sessionText(xr.systemName, xr.systemName + strlen(xr.systemName));
                    sessionText += L"\nSession: ";
                    sessionText += FormatSessionState((int)xr.sessionState);
                    std::wstring modeText = g_hasDisplayZonesExt ?
                        L"XR_DXR_display_zones: ACTIVATING (texture D3D12)" :
                        L"XR_DXR_display_zones: NOT AVAILABLE (DISPLAYXR_ZONES=1?)";
                    modeText += g_inputState.cameraMode ?
                        L"\nKooima: Camera-Centric [C=Toggle]" :
                        L"\nKooima: Display-Centric [C=Toggle]";
                    modeText += g_hasViewRigExt ?
                        L"\nView rig: RUNTIME rig (XR_DXR_view_rig)" :
                        L"\nView rig: unavailable (legacy views)";

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
                    std::wstring viewParamText = FormatViewParams(
                        g_inputState.viewParams.ipdFactor, g_inputState.viewParams.parallaxFactor,
                        dispP1, dispP2, g_inputState.cameraMode);
                    std::wstring helpText = FormatHelpText(xr.pfnRequestDisplayRenderingModeEXT != nullptr, g_inputState.cameraMode, xr.renderingModeCount);

                    uint32_t srcRowPitch = 0;
                    const void* pixels = RenderHudAndMap(*rs.hudRenderer, &srcRowPitch,
                        sessionText, modeText, perfText, dispText, eyeText,
                        cameraText, viewParamText, helpText);
                    if (pixels) {
                        const uint8_t* src = (const uint8_t*)pixels;
                        for (uint32_t row = 0; row < HUD_PIXEL_HEIGHT; row++) {
                            memcpy(rs.hudUploadMapped + row * rs.hudUploadRowPitch,
                                src + row * srcRowPitch,
                                HUD_PIXEL_WIDTH * 4);
                        }
                        UnmapHud(*rs.hudRenderer);

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

            // For mono: compute center view position and projection.
            XMMATRIX monoViewMatrix, monoProjMatrix;
            XrFovf monoFov = {};
            XrPosef monoPose = rawViews[0].pose;
            if (monoMode) {
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
                ID3D12Resource* swapchainTexture = (*rs.swapchainImages)[imageIndex].texture;
                int rtvIdx = rs.rtvBaseIndex + (int)imageIndex;

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
                    uint32_t vpX = tileX * renderW;
                    uint32_t vpY = tileY * renderH;

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

                    RenderScene(renderer, swapchainTexture, rtvIdx,
                        vpX, vpY, renderW, renderH,
                        viewMatrix, projMatrix,
                        useAppProjection ? 1.0f : g_inputState.viewParams.scaleFactor,
                        eye == 0);

                    projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                    projectionViews[eye].subImage.swapchain = xr.swapchain.swapchain;
                    projectionViews[eye].subImage.imageRect.offset = {(int32_t)vpX, (int32_t)vpY};
                    projectionViews[eye].subImage.imageRect.extent = {(int32_t)renderW, (int32_t)renderH};
                    projectionViews[eye].subImage.imageArrayIndex = 0;

                    int safeIdx = (eye < (int)viewCount) ? eye : 0;
                    projectionViews[eye].pose = monoMode ? monoPose : rawViews[safeIdx].pose;
                    projectionViews[eye].fov = useAppProjection ?
                        rigViews[monoMode ? 0 : eye].fov :
                        (monoMode ? monoFov : rawViews[safeIdx].fov);
                }
                viewsPopulated = true;

                if (g_inputState.captureAtlasRequested) {
                    g_inputState.captureAtlasRequested = false;
                    dxr_capture::RequestRuntimeAtlasCapture(
                        xr, APP_NAME, tileColumns, tileRows, rs.hwnd);
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

    // Texture-mode present: even on the fallback path the runtime composited
    // into our shared texture, so present it into our own window swapchain.
    PresentAndMaybeDump(rs);
}

// Destroy the zone/strip/wish resources (before session teardown).
static void CleanupZones() {
    for (uint32_t zi = 0; zi < g_activeZones; zi++) {
        DisplayZone& z = g_zonesArr[zi];
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

    LOG_INFO("=== Cube Zones TEXTURE D3D12 (XR_DXR_display_zones parity test) ===");
    LOG_INFO("Two 3D zones + Local2D strip + wish mask (ADR-027), composited into the app's SHARED D3D12 TEXTURE");
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
                g_texDumpPath = dir + "\\zones_texture_readback.png";
            } else {
                g_texDumpPath = e;
            }
            LOG_INFO("DXR_TEXDUMP set — shared-texture readback will dump to %s at frame %ld",
                     g_texDumpPath.c_str(), kTexDumpFrame);
        }
    }

    // #740 repro: DXR_WIN_SIZE=WxH sets an exact client size. With
    // DXR_ZONES_FULLWIN=1 the single zone == the client rect, mirroring the
    // Unity glued-window case at a precise size so the phase residual (mod the
    // ~5px lens pitch) can be dialed to the issue's 1695x930 / 2959x1654 rows.
    if (const char* ws = getenv("DXR_WIN_SIZE")) {
        int w = 0, h = 0;
        if (sscanf(ws, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
            g_windowWidth = (UINT)w;
            g_windowHeight = (UINT)h;
            LOG_INFO("DXR_WIN_SIZE -> client %ux%u", g_windowWidth, g_windowHeight);
        }
    }

    // #740 phase-transfer harness — see the g_paneOffsetX block at the top.
    if (const char* po = getenv("DXR_PANE_OFFSET")) {
        if (po[0] != '\0') {
            g_paneOffsetX = (int32_t)atol(po);
            if (g_paneOffsetX < 0) g_paneOffsetX = 0;
        }
    }
    if (const char* px = getenv("DXR_PANE_SCREEN_X")) {
        if (px[0] != '\0') g_paneScreenX = (int32_t)atol(px);
    }

    // Create window FIRST (needed for XR_DXR_win32_window_binding). With the
    // harness on, this top-level is the CONTAINER: it owns the app swapchain
    // and is sized to hold the pane at offset D, mirroring the editor shell.
    const bool harness = (g_paneOffsetX >= 0);
    // Latch the pane size before CreateAppWindow: the container's WM_SIZE
    // overwrites g_windowWidth/Height with the CONTAINER's client size.
    const UINT paneW = g_windowWidth, paneH = g_windowHeight;
    const UINT containerW = harness ? (UINT)(g_paneOffsetX + (int32_t)paneW) : paneW;
    HWND hwnd = CreateAppWindow(hInstance, containerW, paneH);
    if (!hwnd) {
        LOG_ERROR("Failed to create window");
        ShutdownLogging();
        return 1;
    }
    if (harness) {
        g_paneHwnd = CreatePaneWindow(hInstance, hwnd, g_paneOffsetX, 0,
                                      (int)paneW, (int)paneH);
        if (!g_paneHwnd) {
            LOG_ERROR("#740 harness: pane creation failed");
            ShutdownLogging();
            return 1;
        }
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
    //
    // #740 harness: slide the CONTAINER left by D so the pane's client origin
    // stays on a fixed panel pixel for every D. The container's non-client
    // border offsets the client origin by the same amount in both arms, so it
    // cancels in the differential and the control (harness off, D absent) lands
    // the bound window's client on those same pixels.
    SetWindowPos(hwnd, nullptr,
                 g_displayScreenLeft + (harness ? (g_paneScreenX - g_paneOffsetX) : g_paneScreenX),
                 g_displayScreenTop, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    // Check for session target extension (required for texture mode)
    if (!xr.hasWin32WindowBindingExt) {
        LOG_ERROR("XR_DXR_win32_window_binding not available — required for shared texture mode");
        MessageBox(hwnd, L"XR_DXR_win32_window_binding extension not available.\nRequired for shared texture mode.",
            L"Error", MB_OK | MB_ICONERROR);
        g_xr = nullptr;
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }
    LOG_INFO("XR_DXR_win32_window_binding extension is available - using shared texture");

    // Get the required GPU adapter LUID from OpenXR
    LUID adapterLuid;
    if (!GetD3D12GraphicsRequirements(xr, &adapterLuid)) {
        LOG_ERROR("Failed to get D3D12 graphics requirements");
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Initialize D3D12 on the correct adapter
    LOG_INFO("Initializing D3D12...");
    D3D12Renderer renderer = {};
    if (!InitializeD3D12WithLUID(renderer, adapterLuid)) {
        LOG_ERROR("D3D12 initialization failed");
        MessageBox(hwnd, L"Failed to initialize D3D12", L"Error", MB_OK | MB_ICONERROR);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create the app-side DXGI window swapchain (texture mode owns presentation).
    // #68 (A): in transparent mode the window is WS_EX_NOREDIRECTIONBITMAP, which
    // needs a COMPOSITION swapchain placed in a DirectComposition visual so the OS
    // blends the shared texture's alpha against the LIVE desktop (real see-through,
    // no captured-bg hack). Opaque mode keeps the plain ForHwnd swapchain.
    g_transparentPresent = TransparentBackgroundEnabled();
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
        HRESULT hr;
        if (g_transparentPresent) {
            // Premultiplied alpha — the zones submit premultiplied RGBA and the DP
            // gate emits premultiplied (alpha=0 ⟹ fully transparent → live desktop).
            scd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
            hr = renderer.dxgiFactory->CreateSwapChainForComposition(
                renderer.commandQueue.Get(), &scd, nullptr, &swapchain1);
            if (SUCCEEDED(hr)) {
                // The D3D12 composition swapchain is presented by the command
                // queue; DComp only needs to host it in a visual tree, so a
                // device-less DComp device (nullptr) is sufficient (it picks the
                // default adapter for composition).
                hr = DCompositionCreateDevice(nullptr, IID_PPV_ARGS(&g_dcompDevice));
                if (SUCCEEDED(hr)) hr = g_dcompDevice->CreateTargetForHwnd(hwnd, TRUE, &g_dcompTarget);
                if (SUCCEEDED(hr)) hr = g_dcompDevice->CreateVisual(&g_dcompVisual);
                if (SUCCEEDED(hr)) hr = g_dcompVisual->SetContent(swapchain1.Get());
                if (SUCCEEDED(hr)) hr = g_dcompTarget->SetRoot(g_dcompVisual.Get());
                if (SUCCEEDED(hr)) hr = g_dcompDevice->Commit();
            }
            if (FAILED(hr)) {
                LOG_ERROR("Transparent (DComp) present setup failed: 0x%08x", hr);
                CleanupD3D12(renderer);
                CleanupOpenXR(xr);
                ShutdownLogging();
                return 1;
            }
            LOG_INFO("#68 (A): transparent DComp present active — alpha=0 shows the live desktop");
        } else {
            hr = renderer.dxgiFactory->CreateSwapChainForHwnd(
                renderer.commandQueue.Get(), hwnd, &scd, nullptr, nullptr, &swapchain1);
            if (FAILED(hr)) {
                LOG_ERROR("Failed to create app swapchain: 0x%08x", hr);
                CleanupD3D12(renderer);
                CleanupOpenXR(xr);
                ShutdownLogging();
                return 1;
            }
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

    // Create blit pipeline (present the shared texture to the window).
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

    // Create the shared D3D12 texture at the worst-case swapchain atlas dims
    // (same sizing as cube_texture_d3d12_win; see ADR-010). The runtime
    // composites into this; the app blits it back to its window each frame.
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
        LOG_INFO("Created shared D3D12 texture: %ux%u, handle=%p", g_sharedWidth, g_sharedHeight, g_sharedHandle);
    }

    // Create SRV for shared texture (for blit)
    CreateSharedTextureSRV(renderer.device.Get());

    // Initialize HUD renderer (standalone device for text rendering)
    HudRenderer hudRenderer = {};
    bool hudOk = InitializeHudRenderer(hudRenderer, HUD_PIXEL_WIDTH, HUD_PIXEL_HEIGHT);
    if (!hudOk) {
        LOG_WARN("HUD renderer init failed - HUD will not be displayed");
    }

    // Create OpenXR session WITH the shared texture handle + app HWND
    // (XR_DXR_win32_window_binding). The shared texture exists BEFORE this
    // call — the runtime opens it at session create. THIS is the texture-mode
    // marker (the handle app passes only the HWND).
    // #740 harness: bind the PANE, not the container. The container keeps the
    // DXGI swapchain (what SR's device association resolves) — that split is
    // exactly the docked-editor condition under test.
    HWND bindHwnd = harness ? g_paneHwnd : hwnd;
    LOG_INFO("Creating OpenXR session (shared texture handle 0x%p + HWND 0x%p)...", g_sharedHandle, bindHwnd);
    if (!CreateSession(xr, renderer.device.Get(), renderer.commandQueue.Get(), g_sharedHandle, bindHwnd)) {
        LOG_ERROR("OpenXR session creation failed");
        MessageBox(hwnd, L"Failed to create OpenXR session", L"Error", MB_OK | MB_ICONERROR);
        if (hudOk) CleanupHudRenderer(hudRenderer);
        CleanupD3D12(renderer);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create reference spaces
    if (!CreateSpaces(xr)) {
        LOG_ERROR("Reference space creation failed");
        CleanupOpenXR(xr);
        if (hudOk) CleanupHudRenderer(hudRenderer);
        CleanupD3D12(renderer);
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
        CleanupD3D12(renderer);
        ShutdownLogging();
        return 1;
    }

    // Enumerate D3D12 swapchain images and create RTVs. The RTV heap is sized
    // generously here (main swapchain + a budget for the two zone swapchains'
    // images, which CreateZoneResources appends at activation via
    // CreateSwapchainRTVs). CreateSwapchainRTVs (re)creates the heap on first
    // call sized to the count we pass — so reserve headroom for the zones.
    std::vector<XrSwapchainImageD3D12KHR> swapchainImages;
    int rtvBaseIndex = 0;
    {
        uint32_t count = xr.swapchain.imageCount;
        swapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)swapchainImages.data());

        std::vector<ID3D12Resource*> textures(count);
        for (uint32_t i = 0; i < count; i++) textures[i] = swapchainImages[i].texture;

        // First call CreateSwapchainRTVs for its side effects (PSO recreation
        // to the swapchain format + depth-buffer creation). It builds a small
        // heap; we then adopt a big heap so the zone swapchains' RTVs can be
        // APPENDED at activation without recreating (and losing) the main RTVs.
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
        // Reserve headroom for the zone swapchains' RTVs. #727 SLICED route needs
        // (images × viewCount) RTVs per zone vs (images) for TILED; 64 covers both.
        if (!AdoptBigRtvHeap(renderer, textures.data(), count,
                (DXGI_FORMAT)xr.swapchain.format, 64)) {
            LOG_ERROR("Failed to adopt big RTV heap");
            CleanupOpenXR(xr);
            if (hudOk) CleanupHudRenderer(hudRenderer);
            CleanupD3D12(renderer);
            ShutdownLogging();
            return 1;
        }
        rtvBaseIndex = 0;
        LOG_INFO("Enumerated %u D3D12 swapchain images (rtvBase=%d, big heap adopted)", count, rtvBaseIndex);
    }

    // Create HUD swapchain and upload resources (fallback path only — in zones
    // mode the layer list stays exactly [zoneA, zoneB, strip]).
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
            LOG_INFO("HUD: enumerated %u D3D12 swapchain images", count);
        } else {
            LOG_WARN("Failed to create HUD swapchain - HUD will not be displayed");
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

    // Show window
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // #740 harness: log the bound surface's ACTUAL client origin on the panel.
    // The differential is only valid if this is byte-identical across arms, and
    // the readback alone CANNOT prove it — the shared texture is window-local,
    // so content lands at texture (0,0) no matter where the window sits. WARN
    // level so it survives the INFO hot-path filter. Harness-only: a plain run
    // has no container to report and should stay quiet.
    if (harness) {
        POINT o = {0, 0};
        ClientToScreen(bindHwnd, &o);
        RECT cr = {};
        GetClientRect(bindHwnd, &cr);
        POINT co = {0, 0};
        ClientToScreen(hwnd, &co);
        LOG_WARN("#740 harness: D=%d bound=0x%p client_origin=(%ld,%ld) client=%ldx%ld "
                 "container_client_origin=(%ld,%ld)",
                 g_paneOffsetX, bindHwnd, o.x, o.y, cr.right - cr.left, cr.bottom - cr.top,
                 co.x, co.y);
    }

    LOG_INFO("");
    LOG_INFO("=== Entering main loop ===");
    LOG_INFO("Texture mode: runtime composites zones into the shared D3D12 texture, app blits it to the window");
    LOG_INFO("Zones: M=wish mode (AUTO/Tier-2/Tier-3), O=zone B overlap toggle");
    LOG_INFO("Controls: WASD=Fly (fallback), V=Mode, T=Eye tracking, F11=Fullscreen, ESC=Quit");
    LOG_INFO("");

    PerformanceStats perfStats = {};
    perfStats.lastTime = std::chrono::high_resolution_clock::now();

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

    LOG_INFO("");
    LOG_INFO("=== Shutting down ===");

    // Wait for GPU idle.
    if (g_blitFence && renderer.commandQueue) {
        g_blitFenceValue++;
        renderer.commandQueue->Signal(g_blitFence.Get(), g_blitFenceValue);
        if (g_blitFence->GetCompletedValue() < g_blitFenceValue) {
            g_blitFence->SetEventOnCompletion(g_blitFenceValue, g_blitFenceEvent);
            WaitForSingleObject(g_blitFenceEvent, INFINITE);
        }
    }

    CleanupZones();

    // Zone command resources.
    if (g_zoneFenceEvent) CloseHandle(g_zoneFenceEvent);
    g_zoneFence.Reset();
    g_zoneCmdList.Reset();
    g_zoneCmdAlloc.Reset();
    g_fadePSO.Reset();
    g_fadeRootSig.Reset();

    // HUD resources.
    if (hudFenceEvent) CloseHandle(hudFenceEvent);
    hudFence.Reset();
    hudCmdList.Reset();
    hudCmdAllocator.Reset();
    if (hudUploadMapped && hudUploadBuffer) {
        hudUploadBuffer->Unmap(0, nullptr);
        hudUploadMapped = nullptr;
    }
    hudUploadBuffer.Reset();

    // Blit resources.
    if (g_blitFenceEvent) CloseHandle(g_blitFenceEvent);
    g_blitFence.Reset();
    g_blitCmdList.Reset();
    g_blitCmdAllocator.Reset();
    g_blitPSO.Reset();
    g_blitRootSig.Reset();
    g_blitSrvHeap.Reset();

    // Shared texture.
    if (g_sharedHandle) {
        CloseHandle(g_sharedHandle);
        g_sharedHandle = nullptr;
    }
    g_sharedTexture.Reset();

    // App swapchain + DComp visual tree.
    ReleaseAppSwapchainRTVs();
    g_appRtvHeap.Reset();
    g_appSwapchain.Reset();
    g_bigRtvHeap.Reset();
    g_dcompVisual.Reset();
    g_dcompTarget.Reset();
    g_dcompDevice.Reset();

    g_xr = nullptr;
    CleanupOpenXR(xr);
    if (hudOk) CleanupHudRenderer(hudRenderer);
    CleanupD3D12(renderer);

    DestroyWindow(hwnd);
    UnregisterClass(WINDOW_CLASS, hInstance);

    LOG_INFO("Application shutdown complete");
    ShutdownLogging();

    return 0;
}
