// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Cube Zones (OpenGL) — XR_EXT_display_zones exerciser (ADR-027)
 *
 * Cloned from cube_handle_gl_win (GL render) + cube_zones_d3d11_win (zones
 * logic) for #613.
 *
 * Exercises the display-zones runtime path (N 3D zones + Local2D strip + the
 * per-frame wish mask) end to end on the OpenGL native compositor:
 *
 *  - Zone A (zoneId=1, left)  : rect {0,180,640,540}, identity display rig,
 *    spin phase 0, dark-red clear.
 *  - Zone B (zoneId=2, right) : rect {700,180,520,360}, display rig with
 *    ipdFactor 0.6 + perspectiveFactor 0.5 (visibly different framing),
 *    spin phase +1.5 rad, FULLY TRANSPARENT clear (premultiplied all-zero) so
 *    the cube floats over the live desktop.
 *  - Local2D strip (top)      : rect {0,0,1280,180}, always on, filled once
 *    with a CPU-generated checker + label band.
 *
 * Each zone owns ONE swapchain sized per xrGetDisplayZoneRecommendedViewSizeEXT,
 * horizontally tiled per view (width = recSize.width * viewCount); each frame
 * runs a zone-scoped locate (XrDisplayZoneEXT + XrDisplayRigEXT chained on
 * XrViewLocateInfo) and submits [projA, projB, strip] with the SAME zone
 * structs chained on the projection layers.
 *
 * Keys (zones mode):
 *  - M : cycle wish mode — 0 AUTO (no frame-end info) / 1 explicit Tier-2
 *        rects. Wish mode 2 (Tier-3 freeform render target) is STUBBED on GL:
 *        XR_EXT_local_3d_zone has no OpenGL render-target binding struct, so M
 *        skips mode 2 and falls back to AUTO (same as the Vulkan zones app).
 *  - O : toggle zone B between its home rect and a rect overlapping zone A.
 *  - V : cycle display rendering mode (2D/3D).
 *  - DXR_ZONES_VALIDATE=1 : chain XR_DISPLAY_ZONES_FRAME_END_VALIDATE_BIT_EXT.
 *  - DXR_ZONES_FADE_PX=N  : content-alpha edge feather width (default 16; 0 off).
 *
 * When the runtime doesn't advertise XR_EXT_display_zones (DISPLAYXR_ZONES=1)
 * the app logs an error once and keeps running as a plain single-projection
 * cube (graceful degrade).
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>

#include "logging.h"
#include "input_handler.h"
#include "xr_session.h"
#include "gl_renderer.h"
#include "atlas_capture.h" // capture-flash window-message helpers (parity)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace DirectX;

static const char* APP_NAME = "cube_zones_gl_win";

static const wchar_t* WINDOW_CLASS = L"DXRCubeZonesGLClass";
static const wchar_t* WINDOW_TITLE = L"OpenGL Cube Zones — XR_EXT_display_zones";

// Global state (shared between main thread and render thread)
static InputState g_inputState;
static std::mutex g_inputMutex;
static std::atomic<bool> g_running{true};
static XrSessionManager* g_xr = nullptr;
static UINT g_windowWidth = 1280;
static UINT g_windowHeight = 720;

// Transparent background — ON BY DEFAULT for this app (zones alpha-composite
// against the desktop by design). DISPLAYXR_TRANSPARENT_BG=0 opts out.
static bool TransparentBackgroundEnabled() {
    static const bool e = []() {
        const char *v = getenv("DISPLAYXR_TRANSPARENT_BG");
        return v == nullptr || *v == '\0' || *v != '0';
    }();
    return e;
}

// DISPLAYXR_ARRAY_LAYOUT=1 switches each zone from the default horizontally
// TILED single-slice swapchain to the ARRAY / single-pass-instanced layout
// (arraySize=viewCount, per-view size, each view rendered into array slice vi
// via glFramebufferTextureLayer, submitted imageArrayIndex=vi). Exercises the
// GL compositor's per-view array-slice sampling — the same content must weave
// with disparity in both layouts. Default off (tiled).
static bool ArrayLayoutEnabled() {
    static const bool e = []() {
        const char *v = getenv("DISPLAYXR_ARRAY_LAYOUT");
        return v != nullptr && *v != '\0' && *v != '0';
    }();
    return e;
}

// ---------------------------------------------------------------------------
// XR_EXT_display_zones state
// ---------------------------------------------------------------------------

static const uint32_t kNumZones = 2;

// Per-zone rig framing for the test (the cube is 0.06 m tall).
static const float kZoneVirtualDisplayHeight = 0.30f;

struct DisplayZone {
    uint32_t zoneId = 0;
    XrRect2Di rect = {};            //!< client-window pixels; locate AND submit use this one variable
    float ipdFactor = 1.0f;
    float perspectiveFactor = 1.0f;
    float spinPhase = 0.0f;         //!< added to the shared cube rotation for this zone
    float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f}; //!< premultiplied RGBA (ADR-027)
    XrSwapchain swapchain = XR_NULL_HANDLE;
    int64_t format = 0;
    uint32_t tileW = 0;             //!< per-view tile width (= recommended view width)
    uint32_t tileH = 0;             //!< per-view tile height
    uint32_t tileCount = 0;         //!< view tiles in the horizontally tiled swapchain
    std::vector<XrSwapchainImageOpenGLKHR> images;
    GLZoneResources gl;             //!< per-image FBOs + depth renderbuffer
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

// Wish modes (M key): 0 AUTO, 1 explicit Tier-2 rects. (2 = Tier-3 STUBBED on
// GL — see header; M skips it.)
static int g_wishMode = 0;

// DXR_ZONES_VALIDATE=1 — chain the validate bit on every frame-end info.
static bool ZonesValidateEnabled() {
    static const bool e = []() {
        const char* v = getenv("DXR_ZONES_VALIDATE");
        return v != nullptr && *v == '1';
    }();
    return e;
}

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

// ---------------------------------------------------------------------------

// XR_EXT_view_rig: per-view staging container for the consumed render-ready
// views (matrices column-major).
struct RigView {
    float view_matrix[16];
    float projection_matrix[16];
    XrFovf fov;
};

// Column-major view matrix from a render-ready XrView pose:
// viewMatrix = R^T * translate(-position).
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
// XrView fov + the app's own clip policy. GL keeps the [-1,1] convention — no
// depth remap (this is the key divergence from the D3D11 zones app, which
// applies convert_projection_gl_to_zero_to_one()).
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

// Fullscreen state
static bool g_fullscreen = false;
static RECT g_savedWindowRect = {};
static DWORD g_savedWindowStyle = 0;

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
    {
        std::lock_guard<std::mutex> lock(g_inputMutex);
        UpdateInputState(g_inputState, msg, wParam, lParam);
    }

    switch (msg) {
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        return 0;

    case WM_LBUTTONUP:
        ReleaseCapture();
        return 0;

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_windowWidth = LOWORD(lParam);
            g_windowHeight = HIWORD(lParam);
        }
        return 0;

    case WM_SYSKEYDOWN:
        // Prevent ALT from activating the system menu modal loop.
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

static HWND CreateAppWindow(HINSTANCE hInstance, int width, int height) {
    const bool transparent = TransparentBackgroundEnabled();
    LOG_INFO("Creating application window (%dx%d, transparent=%d)", width, height, transparent);

    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS | CS_OWNDC;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    // Null brush in transparent mode so the redirection bitmap doesn't paint an
    // opaque floor under the DComp visual.
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

    DWORD exStyle = transparent ? WS_EX_NOREDIRECTIONBITMAP : 0;
    HWND hwnd = CreateWindowEx(exStyle, WINDOW_CLASS, WINDOW_TITLE, WS_OVERLAPPEDWINDOW,
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

// Create OpenGL context: temp legacy context → load wglCreateContextAttribsARB
// → core profile 3.3
static bool CreateOpenGLContext(HWND hwnd, HDC& hDC, HGLRC& hGLRC) {
    hDC = GetDC(hwnd);
    if (!hDC) {
        LOG_ERROR("GetDC failed");
        return false;
    }

    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;

    int pixelFormat = ChoosePixelFormat(hDC, &pfd);
    if (!pixelFormat) {
        LOG_ERROR("ChoosePixelFormat failed");
        return false;
    }
    if (!SetPixelFormat(hDC, pixelFormat, &pfd)) {
        LOG_ERROR("SetPixelFormat failed");
        return false;
    }

    HGLRC tempRC = wglCreateContext(hDC);
    if (!tempRC) {
        LOG_ERROR("wglCreateContext (temp) failed");
        return false;
    }
    if (!wglMakeCurrent(hDC, tempRC)) {
        LOG_ERROR("wglMakeCurrent (temp) failed");
        wglDeleteContext(tempRC);
        return false;
    }

    PFNWGLCREATECONTEXTATTRIBSARBPROC wglCreateContextAttribsARB =
        (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress("wglCreateContextAttribsARB");
    if (!wglCreateContextAttribsARB) {
        LOG_ERROR("wglCreateContextAttribsARB not available");
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(tempRC);
        return false;
    }

    int attribs[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
        WGL_CONTEXT_MINOR_VERSION_ARB, 3,
        WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
        0
    };

    wglMakeCurrent(nullptr, nullptr);
    hGLRC = wglCreateContextAttribsARB(hDC, nullptr, attribs);
    wglDeleteContext(tempRC);

    if (!hGLRC) {
        LOG_ERROR("wglCreateContextAttribsARB failed");
        return false;
    }
    if (!wglMakeCurrent(hDC, hGLRC)) {
        LOG_ERROR("wglMakeCurrent (core profile) failed");
        wglDeleteContext(hGLRC);
        hGLRC = nullptr;
        return false;
    }

    const char* vendor = (const char*)glGetString(GL_VENDOR);
    const char* rendererStr = (const char*)glGetString(GL_RENDERER);
    const char* version = (const char*)glGetString(GL_VERSION);
    LOG_INFO("OpenGL context created:");
    LOG_INFO("  Vendor: %s", vendor ? vendor : "unknown");
    LOG_INFO("  Renderer: %s", rendererStr ? rendererStr : "unknown");
    LOG_INFO("  Version: %s", version ? version : "unknown");
    return true;
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

// ---------------------------------------------------------------------------
// Zones helpers (GL)
// ---------------------------------------------------------------------------

// Prefer an sRGB-encoded format for the Local2D strip when the session
// advertises one (INV-4.6); the CPU-authored bytes are display-referred, so
// declaring them _SRGB keeps the compositor's decode→encode round-trip honest.
// GL codes: GL_SRGB8_ALPHA8=0x8C43, GL_RGBA8=0x8058.
static int64_t PickStripFormat(XrSessionManager& xr) {
    uint32_t n = 0;
    xrEnumerateSwapchainFormats(xr.session, 0, &n, nullptr);
    std::vector<int64_t> formats(n);
    if (n > 0) {
        xrEnumerateSwapchainFormats(xr.session, n, &n, formats.data());
    }
    for (int64_t f : formats) {
        if (f == (int64_t)0x8C43) return f; // GL_SRGB8_ALPHA8
    }
    for (int64_t f : formats) {
        if (f == (int64_t)0x8058) return f; // GL_RGBA8
    }
    return formats.empty() ? (int64_t)0x8058 : formats[0];
}

// Create the always-on Local2D strip swapchain and fill it once (static
// content: acquire/fill/release once; the layer references the released image
// every frame). Checker + a solid label band; OPAQUE alpha throughout. The GL
// context is current on the render thread.
static bool CreateAndFillStrip(XrSessionManager& xr) {
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
    std::vector<XrSwapchainImageOpenGLKHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
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

    size_t stride = (size_t)w * 4; // RGBA8 (GL upload order)
    std::vector<uint8_t> buf(stride * h);
    for (uint32_t y = 0; y < h; y++) {
        uint8_t* row = buf.data() + (size_t)y * stride;
        for (uint32_t x = 0; x < w; x++) {
            uint8_t* px = row + (size_t)x * 4; // R,G,B,A
            // Label band: solid amber bar near the left so captures read
            // "this is the 2D strip" at a glance.
            bool label = (x >= 40 && x < 360 && y >= 70 && y < 110);
            if (label) {
                px[0] = 255; // R
                px[1] = 170; // G
                px[2] = 0;   // B
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

    while (glGetError() != GL_NO_ERROR) {}
    glBindTexture(GL_TEXTURE_2D, imgs[idx].image);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
    glFlush();
    GLenum glErr = glGetError();
    glBindTexture(GL_TEXTURE_2D, 0);
    if (glErr != GL_NO_ERROR) {
        LOG_WARN("[zones] strip: glTexSubImage2D error 0x%X", glErr);
    }

    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(g_strip.swapchain, &ri);
    return true;
}

// Create one zone's swapchain + per-image FBOs + depth, sized per
// xrGetDisplayZoneRecommendedViewSizeEXT, horizontally tiled per view.
static bool CreateZoneResources(XrSessionManager& xr, GLRenderer& renderer,
                                DisplayZone& z, uint32_t viewCount) {
    XrExtent2Di rec = {};
    XrResult r = g_zones.pfnGetViewSize(xr.session, &z.rect, &rec);
    if (XR_FAILED(r) || rec.width <= 0 || rec.height <= 0) {
        LOG_ERROR("[zones] zone %u: xrGetDisplayZoneRecommendedViewSizeEXT failed (0x%x, %dx%d)",
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
    // ARRAY: one per-view image with `viewCount` slices. TILED: one wide image.
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

    uint32_t n = 0;
    xrEnumerateSwapchainImages(z.swapchain, 0, &n, nullptr);
    z.images.resize(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
    if (n == 0 || XR_FAILED(xrEnumerateSwapchainImages(z.swapchain, n, &n,
                                                       (XrSwapchainImageBaseHeader*)z.images.data()))) {
        LOG_ERROR("[zones] zone %u: xrEnumerateSwapchainImages failed", z.zoneId);
        return false;
    }

    std::vector<GLuint> texIds(n);
    for (uint32_t i = 0; i < n; i++) texIds[i] = z.images[i].image;
    // ARRAY: per-view-sized FBOs (fullW = tileW, arraySize slices). TILED: wide.
    const uint32_t fboFullW = arrayLayout ? z.tileW : (z.tileW * z.tileCount);
    if (!CreateZoneFBOs(z.gl, texIds.data(), n, fboFullW, z.tileH,
                        arrayLayout ? z.tileCount : 1)) {
        LOG_ERROR("[zones] zone %u: FBO creation failed", z.zoneId);
        return false;
    }

    LOG_INFO("[zones] zone %u: rect %d,%d %dx%d -> %s swapchain %ux%u arraySize=%u (%u views of %ux%u)",
             z.zoneId, z.rect.offset.x, z.rect.offset.y, z.rect.extent.width, z.rect.extent.height,
             arrayLayout ? "ARRAY" : "TILED", sci.width, sci.height, sci.arraySize,
             z.tileCount, z.tileW, z.tileH);
    return true;
}

// One-time zones activation: capabilities check + per-zone swapchains + strip.
// On any failure the zones path is permanently disabled (plain fallback).
static void TryActivateZones(XrSessionManager& xr, GLRenderer& renderer) {
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

    // Allocate the zone swapchains at the MAX view count across all modes (#551).
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
    // rad, FULLY TRANSPARENT clear (premultiplied all-zero): the cube floats
    // over the live desktop.
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

    if (!CreateAndFillStrip(xr)) {
        g_hasDisplayZonesExt = false;
        return;
    }

    g_zonesActive = true;
    LOG_INFO("[zones] ACTIVE: zone A %d,%d %dx%d + zone B %d,%d %dx%d + strip %d,%d %dx%d "
             "(views=%u, wish mode 0 AUTO, validate=%d, fade=%.0fpx) — M=wish mode, O=overlap toggle",
             kZoneARect.offset.x, kZoneARect.offset.y, kZoneARect.extent.width, kZoneARect.extent.height,
             g_zonesArr[1].rect.offset.x, g_zonesArr[1].rect.offset.y,
             g_zonesArr[1].rect.extent.width, g_zonesArr[1].rect.extent.height,
             kStripRect.offset.x, kStripRect.offset.y, kStripRect.extent.width, kStripRect.extent.height,
             viewCount, ZonesValidateEnabled() ? 1 : 0, ZoneEdgeFadePx());
}

// Lazily create the one mask handle shared by wish mode 1.
static bool EnsureWishMask(XrSessionManager& xr) {
    if (g_zone.mask != XR_NULL_HANDLE) return true;
    if (!g_zone.pfnCreate) return false;
    XrLocal3DZoneMaskCreateInfoEXT mci = {(XrStructureType)XR_TYPE_LOCAL_3D_ZONE_MASK_CREATE_INFO_EXT};
    mci.maskWidth = 0;
    mci.maskHeight = 0;
    XrResult r = g_zone.pfnCreate(xr.session, &mci, &g_zone.mask);
    if (XR_FAILED(r)) {
        LOG_ERROR("[zones] xrCreateLocal3DZoneMaskEXT failed (0x%x)", (unsigned)r);
        g_zone.mask = XR_NULL_HANDLE;
        return false;
    }
    LOG_INFO("[zones] wish mask created (window backing size)");
    return true;
}

// Re-author the wish for the current mode. Mode 0 authors nothing (AUTO).
// Mode 1 sets the explicit Tier-2 rects. (Mode 2 Tier-3 is unreachable on GL.)
static void ApplyWishAuthoring(XrSessionManager& xr) {
    if (g_wishMode == 1) {
        if (!EnsureWishMask(xr)) return;
        XrRect2Di rects[kNumZones];
        for (uint32_t zi = 0; zi < kNumZones; zi++) rects[zi] = g_zonesArr[zi].rect;
        XrResult r = g_zone.pfnSetRects(g_zone.mask, kNumZones, rects);
        if (XR_FAILED(r)) {
            LOG_ERROR("[zones] xrSetLocal3DZoneFromRectsEXT failed (0x%x)", (unsigned)r);
        }
    }
}

static const char* WishModeName(int mode) {
    switch (mode) {
    case 1: return "explicit Tier-2 rects";
    case 2: return "explicit Tier-3 feathered (UNSUPPORTED on GL)";
    default: return "AUTO";
    }
}

// Edge-triggered M (wish mode cycle) + O (zone B overlap toggle) polling.
static void HandleZoneKeys(XrSessionManager& xr) {
    static bool mPrev = false, oPrev = false;

    const bool mNow = (GetAsyncKeyState('M') & 0x8000) != 0;
    if (mNow && !mPrev) {
        g_wishMode = (g_wishMode + 1) % 3;
        if (g_wishMode == 2) {
            // GL Tier-3 STUB: no OpenGL render-target binding struct exists in
            // XR_EXT_local_3d_zone, so wish mode 2 is impractical to wire.
            // Skip straight to AUTO (matches the Vulkan zones app).
            LOG_WARN("[zones] wish mode 2 (Tier-3) unsupported on GL — falling back to AUTO");
            g_wishMode = 0;
        }
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
        // Keep an explicit wish in sync with the moved rect.
        ApplyWishAuthoring(xr);
    }
    oPrev = oNow;
}

// Per-frame zones path: zone-scoped locate, per-zone render, submit
// [projA, projB, strip] with the zone structs chained on the projections.
static void RenderZonesFrame(XrSessionManager& xr, GLRenderer& renderer,
                             const XrFrameState& frameState) {
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
        // not-yet-tracking first frame) — xrEndFrame rejects them.
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

        // xrLocateViews always reports the MAX view count — submit only the
        // ACTIVE mode's view count (clamped to the zone's tile capacity).
        uint32_t activeViewCount = viewCountOutput;
        if (xr.renderingModeCount > 0 && xr.currentModeIndex < xr.renderingModeCount) {
            activeViewCount = xr.renderingModeViewCounts[xr.currentModeIndex];
        }
        const uint32_t n = (std::min)((std::min)(viewCountOutput, z.tileCount), activeViewCount);
        submitViewCounts[zi] = n;
        projViews[zi].assign(n, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});

        // Render-ready views -> matrices. ZDP-anchored clip. GL keeps the
        // [-1,1] clip-z — NO convert_projection_gl_to_zero_to_one() (that is
        // the D3D depth remap; applying it on GL near-clips the content).
        std::vector<RigView> rigViews(n);
        for (uint32_t vi = 0; vi < n; vi++) {
            const XrView& v = zoneViews[vi];
            ViewMatrixFromXrPose(v.pose, rigViews[vi].view_matrix);
            const float ez = RigLocalEyeZ(rigStructs[zi].pose, v.pose.position);
            const float vH = kZoneVirtualDisplayHeight;
            const float nearZ = (ez - vH > 0.001f) ? (ez - vH) : 0.001f;
            const float farZ = ez + 1000.0f * vH;
            ProjectionFromXrFov(v.fov, nearZ, farZ, rigViews[vi].projection_matrix);
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

        // Per-zone spin phase on the shared rotation (restored after render).
        const float savedRotation = renderer.cubeRotation;
        renderer.cubeRotation += z.spinPhase;

        const bool arrayLayout = z.gl.arrayLayout;
        for (uint32_t vi = 0; vi < n; vi++) {
            const XMMATRIX viewMatrix = ColumnMajorToXMMatrix(rigViews[vi].view_matrix);
            const XMMATRIX projMatrix = ColumnMajorToXMMatrix(rigViews[vi].projection_matrix);

            // Per-zone clear (premultiplied RGBA): zone A opaque tint, zone B
            // fully transparent. HARD GOTCHA (#613): depth clears every tile
            // UNCONDITIONALLY inside RenderZoneTile (color clear is the passed
            // clearColor) — the cube would otherwise z-fail into a dark shadow.
            // ARRAY layout renders view vi into array slice vi (tileX ignored).
            RenderZoneTile(renderer, z.gl, imageIndex,
                           /*tileX*/ vi, z.tileW, z.tileH,
                           viewMatrix, projMatrix,
                           z.clearColor, /*cubeY*/ 0.03f, /*cubeZ*/ 0.0f,
                           /*cubeSize*/ 0.06f, /*slice*/ vi);

            projViews[zi][vi].subImage.swapchain = z.swapchain;
            // ARRAY: full image at slice vi. TILED: tile vi in the wide image.
            projViews[zi][vi].subImage.imageRect.offset = {arrayLayout ? 0 : (int32_t)(vi * z.tileW), 0};
            projViews[zi][vi].subImage.imageRect.extent = {(int32_t)z.tileW, (int32_t)z.tileH};
            projViews[zi][vi].subImage.imageArrayIndex = arrayLayout ? vi : 0;
            projViews[zi][vi].pose = zoneViews[vi].pose;
            projViews[zi][vi].fov = rigViews[vi].fov;
        }

        renderer.cubeRotation = savedRotation;

        // Content-alpha edge feather (ADR-027 rule 4). Skipped in wish mode 1
        // (explicit Tier-2): that wish is M=1 to the hard rect edge, so a
        // content fade inside it would weave to opaque black, not the desktop.
        if (g_wishMode != 1) {
            for (uint32_t vi = 0; vi < n; vi++) {
                DrawZoneEdgeFade(renderer, z.gl, imageIndex,
                                 /*tileX*/ vi, z.tileW, z.tileH, ZoneEdgeFadePx(),
                                 /*slice*/ vi);
            }
        }

        XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(z.swapchain, &ri);
    }

    // Layer list: [projA (zone A chained), projB (zone B chained), strip].
    XrCompositionLayerProjection projLayers[kNumZones];
    XrCompositionLayerLocal2DEXT stripLayer = {(XrStructureType)XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_EXT};
    const XrCompositionLayerBaseHeader* layers[kNumZones + 1] = {};
    uint32_t layerCount = 0;

    for (uint32_t zi = 0; zi < kNumZones; zi++) {
        if (submitViewCounts[zi] == 0) continue;
        projLayers[zi] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        projLayers[zi].next = &zoneStructs[zi]; // SAME instance as the locate chain
        // Content alpha is meaningful (zone B translucent bg + the edge fade):
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
    // requested; in mode 1 the mask is the frame's wish, atomic with the layer
    // set.
    XrDisplayZonesFrameEndInfoEXT zonesEnd = {(XrStructureType)XR_TYPE_DISPLAY_ZONES_FRAME_END_INFO_EXT};
    zonesEnd.flags = 0;
    zonesEnd.wishMask = XR_NULL_HANDLE;
    bool chainZonesEnd = false;
    if (g_wishMode >= 1 && g_zone.mask != XR_NULL_HANDLE) {
        zonesEnd.wishMask = g_zone.mask;
        chainZonesEnd = true;
    }
    if (ZonesValidateEnabled()) {
        zonesEnd.flags |= XR_DISPLAY_ZONES_FRAME_END_VALIDATE_BIT_EXT;
        chainZonesEnd = true;
    }
    if (chainZonesEnd) {
        endInfo.next = &zonesEnd;
    }

    xrEndFrame(xr.session, &endInfo);
}

// Destroy the zone/strip/wish resources (before session teardown).
static void CleanupZones() {
    for (uint32_t zi = 0; zi < kNumZones; zi++) {
        DisplayZone& z = g_zonesArr[zi];
        DestroyZoneFBOs(z.gl);
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

// ---------------------------------------------------------------------------
// Fallback path: plain single-projection cube (extension missing, zones
// activation failed, or pre-activation frames). Mirrors the cube_handle_gl_win
// render loop minus the HUD (kept lean; the HUD lives in the handle app).
// ---------------------------------------------------------------------------
static void RenderFallbackFrame(XrSessionManager& xr, GLRenderer& renderer,
                                std::vector<XrSwapchainImageOpenGLKHR>& /*swapchainImages*/,
                                const XrFrameState& frameState,
                                const InputState& inputSnapshot,
                                uint32_t windowW, uint32_t windowH) {
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
    bool viewsPopulated = false;

    if (frameState.shouldRender) {
        if (LocateViews(xr, frameState.predictedDisplayTime,
            inputSnapshot.cameraPosX, inputSnapshot.cameraPosY, inputSnapshot.cameraPosZ,
            inputSnapshot.yaw, inputSnapshot.pitch, inputSnapshot.viewParams)) {

            XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
            locateInfo.viewConfigurationType = xr.viewConfigType;
            locateInfo.displayTime = frameState.predictedDisplayTime;
            locateInfo.space = xr.localSpace;

            XrViewState viewState = {XR_TYPE_VIEW_STATE};
            uint32_t viewCount = 8;
            XrView rawViews[8];
            for (uint32_t vi = 0; vi < 8; vi++) rawViews[vi] = {XR_TYPE_VIEW};

            const bool useAppProjection =
                xr.hasDisplayInfoExt && xr.displayWidthM > 0.0f && g_hasViewRigExt;
            XrDisplayRigEXT displayRig = {XR_TYPE_DISPLAY_RIG_EXT};
            XrPosef rigPose = {{0, 0, 0, 1}, {0, 0, 0}};
            if (useAppProjection) {
                XMVECTOR rigOri = XMQuaternionRotationRollPitchYaw(
                    inputSnapshot.pitch, inputSnapshot.yaw, 0);
                XMFLOAT4 rq;
                XMStoreFloat4(&rq, rigOri);
                rigPose.orientation = {rq.x, rq.y, rq.z, rq.w};
                rigPose.position = {inputSnapshot.cameraPosX, inputSnapshot.cameraPosY,
                                    inputSnapshot.cameraPosZ};
                displayRig.pose = rigPose;
                displayRig.virtualDisplayHeight =
                    inputSnapshot.viewParams.virtualDisplayHeight / inputSnapshot.viewParams.scaleFactor;
                displayRig.ipdFactor = inputSnapshot.viewParams.ipdFactor;
                displayRig.parallaxFactor = inputSnapshot.viewParams.parallaxFactor;
                displayRig.perspectiveFactor = inputSnapshot.viewParams.perspectiveFactor;
                locateInfo.next = &displayRig;
            }
            xrLocateViews(xr.session, &locateInfo, &viewState, 8, &viewCount, rawViews);

            uint32_t maxTileW = tileColumns > 0 ? xr.swapchain.width / tileColumns : xr.swapchain.width;
            uint32_t maxTileH = tileRows > 0 ? xr.swapchain.height / tileRows : xr.swapchain.height;

            uint32_t renderW, renderH;
            if (monoMode) {
                renderW = windowW;
                renderH = windowH;
                if (renderW > xr.swapchain.width) renderW = xr.swapchain.width;
                if (renderH > xr.swapchain.height) renderH = xr.swapchain.height;
            } else {
                renderW = (uint32_t)(windowW * xr.recommendedViewScaleX);
                renderH = (uint32_t)(windowH * xr.recommendedViewScaleY);
                if (renderW > maxTileW) renderW = maxTileW;
                if (renderH > maxTileH) renderH = maxTileH;
            }

            std::vector<RigView> rigViews(eyeCount);
            if (useAppProjection) {
                const float rigVH =
                    inputSnapshot.viewParams.virtualDisplayHeight / inputSnapshot.viewParams.scaleFactor;
                for (int i = 0; i < eyeCount; i++) {
                    const XrView& v = rawViews[(i < (int)viewCount) ? i : 0];
                    ViewMatrixFromXrPose(v.pose, rigViews[i].view_matrix);
                    float ez = RigLocalEyeZ(rigPose, v.pose.position);
                    float nearZ = (ez - rigVH > 0.001f) ? (ez - rigVH) : 0.001f;
                    float farZ = ez + 1000.0f * rigVH;
                    ProjectionFromXrFov(v.fov, nearZ, farZ, rigViews[i].projection_matrix);
                    rigViews[i].fov = v.fov;
                }
            }

            XrPosef monoPose = rawViews[0].pose;
            if (monoMode) {
                XrVector3f center = {0.0f, 0.0f, 0.0f};
                int cnt = (int)viewCount; if (cnt < 1) cnt = 1;
                for (int v = 0; v < cnt; v++) {
                    center.x += rawViews[v].pose.position.x;
                    center.y += rawViews[v].pose.position.y;
                    center.z += rawViews[v].pose.position.z;
                }
                monoPose.position = {center.x / cnt, center.y / cnt, center.z / cnt};
            }

            uint32_t imageIndex;
            if (AcquireSwapchainImage(xr, imageIndex)) {
                for (int eye = 0; eye < eyeCount; eye++) {
                    XMMATRIX viewMatrix, projMatrix;
                    if (useAppProjection) {
                        int vi = monoMode ? 0 : eye;
                        viewMatrix = ColumnMajorToXMMatrix(rigViews[vi].view_matrix);
                        projMatrix = ColumnMajorToXMMatrix(rigViews[vi].projection_matrix);
                    } else {
                        int vi = (eye < (int)viewCount) ? eye : 0;
                        viewMatrix = xr.viewMatrices[vi];
                        projMatrix = xr.projMatrices[vi];
                    }

                    uint32_t tileX = monoMode ? 0 : (eye % tileColumns);
                    uint32_t tileY = monoMode ? 0 : (eye / tileColumns);
                    uint32_t vpX = tileX * renderW;
                    uint32_t vpY = tileY * renderH;

                    RenderScene(renderer, imageIndex, vpX, vpY, renderW, renderH,
                                viewMatrix, projMatrix,
                                useAppProjection ? 1.0f : inputSnapshot.viewParams.scaleFactor);

                    projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                    projectionViews[eye].subImage.swapchain = xr.swapchain.swapchain;
                    projectionViews[eye].subImage.imageRect.offset = {(int32_t)vpX, (int32_t)vpY};
                    projectionViews[eye].subImage.imageRect.extent = {(int32_t)renderW, (int32_t)renderH};
                    projectionViews[eye].subImage.imageArrayIndex = 0;
                    int safeIdx = (eye < (int)viewCount) ? eye : 0;
                    projectionViews[eye].pose = monoMode ? monoPose : rawViews[safeIdx].pose;
                    projectionViews[eye].fov = useAppProjection ?
                        rigViews[monoMode ? 0 : eye].fov :
                        (monoMode ? rawViews[0].fov : rawViews[safeIdx].fov);
                }
                viewsPopulated = true;
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

    XrCompositionLayerFlags projLayerFlags = TransparentBackgroundEnabled()
        ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
    EndFrame(xr, frameState.predictedDisplayTime, projectionViews.data(),
             (uint32_t)eyeCount, projLayerFlags);
}

// ---------------------------------------------------------------------------

static void RenderThreadFunc(
    HWND hwnd, HDC hDC, HGLRC hGLRC,
    XrSessionManager* xr, GLRenderer* renderer,
    std::vector<XrSwapchainImageOpenGLKHR>* swapchainImages)
{
    LOG_INFO("[RenderThread] Started");

    // GL context becomes current on THIS thread (and stays current); all GL +
    // swapchain calls — including the zone FBOs and strip upload — happen here.
    if (!wglMakeCurrent(hDC, hGLRC)) {
        LOG_ERROR("[RenderThread] wglMakeCurrent failed");
        return;
    }

    PerformanceStats perfStats = {};
    perfStats.lastTime = std::chrono::high_resolution_clock::now();

    while (g_running.load() && !xr->exitRequested) {
        InputState inputSnapshot;
        bool cycleModeRequested = false;
        int32_t absoluteModeRequest = -1;
        uint32_t windowW, windowH;
        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            inputSnapshot = g_inputState;
            g_inputState.resetViewRequested = false;
            g_inputState.fullscreenToggleRequested = false;
            g_inputState.eyeTrackingModeToggleRequested = false;
            cycleModeRequested = g_inputState.cycleRenderingModeRequested;
            g_inputState.cycleRenderingModeRequested = false;
            absoluteModeRequest = g_inputState.absoluteRenderingModeRequested;
            g_inputState.absoluteRenderingModeRequested = -1;
            windowW = g_windowWidth;
            windowH = g_windowHeight;
        }

        // Rendering mode requests (V=cycle, 0-8=absolute).
        if (cycleModeRequested && xr->pfnRequestDisplayRenderingModeEXT &&
            xr->session != XR_NULL_HANDLE && xr->renderingModeCount > 0) {
            uint32_t next = (xr->currentModeIndex + 1) % xr->renderingModeCount;
            xr->pfnRequestDisplayRenderingModeEXT(xr->session, next);
        }
        if (absoluteModeRequest >= 0 && xr->pfnRequestDisplayRenderingModeEXT &&
            xr->session != XR_NULL_HANDLE &&
            (uint32_t)absoluteModeRequest < xr->renderingModeCount) {
            xr->pfnRequestDisplayRenderingModeEXT(xr->session, (uint32_t)absoluteModeRequest);
        }

        UpdatePerformanceStats(perfStats);
        UpdateCameraMovement(inputSnapshot, perfStats.deltaTime, xr->displayHeightM);
        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_inputState.cameraPosX = inputSnapshot.cameraPosX;
            g_inputState.cameraPosY = inputSnapshot.cameraPosY;
            g_inputState.cameraPosZ = inputSnapshot.cameraPosZ;
        }

        // Cube spin (both zones share the rotation, offset in phase).
        UpdateScene(*renderer, perfStats.deltaTime, xr->spinSpeed);
        PollEvents(*xr);

        if (xr->sessionRunning) {
            XrFrameState frameState;
            if (BeginFrame(*xr, frameState)) {
                // ---- zones path ----
                g_zonesFrameCounter++;
                if (g_hasDisplayZonesExt && !g_zonesActive && !g_zonesAttempted &&
                    g_zonesFrameCounter >= kZonesActivationFrame) {
                    TryActivateZones(*xr, *renderer);
                }
                if (g_zonesActive && g_hasDisplayZonesExt) {
                    HandleZoneKeys(*xr);
                    if (frameState.shouldRender) {
                        RenderZonesFrame(*xr, *renderer, frameState);
                    } else {
                        XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
                        endInfo.displayTime = frameState.predictedDisplayTime;
                        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                        endInfo.layerCount = 0;
                        endInfo.layers = nullptr;
                        xrEndFrame(xr->session, &endInfo);
                    }
                } else {
                    // ---- fallback: plain single-projection cube ----
                    RenderFallbackFrame(*xr, *renderer, *swapchainImages, frameState,
                                        inputSnapshot, windowW, windowH);
                }
            }
        } else {
            Sleep(100);
        }
    }

    wglMakeCurrent(nullptr, nullptr);

    if (xr->exitRequested && g_running.load()) {
        PostMessage(hwnd, WM_CLOSE, 0, 0);
    }
    LOG_INFO("[RenderThread] Exiting");
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    if (!InitializeLogging(APP_NAME)) {
        MessageBox(nullptr, L"Failed to initialize logging", L"Warning", MB_OK | MB_ICONWARNING);
    }

    LOG_INFO("=== Cube Zones (OpenGL, XR_EXT_display_zones) ===");
    LOG_INFO("Two 3D zones + Local2D strip + wish mask (ADR-027)");
    LOG_INFO("Runtime dev gate: DISPLAYXR_ZONES=1 must be set for the runtime to advertise the extension");

    HWND hwnd = CreateAppWindow(hInstance, g_windowWidth, g_windowHeight);
    if (!hwnd) {
        LOG_ERROR("Failed to create window");
        ShutdownLogging();
        return 1;
    }

    HDC hDC = nullptr;
    HGLRC hGLRC = nullptr;
    if (!CreateOpenGLContext(hwnd, hDC, hGLRC)) {
        LOG_ERROR("OpenGL context creation failed");
        ShutdownLogging();
        return 1;
    }

    if (!LoadGLFunctions()) {
        LOG_ERROR("Failed to load GL function pointers");
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(hGLRC);
        ShutdownLogging();
        return 1;
    }

    XrSessionManager xr = {};
    g_xr = &xr;
    if (!InitializeOpenXR(xr)) {
        LOG_ERROR("OpenXR initialization failed");
        g_xr = nullptr;
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(hGLRC);
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

    if (!GetOpenGLGraphicsRequirements(xr)) {
        LOG_ERROR("Failed to get OpenGL graphics requirements");
        CleanupOpenXR(xr);
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(hGLRC);
        ShutdownLogging();
        return 1;
    }

    if (!CreateSession(xr, hDC, hGLRC, hwnd)) {
        LOG_ERROR("OpenXR session creation failed");
        CleanupOpenXR(xr);
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(hGLRC);
        ShutdownLogging();
        return 1;
    }

    if (!CreateSpaces(xr)) {
        LOG_ERROR("Reference space creation failed");
        CleanupOpenXR(xr);
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(hGLRC);
        ShutdownLogging();
        return 1;
    }

    // Single fallback swapchain at native display resolution. The zones path
    // creates its own per-zone swapchains at activation; this one serves the
    // pre-activation frames and the no-extension fallback.
    if (!CreateSwapchain(xr)) {
        LOG_ERROR("Swapchain creation failed");
        CleanupOpenXR(xr);
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(hGLRC);
        ShutdownLogging();
        return 1;
    }

    std::vector<XrSwapchainImageOpenGLKHR> swapchainImages;
    {
        uint32_t count = xr.swapchain.imageCount;
        swapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
        xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)swapchainImages.data());
        LOG_INFO("Enumerated %u OpenGL swapchain images", count);
    }

    GLRenderer glRenderer = {};
    if (!InitializeGLRenderer(glRenderer)) {
        LOG_ERROR("GL renderer initialization failed");
        CleanupOpenXR(xr);
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(hGLRC);
        ShutdownLogging();
        return 1;
    }

    // FBOs for the fallback SBS swapchain.
    {
        uint32_t count = xr.swapchain.imageCount;
        std::vector<GLuint> textures(count);
        for (uint32_t i = 0; i < count; i++) textures[i] = swapchainImages[i].image;
        if (!CreateSwapchainFBOs(glRenderer, textures.data(), count,
            xr.swapchain.width, xr.swapchain.height)) {
            LOG_ERROR("Failed to create FBOs for swapchain");
            CleanupGLRenderer(glRenderer);
            CleanupOpenXR(xr);
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(hGLRC);
            ShutdownLogging();
            return 1;
        }
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    LOG_INFO("");
    LOG_INFO("=== Entering main loop ===");
    LOG_INFO("Zones: M=wish mode (AUTO/Tier-2; Tier-3 unsupported on GL), O=zone B overlap toggle");
    LOG_INFO("Controls: WASD=Fly (fallback), V=Mode, F11=Fullscreen, ESC=Quit");
    LOG_INFO("");

    // Release GL context from main thread before handing to render thread.
    wglMakeCurrent(nullptr, nullptr);

    g_inputState.viewParams.virtualDisplayHeight = 0.24f;
    g_inputState.renderingModeCount = xr.renderingModeCount;

    std::thread renderThread(RenderThreadFunc, hwnd, hDC, hGLRC, &xr, &glRenderer,
                             &swapchainImages);

    MSG msg = {};
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    g_running.store(false);
    LOG_INFO("Main thread: waiting for render thread...");
    renderThread.join();
    LOG_INFO("Main thread: render thread joined");

    LOG_INFO("");
    LOG_INFO("=== Shutting down ===");

    // Re-acquire GL context for cleanup (zone FBOs + renderer live on the GPU).
    wglMakeCurrent(hDC, hGLRC);

    CleanupZones();
    CleanupGLRenderer(glRenderer);
    g_xr = nullptr;
    CleanupOpenXR(xr);

    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(hGLRC);
    ReleaseDC(hwnd, hDC);

    DestroyWindow(hwnd);
    UnregisterClass(WINDOW_CLASS, hInstance);

    LOG_INFO("Application shutdown complete");
    ShutdownLogging();
    return 0;
}
