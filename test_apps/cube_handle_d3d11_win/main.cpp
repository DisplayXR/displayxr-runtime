// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  SR Cube OpenXR Ext - OpenXR with XR_EXT_win32_window_binding extension
 *
 * This application demonstrates OpenXR with the XR_EXT_win32_window_binding extension.
 * The application creates and controls its own window for rendering.
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <wrl/client.h>

#include "logging.h"
#include "input_handler.h"
#include "d3d11_renderer.h"
#include "text_overlay.h"
#include "hud_renderer.h"
#include "xr_session.h"
#include "projection_depth.h"
#include "atlas_capture.h"

#include <chrono>
#include <cmath>
#include <string>
#include <sstream>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

// Application name for logging
static const char* APP_NAME = "cube_handle_d3d11_win";

// Window settings
static const wchar_t* WINDOW_CLASS = L"SRCubeOpenXRExtClass";
static const wchar_t* WINDOW_TITLE = L"D3D11 Cube \u2014 D3D11 Native Compositor (External Window)";

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

// XR_EXT_view_rig (#396 W7 dogfood): the app chains a rig descriptor on every
// xrLocateViews and consumes the runtime's render-ready XrView{pose, fov}
// directly — the per-frame Kooima generation (display3d_resolve_window_rect +
// *_compute_views) is deleted; only clip policy stays app-side. Per-view
// staging container for the consumed views (matrices column-major):
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
// eyeWorld). The display rig places the virtual display plane at the rig
// pose, so the eye's distance to that plane is the rig-local z — NOT
// pose.position.z once the player flies (non-identity rig pose). Degenerates
// to pose.position.z at identity.
static float RigLocalEyeZ(const XrPosef& rig, const XrVector3f& eyeWorld) {
    const float dx = eyeWorld.x - rig.position.x;
    const float dy = eyeWorld.y - rig.position.y;
    const float dz = eyeWorld.z - rig.position.z;
    // Rotate the delta by the conjugate quaternion (inverse rotation).
    const float qx = -rig.orientation.x, qy = -rig.orientation.y;
    const float qz = -rig.orientation.z, qw = rig.orientation.w;
    // v' = v + 2*q_vec x (q_vec x v + qw*v); we only need the z component.
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
        // freeze rendering on this single-threaded app.  The ALT key state is
        // still readable via GetKeyState() for our input handler.
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
        // Render thread requested a capture-flash; start it on this thread
        // (the message-pump thread that owns the HWND).
        dxr_capture::TriggerCaptureFlash(hwnd);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// DISPLAYXR_TRANSPARENT_BG=1 → cube clears RGBA(0,0,0,0), window uses
// WS_EX_NOREDIRECTIONBITMAP + null brush so DComp can show the desktop
// through the cube's transparent regions. Mirrors cube_handle_vk_win.
static bool TransparentBackgroundEnabled() {
    static const bool e = []() {
        const char *v = getenv("DISPLAYXR_TRANSPARENT_BG");
        return v != nullptr && *v != '\0' && *v != '0';
    }();
    return e;
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
    // black under the DComp composition swap chain.
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
    HWND hwnd = CreateWindowEx(
        exStyle,
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
    // Single source of truth: the runtime owns the current mode. Keypresses
    // are REQUESTS — we call xrRequestDisplayRenderingModeEXT and let the
    // XrEventDataRenderingModeChangedEXT event update xr.currentModeIndex.
    // Render paths and HUD read xr.currentModeIndex directly.
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

    // #228 Tier 1 smoke test: 'B' fires xrRequestFilePickerEXT and prints
    // the immediate return code. The completion event (success/cancel +
    // path) lands later through PollEvents → XR_TYPE_EVENT_DATA_FILE_PICKER_COMPLETE_EXT.
    //
    // ------------------------------------------------------------------
    // CI / headless smoke-test hook for #228.
    // ------------------------------------------------------------------
    // Setting `DISPLAYXR_AUTOFIRE_FILE_PICKER=1` in this app's
    // environment causes ONE xrRequestFilePickerEXT call to fire
    // automatically ~300 frames after the session enters running
    // state (≈5 s at 60 Hz), without any keypress. The flag is reset
    // after the first fire — there is no per-frame call cost.
    //
    // Why this exists: PostMessage(WM_KEYDOWN, 'B') from an external
    // PowerShell test driver against the cube's hidden-under-workspace
    // HWND has been flaky in practice (the keypress can arrive at the
    // exact instant the IPC pipe transitions to broken on some runs,
    // racing a session-lost teardown). The auto-fire path bypasses
    // Win32 input entirely so headless CI can verify the end-to-end
    // OpenXR contract regardless of input plumbing.
    //
    // Off-by-default, gated on an env var so a normal user double-
    // clicking the cube never gets a spurious picker request.
    {
        static int s_autofire_frames = 0;
        static bool s_autofire_done = false;
        if (!s_autofire_done && xr.sessionRunning) {
            s_autofire_frames++;
            if (s_autofire_frames > 300) {
                const char *autofire = getenv("DISPLAYXR_AUTOFIRE_FILE_PICKER");
                if (autofire && autofire[0] == '1') {
                    LOG_INFO("[#228] AUTO-FIRE: setting filePickerRequestRequested");
                    g_inputState.filePickerRequestRequested = true;
                }
                s_autofire_done = true;
            }
        }
    }
    if (g_inputState.filePickerRequestRequested) {
        g_inputState.filePickerRequestRequested = false;
        if (xr.pfnRequestFilePickerEXT && xr.session != XR_NULL_HANDLE) {
            XrFilePickerInfoEXT info = {XR_TYPE_FILE_PICKER_INFO_EXT};
            info.next = nullptr;
            info.mode = XR_FILE_PICKER_MODE_OPEN_EXT;
            info.flags = XR_FILE_PICKER_FLAG_NONE_EXT;
            strncpy_s(info.title, "Smoke test (#228)", _TRUNCATE);
            strncpy_s(info.defaultPath, "C:\\", _TRUNCATE);
            info.filterCount = 1;
            strncpy_s(info.filters[0].description, "Images", _TRUNCATE);
            strncpy_s(info.filters[0].extensions, "*.png;*.jpg", _TRUNCATE);
            XrAsyncRequestIdEXT rid = XR_NULL_ASYNC_REQUEST_ID_EXT;
            XrResult fr = xr.pfnRequestFilePickerEXT(xr.session, &info, &rid);
            LOG_INFO("[#228] xrRequestFilePickerEXT -> rc=0x%x requestId=%llu",
                (unsigned)fr, (unsigned long long)rid);
            if (XR_SUCCEEDED(fr)) {
                // Feed the shared PollEvents completion state machine
                // (displayxr::common) so the completion event matches the
                // live request instead of logging a stale-id warning.
                xr.filePickerRequestId = rid;
                xr.filePickerInFlight = true;
                xr.filePickerHasResult = false;
            }
        } else {
            LOG_INFO("[#228] xrRequestFilePickerEXT not available (ext missing or PFN NULL)");
        }
    }

    // Handle eye tracking mode toggle (T key)
    if (g_inputState.eyeTrackingModeToggleRequested) {
        g_inputState.eyeTrackingModeToggleRequested = false;
        if (xr.pfnRequestEyeTrackingModeEXT && xr.session != XR_NULL_HANDLE) {
            XrEyeTrackingModeEXT newMode = (xr.activeEyeTrackingMode == XR_EYE_TRACKING_MODE_MANAGED_EXT)
                ? XR_EYE_TRACKING_MODE_MANUAL_EXT : XR_EYE_TRACKING_MODE_MANAGED_EXT;
            XrResult etResult = xr.pfnRequestEyeTrackingModeEXT(xr.session, newMode);
            LOG_INFO("Eye tracking mode -> %s (%s)",
                newMode == XR_EYE_TRACKING_MODE_MANUAL_EXT ? "MANUAL" : "MANAGED",
                XR_SUCCEEDED(etResult) ? "OK" : "unsupported");
        }
    }

    // Update scene (cube rotation) — speed agent-settable via cube-d3d11__set_spin (#457)
    UpdateScene(renderer, rs.perfStats->deltaTime, xr.spinSpeed);

    // Poll OpenXR events
    PollEvents(xr);

    // Only render if session is running
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

                    // Get raw view poses (pre-player-transform) for projection views.
                    XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
                    locateInfo.viewConfigurationType = xr.viewConfigType;
                    locateInfo.displayTime = frameState.predictedDisplayTime;
                    locateInfo.space = xr.localSpace;

                    XrViewState viewState = {XR_TYPE_VIEW_STATE};
                    uint32_t viewCount = 8;
                    XrView rawViews[8];
                    for (uint32_t vi = 0; vi < 8; vi++) rawViews[vi] = {XR_TYPE_VIEW};

                    // XR_EXT_view_rig (#396 W7): drive the runtime rig matching
                    // the app's current mode (C selects the rig) with the app's
                    // tunables — the runtime owns the window/canvas resolve and
                    // the Kooima math, and returns render-ready XrView{pose, fov}.
                    // Per-locate semantics: the rig must be chained on every
                    // consume locate.
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

                    // Max per-tile capacity from swapchain
                    uint32_t maxTileW = tileColumns > 0 ? xr.swapchain.width / tileColumns : xr.swapchain.width;
                    uint32_t maxTileH = tileRows > 0 ? xr.swapchain.height / tileRows : xr.swapchain.height;

                    std::vector<RigView> stereoViews(eyeCount);
                    if (useAppProjection) {
                        // Consume the runtime's render-ready XrView{pose, fov}.
                        // Only clip policy (near/far + the GL→[0,1] depth
                        // remap) stays app-side, by design (fov is
                        // clip-independent). Camera rig: same absolute clip as
                        // the old app-side camera path. Display rig: the
                        // ZDP-anchored clip (near = ez - vH, far = ez +
                        // 1000·vH; ez = eye distance to the virtual display
                        // plane = rig-local z of the view pose).
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

                    // [Commented out — will be reused for 3D-positioned HUD later]
                    // ConvergencePlane convPlane = LocateConvergencePlane(rawViews);

                    // Render HUD to window-space layer swapchain (once per frame, before eye loop)
                    if (g_inputState.hudVisible && xr.hasHudSwapchain && rs.hudSwapchainImages && !rs.hudSwapchainImages->empty() && rs.hudOk) {
                        uint32_t hudImageIndex;
                        if (AcquireHudSwapchainImage(xr, hudImageIndex)) {
                            std::wstring sessionText(xr.systemName, xr.systemName + strlen(xr.systemName));
                            sessionText += L"\nSession: ";
                            sessionText += FormatSessionState((int)xr.sessionState);
                            std::wstring modeText = xr.hasWin32WindowBindingExt ?
                                L"XR_EXT_win32_window_binding: ACTIVE (D3D11)" :
                                L"XR_EXT_win32_window_binding: NOT AVAILABLE (D3D11)";
                            modeText += g_inputState.cameraMode ?
                                L"\nKooima: Camera-Centric [C=Toggle]" :
                                L"\nKooima: Display-Centric [C=Toggle]";
                            modeText += g_hasViewRigExt ?
                                (g_inputState.cameraMode ?
                                    L"\nView rig: RUNTIME camera rig (XR_EXT_view_rig)" :
                                    L"\nView rig: RUNTIME display rig (XR_EXT_view_rig)") :
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

                    // For mono: compute center eye position and projection
                    XMMATRIX monoViewMatrix, monoProjMatrix;
                    XrFovf monoFov = {};
                    XrPosef monoPose = rawViews[0].pose;
                    if (monoMode) {
                        // Center eye = average of all view positions
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

                        // When useAppProjection, mono view+proj come from stereoViews[0]
                        // (the runtime centers the rig view poses in 2D mode).
                        // Only need fallback when !useAppProjection.
                        if (!useAppProjection) {
                            monoProjMatrix = xr.projMatrices[0];  // Close enough for 2D
                            monoFov = rawViews[0].fov;

                            // Build center-eye view matrix from scratch (same as LocateViews)
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

                        // Clear entire color+depth once before eye loop.
                        // Transparent mode (DISPLAYXR_TRANSPARENT_BG=1): clear to
                        // RGBA(0,0,0,0) so the Leia DP's compose-under-bg pass
                        // shows the desktop wherever the cube didn't draw.
                        float clearColor[4];
                        if (TransparentBackgroundEnabled()) {
                            clearColor[0] = 0.0f; clearColor[1] = 0.0f;
                            clearColor[2] = 0.0f; clearColor[3] = 0.0f;
                        } else {
                            clearColor[0] = 0.05f; clearColor[1] = 0.05f;
                            clearColor[2] = 0.25f; clearColor[3] = 1.0f;
                        }
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
                                viewMatrix = ColumnMajorToXMMatrix(stereoViews[vi].view_matrix);
                                projMatrix = ColumnMajorToXMMatrix(stereoViews[vi].projection_matrix);
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
                                stereoViews[monoMode ? 0 : eye].fov :
                                (monoMode ? monoFov : rawViews[safeIdx].fov);
                        }

                        if (rtv) rtv->Release();

                        // 'I' key: snapshot the multi-view atlas to a PNG via the
                        // runtime (XR_EXT_atlas_capture). Skipped for mono (1×1).
                        if (g_inputState.captureAtlasRequested) {
                            g_inputState.captureAtlasRequested = false;
                            dxr_capture::RequestRuntimeAtlasCapture(
                                xr, APP_NAME, tileColumns, tileRows, rs.hwnd);
                        }

                        ReleaseSwapchainImage(xr);
                    }
                }
            }

            // Submit frame with window-space HUD layer if visible
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
    } else {
        Sleep(100);
    }
}

// Main entry point
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    // Initialize logging
    if (!InitializeLogging(APP_NAME)) {
        MessageBox(nullptr, L"Failed to initialize logging", L"Warning", MB_OK | MB_ICONWARNING);
    }

    LOG_INFO("=== SR Cube OpenXR Ext Application ===");
    LOG_INFO("OpenXR with XR_EXT_win32_window_binding extension");
    LOG_INFO("Application creates and controls its own window");

    // Create window FIRST (needed for XR_EXT_win32_window_binding)
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

    // Check for session target extension
    if (!xr.hasWin32WindowBindingExt) {
        LOG_WARN("XR_EXT_win32_window_binding not available - runtime will create its own window");
        MessageBox(hwnd, L"XR_EXT_win32_window_binding extension not available.\nRuntime will create its own window.",
            L"Warning", MB_OK | MB_ICONWARNING);
    } else {
        LOG_INFO("XR_EXT_win32_window_binding extension is available - using app window");
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

    // Create OpenXR session WITH window handle (XR_EXT_win32_window_binding)
    LOG_INFO("Creating OpenXR session with XR_EXT_win32_window_binding (HWND: 0x%p)...", hwnd);
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

    // Create single swapchain at native display resolution
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

    // Create HUD swapchain for window-space layer submission
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

    // Create single depth buffer at full swapchain dimensions
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
    LOG_INFO("XR rendering happens in the application window (XR_EXT_win32_window_binding)");
    LOG_INFO("Single-threaded: message pump + render on the main thread (WM_PAINT during drag/resize)");
    LOG_INFO("Controls: WASD=Fly, QE=Up/Down, Mouse=Look, Space/DblClick=Reset, P=Parallax, V=Mode, SHIFT+TAB=HUD, F11=Fullscreen, ESC=Quit");
    LOG_INFO("");

    PerformanceStats perfStats = {};
    perfStats.lastTime = std::chrono::high_resolution_clock::now();

    // Set virtual display height (app units). 0.24 = 4x the 0.06m cube height.
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
    g_renderState = &rs;

    // Single-threaded main loop: pump messages, then render one frame.
    // During drag/resize, DefWindowProc enters a modal loop that blocks
    // PeekMessage — WM_PAINT fires inside that modal loop to keep rendering.
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
