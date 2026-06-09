// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  SR Cube OpenXR Ext GL - OpenXR with XR_EXT_win32_window_binding (OpenGL)
 *
 * OpenGL port of cube_handle_d3d11. Projection layer + window-space HUD overlay.
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>

#include "logging.h"
#include "input_handler.h"
#include "xr_session.h"
#include "gl_renderer.h"
#include "hud_renderer.h"
#include "text_overlay.h"
#include "atlas_capture.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <string>
#include <mutex>
#include <thread>
#include <vector>

using namespace DirectX;

static const char* APP_NAME = "cube_handle_gl_win";

// HUD overlay fractions: WIDTH_FRACTION anchors how wide the HUD appears on screen;
// HEIGHT_FRACTION sets the HUD texture pixel height (aspect ratio preserved dynamically).
static const float HUD_WIDTH_FRACTION = 0.30f;
static const float HUD_HEIGHT_FRACTION = 0.50f;

static const wchar_t* WINDOW_CLASS = L"SRCubeOpenXRExtGLClass";
static const wchar_t* WINDOW_TITLE = L"OpenGL Cube \u2014 GL Native Compositor (External Window)";

// Global state (shared between main thread and render thread)
static InputState g_inputState;
static std::mutex g_inputMutex;
static std::atomic<bool> g_running{true};
static XrSessionManager* g_xr = nullptr;
static UINT g_windowWidth = 1280;
static UINT g_windowHeight = 720;

// #439 Phase 3 — handle + mask + Local2D layer modes (§8 cases 2/3/4), GL leg.
// DXR_LOCAL2D_PANEL=1 (case 3, implicit mask) / +DXR_LOCAL2D_MASK=1 (case 2,
// explicit Tier-2 island mask) / +DXR_LOCAL2D_PANEL2=1 (case 4, second
// unpremultiplied overlapping panel). The GL Leia DP is chroma-key-only, so
// desktop-show-through behaves differently than D3D11/VK — expected.
static bool g_l2dPanel = false;
static bool g_l2dMask = false;
static bool g_l2dPanel2 = false;
static bool g_l2dActive = false;
// #491 part 3 — 2D-under backdrop (0=off, 2=opaque, 3=semi-transparent).
static int g_l2dBackdropVariant = 0;
static long g_l2dFrameCounter = 0;
static const long g_l2dActivationFrame = 10;

struct L2DPanel {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    uint32_t w = 0, h = 0;
};
static L2DPanel g_panel1, g_panel2, g_backdrop;
static XrRect2Di g_panel1Rect, g_panel2Rect, g_backdropRect;

// Fullscreen state
static bool g_fullscreen = false;
static RECT g_savedWindowRect = {};
static DWORD g_savedWindowStyle = 0;

// XR_EXT_view_rig (#396 W7 dogfood): the app chains XrDisplayRigEXT on every
// xrLocateViews and consumes the runtime's render-ready XrView{pose, fov}
// directly — the per-frame Kooima generation is deleted; only clip policy
// stays app-side. Per-view staging container (matrices column-major):
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
// XrView fov + the app's own clip policy (fov is clip-independent). GL keeps
// the [-1,1] convention — no depth remap.
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

// Toggle fullscreen mode for the app window
static void ToggleFullscreen(HWND hwnd) {
    if (g_fullscreen) {
        // Exit fullscreen - restore window style and position
        SetWindowLong(hwnd, GWL_STYLE, g_savedWindowStyle);
        SetWindowPos(hwnd, HWND_TOP,
            g_savedWindowRect.left, g_savedWindowRect.top,
            g_savedWindowRect.right - g_savedWindowRect.left,
            g_savedWindowRect.bottom - g_savedWindowRect.top,
            SWP_FRAMECHANGED);
        g_fullscreen = false;
        LOG_INFO("Exited fullscreen mode");
    } else {
        // Enter fullscreen - save state and go borderless
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
        SetCapture(hwnd);  // Outside mutex — safe from reentrant deadlock
        return 0;

    case WM_LBUTTONUP:
        ReleaseCapture();  // Outside mutex — WM_CAPTURECHANGED can safely re-enter
        return 0;

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            std::lock_guard<std::mutex> lock(g_inputMutex);
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
    LOG_INFO("Creating application window (%dx%d)", width, height);

    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS | CS_OWNDC;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
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

    HWND hwnd = CreateWindowEx(0, WINDOW_CLASS, WINDOW_TITLE, WS_OVERLAPPEDWINDOW,
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

// Create OpenGL context: temp legacy context → load wglCreateContextAttribsARB → core profile 3.3
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

    // Create temporary legacy context to load WGL extensions
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

    // Load wglCreateContextAttribsARB
    PFNWGLCREATECONTEXTATTRIBSARBPROC wglCreateContextAttribsARB =
        (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress("wglCreateContextAttribsARB");

    if (!wglCreateContextAttribsARB) {
        LOG_ERROR("wglCreateContextAttribsARB not available");
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(tempRC);
        return false;
    }

    // Create core profile 3.3 context
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

// #439 Phase 3 — create a window-anchored Local2D panel swapchain and fill it
// once via glTexSubImage2D (the GL context is current on the render thread).
// Static content: acquire/fill/release once; the layer references the released
// image every frame. RGBA8; same probe imagery as the D3D11/D3D12 legs.
//  variant 0 — crispness: opaque fine 8-px checker core + 24-px half-transparent
//              green border (premultiplied bytes).
//  variant 1 — stacking/alpha: UNPREMULTIPLIED orange a=128 + opaque white
//              diagonal stripes (submitted with the unpremultiplied bit).
static bool CreateAndFillL2DPanel(XrSessionManager& xr, uint32_t w, uint32_t h, int variant, L2DPanel& out) {
    if (w == 0 || h == 0) {
        return false;
    }

    XrSwapchainCreateInfo sci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    sci.format = 0x8058; // GL_RGBA8
    sci.sampleCount = 1;
    sci.width = w;
    sci.height = h;
    sci.faceCount = 1;
    sci.arraySize = 1;
    sci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(xr.session, &sci, &out.swapchain))) {
        LOG_ERROR("Local2D panel: xrCreateSwapchain failed");
        return false;
    }
    out.w = w;
    out.h = h;

    uint32_t n = 0;
    xrEnumerateSwapchainImages(out.swapchain, 0, &n, nullptr);
    std::vector<XrSwapchainImageOpenGLKHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
    if (n == 0 || XR_FAILED(xrEnumerateSwapchainImages(out.swapchain, n, &n,
                                                       (XrSwapchainImageBaseHeader*)imgs.data()))) {
        LOG_ERROR("Local2D panel: xrEnumerateSwapchainImages failed");
        return false;
    }

    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t idx = 0;
    if (XR_FAILED(xrAcquireSwapchainImage(out.swapchain, &ai, &idx))) {
        return false;
    }
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(out.swapchain, &wi);

    size_t stride = (size_t)w * 4; // RGBA8
    std::vector<uint8_t> buf(stride * h);
    const uint32_t border = 24;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t* row = buf.data() + (size_t)y * stride;
        for (uint32_t x = 0; x < w; x++) {
            uint8_t* px = row + (size_t)x * 4; // R,G,B,A
            if (variant == 2) {
                // #491 part 3 backdrop (opaque): coarse cyan/blue checker.
                bool check = (((x / 32) + (y / 32)) & 1) != 0;
                px[0] = 0; px[1] = check ? 120 : 40; px[2] = check ? 200 : 90; px[3] = 255;
            } else if (variant == 3) {
                // #491 part 3 backdrop (semi-transparent ~50%, PREMULTIPLIED) —
                // the desktop shows through it.
                bool check = (((x / 32) + (y / 32)) & 1) != 0;
                if (check) { px[0] = 110; px[1] = 0; px[2] = 110; px[3] = 128; }
                else       { px[0] = 90; px[1] = 90; px[2] = 0; px[3] = 128; }
            } else if (variant == 0) {
                bool inBorder = (x < border || y < border || x >= w - border || y >= h - border);
                if (inBorder) {
                    px[0] = 0; px[1] = 128; px[2] = 0; px[3] = 128; // half-transparent green, premultiplied
                } else {
                    bool check = (((x / 8) + (y / 8)) & 1) != 0;
                    uint8_t v = check ? 235 : 40;
                    px[0] = v; px[1] = v; px[2] = v; px[3] = 255; // opaque fine checker
                }
            } else {
                bool stripe = (((x + y) / 16) & 1) != 0;
                if (stripe) {
                    px[0] = 255; px[1] = 255; px[2] = 255; px[3] = 255; // opaque white stripes
                } else {
                    px[0] = 255; px[1] = 165; px[2] = 0; px[3] = 128; // UNPREMULTIPLIED orange a=128
                }
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
        LOG_WARN("Local2D panel: glTexSubImage2D error 0x%X", glErr);
    }

    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(out.swapchain, &ri);
    return true;
}

static void RenderThreadFunc(
    HWND hwnd,
    HDC hDC,
    HGLRC hGLRC,
    XrSessionManager* xr,
    GLRenderer* renderer,
    std::vector<XrSwapchainImageOpenGLKHR>* swapchainImages,
    HudRenderer* hud,
    uint32_t hudWidth,
    uint32_t hudHeight,
    std::vector<XrSwapchainImageOpenGLKHR>* hudSwapchainImages)
{
    LOG_INFO("[RenderThread] Started");

    // Make the GL context current on this thread
    if (!wglMakeCurrent(hDC, hGLRC)) {
        LOG_ERROR("[RenderThread] wglMakeCurrent failed");
        return;
    }

    PerformanceStats perfStats = {};
    perfStats.lastTime = std::chrono::high_resolution_clock::now();

    while (g_running.load() && !xr->exitRequested) {
        InputState inputSnapshot;
        bool resetRequested = false;
        bool cycleModeRequested = false;
        int32_t absoluteModeRequest = -1;
        uint32_t windowW, windowH;
        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            inputSnapshot = g_inputState;
            resetRequested = g_inputState.resetViewRequested;
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

        // Rendering mode requests (V=cycle, 0-8=absolute). Single source of
        // truth: the runtime owns current mode via xr->currentModeIndex.
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
            if (resetRequested) {
                g_inputState.yaw = inputSnapshot.yaw;
                g_inputState.pitch = inputSnapshot.pitch;
                g_inputState.viewParams = inputSnapshot.viewParams;
            }
        }
        // Handle eye tracking mode toggle (T key)
        if (inputSnapshot.eyeTrackingModeToggleRequested) {
            if (xr->pfnRequestEyeTrackingModeEXT && xr->session != XR_NULL_HANDLE) {
                XrEyeTrackingModeEXT newMode = (xr->activeEyeTrackingMode == XR_EYE_TRACKING_MODE_MANAGED_EXT)
                    ? XR_EYE_TRACKING_MODE_MANUAL_EXT : XR_EYE_TRACKING_MODE_MANAGED_EXT;
                XrResult etResult = xr->pfnRequestEyeTrackingModeEXT(xr->session, newMode);
                LOG_INFO("Eye tracking mode -> %s (%s)",
                    newMode == XR_EYE_TRACKING_MODE_MANUAL_EXT ? "MANUAL" : "MANAGED",
                    XR_SUCCEEDED(etResult) ? "OK" : "unsupported");
            }
        }

        // Cube spin speed is agent-settable via cube-gl__set_spin (#457)
        UpdateScene(*renderer, perfStats.deltaTime, xr->spinSpeed);
        PollEvents(*xr);

        if (xr->sessionRunning) {
            XrFrameState frameState;
            if (BeginFrame(*xr, frameState)) {
                bool rendered = false;
                bool hudSubmitted = false;

                // Get N-view mode info from enumerated rendering modes
                uint32_t modeViewCount = (xr->renderingModeCount > 0 && xr->currentModeIndex < xr->renderingModeCount)
                    ? xr->renderingModeViewCounts[xr->currentModeIndex] : 2;
                uint32_t tileColumns = (xr->renderingModeCount > 0 && xr->currentModeIndex < xr->renderingModeCount)
                    ? xr->renderingModeTileColumns[xr->currentModeIndex] : 2;
                uint32_t tileRows = (xr->renderingModeCount > 0 && xr->currentModeIndex < xr->renderingModeCount)
                    ? xr->renderingModeTileRows[xr->currentModeIndex] : 1;
                bool monoMode = (xr->renderingModeCount > 0 && !xr->renderingModeDisplay3D[xr->currentModeIndex]);
                if (xr->renderingModeCount > 0 && xr->currentModeIndex < xr->renderingModeCount) {
                    xr->recommendedViewScaleX = xr->renderingModeScaleX[xr->currentModeIndex];
                    xr->recommendedViewScaleY = xr->renderingModeScaleY[xr->currentModeIndex];
                }
                int eyeCount = monoMode ? 1 : (int)modeViewCount;
                std::vector<XrCompositionLayerProjectionView> projectionViews(eyeCount, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});

                if (frameState.shouldRender) {
                    if (LocateViews(*xr, frameState.predictedDisplayTime,
                        inputSnapshot.cameraPosX, inputSnapshot.cameraPosY, inputSnapshot.cameraPosZ,
                        inputSnapshot.yaw, inputSnapshot.pitch,
                        inputSnapshot.viewParams)) {

                        // Get raw view poses for projection views.
                        // Use DISPLAY space when available: it is physically anchored to the
                        // display center and unaffected by recentering, which is the correct
                        XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
                        locateInfo.viewConfigurationType = xr->viewConfigType;
                        locateInfo.displayTime = frameState.predictedDisplayTime;
                        locateInfo.space = xr->localSpace;

                        XrViewState viewState = {XR_TYPE_VIEW_STATE};
                        uint32_t viewCount = 8;
                        XrView rawViews[8];
                        for (uint32_t vi = 0; vi < 8; vi++) rawViews[vi] = {XR_TYPE_VIEW};

                        // XR_EXT_view_rig (#396 W7): drive the runtime display
                        // rig with the app's tunables — the runtime owns the
                        // window resolve and the Kooima math, and returns
                        // render-ready XrView{pose, fov}. Per-locate semantics:
                        // chain the rig on every consume locate.
                        const bool useAppProjection =
                            xr->hasDisplayInfoExt && xr->displayWidthM > 0.0f && g_hasViewRigExt;
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
                        xrLocateViews(xr->session, &locateInfo, &viewState, 8, &viewCount, rawViews);

                        // Max per-tile capacity from swapchain
                        uint32_t maxTileW = tileColumns > 0 ? xr->swapchain.width / tileColumns : xr->swapchain.width;
                        uint32_t maxTileH = tileRows > 0 ? xr->swapchain.height / tileRows : xr->swapchain.height;

                        // Compute render dims: mono uses full swapchain, stereo uses tile size
                        uint32_t renderW, renderH;
                        if (monoMode) {
                            renderW = windowW;
                            renderH = windowH;
                            if (renderW > xr->swapchain.width) renderW = xr->swapchain.width;
                            if (renderH > xr->swapchain.height) renderH = xr->swapchain.height;
                        } else {
                            renderW = (uint32_t)(windowW * xr->recommendedViewScaleX);
                            renderH = (uint32_t)(windowH * xr->recommendedViewScaleY);
                            if (renderW > maxTileW) renderW = maxTileW;
                            if (renderH > maxTileH) renderH = maxTileH;
                        }

                        // --- Consume the runtime's render-ready XrView{pose, fov} ---
                        // Only clip policy (near/far) stays app-side, by design
                        // (fov is clip-independent). ZDP-anchored clip: near =
                        // ez - vH, far = ez + 1000·vH; ez = rig-local z of the
                        // view pose. GL keeps the [-1,1] clip-z — no remap.
                        std::vector<RigView> stereoViews(eyeCount);
                        if (useAppProjection) {
                            const float rigVH =
                                inputSnapshot.viewParams.virtualDisplayHeight / inputSnapshot.viewParams.scaleFactor;
                            for (int i = 0; i < eyeCount; i++) {
                                const XrView& v = rawViews[(i < (int)viewCount) ? i : 0];
                                ViewMatrixFromXrPose(v.pose, stereoViews[i].view_matrix);
                                float ez = RigLocalEyeZ(rigPose, v.pose.position);
                                float nearZ = (ez - rigVH > 0.001f) ? (ez - rigVH) : 0.001f;
                                float farZ = ez + 1000.0f * rigVH;
                                ProjectionFromXrFov(v.fov, nearZ, farZ, stereoViews[i].projection_matrix);
                                stereoViews[i].fov = v.fov;
                            }
                        }

                        rendered = true;

                        // For mono: compute center eye position and projection
                        XMMATRIX monoViewMatrix, monoProjMatrix;
                        XrFovf monoFov = {};
                        XrPosef monoPose = rawViews[0].pose;
                        if (monoMode) {
                            // Center eye = average of all view positions
                            XrVector3f center = {0.0f, 0.0f, 0.0f};
                            int cnt = (int)viewCount;
                            if (cnt < 1) cnt = 1;
                            for (int v = 0; v < cnt; v++) {
                                center.x += rawViews[v].pose.position.x;
                                center.y += rawViews[v].pose.position.y;
                                center.z += rawViews[v].pose.position.z;
                            }
                            monoPose.position.x = center.x / cnt;
                            monoPose.position.y = center.y / cnt;
                            monoPose.position.z = center.z / cnt;

                            // When useAppProjection, mono view+proj come from stereoViews[0]
                            if (!useAppProjection) {
                                monoProjMatrix = xr->projMatrices[0];
                                monoFov = rawViews[0].fov;

                                XMVECTOR centerLocalPos = XMVectorSet(
                                    monoPose.position.x, monoPose.position.y, monoPose.position.z, 0.0f);
                                XMVECTOR localOri = XMVectorSet(
                                    rawViews[0].pose.orientation.x, rawViews[0].pose.orientation.y,
                                    rawViews[0].pose.orientation.z, rawViews[0].pose.orientation.w);

                                float monoM2vView = 1.0f;
                                if (inputSnapshot.viewParams.virtualDisplayHeight > 0.0f && xr->displayHeightM > 0.0f)
                                    monoM2vView = inputSnapshot.viewParams.virtualDisplayHeight / xr->displayHeightM;
                                float eyeScale = inputSnapshot.viewParams.perspectiveFactor * monoM2vView / inputSnapshot.viewParams.scaleFactor;
                                XMVECTOR playerOri = XMQuaternionRotationRollPitchYaw(
                                    inputSnapshot.pitch, inputSnapshot.yaw, 0);
                                XMVECTOR playerPos = XMVectorSet(
                                    inputSnapshot.cameraPosX, inputSnapshot.cameraPosY,
                                    inputSnapshot.cameraPosZ, 0.0f);

                                XMVECTOR worldPos = XMVector3Rotate(centerLocalPos * eyeScale, playerOri) + playerPos;
                                XMVECTOR worldOri = XMQuaternionMultiply(localOri, playerOri);

                                XMMATRIX rot = XMMatrixTranspose(XMMatrixRotationQuaternion(worldOri));
                                XMFLOAT3 wp;
                                XMStoreFloat3(&wp, worldPos);
                                monoViewMatrix = XMMatrixTranslation(-wp.x, -wp.y, -wp.z) * rot;
                            }
                        }

                        uint32_t imageIndex;
                        if (AcquireSwapchainImage(*xr, imageIndex)) {
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
                                    viewMatrix = xr->viewMatrices[eye];
                                    projMatrix = xr->projMatrices[eye];
                                }

                                // Tile-aware viewport: place each view in the correct tile position
                                uint32_t tileX = monoMode ? 0 : (eye % tileColumns);
                                uint32_t tileY = monoMode ? 0 : (eye / tileColumns);
                                uint32_t vpX = tileX * renderW;
                                uint32_t vpY = tileY * renderH;

                                RenderScene(*renderer, imageIndex,
                                    vpX, vpY,
                                    renderW, renderH,
                                    viewMatrix, projMatrix,
                                    useAppProjection ? 1.0f : inputSnapshot.viewParams.scaleFactor);

                                projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                                projectionViews[eye].subImage.swapchain = xr->swapchain.swapchain;
                                projectionViews[eye].subImage.imageRect.offset = {
                                    (int32_t)vpX, (int32_t)vpY
                                };
                                projectionViews[eye].subImage.imageRect.extent = {
                                    (int32_t)renderW,
                                    (int32_t)renderH
                                };
                                projectionViews[eye].subImage.imageArrayIndex = 0;
                                projectionViews[eye].pose = monoMode ? monoPose : rawViews[eye < (int)viewCount ? eye : 0].pose;
                                projectionViews[eye].fov = useAppProjection ?
                                    stereoViews[monoMode ? 0 : eye].fov :
                                    (monoMode ? monoFov : rawViews[eye < (int)viewCount ? eye : 0].fov);
                            }

                            // 'I' key: snapshot the multi-view atlas. Skipped
                            // for mono (1×1) layouts.
                            if (inputSnapshot.captureAtlasRequested) {
                                {
                                    std::lock_guard<std::mutex> lk(g_inputMutex);
                                    g_inputState.captureAtlasRequested = false;
                                }
                                dxr_capture::RequestRuntimeAtlasCapture(
                                    *xr, APP_NAME, tileColumns, tileRows, hwnd);
                            }

                            ReleaseSwapchainImage(*xr);
                        } else {
                            rendered = false;
                        }

                        // Render HUD to window-space layer swapchain
                        if (rendered && inputSnapshot.hudVisible && hud && xr->hasHudSwapchain && hudSwapchainImages) {
                            uint32_t hudImageIndex;
                            if (AcquireHudSwapchainImage(*xr, hudImageIndex)) {
                                std::wstring sessionText(xr->systemName, xr->systemName + strlen(xr->systemName));
                                sessionText += L"\nSession: ";
                                sessionText += FormatSessionState((int)xr->sessionState);
                                std::wstring modeText = xr->hasWin32WindowBindingExt ?
                                    L"XR_EXT_win32_window_binding: ACTIVE (OpenGL)" :
                                    L"XR_EXT_win32_window_binding: NOT AVAILABLE (OpenGL)";
                                uint32_t dispMaxTileW = tileColumns > 0 ? xr->swapchain.width / tileColumns : xr->swapchain.width;
                                uint32_t dispMaxTileH = tileRows > 0 ? xr->swapchain.height / tileRows : xr->swapchain.height;
                                uint32_t dispRenderW, dispRenderH;
                                if (monoMode) {
                                    dispRenderW = windowW;
                                    dispRenderH = windowH;
                                    if (dispRenderW > xr->swapchain.width) dispRenderW = xr->swapchain.width;
                                    if (dispRenderH > xr->swapchain.height) dispRenderH = xr->swapchain.height;
                                } else {
                                    dispRenderW = (uint32_t)(windowW * xr->recommendedViewScaleX);
                                    dispRenderH = (uint32_t)(windowH * xr->recommendedViewScaleY);
                                    if (dispRenderW > dispMaxTileW) dispRenderW = dispMaxTileW;
                                    if (dispRenderH > dispMaxTileH) dispRenderH = dispMaxTileH;
                                }
                                std::wstring perfText = FormatPerformanceInfo(perfStats.fps, perfStats.frameTimeMs,
                                    dispRenderW, dispRenderH,
                                    windowW, windowH);
                                std::wstring dispText = FormatDisplayInfo(xr->displayWidthM, xr->displayHeightM,
                                    xr->nominalViewerX, xr->nominalViewerY, xr->nominalViewerZ);
                                dispText += L"\n" + FormatScaleInfo(xr->recommendedViewScaleX, xr->recommendedViewScaleY);
                                dispText += L"\n" + FormatMode(xr->currentModeIndex, xr->pfnRequestDisplayRenderingModeEXT != nullptr,
                                    (xr->renderingModeCount > 0 && xr->currentModeIndex < xr->renderingModeCount) ? xr->renderingModeNames[xr->currentModeIndex] : nullptr,
                                    xr->renderingModeCount,
                                    xr->renderingModeCount > 0 ? xr->renderingModeDisplay3D[xr->currentModeIndex] : true,
                                    xr->renderingModeCount > 0 ? xr->renderingModeIsRequestable[xr->currentModeIndex] : true);
                                std::wstring eyeText = FormatEyeTrackingInfo(
                                    xr->eyePositions, (uint32_t)eyeCount,
                                    xr->eyeTrackingActive, xr->isEyeTracking,
                                    xr->activeEyeTrackingMode, xr->supportedEyeTrackingModes);

                                float fwdX = -sinf(inputSnapshot.yaw) * cosf(inputSnapshot.pitch);
                                float fwdY =  sinf(inputSnapshot.pitch);
                                float fwdZ = -cosf(inputSnapshot.yaw) * cosf(inputSnapshot.pitch);
                                std::wstring cameraText = FormatCameraInfo(
                                    inputSnapshot.cameraPosX, inputSnapshot.cameraPosY, inputSnapshot.cameraPosZ,
                                    fwdX, fwdY, fwdZ);
                                float hudM2v = 1.0f;
                                if (inputSnapshot.viewParams.virtualDisplayHeight > 0.0f && xr->displayHeightM > 0.0f)
                                    hudM2v = inputSnapshot.viewParams.virtualDisplayHeight / xr->displayHeightM;
                                std::wstring stereoText = FormatViewParams(
                                    inputSnapshot.viewParams.ipdFactor, inputSnapshot.viewParams.parallaxFactor,
                                    inputSnapshot.viewParams.perspectiveFactor, inputSnapshot.viewParams.scaleFactor);
                                {
                                    wchar_t vhBuf[64];
                                    swprintf(vhBuf, 64, L"\nvHeight: %.3f  m2v: %.3f",
                                        inputSnapshot.viewParams.virtualDisplayHeight, hudM2v);
                                    stereoText += vhBuf;
                                }
                                std::wstring helpText = FormatHelpText(xr->pfnRequestDisplayRenderingModeEXT != nullptr, false, xr->renderingModeCount);

                                uint32_t srcRowPitch = 0;
                                const void* pixels = RenderHudAndMap(*hud, &srcRowPitch,
                                    sessionText, modeText, perfText, dispText, eyeText,
                                    cameraText, stereoText, helpText);
                                bool uploadOk = false;
                                if (pixels) {
                                    // Clear any prior GL errors
                                    while (glGetError() != GL_NO_ERROR) {}

                                    GLuint hudTexId = (*hudSwapchainImages)[hudImageIndex].image;
                                    glBindTexture(GL_TEXTURE_2D, hudTexId);
                                    // GL native compositor's window-space vertex shader already
                                    // flips UV.y because it expects HUD pixels in top-down
                                    // (D2D/CG bitmap) order — match the macOS GL cube and
                                    // upload straight without a manual row-flip.
                                    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                                    if (srcRowPitch == hudWidth * 4) {
                                        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, hudWidth, hudHeight,
                                            GL_RGBA, GL_UNSIGNED_BYTE, pixels);
                                    } else {
                                        const uint8_t* src = (const uint8_t*)pixels;
                                        for (uint32_t row = 0; row < hudHeight; row++) {
                                            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row, hudWidth, 1,
                                                GL_RGBA, GL_UNSIGNED_BYTE,
                                                src + row * srcRowPitch);
                                        }
                                    }
                                    // Force GL to flush and check for errors
                                    glFlush();
                                    GLenum glErr = glGetError();
                                    glBindTexture(GL_TEXTURE_2D, 0);
                                    UnmapHud(*hud);

                                    if (glErr != GL_NO_ERROR) {
                                        LOG_WARN("[HUD] glTexSubImage2D error 0x%X on HUD swapchain texture %u — skipping HUD layer",
                                            glErr, hudTexId);
                                    } else {
                                        uploadOk = true;
                                    }
                                }

                                bool releaseOk = ReleaseHudSwapchainImage(*xr);
                                if (!releaseOk) {
                                    LOG_WARN("[HUD] ReleaseHudSwapchainImage failed — skipping HUD layer");
                                }
                                hudSubmitted = uploadOk && releaseOk;
                            }
                        }
                    }
                }

                // #439 cases 2/3/4 activation: create + fill the panel(s) a
                // few frames in (GL context current on this thread).
                if (g_l2dPanel && !g_l2dActive && g_l2dFrameCounter >= g_l2dActivationFrame) {
                    static bool attempted = false;
                    if (!attempted) {
                        attempted = true;
                        uint32_t winW = g_windowWidth;
                        uint32_t winH = g_windowHeight;
                        uint32_t pw = winW * 3 / 8;
                        uint32_t ph = winH * 5 / 16;
                        // #491 validation aid: DXR_LOCAL2D_OVERCUBE centers the
                        // panel over the cube + uses the diagonal-stripes variant
                        // (clearest read of glass-over-3D).
                        const char* oc = getenv("DXR_LOCAL2D_OVERCUBE");
                        bool overCube = (oc && *oc == '1');
                        int p1variant = overCube ? 1 : 0;
                        if (overCube) {
                            g_panel1Rect.offset = {(int32_t)(winW / 2 - pw / 2), (int32_t)(winH / 2 - ph / 2)};
                        } else {
                            g_panel1Rect.offset = {(int32_t)(winW / 16), (int32_t)(winH * 9 / 16)};
                        }
                        g_panel1Rect.extent = {(int32_t)pw, (int32_t)ph};
                        bool ok = CreateAndFillL2DPanel(*xr, pw, ph, p1variant, g_panel1);

                        // #491 part 3 — large backdrop submitted BEFORE the projection
                        // (a 2D-under layer): the flat 2D plane the cube floats in front of.
                        if (ok && g_l2dBackdropVariant != 0) {
                            uint32_t bw = winW * 3 / 4;
                            uint32_t bh = winH * 3 / 4;
                            g_backdropRect.offset = {(int32_t)(winW / 2 - bw / 2), (int32_t)(winH / 2 - bh / 2)};
                            g_backdropRect.extent = {(int32_t)bw, (int32_t)bh};
                            ok = CreateAndFillL2DPanel(*xr, bw, bh, g_l2dBackdropVariant, g_backdrop);
                        }

                        if (ok && g_l2dPanel2) {
                            g_panel2Rect.offset = {g_panel1Rect.offset.x + (int32_t)(pw / 2),
                                                   g_panel1Rect.offset.y - (int32_t)(ph / 4)};
                            g_panel2Rect.extent = {(int32_t)pw, (int32_t)ph};
                            ok = CreateAndFillL2DPanel(*xr, pw, ph, 1, g_panel2);
                        }

                        if (ok && g_l2dMask && g_zone.available && g_zone.pfnCreate && g_zone.pfnSetRects &&
                            g_zone.pfnSubmit) {
                            XrLocal3DZoneMaskCreateInfoEXT mci = {
                                (XrStructureType)XR_TYPE_LOCAL_3D_ZONE_MASK_CREATE_INFO_EXT};
                            mci.maskWidth = 0;
                            mci.maskHeight = 0;
                            ok = XR_SUCCEEDED(g_zone.pfnCreate(xr->session, &mci, &g_zone.mask));
                            if (ok) {
                                XrRect2Di islands[2];
                                islands[0].offset = {(int32_t)(winW * 7 / 16), (int32_t)(winH / 4)};
                                islands[0].extent = {(int32_t)(winW * 7 / 16), (int32_t)(winH / 2)};
                                islands[1].offset = {(int32_t)(winW / 16), (int32_t)(winH / 16)};
                                islands[1].extent = {(int32_t)(winW / 4), (int32_t)(winH / 4)};
                                ok = XR_SUCCEEDED(g_zone.pfnSetRects(g_zone.mask, 2, islands)) &&
                                     XR_SUCCEEDED(g_zone.pfnSubmit(g_zone.mask));
                            }
                        }

                        if (ok) {
                            g_l2dActive = true;
                            LOG_INFO("Local2D panels active: panel1 %d,%d %ux%u%s%s",
                                     g_panel1Rect.offset.x, g_panel1Rect.offset.y, pw, ph,
                                     g_l2dPanel2 ? " + panel2 (unpremultiplied, overlapping)" : "",
                                     g_l2dMask ? " + explicit Tier-2 island mask" : " (implicit mask)");
                        } else {
                            LOG_ERROR("Local2D panel activation failed");
                        }
                    }
                }

                // submitViewCount = eyeCount (mono=1, stereo=N)
                uint32_t submitViewCount = (uint32_t)eyeCount;
                if (g_l2dActive && g_panel1.swapchain != XR_NULL_HANDLE) {
                    // #439 cases 2/3/4: manual projection + Local2D layer list
                    // (the shared EndFrame helpers don't carry the Local2D type).
                    XrCompositionLayerProjection projLayer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
                    projLayer.space = xr->localSpace;
                    projLayer.viewCount = (uint32_t)eyeCount;
                    projLayer.views = projectionViews.data();

                    XrCompositionLayerLocal2DEXT panel1Layer = {
                        (XrStructureType)XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_EXT};
                    XrCompositionLayerLocal2DEXT panel2Layer = {
                        (XrStructureType)XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_EXT};
                    XrCompositionLayerLocal2DEXT backdropLayer = {
                        (XrStructureType)XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_EXT};
                    const XrCompositionLayerBaseHeader* layers[4] = {nullptr, nullptr, nullptr, nullptr};
                    uint32_t layerCount = 0;

                    // #491 part 3 — backdrop BEFORE the projection (a 2D-under layer).
                    if (g_l2dBackdropVariant != 0 && g_backdrop.swapchain != XR_NULL_HANDLE) {
                        backdropLayer.layerFlags = 0; // premultiplied bytes
                        backdropLayer.subImage.swapchain = g_backdrop.swapchain;
                        backdropLayer.subImage.imageRect.offset = {0, 0};
                        backdropLayer.subImage.imageRect.extent = {(int32_t)g_backdrop.w, (int32_t)g_backdrop.h};
                        backdropLayer.subImage.imageArrayIndex = 0;
                        backdropLayer.rect = g_backdropRect;
                        layers[layerCount++] = (XrCompositionLayerBaseHeader*)&backdropLayer;
                    }

                    layers[layerCount++] = (XrCompositionLayerBaseHeader*)&projLayer;

                    panel1Layer.layerFlags = 0; // premultiplied bytes
                    panel1Layer.subImage.swapchain = g_panel1.swapchain;
                    panel1Layer.subImage.imageRect.offset = {0, 0};
                    panel1Layer.subImage.imageRect.extent = {(int32_t)g_panel1.w, (int32_t)g_panel1.h};
                    panel1Layer.subImage.imageArrayIndex = 0;
                    panel1Layer.rect = g_panel1Rect;
                    layers[layerCount++] = (XrCompositionLayerBaseHeader*)&panel1Layer;

                    if (g_l2dPanel2 && g_panel2.swapchain != XR_NULL_HANDLE) {
                        panel2Layer.layerFlags = XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
                        panel2Layer.subImage.swapchain = g_panel2.swapchain;
                        panel2Layer.subImage.imageRect.offset = {0, 0};
                        panel2Layer.subImage.imageRect.extent = {(int32_t)g_panel2.w, (int32_t)g_panel2.h};
                        panel2Layer.subImage.imageArrayIndex = 0;
                        panel2Layer.rect = g_panel2Rect;
                        layers[layerCount++] = (XrCompositionLayerBaseHeader*)&panel2Layer;
                    }

                    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
                    endInfo.displayTime = frameState.predictedDisplayTime;
                    endInfo.environmentBlendMode = xr->runtimeSupportsAlphaBlend
                        ? XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND
                        : XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                    endInfo.layerCount = layerCount;
                    endInfo.layers = layers;
                    xrEndFrame(xr->session, &endInfo);
                } else if (hudSubmitted) {
                    LOG_DEBUG("[Frame] Submitting EndFrame with HUD (layerCount=2)");
                    float hudAR = (float)hudWidth / (float)hudHeight;
                    float windowAR = (windowW > 0 && windowH > 0) ? (float)windowW / (float)windowH : 1.0f;
                    float fracW = HUD_WIDTH_FRACTION;
                    float fracH = fracW * windowAR / hudAR;
                    if (fracH > 1.0f) { fracH = 1.0f; fracW = hudAR / windowAR; }
                    if (!EndFrameWithWindowSpaceHud(*xr, frameState.predictedDisplayTime, projectionViews.data(),
                        0.0f, 0.0f, fracW, fracH, 0.0f, submitViewCount)) {
                        LOG_WARN("[Frame] EndFrameWithWindowSpaceHud FAILED — disabling HUD for this session");
                        hud = nullptr;  // Disable HUD for subsequent frames
                    }
                    LOG_DEBUG("[Frame] EndFrame with HUD returned");
                } else {
                    EndFrame(*xr, frameState.predictedDisplayTime, projectionViews.data(), submitViewCount);
                }
                g_l2dFrameCounter++;
            }
        } else {
            Sleep(100);
        }
    }

    // Release GL context from this thread
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

    LOG_INFO("=== SR Cube OpenXR Ext OpenGL Application ===");

    // #439 Phase 3 — handle + mask + Local2D layer modes (§8 cases 2/3/4).
    {
        const char* e = getenv("DXR_LOCAL2D_PANEL");
        if (e && *e == '1') g_l2dPanel = true;
        e = getenv("DXR_LOCAL2D_MASK");
        if (e && *e == '1') g_l2dMask = true;
        e = getenv("DXR_LOCAL2D_PANEL2");
        if (e && *e == '1') g_l2dPanel2 = true;
        // #491 part 3 — DXR_LOCAL2D_BACKDROP=1 ⟹ opaque (variant 2); =2 ⟹
        // semi-transparent (variant 3, desktop shows through). Implies the panel path.
        e = getenv("DXR_LOCAL2D_BACKDROP");
        if (e && (*e == '1' || *e == '2')) {
            g_l2dBackdropVariant = (*e == '2') ? 3 : 2;
            g_l2dPanel = true;
        }
        if (g_l2dPanel) {
            LOG_INFO("DXR_LOCAL2D_PANEL=1 — Local2D panel layer%s%s%s",
                g_l2dPanel2 ? " + panel2 (unpremultiplied, overlapping)" : "",
                g_l2dMask ? " + explicit Tier-2 island mask" : " (implicit mask)",
                g_l2dBackdropVariant == 2 ? " + opaque 2D-under backdrop" :
                g_l2dBackdropVariant == 3 ? " + semi-transparent 2D-under backdrop" : "");
        }
    }

    HWND hwnd = CreateAppWindow(hInstance, g_windowWidth, g_windowHeight);
    if (!hwnd) {
        LOG_ERROR("Failed to create window");
        ShutdownLogging();
        return 1;
    }

    // Create OpenGL context (temp → core profile 3.3)
    HDC hDC = nullptr;
    HGLRC hGLRC = nullptr;
    if (!CreateOpenGLContext(hwnd, hDC, hGLRC)) {
        LOG_ERROR("OpenGL context creation failed");
        ShutdownLogging();
        return 1;
    }

    // Load GL function pointers (context must be current)
    if (!LoadGLFunctions()) {
        LOG_ERROR("Failed to load GL function pointers");
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(hGLRC);
        ShutdownLogging();
        return 1;
    }

    // Initialize OpenXR (must happen after GL context is current for requirements query)
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

    // Get OpenGL graphics requirements
    if (!GetOpenGLGraphicsRequirements(xr)) {
        LOG_ERROR("Failed to get OpenGL graphics requirements");
        CleanupOpenXR(xr);
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(hGLRC);
        ShutdownLogging();
        return 1;
    }

    // Create session with GL context + window handle
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

    if (!CreateSwapchain(xr)) {
        LOG_ERROR("Swapchain creation failed");
        CleanupOpenXR(xr);
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(hGLRC);
        ShutdownLogging();
        return 1;
    }

    // Enumerate OpenGL swapchain images (single SBS swapchain)
    std::vector<XrSwapchainImageOpenGLKHR> swapchainImages;
    {
        uint32_t count = xr.swapchain.imageCount;
        swapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
        xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)swapchainImages.data());
        LOG_INFO("Enumerated %u OpenGL swapchain images", count);
    }

    // Initialize GL renderer (shaders, geometry)
    GLRenderer glRenderer = {};
    if (!InitializeGLRenderer(glRenderer)) {
        LOG_ERROR("GL renderer initialization failed");
        CleanupOpenXR(xr);
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(hGLRC);
        ShutdownLogging();
        return 1;
    }

    // Create FBOs for swapchain images (single SBS swapchain)
    {
        uint32_t count = xr.swapchain.imageCount;
        std::vector<GLuint> textures(count);
        for (uint32_t i = 0; i < count; i++) {
            textures[i] = swapchainImages[i].image;
        }

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

    // Initialize HUD renderer for window-space layer overlay
    uint32_t hudWidth = (uint32_t)(xr.swapchain.width * HUD_WIDTH_FRACTION);
    uint32_t hudHeight = (uint32_t)(xr.swapchain.height * HUD_HEIGHT_FRACTION);

    HudRenderer hudRenderer = {};
    bool hudOk = InitializeHudRenderer(hudRenderer, hudWidth, hudHeight);
    if (!hudOk) {
        LOG_WARN("HUD renderer init failed - HUD will not be displayed");
    }

    // Create HUD swapchain for window-space layer submission
    std::vector<XrSwapchainImageOpenGLKHR> hudSwapImages;
    if (hudOk) {
        if (CreateHudSwapchain(xr, hudWidth, hudHeight)) {
            uint32_t count = xr.hudSwapchain.imageCount;
            hudSwapImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
            xrEnumerateSwapchainImages(xr.hudSwapchain.swapchain, count, &count,
                (XrSwapchainImageBaseHeader*)hudSwapImages.data());
            LOG_INFO("HUD swapchain: enumerated %u OpenGL images", count);
        } else {
            LOG_WARN("HUD swapchain creation failed - HUD will not be displayed");
            hudOk = false;
        }
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    LOG_INFO("");
    LOG_INFO("=== Entering main loop ===");
    LOG_INFO("Controls: WASD=Fly, QE=Up/Down, Mouse=Look, Space/DblClick=Reset, V=Mode, SHIFT+TAB=HUD, F11=Fullscreen, ESC=Quit");
    LOG_INFO("");

    // Release GL context from main thread before handing to render thread
    wglMakeCurrent(nullptr, nullptr);

    // Set virtual display height (app units). 0.24 = 4x the 0.06m cube height.
    g_inputState.viewParams.virtualDisplayHeight = 0.24f;
    g_inputState.renderingModeCount = xr.renderingModeCount;

    std::thread renderThread(RenderThreadFunc, hwnd, hDC, hGLRC, &xr, &glRenderer,
        &swapchainImages,
        hudOk ? &hudRenderer : nullptr, hudWidth, hudHeight,
        hudOk ? &hudSwapImages : nullptr);

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

    // Re-acquire GL context for cleanup
    wglMakeCurrent(hDC, hGLRC);

    if (hudOk) CleanupHudRenderer(hudRenderer);
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
