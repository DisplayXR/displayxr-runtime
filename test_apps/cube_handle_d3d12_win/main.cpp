// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  SR Cube OpenXR Ext D3D12 - OpenXR with XR_EXT_win32_window_binding (D3D12)
 *
 * D3D12 port of cube_handle_d3d11 with window-space HUD overlay.
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>

#include "logging.h"
#include "input_handler.h"
#include "xr_session.h"
#include "d3d12_renderer.h"
#include "hud_renderer.h"
#include "text_overlay.h"
#include "projection_depth.h"
#include "atlas_capture.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <wrl/client.h>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

static const char* APP_NAME = "cube_handle_d3d12_win";

// HUD overlay: WIDTH_FRACTION anchors how wide the HUD appears on screen.
// D3D12 swapchains are imported into the Vulkan native compositor via shared handles.
// Non-power-of-2 dimensions cause a size mismatch between D3D12 and Vulkan memory layouts
// (nvidia bug - see comp_d3d12_client.cpp). Use power-of-2 to avoid this.
static const uint32_t HUD_PIXEL_WIDTH = 512;
static const uint32_t HUD_PIXEL_HEIGHT = 560;
static const float HUD_WIDTH_FRACTION = 0.30f;

static const wchar_t* WINDOW_CLASS = L"SRCubeOpenXRExtD3D12Class";
static const wchar_t* WINDOW_TITLE = L"D3D12 Cube \u2014 D3D12 Native Compositor (External Window)";

// Global state (shared between main thread and render thread)
static InputState g_inputState;
static std::mutex g_inputMutex;
static std::atomic<bool> g_running{true};
static XrSessionManager* g_xr = nullptr;
static UINT g_windowWidth = 1280;
static UINT g_windowHeight = 720;

// Fullscreen state
static bool g_fullscreen = false;
static RECT g_savedWindowRect = {};
static DWORD g_savedWindowStyle = 0;

// #439 Phase 3 — handle + mask + Local2D layer modes (§8 cases 2/3/4), D3D12 leg.
// DXR_LOCAL2D_PANEL=1  — submit a Local2D panel layer (case 3: layer-only,
//                        IMPLICIT mask from the panel rect, zero mask calls).
// DXR_LOCAL2D_MASK=1   — additionally create + submit an explicit Tier-2 mask
//                        with 3D island rects (case 2: handle + mask + layer —
//                        islands weave, panel crisp, desktop where neither
//                        covers).
// DXR_LOCAL2D_PANEL2=1 — additionally submit a second, overlapping panel with
//                        XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT (case 4:
//                        list-order stacking + alpha fringing).
static bool g_l2dPanel = false;
static bool g_l2dMask = false;
static bool g_l2dPanel2 = false;
static bool g_l2dActive = false; // set once panels (+ optional mask) are live
static long g_l2dFrameCounter = 0;
static const long g_l2dActivationFrame = 10;

struct L2DPanel {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    uint32_t w = 0, h = 0;
};
static L2DPanel g_panel1, g_panel2;
static XrRect2Di g_panel1Rect, g_panel2Rect;

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

// DISPLAYXR_TRANSPARENT_BG=1 → cube clears RGBA(0,0,0,0), window uses
// WS_EX_NOREDIRECTIONBITMAP + null brush so DComp can show the desktop
// through the cube's transparent regions. Mirrors cube_handle_vk_win and
// cube_handle_d3d11_win.
static bool TransparentBackgroundEnabled() {
    static const bool e = []() {
        const char *v = getenv("DISPLAYXR_TRANSPARENT_BG");
        return v != nullptr && *v != '\0' && *v != '0';
    }();
    return e;
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
// once (static content: acquire/fill/release once; the layer references the
// released image every frame). D3D12 fill = an UPLOAD-heap staging buffer copied
// into the swapchain image via a one-shot DIRECT command list (the same pattern
// the HUD path uses), leaving the image in RENDER_TARGET (the swapchain's
// created/contract state — the runtime's flatten samples it as RENDER_TARGET).
//  variant 0 — crispness panel: opaque fine 8-px checker core with a 24-px
//              half-transparent green border (PREMULTIPLIED bytes), so the
//              border resolves against the desktop where M=0.
//  variant 1 — stacking/alpha panel: UNPREMULTIPLIED orange at a=128 with
//              opaque white diagonal stripes; submitted with
//              XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT (fringing check
//              for the SrcAlpha flatten path).
static bool CreateAndFillL2DPanel(XrSessionManager& xr, ID3D12Device* device, ID3D12CommandQueue* queue,
                                  uint32_t w, uint32_t h, int variant, L2DPanel& out) {
    if (w == 0 || h == 0 || device == nullptr || queue == nullptr) {
        return false;
    }

    XrSwapchainCreateInfo sci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    sci.format = (int64_t)DXGI_FORMAT_B8G8R8A8_UNORM;
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
    std::vector<XrSwapchainImageD3D12KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
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

    // Build the BGRA8 content (same probe imagery as the D3D11 leg).
    size_t srcStride = (size_t)w * 4;
    std::vector<uint8_t> buf(srcStride * h);
    const uint32_t border = 24;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t* row = buf.data() + (size_t)y * srcStride;
        for (uint32_t x = 0; x < w; x++) {
            uint8_t* px = row + (size_t)x * 4; // B,G,R,A
            if (variant == 0) {
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
                    px[0] = 0; px[1] = 165; px[2] = 255; px[3] = 128; // UNPREMULTIPLIED orange a=128
                }
            }
        }
    }

    // Upload buffer (256-byte-aligned rows per the D3D12 placed-footprint rule).
    const uint32_t alignedRowPitch =
        (uint32_t)((srcStride + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(size_t)(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1));
    const uint64_t uploadSize = (uint64_t)alignedRowPitch * h;

    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC ubDesc = {};
    ubDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    ubDesc.Width = uploadSize;
    ubDesc.Height = 1;
    ubDesc.DepthOrArraySize = 1;
    ubDesc.MipLevels = 1;
    ubDesc.Format = DXGI_FORMAT_UNKNOWN;
    ubDesc.SampleDesc.Count = 1;
    ubDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    Microsoft::WRL::ComPtr<ID3D12Resource> upload;
    if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &ubDesc,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&upload)))) {
        LOG_ERROR("Local2D panel: upload buffer create failed");
        return false;
    }
    uint8_t* mapped = nullptr;
    D3D12_RANGE noRead = {0, 0};
    if (FAILED(upload->Map(0, &noRead, (void**)&mapped))) {
        LOG_ERROR("Local2D panel: upload buffer map failed");
        return false;
    }
    for (uint32_t y = 0; y < h; y++) {
        memcpy(mapped + (size_t)y * alignedRowPitch, buf.data() + (size_t)y * srcStride, srcStride);
    }
    upload->Unmap(0, nullptr);

    // One-shot DIRECT command list: RENDER_TARGET -> COPY_DEST, copy, -> RENDER_TARGET.
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> alloc;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cl;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                         IID_PPV_ARGS(&cl))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        LOG_ERROR("Local2D panel: command objects create failed");
        return false;
    }

    ID3D12Resource* tex = imgs[idx].texture;
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = tex;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    cl->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = upload.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint.Offset = 0;
    srcLoc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    srcLoc.PlacedFootprint.Footprint.Width = w;
    srcLoc.PlacedFootprint.Footprint.Height = h;
    srcLoc.PlacedFootprint.Footprint.Depth = 1;
    srcLoc.PlacedFootprint.Footprint.RowPitch = alignedRowPitch;
    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = tex;
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;
    cl->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    cl->ResourceBarrier(1, &barrier);
    cl->Close();

    ID3D12CommandList* lists[] = {cl.Get()};
    queue->ExecuteCommandLists(1, lists);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    queue->Signal(fence.Get(), 1);
    if (fence->GetCompletedValue() < 1) {
        fence->SetEventOnCompletion(1, ev);
        WaitForSingleObject(ev, INFINITE);
    }
    CloseHandle(ev);

    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(out.swapchain, &ri);
    return true;
}

static void RenderThreadFunc(
    HWND hwnd,
    XrSessionManager* xr,
    D3D12Renderer* renderer,
    std::vector<XrSwapchainImageD3D12KHR>* swapchainImages,
    int rtvBaseIndex,
    HudRenderer* hud,
    std::vector<XrSwapchainImageD3D12KHR>* hudSwapchainImages,
    ID3D12Resource* hudUploadBuffer,
    uint8_t* hudUploadMapped,
    uint32_t hudUploadRowPitch,
    ID3D12CommandAllocator* hudCmdAllocator,
    ID3D12GraphicsCommandList* hudCmdList,
    ID3D12Fence* hudFence,
    HANDLE hudFenceEvent)
{
    LOG_INFO("[RenderThread] Started");

    PerformanceStats perfStats = {};
    perfStats.lastTime = std::chrono::high_resolution_clock::now();
    UINT64 hudFenceValue = 0;

    while (g_running.load() && !xr->exitRequested) {
        InputState inputSnapshot;
        bool resetRequested = false;
        uint32_t windowW, windowH;
        bool cycleModeRequested = false;
        int32_t absoluteModeRequest = -1;
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

        // Cube spin speed is agent-settable via cube-d3d12__set_spin (#457)
        UpdateScene(*renderer, perfStats.deltaTime, xr->spinSpeed);
        PollEvents(*xr);

        if (xr->sessionRunning) {
            XrFrameState frameState;
            if (BeginFrame(*xr, frameState)) {
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
                bool rendered = false;
                bool hudSubmitted = false;

                if (frameState.shouldRender) {
                    if (LocateViews(*xr, frameState.predictedDisplayTime,
                        inputSnapshot.cameraPosX, inputSnapshot.cameraPosY, inputSnapshot.cameraPosZ,
                        inputSnapshot.yaw, inputSnapshot.pitch,
                        inputSnapshot.viewParams)) {

                        // Get raw view poses for projection views.
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
                        // Only clip policy (near/far + the GL→[0,1] depth remap)
                        // stays app-side, by design (fov is clip-independent).
                        // ZDP-anchored clip: near = ez - vH, far = ez + 1000·vH;
                        // ez = rig-local z of the view pose.
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
                                convert_projection_gl_to_zero_to_one(stereoViews[i].projection_matrix);
                                stereoViews[i].fov = v.fov;
                            }
                        }

                        rendered = true;

                        // For mono: compute center eye position and projection
                        XMMATRIX monoViewMatrix, monoProjMatrix;
                        XrPosef monoPose = rawViews[0].pose;
                        if (monoMode) {
                            // Center eye = average of all N views
                            XrVector3f center = {0.0f, 0.0f, 0.0f};
                            for (uint32_t v = 0; v < viewCount; v++) {
                                center.x += rawViews[v].pose.position.x;
                                center.y += rawViews[v].pose.position.y;
                                center.z += rawViews[v].pose.position.z;
                            }
                            if (viewCount > 0) {
                                center.x /= viewCount; center.y /= viewCount; center.z /= viewCount;
                            }
                            monoPose.position = center;

                            // When useAppProjection, mono view+proj come from stereoViews[0]
                            if (!useAppProjection) {
                                monoProjMatrix = xr->projMatrices[0];

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
                            ID3D12Resource* swapchainTexture = (*swapchainImages)[imageIndex].texture;
                            int rtvIdx = rtvBaseIndex + (int)imageIndex;

                            for (int eye = 0; eye < eyeCount; eye++) {
                                // Tile-aware viewport positioning
                                uint32_t tileX = monoMode ? 0 : (eye % tileColumns);
                                uint32_t tileY = monoMode ? 0 : (eye / tileColumns);
                                uint32_t vpX = tileX * renderW;
                                uint32_t vpY = tileY * renderH;

                                XMMATRIX viewMatrix, projMatrix;
                                if (useAppProjection) {
                                    int vi = monoMode ? 0 : eye;
                                    viewMatrix = ColumnMajorToXMMatrix(stereoViews[vi].view_matrix);
                                    projMatrix = ColumnMajorToXMMatrix(stereoViews[vi].projection_matrix);
                                } else if (monoMode) {
                                    viewMatrix = monoViewMatrix;
                                    projMatrix = monoProjMatrix;
                                } else {
                                    int si = eye < 2 ? eye : 0;
                                    viewMatrix = xr->viewMatrices[si];
                                    projMatrix = xr->projMatrices[si];
                                }

                                RenderScene(*renderer, swapchainTexture, rtvIdx,
                                    vpX, vpY,
                                    renderW, renderH,
                                    viewMatrix, projMatrix,
                                    useAppProjection ? 1.0f : inputSnapshot.viewParams.scaleFactor,
                                    eye == 0);  // clear only on first eye

                                projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                                projectionViews[eye].subImage.swapchain = xr->swapchain.swapchain;
                                projectionViews[eye].subImage.imageRect.offset = {
                                    (int32_t)vpX, (int32_t)vpY};
                                projectionViews[eye].subImage.imageRect.extent = {
                                    (int32_t)renderW, (int32_t)renderH};
                                projectionViews[eye].subImage.imageArrayIndex = 0;
                                int rawIdx = (eye < (int)viewCount) ? eye : 0;
                                projectionViews[eye].pose = monoMode ? monoPose : rawViews[rawIdx].pose;
                                projectionViews[eye].fov = useAppProjection ?
                                    stereoViews[monoMode ? 0 : eye].fov :
                                    (monoMode ? rawViews[0].fov : rawViews[rawIdx].fov);
                            }

                            // 'I' key: snapshot the multi-view atlas. Skipped
                            // for mono (1×1) layouts. RenderScene leaves the
                            // texture in D3D12_RESOURCE_STATE_COMMON.
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
                                    L"XR_EXT_win32_window_binding: ACTIVE (D3D12)" :
                                    L"XR_EXT_win32_window_binding: NOT AVAILABLE (D3D12)";
                                uint32_t dispRenderW, dispRenderH;
                                if (monoMode) {
                                    dispRenderW = windowW;
                                    dispRenderH = windowH;
                                    if (dispRenderW > xr->swapchain.width) dispRenderW = xr->swapchain.width;
                                    if (dispRenderH > xr->swapchain.height) dispRenderH = xr->swapchain.height;
                                } else {
                                    dispRenderW = (uint32_t)(windowW * xr->recommendedViewScaleX);
                                    dispRenderH = (uint32_t)(windowH * xr->recommendedViewScaleY);
                                    if (dispRenderW > maxTileW) dispRenderW = maxTileW;
                                    if (dispRenderH > maxTileH) dispRenderH = maxTileH;
                                }
                                std::wstring perfText = FormatPerformanceInfo(perfStats.fps, perfStats.frameTimeMs,
                                    dispRenderW, dispRenderH, windowW, windowH);
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
                                const void* pixels = RenderHudAndMap(*hud, &srcRowPitch, sessionText, modeText, perfText, dispText, eyeText,
                                    cameraText, stereoText, helpText);
                                if (pixels) {
                                    // Copy pixels row-by-row to D3D12 upload buffer (256-byte aligned rows)
                                    const uint8_t* src = (const uint8_t*)pixels;
                                    for (uint32_t row = 0; row < HUD_PIXEL_HEIGHT; row++) {
                                        memcpy(hudUploadMapped + row * hudUploadRowPitch,
                                            src + row * srcRowPitch,
                                            HUD_PIXEL_WIDTH * 4);
                                    }
                                    UnmapHud(*hud);

                                    // Record D3D12 commands: copy upload buffer to HUD swapchain texture
                                    ID3D12Resource* hudTex = (*hudSwapchainImages)[hudImageIndex].texture;

                                    hudCmdAllocator->Reset();
                                    hudCmdList->Reset(hudCmdAllocator, nullptr);

                                    // Barrier: COMMON -> COPY_DEST
                                    D3D12_RESOURCE_BARRIER barrier = {};
                                    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                                    barrier.Transition.pResource = hudTex;
                                    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                                    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                                    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                                    hudCmdList->ResourceBarrier(1, &barrier);

                                    // CopyTextureRegion from upload buffer
                                    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
                                    srcLoc.pResource = hudUploadBuffer;
                                    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                                    srcLoc.PlacedFootprint.Offset = 0;
                                    srcLoc.PlacedFootprint.Footprint.Format = (DXGI_FORMAT)xr->hudSwapchain.format;
                                    srcLoc.PlacedFootprint.Footprint.Width = HUD_PIXEL_WIDTH;
                                    srcLoc.PlacedFootprint.Footprint.Height = HUD_PIXEL_HEIGHT;
                                    srcLoc.PlacedFootprint.Footprint.Depth = 1;
                                    srcLoc.PlacedFootprint.Footprint.RowPitch = hudUploadRowPitch;

                                    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
                                    dstLoc.pResource = hudTex;
                                    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                                    dstLoc.SubresourceIndex = 0;

                                    hudCmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

                                    // Barrier: COPY_DEST -> COMMON
                                    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                                    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
                                    hudCmdList->ResourceBarrier(1, &barrier);

                                    hudCmdList->Close();

                                    // Execute and wait
                                    ID3D12CommandList* lists[] = { hudCmdList };
                                    renderer->commandQueue->ExecuteCommandLists(1, lists);
                                    hudFenceValue++;
                                    renderer->commandQueue->Signal(hudFence, hudFenceValue);
                                    if (hudFence->GetCompletedValue() < hudFenceValue) {
                                        hudFence->SetEventOnCompletion(hudFenceValue, hudFenceEvent);
                                        WaitForSingleObject(hudFenceEvent, INFINITE);
                                    }

                                    hudSubmitted = true;
                                }

                                ReleaseHudSwapchainImage(*xr);
                            }
                        }
                    }
                }

                // #439 cases 2/3/4 activation: create + fill the panel
                // swapchain(s) (+ the explicit Tier-2 island mask for case 2) a
                // few frames in, once the session is running and dims settled.
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
                        bool ok = CreateAndFillL2DPanel(*xr, renderer->device.Get(), renderer->commandQueue.Get(),
                                                        pw, ph, p1variant, g_panel1);

                        if (ok && g_l2dPanel2) {
                            // Overlaps panel 1's top-right quadrant — list-order
                            // stacking check (panel 2 is later = on top).
                            g_panel2Rect.offset = {g_panel1Rect.offset.x + (int32_t)(pw / 2),
                                                   g_panel1Rect.offset.y - (int32_t)(ph / 4)};
                            g_panel2Rect.extent = {(int32_t)pw, (int32_t)ph};
                            ok = CreateAndFillL2DPanel(*xr, renderer->device.Get(), renderer->commandQueue.Get(),
                                                       pw, ph, 1, g_panel2);
                        }

                        if (ok && g_l2dMask && g_zone.available && g_zone.pfnCreate && g_zone.pfnSetRects &&
                            g_zone.pfnSubmit) {
                            XrLocal3DZoneMaskCreateInfoEXT mci = {
                                (XrStructureType)XR_TYPE_LOCAL_3D_ZONE_MASK_CREATE_INFO_EXT};
                            mci.maskWidth = 0; // runtime picks the window backing size
                            mci.maskHeight = 0;
                            ok = XR_SUCCEEDED(g_zone.pfnCreate(xr->session, &mci, &g_zone.mask));
                            if (ok) {
                                // Two 3D islands: a large center-right one and a
                                // small top-left one. Everything else is 2D — the
                                // panel where it covers, desktop (final.a = 0)
                                // where nothing does.
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

                // End frame. #439 cases 2/3/4: when Local2D panels are active,
                // build the layer list manually (projection + panels in list
                // order) and submit raw — the shared EndFrame helpers don't
                // carry the Local2D layer type. Otherwise the normal paths.
                uint32_t submitViewCount = (xr->renderingModeCount > 0 && xr->currentModeIndex < xr->renderingModeCount) ? xr->renderingModeViewCounts[xr->currentModeIndex] : 2;
                LOG_INFO("[FRAME] EndFrame: rendered=%d hudSubmitted=%d viewCount=%u", rendered, hudSubmitted, submitViewCount);
                if (g_l2dActive && g_panel1.swapchain != XR_NULL_HANDLE) {
                    XrCompositionLayerProjection projLayer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
                    projLayer.space = xr->localSpace;
                    projLayer.viewCount = (uint32_t)eyeCount;
                    projLayer.views = projectionViews.data();

                    XrCompositionLayerLocal2DEXT panel1Layer = {
                        (XrStructureType)XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_EXT};
                    XrCompositionLayerLocal2DEXT panel2Layer = {
                        (XrStructureType)XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_EXT};
                    const XrCompositionLayerBaseHeader* layers[3] = {
                        (XrCompositionLayerBaseHeader*)&projLayer, nullptr, nullptr};
                    uint32_t layerCount = 1;

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

                    // ALPHA_BLEND is what makes the desktop show through where
                    // the mask is 2D + the layer alpha < 1 (§4.2 output rule +
                    // the panel's half-transparent border). runtimeSupportsAlphaBlend
                    // is resolved by the pre-activation frames' SelectEnvBlendMode.
                    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
                    endInfo.displayTime = frameState.predictedDisplayTime;
                    endInfo.environmentBlendMode = xr->runtimeSupportsAlphaBlend
                        ? XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND
                        : XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                    endInfo.layerCount = layerCount;
                    endInfo.layers = layers;
                    xrEndFrame(xr->session, &endInfo);
                } else if (rendered && hudSubmitted) {
                    float hudAR = (float)HUD_PIXEL_WIDTH / (float)HUD_PIXEL_HEIGHT;
                    float windowAR = (windowW > 0 && windowH > 0) ? (float)windowW / (float)windowH : 1.0f;
                    float fracW = HUD_WIDTH_FRACTION;
                    float fracH = fracW * windowAR / hudAR;
                    if (fracH > 1.0f) { fracH = 1.0f; fracW = hudAR / windowAR; }
                    EndFrameWithWindowSpaceHud(*xr, frameState.predictedDisplayTime, projectionViews.data(),
                        0.0f, 0.0f, fracW, fracH, 0.0f, submitViewCount);
                } else if (rendered) {
                    EndFrame(*xr, frameState.predictedDisplayTime, projectionViews.data(), submitViewCount);
                } else {
                    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
                    endInfo.displayTime = frameState.predictedDisplayTime;
                    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                    endInfo.layerCount = 0;
                    endInfo.layers = nullptr;
                    xrEndFrame(xr->session, &endInfo);
                }
                g_l2dFrameCounter++;
            }
        } else {
            Sleep(100);
        }
    }

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

    LOG_INFO("=== SR Cube OpenXR Ext D3D12 Application ===");

    // #439 Phase 3 — handle + mask + Local2D layer modes (§8 cases 2/3/4).
    {
        const char* e = getenv("DXR_LOCAL2D_PANEL");
        if (e && *e == '1') g_l2dPanel = true;
        e = getenv("DXR_LOCAL2D_MASK");
        if (e && *e == '1') g_l2dMask = true;
        e = getenv("DXR_LOCAL2D_PANEL2");
        if (e && *e == '1') g_l2dPanel2 = true;
        if (g_l2dPanel) {
            LOG_INFO("DXR_LOCAL2D_PANEL=1 — Local2D panel layer%s%s",
                g_l2dPanel2 ? " + panel2 (unpremultiplied, overlapping)" : "",
                g_l2dMask ? " + explicit Tier-2 island mask" : " (implicit mask)");
        }
    }

    HWND hwnd = CreateAppWindow(hInstance, g_windowWidth, g_windowHeight);
    if (!hwnd) {
        LOG_ERROR("Failed to create window");
        ShutdownLogging();
        return 1;
    }

    // Initialize OpenXR
    XrSessionManager xr = {};
    g_xr = &xr;
    if (!InitializeOpenXR(xr)) {
        LOG_ERROR("OpenXR initialization failed");
        g_xr = nullptr;
        ShutdownLogging();
        return 1;
    }

    // Get D3D12 graphics requirements
    LUID adapterLuid;
    if (!GetD3D12GraphicsRequirements(xr, &adapterLuid)) {
        LOG_ERROR("Failed to get D3D12 graphics requirements");
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Initialize D3D12
    D3D12Renderer renderer = {};
    if (!InitializeD3D12WithLUID(renderer, adapterLuid)) {
        LOG_ERROR("D3D12 initialization failed");
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create session
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

    if (!CreateSwapchain(xr)) {
        LOG_ERROR("Swapchain creation failed");
        CleanupOpenXR(xr);
        CleanupD3D12(renderer);
        ShutdownLogging();
        return 1;
    }

    // Enumerate D3D12 swapchain images (single SBS swapchain)
    std::vector<XrSwapchainImageD3D12KHR> swapchainImages;
    int rtvBaseIndex = 0;
    {
        uint32_t count = xr.swapchain.imageCount;
        swapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)swapchainImages.data());
        LOG_INFO("Enumerated %u D3D12 swapchain images", count);

        // Collect ID3D12Resource pointers for RTV creation
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
            CleanupD3D12(renderer);
            ShutdownLogging();
            return 1;
        }
    }

    // Initialize HUD renderer (standalone D3D11 device for text rendering)
    HudRenderer hudRenderer = {};
    bool hudOk = InitializeHudRenderer(hudRenderer, HUD_PIXEL_WIDTH, HUD_PIXEL_HEIGHT);
    if (!hudOk) {
        LOG_WARN("HUD renderer init failed - HUD will not be displayed");
    }

    // Create HUD swapchain for window-space layer submission
    std::vector<XrSwapchainImageD3D12KHR> hudSwapImages;
    if (hudOk) {
        if (CreateHudSwapchain(xr, HUD_PIXEL_WIDTH, HUD_PIXEL_HEIGHT)) {
            uint32_t count = xr.hudSwapchain.imageCount;
            hudSwapImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
            xrEnumerateSwapchainImages(xr.hudSwapchain.swapchain, count, &count,
                (XrSwapchainImageBaseHeader*)hudSwapImages.data());
            LOG_INFO("HUD swapchain: enumerated %u D3D12 images", count);
        } else {
            LOG_WARN("HUD swapchain creation failed - HUD will not be displayed");
            hudOk = false;
        }
    }

    // Create D3D12 upload resources for HUD pixel transfer
    ComPtr<ID3D12Resource> hudUploadBuffer;
    uint8_t* hudUploadMapped = nullptr;
    ComPtr<ID3D12CommandAllocator> hudCmdAllocator;
    ComPtr<ID3D12GraphicsCommandList> hudCmdList;
    ComPtr<ID3D12Fence> hudFence;
    HANDLE hudFenceEvent = nullptr;
    // Row pitch must be aligned to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256 bytes)
    uint32_t hudUploadRowPitch = (HUD_PIXEL_WIDTH * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
        & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);

    if (hudOk) {
        // Upload buffer (UPLOAD heap, persistently mapped)
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
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&hudUploadBuffer));
        if (FAILED(hr)) {
            LOG_WARN("Failed to create HUD upload buffer: 0x%08X", hr);
            hudOk = false;
        }

        if (hudOk) {
            D3D12_RANGE readRange = {0, 0}; // no CPU reads
            hr = hudUploadBuffer->Map(0, &readRange, (void**)&hudUploadMapped);
            if (FAILED(hr)) {
                LOG_WARN("Failed to map HUD upload buffer: 0x%08X", hr);
                hudOk = false;
            }
        }

        if (hudOk) {
            hr = renderer.device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&hudCmdAllocator));
            if (FAILED(hr)) {
                LOG_WARN("Failed to create HUD command allocator: 0x%08X", hr);
                hudOk = false;
            }
        }

        if (hudOk) {
            hr = renderer.device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT, hudCmdAllocator.Get(), nullptr,
                IID_PPV_ARGS(&hudCmdList));
            if (FAILED(hr)) {
                LOG_WARN("Failed to create HUD command list: 0x%08X", hr);
                hudOk = false;
            } else {
                hudCmdList->Close(); // start in closed state
            }
        }

        if (hudOk) {
            hr = renderer.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&hudFence));
            if (FAILED(hr)) {
                LOG_WARN("Failed to create HUD fence: 0x%08X", hr);
                hudOk = false;
            } else {
                hudFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            }
        }

        if (hudOk) {
            LOG_INFO("HUD D3D12 resources created (%ux%u, row pitch %u)", HUD_PIXEL_WIDTH, HUD_PIXEL_HEIGHT, hudUploadRowPitch);
        }
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    LOG_INFO("");
    LOG_INFO("=== Entering main loop ===");
    LOG_INFO("Controls: WASD=Fly, QE=Up/Down, Mouse=Look, Space/DblClick=Reset, V=Mode, SHIFT+TAB=HUD, F11=Fullscreen, ESC=Quit");
    LOG_INFO("");

    // Set virtual display height (app units). 0.24 = 4x the 0.06m cube height.
    g_inputState.viewParams.virtualDisplayHeight = 0.24f;
    g_inputState.renderingModeCount = xr.renderingModeCount;

    std::thread renderThread(RenderThreadFunc, hwnd, &xr, &renderer,
        &swapchainImages, rtvBaseIndex,
        hudOk ? &hudRenderer : nullptr,
        hudOk ? &hudSwapImages : nullptr,
        hudUploadBuffer.Get(), hudUploadMapped, hudUploadRowPitch,
        hudCmdAllocator.Get(), hudCmdList.Get(), hudFence.Get(), hudFenceEvent);

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

    // Clean up HUD resources
    if (hudFenceEvent) CloseHandle(hudFenceEvent);
    hudFence.Reset();
    hudCmdList.Reset();
    hudCmdAllocator.Reset();
    if (hudUploadMapped && hudUploadBuffer) {
        hudUploadBuffer->Unmap(0, nullptr);
        hudUploadMapped = nullptr;
    }
    hudUploadBuffer.Reset();
    if (hudOk) CleanupHudRenderer(hudRenderer);

    g_xr = nullptr;
    CleanupOpenXR(xr);
    CleanupD3D12(renderer);

    DestroyWindow(hwnd);
    UnregisterClass(WINDOW_CLASS, hInstance);

    LOG_INFO("Application shutdown complete");
    ShutdownLogging();

    return 0;
}
