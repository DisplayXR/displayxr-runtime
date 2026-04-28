// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  SR 3DGS OpenXR Ext VK - 3D Gaussian Splatting with OpenXR (Vulkan)
 *
 * Renders 3D Gaussian Splatting scenes on tracked 3D displays via OpenXR.
 * Based on cube_handle_vk with the cube/grid renderer replaced by
 * a 3DGS.cpp compute pipeline.  Features a "Load Scene" button as a
 * window-space layer overlay.
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <commdlg.h>
#include <shlwapi.h>
#include <shlobj.h>
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

#include "logging.h"
#include "input_handler.h"
#include "xr_session.h"
#include "gs_renderer.h"
#include "gs_scene_loader.h"
#include "display3d_view.h"

#include "hud_renderer.h"
#include "text_overlay.h"
#include "atlas_capture.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace DirectX;

static const char* APP_NAME = "gaussian_splatting_handle_vk_win";

static const wchar_t* WINDOW_CLASS = L"SR3DGSOpenXRExtVKClass";
static const wchar_t* WINDOW_TITLE = L"DisplayXR Gaussian Splat Viewer Demo";

// HUD overlay fractions. Layer spans full window height so chrome buttons
// can sit at the window top while the info panel anchors to the bottom-left
// (matching the macOS demo's split). The vk_native compositor now uses an
// alpha-blended draw pass for window-space layers, so the empty middle of
// the texture stays invisible. Font sizing is anchored to the legacy
// 0.5-fraction so text doesn't grow with the taller texture.
static const float HUD_WIDTH_FRACTION = 0.30f;
static const float HUD_HEIGHT_FRACTION = 1.0f;
static const float HUD_FONT_BASE_FRACTION = 0.50f;

// Top-bar buttons live inside the HUD overlay's window-space footprint
// (left strip of the window, fracW × fracH = HUD_WIDTH_FRACTION × HUD_HEIGHT_FRACTION).
// All values are absolute window-fractions for hit-testing; they're
// translated into HUD-pixel coordinates when passed to RenderHudAndMap.
static const float OPEN_BTN_X_FRACTION = 0.010f;
static const float OPEN_BTN_Y_FRACTION = 0.010f;
static const float OPEN_BTN_WIDTH_FRACTION  = 0.060f;
static const float OPEN_BTN_HEIGHT_FRACTION = 0.030f;

static const float MODE_BTN_X_FRACTION = 0.075f;
static const float MODE_BTN_Y_FRACTION = 0.010f;
static const float MODE_BTN_WIDTH_FRACTION  = 0.140f;
static const float MODE_BTN_HEIGHT_FRACTION = 0.030f;


// sim_display output mode switching (legacy — replaced by unified rendering mode)
typedef void (*PFN_sim_display_set_output_mode)(int mode);
static PFN_sim_display_set_output_mode g_pfnSetOutputMode = nullptr;

// Global state
static InputState g_inputState;
static std::mutex g_inputMutex;
static std::atomic<bool> g_running{true};
static XrSessionManager* g_xr = nullptr;
static UINT g_windowWidth = 1280;
static UINT g_windowHeight = 720;

// 3DGS state
static GsRenderer g_gsRenderer;
// Cross-thread scene-load queue: the file dialog runs on the main (message-pump)
// thread, but the actual GsRenderer::loadScene() submits Vulkan work on the
// graphics queue and so MUST run on the same thread that drives per-frame
// rendering — otherwise concurrent vkQueueSubmit/vkQueueWaitIdle from two
// threads on a single VkQueue is undefined behaviour and crashes some drivers
// (NVIDIA in particular). Main thread posts the picked path here; the render
// thread picks it up between frames.
static std::atomic<bool> g_loadRequested{false};
static std::string g_pendingLoadPath;
static std::mutex g_pendingLoadPathMutex;
// 'I' key: capture the multi-view atlas region (cols × rows × renderW × renderH)
// of the swapchain to a PNG in %USERPROFILE%\Pictures\DisplayXR\. Skipped for
// 1×1 (mono) layouts. Helper lives in test_apps/common/atlas_capture*.
static std::atomic<bool> g_captureAtlasRequested{false};
static std::string g_loadedFileName;
static std::mutex g_sceneMutex;

// Fallback vHeight when no scene is loaded or auto-fit hits a degenerate
// extent. Matches macOS demo's kDefaultVirtualDisplayHeightM (1.5m).
static constexpr float kFallbackVirtualDisplayHeightM = 1.5f;
// Comfort margin is baked into getMainObjectBounds (which picks a different
// multiplier for single-object vs scene-with-central-object). Keep this at
// 1.0 to mean "no extra margin on top of what the bounds method returned".
static constexpr float kAutoFitVerticalComfort = 1.0f;

// Cached auto-fit pose for the currently loaded scene. Reused by Reset
// so 'Space' returns to the framed pose rather than world origin.
static float g_fitCenter[3] = {0.0f, 0.0f, 0.0f};
static float g_fitVHeight   = kFallbackVirtualDisplayHeightM;
static float g_fitYaw       = 0.0f;
static std::atomic<bool> g_fitValid{false};

// Compute robust scene bounds (5th–95th percentile per axis) and stage
// new display-rig pose + vHeight on g_inputState. Display orientation is
// kept identity (forward = world −Z): splats have no canonical front, and
// any heuristic (PCA, etc.) can pick the wrong side; the user can rotate
// with mouse drag from a predictable starting pose.
// Caller must hold g_sceneMutex (we read pickData_ from the renderer).
static void ApplyAutoFitForLoadedScene_locked() {
    float center[3], extent[3];
    // Voxel-density flood-fill — see the macOS demo for rationale.
    bool ok = g_gsRenderer.getMainObjectBounds(64u, center, extent);
    if (ok) {
        g_fitCenter[0] = center[0];
        g_fitCenter[1] = center[1];
        g_fitCenter[2] = center[2];
        float vh = extent[1] * kAutoFitVerticalComfort;
        // Degenerate scene (all splats in a thin slice) — fall back to a
        // sensible vHeight rather than failing the fit. Mirrors macOS:1399.
        if (!(vh > 1e-3f)) vh = kFallbackVirtualDisplayHeightM;
        g_fitVHeight = vh;
        // Search 8 yaws to face the captured side of the scene.
        float viewerOffset[3] = {0.0f, 0.1f, 0.6f};
        g_fitYaw = g_gsRenderer.findBestYaw(g_fitCenter, viewerOffset, 8);
        LOG_INFO("Auto-fit: center=(%.3f, %.3f, %.3f) extent=(%.3f, %.3f, %.3f) vHeight=%.3f yaw=%.0fdeg",
                 center[0], center[1], center[2],
                 extent[0], extent[1], extent[2], vh, g_fitYaw * 57.2957795f);
    }
    g_fitValid.store(ok);

    std::lock_guard<std::mutex> lock(g_inputMutex);
    g_inputState.cameraPosX = ok ? g_fitCenter[0] : 0.0f;
    g_inputState.cameraPosY = ok ? g_fitCenter[1] : 0.0f;
    g_inputState.cameraPosZ = ok ? g_fitCenter[2] : 0.0f;
    g_inputState.yaw = ok ? g_fitYaw : 0.0f;
    g_inputState.pitch = 0.0f;
    g_inputState.viewParams.virtualDisplayHeight = ok ? g_fitVHeight : kFallbackVirtualDisplayHeightM;
    g_inputState.viewParams.scaleFactor = 1.0f;

    // Per-format orientation correction is now done at load time (PLY loader
    // converts RDF+X-mirror → canonical RUB; SPZ loader uses RUB natively).
    // Renderer's GsRenderer::updateUniforms negates the Y row of proj_mat to
    // match the +Y-up convention. No runtime view-stage flips needed.

    // Route the first post-load frame through the same reset path Space uses,
    // so app-start view params (perspectiveFactor, scaleFactor, etc.) match
    // the Space-reset state.
    g_inputState.resetViewRequested = true;

    // Treat scene load as a fresh user interaction so the auto-orbit idle
    // timer restarts. Without this, an asset loaded after the 10s idle
    // threshold starts rotating immediately on first display.
    {
        using namespace std::chrono;
        g_inputState.lastInputTimeSec = (double)duration_cast<microseconds>(
            high_resolution_clock::now().time_since_epoch()).count() * 1e-6;
        g_inputState.animationActive = false;
    }
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

static bool PointInFractionRect(int mouseX, int mouseY, int windowW, int windowH,
                                float xf, float yf, float wf, float hf) {
    if (windowW <= 0 || windowH <= 0) return false;
    float fx = (float)mouseX / (float)windowW;
    float fy = (float)mouseY / (float)windowH;
    return (fx >= xf && fx <= xf + wf && fy >= yf && fy <= yf + hf);
}

static bool IsClickOnLoadButton(int mouseX, int mouseY, int windowW, int windowH) {
    return PointInFractionRect(mouseX, mouseY, windowW, windowH,
        OPEN_BTN_X_FRACTION, OPEN_BTN_Y_FRACTION,
        OPEN_BTN_WIDTH_FRACTION, OPEN_BTN_HEIGHT_FRACTION);
}

static bool IsClickOnModeButton(int mouseX, int mouseY, int windowW, int windowH) {
    return PointInFractionRect(mouseX, mouseY, windowW, windowH,
        MODE_BTN_X_FRACTION, MODE_BTN_Y_FRACTION,
        MODE_BTN_WIDTH_FRACTION, MODE_BTN_HEIGHT_FRACTION);
}

// Atlas capture helpers live in test_apps/common/atlas_capture* — see
// dxr_capture::CaptureAtlasRegionVk / TriggerCaptureFlash / MakeCapturePath.

// Attempt to auto-load butterfly.spz from next to the exe.
static void TryAutoLoadBundledScene() {
    char exePath[MAX_PATH] = {0};
    if (!GetModuleFileNameA(nullptr, exePath, MAX_PATH)) return;
    // Strip basename
    char *lastSlash = strrchr(exePath, '\\');
    if (!lastSlash) lastSlash = strrchr(exePath, '/');
    if (!lastSlash) return;
    *(lastSlash + 1) = '\0';
    std::string path = std::string(exePath) + "butterfly.spz";
    if (!PathFileExistsA(path.c_str())) {
        LOG_INFO("No bundled scene at %s (skipping auto-load)", path.c_str());
        return;
    }
    if (!ValidateSceneFile(path)) return;
    LOG_INFO("Auto-loading bundled scene: %s", path.c_str());
    std::lock_guard<std::mutex> lock(g_sceneMutex);
    if (g_gsRenderer.loadScene(path.c_str())) {
        g_loadedFileName = GetPlyFilename(path);
        LOG_INFO("Loaded %s (%s)", g_loadedFileName.c_str(), GetPlyFileSize(path).c_str());
        ApplyAutoFitForLoadedScene_locked();
    } else {
        LOG_WARN("Auto-load failed for %s", path.c_str());
    }
}

// Open a file dialog and load a .ply or .spz scene (called from main thread)
static void OpenLoadDialog(HWND hwnd) {
    OPENFILENAMEA ofn = {};
    char filePath[MAX_PATH] = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = "3DGS Files (*.ply;*.spz)\0*.ply;*.spz\0PLY Files (*.ply)\0*.ply\0SPZ Files (*.spz)\0*.spz\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filePath;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = "Load Gaussian Splatting Scene";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameA(&ofn)) {
        std::string path(filePath);
        if (ValidateSceneFile(path)) {
            // Hand the path off to the render thread (see g_pendingLoadPath
            // comment above). Doing the load here would race the render
            // thread's per-frame queue submissions and crash NVIDIA drivers.
            {
                std::lock_guard<std::mutex> lock(g_pendingLoadPathMutex);
                g_pendingLoadPath = path;
            }
            g_loadRequested.store(true, std::memory_order_release);
            LOG_INFO("Queued 3DGS scene load: %s", path.c_str());
        } else {
            MessageBoxA(hwnd, "Invalid scene file. Supported formats: .ply, .spz", "Load Error", MB_OK | MB_ICONERROR);
        }
    }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    {
        std::lock_guard<std::mutex> lock(g_inputMutex);
        UpdateInputState(g_inputState, msg, wParam, lParam);
    }

    switch (msg) {
    case WM_LBUTTONDOWN: {
        int mx = LOWORD(lParam);
        int my = HIWORD(lParam);
        // UpdateInputState above already set leftButton/dragging=true. For
        // button clicks (which post a message to run a modal dialog or change
        // mode), clear that drag state — otherwise the modal eats the
        // matching WM_LBUTTONUP and subsequent mouse motion is interpreted as
        // a scene drag.
        if (IsClickOnLoadButton(mx, my, g_windowWidth, g_windowHeight)) {
            {
                std::lock_guard<std::mutex> lock(g_inputMutex);
                g_inputState.leftButton = false;
                g_inputState.dragging = false;
            }
            PostMessage(hwnd, WM_USER + 1, 0, 0);
            return 0;
        }
        if (IsClickOnModeButton(mx, my, g_windowWidth, g_windowHeight)) {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_inputState.leftButton = false;
            g_inputState.dragging = false;
            if (g_inputState.renderingModeCount > 0) {
                g_inputState.currentRenderingMode =
                    (g_inputState.currentRenderingMode + 1) % g_inputState.renderingModeCount;
            }
            g_inputState.renderingModeChangeRequested = true;
            return 0;
        }
        SetCapture(hwnd);
        return 0;
    }
    case WM_LBUTTONUP:
        ReleaseCapture();
        return 0;

    case WM_USER + 1:
        OpenLoadDialog(hwnd);
        return 0;

    case dxr_capture::kFlashUserMsg:
        // Render thread requested a capture-flash; start it on this thread
        // (the message-pump thread that owns the HWND).
        dxr_capture::TriggerCaptureFlash(hwnd);
        return 0;

    case WM_TIMER:
        if (wParam == dxr_capture::kFlashTimerId) {
            dxr_capture::TickCaptureFlash(hwnd);
            return 0;
        }
        break;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            PostMessage(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        if (wParam == VK_F11) {
            ToggleFullscreen(hwnd);
            return 0;
        }
        // L key = load shortcut
        if (wParam == 'L') {
            PostMessage(hwnd, WM_USER + 1, 0, 0);
            return 0;
        }
        // I key = capture multi-view atlas
        if (wParam == 'I' || wParam == 'i') {
            g_captureAtlasRequested.store(true);
        }
        break;

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_windowWidth = LOWORD(lParam);
            g_windowHeight = HIWORD(lParam);
        }
        return 0;

    case WM_CLOSE:
        if (g_xr && g_xr->session != XR_NULL_HANDLE && g_xr->sessionRunning) {
            xrRequestExitSession(g_xr->session);
            return 0;
        }
        g_running.store(false);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static HWND CreateAppWindow(HINSTANCE hInstance, int width, int height) {
    LOG_INFO("Creating application window (%dx%d)", width, height);

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

// Render a simple "no scene" placeholder by clearing to dark gray
static void RenderPlaceholder(VkDevice device, VkQueue queue, VkCommandPool cmdPool,
                               VkImage image, uint32_t width, uint32_t height) {
    VkCommandBufferAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = cmdPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device, &allocInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    // Transition to TRANSFER_DST
    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkClearColorValue clearColor = {{0.1f, 0.1f, 0.12f, 1.0f}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &range);

    // Transition to COLOR_ATTACHMENT
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    vkFreeCommandBuffers(device, cmdPool, 1, &cmd);
}

static void RenderThreadFunc(
    HWND hwnd,
    XrSessionManager* xr,
    VkDevice vkDevice,
    VkQueue graphicsQueue,
    uint32_t queueFamilyIndex,
    VkInstance vkInstance,
    VkPhysicalDevice physDevice,
    std::vector<VkImage>* swapchainVkImages,
    HudRenderer* hud,
    uint32_t hudWidth,
    uint32_t hudHeight,
    VkBuffer hudStagingBuffer,
    void* hudStagingMapped,
    VkCommandPool hudCmdPool,
    std::vector<XrSwapchainImageVulkanKHR>* hudSwapchainImages,
    VkCommandPool loadBtnCmdPool,
    std::vector<XrSwapchainImageVulkanKHR>* loadBtnSwapchainImages,
    uint32_t loadBtnWidth,
    uint32_t loadBtnHeight)
{
    LOG_INFO("[RenderThread] Started");

    PerformanceStats perfStats = {};
    perfStats.lastTime = std::chrono::high_resolution_clock::now();

    // Command pool for placeholder rendering
    VkCommandPool renderCmdPool = VK_NULL_HANDLE;
    {
        VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamilyIndex;
        vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &renderCmdPool);
    }

    while (g_running.load() && !xr->exitRequested) {
        InputState inputSnapshot;
        bool resetRequested = false;
        bool animateToggle = false;
        bool loadReq = false;
        uint32_t windowW, windowH;
        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            inputSnapshot = g_inputState;
        }
        // Pitch sign flip for the GS demo: shared input_handler.cpp mutates
        // pitch with `-= dy` (cube_handle convention, paired with negative
        // VkViewport.height Y-flip at rasterization). The GS demo Y-flips
        // at the *view* stage in gs_renderer.cpp instead — that inverts the
        // visual response, so we negate here to restore drag-down → look-down.
        // Only `renderPitch` is used downstream; `inputSnapshot.pitch` itself
        // stays in shared-handler convention so the writeback at end of frame
        // doesn't compound.
        float renderPitch = -inputSnapshot.pitch;
        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            resetRequested = g_inputState.resetViewRequested;
            animateToggle = g_inputState.animateToggleRequested;
            loadReq = g_inputState.loadRequested;
            g_inputState.resetViewRequested = false;
            g_inputState.teleportRequested = false;
            g_inputState.fullscreenToggleRequested = false;
            g_inputState.renderingModeChangeRequested = false;
            g_inputState.eyeTrackingModeToggleRequested = false;
            g_inputState.animateToggleRequested = false;
            g_inputState.loadRequested = false;
            if (animateToggle) {
                g_inputState.animateEnabled = !g_inputState.animateEnabled;
                inputSnapshot.animateEnabled = g_inputState.animateEnabled;
            }
            windowW = g_windowWidth;
            windowH = g_windowHeight;
        }

        // Request main thread to open file dialog when L key or Load button was pressed.
        if (loadReq) {
            PostMessage(hwnd, WM_USER + 1, 0, 0);
        }

        // Drain a queued scene load (set by OpenLoadDialog on the main
        // thread). We must run loadScene here because it submits Vulkan work
        // on the graphics queue, and that queue is exclusively driven by this
        // (render) thread for per-frame submissions — see g_pendingLoadPath.
        if (g_loadRequested.exchange(false, std::memory_order_acquire)) {
            std::string path;
            {
                std::lock_guard<std::mutex> lock(g_pendingLoadPathMutex);
                path = std::move(g_pendingLoadPath);
                g_pendingLoadPath.clear();
            }
            if (!path.empty()) {
                LOG_INFO("Loading 3DGS scene: %s", path.c_str());
                std::lock_guard<std::mutex> lock(g_sceneMutex);
                if (g_gsRenderer.loadScene(path.c_str())) {
                    g_loadedFileName = GetPlyFilename(path);
                    LOG_INFO("Scene loaded: %s (%s)", g_loadedFileName.c_str(),
                        GetPlyFileSize(path).c_str());
                    ApplyAutoFitForLoadedScene_locked();
                } else {
                    LOG_ERROR("Failed to load scene: %s", path.c_str());
                    MessageBoxA(hwnd, "Failed to load scene file.\nThe file may be corrupt or unsupported.",
                        "Load Error", MB_OK | MB_ICONERROR);
                }
            }
        }

        // Handle rendering mode change (V=cycle, 0-8=direct)
        if (inputSnapshot.renderingModeChangeRequested) {
            if (xr->pfnRequestDisplayRenderingModeEXT && xr->session != XR_NULL_HANDLE) {
                xr->pfnRequestDisplayRenderingModeEXT(xr->session, inputSnapshot.currentRenderingMode);
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

        UpdatePerformanceStats(perfStats);
        UpdateCameraMovement(inputSnapshot, perfStats.deltaTime, xr->displayHeightM);

        // On Space-reset: shared UpdateCameraMovement returns to (0,0,0) + default
        // vHeight. For the splat demo, restore the per-scene auto-fit pose instead.
        if (resetRequested && g_fitValid.load()) {
            inputSnapshot.cameraPosX = g_fitCenter[0];
            inputSnapshot.cameraPosY = g_fitCenter[1];
            inputSnapshot.cameraPosZ = g_fitCenter[2];
            inputSnapshot.yaw = g_fitYaw;
            inputSnapshot.viewParams.virtualDisplayHeight = g_fitVHeight;
        }

        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_inputState.cameraPosX = inputSnapshot.cameraPosX;
            g_inputState.cameraPosY = inputSnapshot.cameraPosY;
            g_inputState.cameraPosZ = inputSnapshot.cameraPosZ;
            // Pose slerp and auto-orbit mutate yaw/pitch each frame — copy back.
            g_inputState.yaw = inputSnapshot.yaw;
            g_inputState.pitch = inputSnapshot.pitch;
            g_inputState.transitioning = inputSnapshot.transitioning;
            g_inputState.transitionT = inputSnapshot.transitionT;
            g_inputState.animationActive = inputSnapshot.animationActive;
            if (resetRequested) {
                g_inputState.viewParams = inputSnapshot.viewParams;
                // Auto-orbit always on; reset only clears the in-flight
                // transition. The shared UpdateCameraMovement may set
                // animateEnabled=false on Space — re-assert true here.
                g_inputState.animateEnabled = true;
                g_inputState.transitioning = false;
            }
        }

        PollEvents(*xr);

        if (xr->sessionRunning) {
            XrFrameState frameState;
            if (BeginFrame(*xr, frameState)) {
                XrCompositionLayerProjectionView projectionViews[2] = {};
                bool rendered = false;
                bool hudSubmitted = false;
                bool loadBtnSubmitted = false;

                if (frameState.shouldRender) {
                    if (LocateViews(*xr, frameState.predictedDisplayTime,
                        inputSnapshot.cameraPosX, -inputSnapshot.cameraPosY, inputSnapshot.cameraPosZ,
                        inputSnapshot.yaw, renderPitch,
                        inputSnapshot.viewParams)) {

                        XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
                        locateInfo.viewConfigurationType = xr->viewConfigType;
                        locateInfo.displayTime = frameState.predictedDisplayTime;
                        locateInfo.space = xr->localSpace;

                        XrViewState viewState = {XR_TYPE_VIEW_STATE};
                        uint32_t viewCount = 2;
                        XrView rawViews[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
                        xrLocateViews(xr->session, &locateInfo, &viewState, 2, &viewCount, rawViews);

                        bool monoMode = (xr->renderingModeCount > 0 && !xr->renderingModeDisplay3D[inputSnapshot.currentRenderingMode]);

                        // Per-view extent driven entirely by the current rendering
                        // mode's view_scale and the live window size. Atlas dims
                        // (cols × renderW, rows × renderH) are what gets written to
                        // the swapchain and snapshotted by the 'I' key. Swapchain
                        // creation already sized for the largest atlas, so no clamp.
                        // Falls back to the global recommendedViewScale (and 1.0 for
                        // mono) if the runtime didn't advertise per-mode info.
                        float scaleX, scaleY;
                        uint32_t cols, rows;
                        if (xr->renderingModeCount > 0) {
                            uint32_t mode = inputSnapshot.currentRenderingMode;
                            scaleX = xr->renderingModeScaleX[mode];
                            scaleY = xr->renderingModeScaleY[mode];
                            cols   = xr->renderingModeTileColumns[mode] ? xr->renderingModeTileColumns[mode] : 1u;
                            rows   = xr->renderingModeTileRows[mode]    ? xr->renderingModeTileRows[mode]    : 1u;
                        } else if (monoMode) {
                            scaleX = 1.0f; scaleY = 1.0f; cols = 1u; rows = 1u;
                        } else {
                            scaleX = xr->recommendedViewScaleX;
                            scaleY = xr->recommendedViewScaleY;
                            cols = 2u; rows = 1u;  // legacy SBS default
                        }
                        uint32_t renderW = (uint32_t)((double)windowW * scaleX);
                        uint32_t renderH = (uint32_t)((double)windowH * scaleY);
                        if (renderW == 0) renderW = 1;
                        if (renderH == 0) renderH = 1;

                        // App-side Kooima stereo projection
                        Display3DView stereoViews[2];
                        bool useAppProjection = (xr->hasDisplayInfoExt && xr->displayWidthM > 0.0f);
                        if (useAppProjection) {
                            float dispPxW = xr->displayPixelWidth > 0 ? (float)xr->displayPixelWidth : (float)xr->swapchain.width;
                            float dispPxH = xr->displayPixelHeight > 0 ? (float)xr->displayPixelHeight : (float)xr->swapchain.height;
                            float pxSizeX = xr->displayWidthM / dispPxW;
                            float pxSizeY = xr->displayHeightM / dispPxH;
                            float winW_m = (float)windowW * pxSizeX;
                            float winH_m = (float)windowH * pxSizeY;

                            // Window-relative Kooima: compute eye offset from window center
                            float eyeOffsetX = 0.0f, eyeOffsetY = 0.0f;
                            {
                                POINT clientOrigin = {0, 0};
                                ClientToScreen(hwnd, &clientOrigin);
                                HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                                MONITORINFO mi = {sizeof(mi)};
                                if (GetMonitorInfo(hMon, &mi)) {
                                    float winCenterX = (float)(clientOrigin.x - mi.rcMonitor.left) + windowW / 2.0f;
                                    float winCenterY = (float)(clientOrigin.y - mi.rcMonitor.top) + windowH / 2.0f;
                                    float dispW = (float)(mi.rcMonitor.right - mi.rcMonitor.left);
                                    float dispH = (float)(mi.rcMonitor.bottom - mi.rcMonitor.top);
                                    eyeOffsetX = (winCenterX - dispW / 2.0f) * pxSizeX;
                                    eyeOffsetY = -((winCenterY - dispH / 2.0f) * pxSizeY);
                                }
                            }

                            XrVector3f rawLeft = rawViews[0].pose.position;
                            rawLeft.x -= eyeOffsetX; rawLeft.y -= eyeOffsetY;
                            XrVector3f rawRight = rawViews[1].pose.position;
                            rawRight.x -= eyeOffsetX; rawRight.y -= eyeOffsetY;
                            // GsRenderer::updateUniforms Y-mirrors the world; displayPose
                            // below is fed in render frame (cameraPosY negated). The
                            // rawEyes must live in the same render frame so the asymmetric
                            // Kooima projection's eye-vs-display geometry stays consistent
                            // — otherwise vertical eye parallax comes out inverted.
                            rawLeft.y  = -rawLeft.y;
                            rawRight.y = -rawRight.y;
                            if (monoMode) {
                                XrVector3f center = {
                                    (rawLeft.x + rawRight.x) * 0.5f,
                                    (rawLeft.y + rawRight.y) * 0.5f,
                                    (rawLeft.z + rawRight.z) * 0.5f};
                                rawLeft = rawRight = center;
                            }

                            Display3DTunables tunables;
                            tunables.ipd_factor = inputSnapshot.viewParams.ipdFactor;
                            tunables.parallax_factor = inputSnapshot.viewParams.parallaxFactor;
                            tunables.perspective_factor = inputSnapshot.viewParams.perspectiveFactor;
                            tunables.virtual_display_height = inputSnapshot.viewParams.virtualDisplayHeight / inputSnapshot.viewParams.scaleFactor;

                            XrPosef displayPose;
                            XMVECTOR pOri = XMQuaternionRotationRollPitchYaw(
                                renderPitch, inputSnapshot.yaw, 0);
                            XMFLOAT4 q;
                            XMStoreFloat4(&q, pOri);
                            displayPose.orientation = {q.x, q.y, q.z, q.w};
                            // GsRenderer Y-mirrors the world inside updateUniforms (see comment
                            // in gs_renderer.cpp). The off-axis Kooima projection assumes the
                            // mirror is reflected in the displayPose passed to display3d_compute_views,
                            // so negate Y here to keep the eye-vs-display geometry consistent.
                            displayPose.position = {inputSnapshot.cameraPosX, -inputSnapshot.cameraPosY, inputSnapshot.cameraPosZ};

                            // nominalViewer in render frame too (Y mirrored) — used for
                            // parallax-factor lerp, must match the eye/displayPose frame.
                            XrVector3f nominalViewer = {xr->nominalViewerX, -xr->nominalViewerY, xr->nominalViewerZ};
                            Display3DScreen screen = {winW_m, winH_m};

                            XrVector3f rawEyes[2] = {rawLeft, rawRight};
                            display3d_compute_views(
                                rawEyes, 2, &nominalViewer,
                                &screen, &tunables, &displayPose,
                                0.01f, 100.0f, stereoViews);
                        }

                        // Double-click focus: center-eye ray through mouse, pick splat,
                        // smoothly re-pose the virtual display to face back along the ray.
                        if (inputSnapshot.teleportRequested && useAppProjection) {
                            float ndcX = 2.0f * inputSnapshot.teleportMouseX / (float)windowW - 1.0f;
                            float ndcY = -(2.0f * inputSnapshot.teleportMouseY / (float)windowH - 1.0f);

                            float dispPxW2 = xr->displayPixelWidth > 0 ? (float)xr->displayPixelWidth : (float)xr->swapchain.width;
                            float dispPxH2 = xr->displayPixelHeight > 0 ? (float)xr->displayPixelHeight : (float)xr->swapchain.height;
                            float winW_m2 = (float)windowW * (xr->displayWidthM / dispPxW2);
                            float winH_m2 = (float)windowH * (xr->displayHeightM / dispPxH2);
                            Display3DScreen screen2 = {winW_m2, winH_m2};
                            Display3DTunables tunables2;
                            tunables2.ipd_factor = inputSnapshot.viewParams.ipdFactor;
                            tunables2.parallax_factor = inputSnapshot.viewParams.parallaxFactor;
                            tunables2.perspective_factor = inputSnapshot.viewParams.perspectiveFactor;
                            tunables2.virtual_display_height = inputSnapshot.viewParams.virtualDisplayHeight / inputSnapshot.viewParams.scaleFactor;

                            // Center-eye Display3DView from averaged processed display-space eye.
                            XrVector3f centerEyeDisp = {
                                (stereoViews[0].eye_display.x + stereoViews[1].eye_display.x) * 0.5f,
                                (stereoViews[0].eye_display.y + stereoViews[1].eye_display.y) * 0.5f,
                                (stereoViews[0].eye_display.z + stereoViews[1].eye_display.z) * 0.5f};
                            float m2v_post = tunables2.virtual_display_height / winH_m2;
                            float es = tunables2.perspective_factor * m2v_post;
                            XrVector3f centerEyeProcessed = (es != 0.0f)
                                ? XrVector3f{centerEyeDisp.x / es, centerEyeDisp.y / es, centerEyeDisp.z / es}
                                : centerEyeDisp;
                            // Use a real-world-frame displayPose for picking — the unproject
                            // ray needs to be in the same frame as the splats (un-Y-flipped
                            // world). The render-time displayPose has its Y negated for the
                            // renderer's view-stage Y mirror; using that here would put
                            // rayOrigin in a mirror frame and pickGaussian would intersect
                            // against splats in the wrong frame.
                            XrPosef displayPoseLocal;
                            XMVECTOR pOri = XMQuaternionRotationRollPitchYaw(renderPitch, inputSnapshot.yaw, 0);
                            XMFLOAT4 q;
                            XMStoreFloat4(&q, pOri);
                            displayPoseLocal.orientation = {q.x, q.y, q.z, q.w};
                            displayPoseLocal.position = {inputSnapshot.cameraPosX, inputSnapshot.cameraPosY, inputSnapshot.cameraPosZ};
                            Display3DView centerView;
                            display3d_compute_view(&centerEyeProcessed, &screen2, &tunables2,
                                                   &displayPoseLocal, 0.01f, 100.0f, &centerView);

                            XrVector3f rayOriginV, rayDirV;
                            display3d_unproject_ndc_to_ray(ndcX, ndcY,
                                centerView.view_matrix, centerView.projection_matrix,
                                &rayOriginV, &rayDirV);

                            float rayOrigin[3] = {rayOriginV.x, rayOriginV.y, rayOriginV.z};
                            float rayDir[3]    = {rayDirV.x,    rayDirV.y,    rayDirV.z};
                            float hitPos[3];
                            std::lock_guard<std::mutex> sceneLock(g_sceneMutex);
                            if (g_gsRenderer.pickGaussian(rayOrigin, rayDir, hitPos)) {
                                // Both endpoints stored in WORLD frame (the same frame as
                                // inputSnapshot.cameraPosX/Y/Z and inputSnapshot.pitch/yaw)
                                // so the slerp's writeback decodes back into world-frame
                                // pitch/yaw. displayPoseLocal.orientation was built from
                                // renderPitch (-pitch) for the click-pick centerView; we
                                // must NOT reuse that render-frame quaternion here, else
                                // the slerp's `state.pitch = asin(fwd.y)` decode produces
                                // a sign-flipped pitch and the display rotates on teleport.
                                XMVECTOR worldOriQ = XMQuaternionRotationRollPitchYaw(
                                    inputSnapshot.pitch, inputSnapshot.yaw, 0);
                                XMFLOAT4 wq;
                                XMStoreFloat4(&wq, worldOriQ);
                                XrQuaternionf worldOri = {wq.x, wq.y, wq.z, wq.w};

                                XrPosef fromWorld;
                                fromWorld.orientation = worldOri;
                                fromWorld.position = {inputSnapshot.cameraPosX, inputSnapshot.cameraPosY, inputSnapshot.cameraPosZ};
                                XrPosef target;
                                target.position = {hitPos[0], hitPos[1], hitPos[2]};
                                target.orientation = worldOri;  // preserve current orientation — translate-only
                                std::lock_guard<std::mutex> inputLock(g_inputMutex);
                                g_inputState.transitionFrom = fromWorld;
                                g_inputState.transitionTo = target;
                                g_inputState.transitionT = 0.0f;
                                g_inputState.transitioning = true;
                                LOG_INFO("Focus on splat (%.3f, %.3f, %.3f)",
                                    hitPos[0], hitPos[1], hitPos[2]);
                            }
                        }

                        rendered = true;
                        int eyeCount = monoMode ? 1 : 2;

                        // Mono center eye
                        XMMATRIX monoViewMatrix, monoProjMatrix;
                        XrPosef monoPose = rawViews[0].pose;
                        if (monoMode) {
                            monoPose.position.x = (rawViews[0].pose.position.x + rawViews[1].pose.position.x) * 0.5f;
                            monoPose.position.y = (rawViews[0].pose.position.y + rawViews[1].pose.position.y) * 0.5f;
                            monoPose.position.z = (rawViews[0].pose.position.z + rawViews[1].pose.position.z) * 0.5f;

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
                                    renderPitch, inputSnapshot.yaw, 0);
                                XMVECTOR playerPos = XMVectorSet(
                                    inputSnapshot.cameraPosX, -inputSnapshot.cameraPosY,
                                    inputSnapshot.cameraPosZ, 0.0f);
                                XMVECTOR worldPos = XMVector3Rotate(centerLocalPos * eyeScale, playerOri) + playerPos;
                                XMVECTOR worldOri = XMQuaternionMultiply(localOri, playerOri);
                                XMMATRIX rot = XMMatrixTranspose(XMMatrixRotationQuaternion(worldOri));
                                XMFLOAT3 wp;
                                XMStoreFloat3(&wp, worldPos);
                                monoViewMatrix = XMMatrixTranslation(-wp.x, -wp.y, -wp.z) * rot;
                            }
                        }

                        // Build per-eye view/projection matrices (column-major float[16])
                        float viewMat[2][16], projMat[2][16];
                        for (int eye = 0; eye < eyeCount; eye++) {
                            if (useAppProjection) {
                                int srcEye = monoMode ? 0 : eye;
                                memcpy(viewMat[eye], stereoViews[srcEye].view_matrix, sizeof(float) * 16);
                                memcpy(projMat[eye], stereoViews[srcEye].projection_matrix, sizeof(float) * 16);
                            } else {
                                // Fallback: use DirectXMath mono matrices, store as column-major
                                XMMATRIX v = monoMode ? monoViewMatrix :
                                    XMMatrixLookAtRH(XMLoadFloat3((XMFLOAT3*)&rawViews[eye].pose.position),
                                        XMLoadFloat3((XMFLOAT3*)&rawViews[eye].pose.position) + XMVectorSet(0,0,-1,0),
                                        XMVectorSet(0,1,0,0));
                                XMMATRIX p = monoMode ? monoProjMatrix : xr->projMatrices[0];
                                // XMMatrix is row-major; transpose to get column-major for shader
                                XMMATRIX vT = XMMatrixTranspose(v);
                                XMMATRIX pT = XMMatrixTranspose(p);
                                XMStoreFloat4x4((XMFLOAT4X4*)viewMat[eye], vT);
                                XMStoreFloat4x4((XMFLOAT4X4*)projMat[eye], pT);
                            }
                        }

                        uint32_t imageIndex;
                        if (AcquireSwapchainImage(*xr, imageIndex)) {
                            VkFormat colorFormat = (VkFormat)xr->swapchain.format;

                            bool hasGsScene;
                            {
                                std::lock_guard<std::mutex> lock(g_sceneMutex);
                                hasGsScene = g_gsRenderer.hasScene();
                            }

                            if (hasGsScene) {
                                for (int eye = 0; eye < eyeCount; eye++) {
                                    // Row-major eye placement in the atlas; for 2×1 SBS
                                    // this is (0, renderW) at row 0; for mono (cols=1)
                                    // it collapses to (0, 0).
                                    uint32_t col = (uint32_t)eye % cols;
                                    uint32_t row = (uint32_t)eye / cols;
                                    uint32_t vpX = col * renderW;
                                    uint32_t vpY = row * renderH;
                                    g_gsRenderer.renderEye(
                                        (*swapchainVkImages)[imageIndex], colorFormat,
                                        xr->swapchain.width, xr->swapchain.height,
                                        vpX, vpY, renderW, renderH,
                                        viewMat[eye], projMat[eye]);
                                }
                            } else {
                                RenderPlaceholder(vkDevice, graphicsQueue, renderCmdPool,
                                    (*swapchainVkImages)[imageIndex], xr->swapchain.width, xr->swapchain.height);
                            }

                            // 'I' key: dump the rendered atlas (cols × rows × renderW × renderH)
                            // to %USERPROFILE%\Pictures\DisplayXR\<scene>-<n>_<cols>x<rows>.png.
                            // Atlas dims come from the current rendering mode's tile layout
                            // and view scale — works for mono (1×1), SBS (2×1), and any other
                            // layout the runtime advertises. Filename auto-increments.
                            if (g_captureAtlasRequested.exchange(false)) {
                                if (hasGsScene) {
                                    uint32_t atlasW = cols * renderW;
                                    uint32_t atlasH = rows * renderH;
                                    if (atlasW <= xr->swapchain.width &&
                                        atlasH <= xr->swapchain.height) {
                                        std::string sceneName;
                                        {
                                            std::lock_guard<std::mutex> lock(g_sceneMutex);
                                            sceneName = g_loadedFileName;
                                        }
                                        // Strip extension from scene filename
                                        // (e.g. "butterfly.spz" → "butterfly").
                                        auto dot = sceneName.find_last_of('.');
                                        std::string stem = (dot == std::string::npos)
                                            ? sceneName : sceneName.substr(0, dot);
                                        if (stem.empty()) stem = "scene";
                                        std::string outPath = dxr_capture::MakeCapturePath(
                                            stem, cols, rows);
                                        // GS writes via compute imageStore — bytes are
                                        // linear even on an sRGB swapchain; tell the helper
                                        // to mirror the runtime's display-side decode.
                                        bool ok = dxr_capture::CaptureAtlasRegionVk(
                                            vkDevice, physDevice,
                                            graphicsQueue, renderCmdPool,
                                            (*swapchainVkImages)[imageIndex],
                                            (int)colorFormat,
                                            xr->swapchain.width, xr->swapchain.height,
                                            0, 0, atlasW, atlasH, outPath,
                                            /*linearBytesInSrgbImage=*/true);
                                        if (ok) {
                                            LOG_INFO("Captured %ux%u (%ux%u tiles) -> %s",
                                                     atlasW, atlasH, cols, rows, outPath.c_str());
                                            dxr_capture::PostFlashRequest(hwnd);
                                        }
                                    } else {
                                        LOG_WARN("Capture skipped: atlas %ux%u exceeds swapchain %ux%u",
                                                 atlasW, atlasH, xr->swapchain.width, xr->swapchain.height);
                                    }
                                } else {
                                    LOG_WARN("Capture skipped: no scene loaded");
                                }
                            }

                            for (int eye = 0; eye < eyeCount; eye++) {
                                uint32_t col = (uint32_t)eye % cols;
                                uint32_t row = (uint32_t)eye / cols;
                                projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                                projectionViews[eye].subImage.swapchain = xr->swapchain.swapchain;
                                projectionViews[eye].subImage.imageRect.offset = {
                                    (int32_t)(col * renderW), (int32_t)(row * renderH)};
                                projectionViews[eye].subImage.imageRect.extent = {
                                    (int32_t)renderW, (int32_t)renderH};
                                projectionViews[eye].subImage.imageArrayIndex = 0;
                                projectionViews[eye].pose = monoMode ? monoPose : rawViews[eye].pose;
                                projectionViews[eye].fov = useAppProjection ?
                                    stereoViews[monoMode ? 0 : eye].fov :
                                    (monoMode ? rawViews[0].fov : rawViews[eye].fov);
                            }
                            ReleaseSwapchainImage(*xr);
                        } else {
                            rendered = false;
                        }

                        // Render HUD to window-space layer swapchain. Submitted
                        // every frame so the chrome buttons (Open / Mode) stay
                        // visible — the TAB toggle only hides the body backdrop
                        // and text via the `drawBody` flag below.
                        if (rendered && hud && xr->hasHudSwapchain && hudSwapchainImages) {
                            uint32_t hudImageIndex;
                            if (AcquireHudSwapchainImage(*xr, hudImageIndex)) {
                                std::wstring sessionText(xr->systemName, xr->systemName + strlen(xr->systemName));
                                sessionText += L"\nSession: ";
                                sessionText += FormatSessionState((int)xr->sessionState);
                                std::wstring modeText = xr->hasWin32WindowBindingExt ?
                                    L"XR_EXT_win32_window_binding: ACTIVE (Vulkan + 3DGS)" :
                                    L"XR_EXT_win32_window_binding: NOT AVAILABLE";

                                // Scene info
                                std::wstring sceneText = L"\n--- 3DGS Scene ---";
                                {
                                    std::lock_guard<std::mutex> lock(g_sceneMutex);
                                    if (g_gsRenderer.hasScene()) {
                                        std::wstring fname(g_loadedFileName.begin(), g_loadedFileName.end());
                                        sceneText += L"\nLoaded: " + fname;
                                    } else {
                                        sceneText += L"\nNo scene loaded (press L or click Load)";
                                    }
                                }
                                modeText += sceneText;

                                // Per-view extent for HUD display — same formula as the
                                // render path (window × view_scale of the current mode).
                                float dispScaleX, dispScaleY;
                                if (xr->renderingModeCount > 0) {
                                    uint32_t mode = inputSnapshot.currentRenderingMode;
                                    dispScaleX = xr->renderingModeScaleX[mode];
                                    dispScaleY = xr->renderingModeScaleY[mode];
                                } else {
                                    dispScaleX = xr->recommendedViewScaleX;
                                    dispScaleY = xr->recommendedViewScaleY;
                                }
                                uint32_t dispRenderW = (uint32_t)((double)windowW * dispScaleX);
                                uint32_t dispRenderH = (uint32_t)((double)windowH * dispScaleY);
                                if (dispRenderW == 0) dispRenderW = 1;
                                if (dispRenderH == 0) dispRenderH = 1;
                                std::wstring perfText = FormatPerformanceInfo(perfStats.fps, perfStats.frameTimeMs,
                                    dispRenderW, dispRenderH, windowW, windowH);
                                std::wstring dispText = FormatDisplayInfo(xr->displayWidthM, xr->displayHeightM,
                                    xr->nominalViewerX, xr->nominalViewerY, xr->nominalViewerZ);
                                dispText += L"\n" + FormatScaleInfo(xr->recommendedViewScaleX, xr->recommendedViewScaleY);
                                dispText += L"\n" + FormatMode(inputSnapshot.currentRenderingMode, xr->pfnRequestDisplayRenderingModeEXT != nullptr,
                                    (xr->renderingModeCount > 0 && inputSnapshot.currentRenderingMode < xr->renderingModeCount) ? xr->renderingModeNames[inputSnapshot.currentRenderingMode] : nullptr,
                                    xr->renderingModeCount,
                                    xr->renderingModeCount > 0 ? xr->renderingModeDisplay3D[inputSnapshot.currentRenderingMode] : true);
                                std::wstring eyeText = FormatEyeTrackingInfo(
                                    xr->eyePositions, (uint32_t)eyeCount,
                                    xr->eyeTrackingActive, xr->isEyeTracking,
                                    xr->activeEyeTrackingMode, xr->supportedEyeTrackingModes);

                                float fwdX = -sinf(inputSnapshot.yaw) * cosf(renderPitch);
                                float fwdY =  sinf(renderPitch);
                                float fwdZ = -cosf(inputSnapshot.yaw) * cosf(renderPitch);
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
                                    wchar_t vhBuf[96];
                                    int depthPct = (int)(inputSnapshot.viewParams.ipdFactor * 100.0f + 0.5f);
                                    const wchar_t* orbitLbl = inputSnapshot.animateEnabled
                                        ? (inputSnapshot.animationActive ? L"ON (running)" : L"ON (idle countdown)")
                                        : L"OFF";
                                    swprintf(vhBuf, 96, L"\nvHeight: %.3f  m2v: %.3f\nDepth/IPD: %d%%  Auto-Orbit: %s",
                                        inputSnapshot.viewParams.virtualDisplayHeight, hudM2v, depthPct, orbitLbl);
                                    stereoText += vhBuf;
                                }
                                std::wstring helpText = L"[WASDEQ] Move | [LMB-drag] Rotate | [Scroll] Zoom\n"
                                    L"[DblClick] Focus | [-/=] Depth | [Space] Reset\n"
                                    L"[M] Auto-Orbit | [V] Mode | [L] Load | [Tab] HUD | [ESC] Quit";

                                // Top-bar buttons. Translate window-fraction click
                                // regions into HUD-pixel coords using the HUD's
                                // own footprint constants (the HUD covers window
                                // fraction (0,0) → (HUD_WIDTH_FRACTION,
                                // HUD_HEIGHT_FRACTION); inside that, pixel space
                                // is (0,0) → (hudWidth, hudHeight)).
                                std::vector<HudButton> buttons;
                                {
                                    auto toHudPx = [&](float xf, float yf, float wf, float hf, const std::wstring& label) {
                                        HudButton b;
                                        b.label = label;
                                        b.x = (xf / HUD_WIDTH_FRACTION)  * (float)hudWidth;
                                        b.y = (yf / HUD_HEIGHT_FRACTION) * (float)hudHeight;
                                        b.width  = (wf / HUD_WIDTH_FRACTION)  * (float)hudWidth;
                                        b.height = (hf / HUD_HEIGHT_FRACTION) * (float)hudHeight;
                                        return b;
                                    };
                                    buttons.push_back(toHudPx(
                                        OPEN_BTN_X_FRACTION, OPEN_BTN_Y_FRACTION,
                                        OPEN_BTN_WIDTH_FRACTION, OPEN_BTN_HEIGHT_FRACTION,
                                        L"Open…"));
                                    std::wstring modeLabel = L"Mode";
                                    if (xr->renderingModeCount > 0 &&
                                        inputSnapshot.currentRenderingMode < xr->renderingModeCount &&
                                        xr->renderingModeNames[inputSnapshot.currentRenderingMode]) {
                                        const char* nm = xr->renderingModeNames[inputSnapshot.currentRenderingMode];
                                        modeLabel = L"Mode: " + std::wstring(nm, nm + strlen(nm));
                                    }
                                    buttons.push_back(toHudPx(
                                        MODE_BTN_X_FRACTION, MODE_BTN_Y_FRACTION,
                                        MODE_BTN_WIDTH_FRACTION, MODE_BTN_HEIGHT_FRACTION,
                                        modeLabel));
                                }

                                uint32_t srcRowPitch = 0;
                                const void* pixels = RenderHudAndMap(*hud, &srcRowPitch, sessionText, modeText, perfText, dispText, eyeText,
                                    cameraText, stereoText, helpText, buttons,
                                    /*drawBody=*/inputSnapshot.hudVisible,
                                    /*bodyAtBottom=*/true);
                                if (pixels) {
                                    const uint8_t* src = (const uint8_t*)pixels;
                                    uint8_t* dst = (uint8_t*)hudStagingMapped;
                                    for (uint32_t row = 0; row < hudHeight; row++) {
                                        memcpy(dst + row * hudWidth * 4, src + row * srcRowPitch, hudWidth * 4);
                                    }
                                    UnmapHud(*hud);
                                }

                                // Copy staging buffer to HUD swapchain image
                                VkCommandBufferAllocateInfo cmdAllocInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
                                cmdAllocInfo.commandPool = hudCmdPool;
                                cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                                cmdAllocInfo.commandBufferCount = 1;

                                VkCommandBuffer cmdBuf;
                                vkAllocateCommandBuffers(vkDevice, &cmdAllocInfo, &cmdBuf);

                                VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                                beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                                vkBeginCommandBuffer(cmdBuf, &beginInfo);

                                VkImage hudImg = (*hudSwapchainImages)[hudImageIndex].image;

                                VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                                barrier.srcAccessMask = 0;
                                barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                                barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                                barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                                barrier.image = hudImg;
                                barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                                vkCmdPipelineBarrier(cmdBuf,
                                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    0, 0, nullptr, 0, nullptr, 1, &barrier);

                                VkBufferImageCopy region = {};
                                region.bufferRowLength = hudWidth;
                                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                                region.imageOffset = {0, 0, 0};
                                region.imageExtent = {hudWidth, hudHeight, 1};
                                vkCmdCopyBufferToImage(cmdBuf, hudStagingBuffer, hudImg,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

                                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                                barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                                barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                                barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                                vkCmdPipelineBarrier(cmdBuf,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                    0, 0, nullptr, 0, nullptr, 1, &barrier);

                                vkEndCommandBuffer(cmdBuf);

                                VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
                                submitInfo.commandBufferCount = 1;
                                submitInfo.pCommandBuffers = &cmdBuf;
                                vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
                                vkQueueWaitIdle(graphicsQueue);

                                vkFreeCommandBuffers(vkDevice, hudCmdPool, 1, &cmdBuf);

                                ReleaseHudSwapchainImage(*xr);
                                hudSubmitted = true;
                            }
                        }

                    }
                }

                // Submit frame
                uint32_t submitViewCount = (xr->renderingModeCount > 0 && inputSnapshot.currentRenderingMode < xr->renderingModeCount) ? xr->renderingModeViewCounts[inputSnapshot.currentRenderingMode] : 2;
                if (rendered && hudSubmitted) {
                    // Layer spans the full HUD footprint (full window height
                    // by HUD_WIDTH_FRACTION). Empty regions (alpha=0) are
                    // invisible thanks to the alpha-blended composite path
                    // in the vk_native compositor.
                    EndFrameWithWindowSpaceHud(*xr, frameState.predictedDisplayTime, projectionViews,
                        0.0f, 0.0f, HUD_WIDTH_FRACTION, HUD_HEIGHT_FRACTION, 0.0f, submitViewCount);
                } else if (rendered) {
                    EndFrame(*xr, frameState.predictedDisplayTime, projectionViews, submitViewCount);
                } else {
                    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
                    endInfo.displayTime = frameState.predictedDisplayTime;
                    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                    endInfo.layerCount = 0;
                    endInfo.layers = nullptr;
                    xrEndFrame(xr->session, &endInfo);
                }
            }
        } else {
            Sleep(100);
        }
    }

    if (renderCmdPool != VK_NULL_HANDLE)
        vkDestroyCommandPool(vkDevice, renderCmdPool, nullptr);

    if (xr->exitRequested && g_running.load()) {
        PostMessage(hwnd, WM_CLOSE, 0, 0);
    }

    LOG_INFO("[RenderThread] Exiting");
}

// Global crash handler
static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* exInfo) {
    const char* excName = "UNKNOWN";
    switch (exInfo->ExceptionRecord->ExceptionCode) {
        case EXCEPTION_ACCESS_VIOLATION:      excName = "ACCESS_VIOLATION"; break;
        case EXCEPTION_STACK_OVERFLOW:        excName = "STACK_OVERFLOW"; break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    excName = "INT_DIVIDE_BY_ZERO"; break;
        case EXCEPTION_ILLEGAL_INSTRUCTION:   excName = "ILLEGAL_INSTRUCTION"; break;
        case EXCEPTION_IN_PAGE_ERROR:         excName = "IN_PAGE_ERROR"; break;
        case EXCEPTION_GUARD_PAGE:            excName = "GUARD_PAGE"; break;
    }
    LOG_ERROR("!!! UNHANDLED EXCEPTION: %s (0x%08X) at address 0x%p !!!",
        excName, exInfo->ExceptionRecord->ExceptionCode,
        exInfo->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_CONTINUE_SEARCH;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    SetUnhandledExceptionFilter(CrashHandler);

    if (!InitializeLogging(APP_NAME)) {
        MessageBox(nullptr, L"Failed to initialize logging", L"Warning", MB_OK | MB_ICONWARNING);
    }

    LOG_INFO("=== SR 3DGS OpenXR Ext Vulkan Application ===");

    HWND hwnd = CreateAppWindow(hInstance, g_windowWidth, g_windowHeight);
    if (!hwnd) {
        LOG_ERROR("Failed to create window");
        ShutdownLogging();
        return 1;
    }

    // Add DisplayXR to DLL search path
    {
        HKEY hKey;
        char installPath[MAX_PATH] = {0};
        DWORD pathSize = sizeof(installPath);
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\DisplayXR\\Runtime", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            if (RegQueryValueExA(hKey, "InstallPath", nullptr, nullptr, (LPBYTE)installPath, &pathSize) == ERROR_SUCCESS) {
                LOG_INFO("Adding DisplayXR install path to DLL search: %s", installPath);
                SetDllDirectoryA(installPath);
            }
            RegCloseKey(hKey);
        }
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

    // Try to load sim_display_set_output_mode
    {
        HMODULE rtModule = GetModuleHandleA("openxr_displayxr.dll");
        if (!rtModule) rtModule = GetModuleHandleA("openxr_displayxr");
        if (rtModule) {
            g_pfnSetOutputMode = (PFN_sim_display_set_output_mode)GetProcAddress(rtModule, "sim_display_set_output_mode");
        }
        LOG_INFO("sim_display output mode: %s", g_pfnSetOutputMode ? "available" : "not available");
    }

    // Get Vulkan graphics requirements
    if (!GetVulkanGraphicsRequirements(xr)) {
        LOG_ERROR("Failed to get Vulkan graphics requirements");
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create Vulkan instance
    VkInstance vkInstance = VK_NULL_HANDLE;
    if (!CreateVulkanInstance(xr, vkInstance)) {
        LOG_ERROR("Vulkan instance creation failed");
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Get physical device
    VkPhysicalDevice physDevice = VK_NULL_HANDLE;
    if (!GetVulkanPhysicalDevice(xr, vkInstance, physDevice)) {
        LOG_ERROR("Failed to get Vulkan physical device");
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Get device extensions
    std::vector<const char*> deviceExtensions;
    std::vector<std::string> extensionStorage;
    if (!GetVulkanDeviceExtensions(xr, vkInstance, physDevice, deviceExtensions, extensionStorage)) {
        LOG_ERROR("Failed to get Vulkan device extensions");
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Find graphics queue family
    uint32_t queueFamilyIndex = 0;
    if (!FindGraphicsQueueFamily(physDevice, queueFamilyIndex)) {
        LOG_ERROR("No graphics queue family found");
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create logical device
    VkDevice vkDevice = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    if (!CreateVulkanDevice(physDevice, queueFamilyIndex, deviceExtensions, vkDevice, graphicsQueue)) {
        LOG_ERROR("Vulkan device creation failed");
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create session
    if (!CreateSession(xr, vkInstance, physDevice, vkDevice, queueFamilyIndex, 0, hwnd)) {
        LOG_ERROR("OpenXR session creation failed");
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

    if (!CreateSwapchain(xr)) {
        LOG_ERROR("Swapchain creation failed");
        CleanupOpenXR(xr);
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        ShutdownLogging();
        return 1;
    }

    // Enumerate Vulkan swapchain images
    std::vector<XrSwapchainImageVulkanKHR> swapchainImages;
    {
        uint32_t count = xr.swapchain.imageCount;
        swapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)swapchainImages.data());
        LOG_INFO("Enumerated %u Vulkan swapchain images", count);

        // Extract VkImage handles for render thread access
    }
    std::vector<VkImage> swapchainVkImages(swapchainImages.size());
    for (uint32_t i = 0; i < (uint32_t)swapchainImages.size(); i++) {
        swapchainVkImages[i] = swapchainImages[i].image;
    }

    // Initialize 3DGS renderer with the OpenXR Vulkan device
    {
        uint32_t renderW = xr.swapchain.width;   // Full width — mono uses entire swapchain
        uint32_t renderH = xr.swapchain.height;
        if (!g_gsRenderer.init(vkInstance, physDevice, vkDevice, graphicsQueue,
                               queueFamilyIndex, renderW, renderH)) {
            LOG_WARN("3DGS renderer init failed - scene rendering will not be available");
        } else {
            TryAutoLoadBundledScene();
        }
    }

    // Initialize HUD renderer
    uint32_t hudWidth = (uint32_t)(xr.swapchain.width * HUD_WIDTH_FRACTION);
    uint32_t hudHeight = (uint32_t)(xr.swapchain.height * HUD_HEIGHT_FRACTION);

    HudRenderer hudRenderer = {};
    uint32_t hudFontBaseHeight = (uint32_t)(xr.swapchain.height * HUD_FONT_BASE_FRACTION);
    bool hudOk = InitializeHudRenderer(hudRenderer, hudWidth, hudHeight, hudFontBaseHeight);
    if (!hudOk) {
        LOG_WARN("HUD renderer init failed - HUD will not be displayed");
    }

    // Create HUD swapchain
    std::vector<XrSwapchainImageVulkanKHR> hudSwapImages;
    if (hudOk) {
        if (CreateHudSwapchain(xr, hudWidth, hudHeight)) {
            uint32_t count = xr.hudSwapchain.imageCount;
            hudSwapImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
            xrEnumerateSwapchainImages(xr.hudSwapchain.swapchain, count, &count,
                (XrSwapchainImageBaseHeader*)hudSwapImages.data());
            LOG_INFO("HUD swapchain: enumerated %u Vulkan images", count);
        } else {
            LOG_WARN("HUD swapchain creation failed - HUD will not be displayed");
            hudOk = false;
        }
    }

    // Create HUD staging buffer
    VkBuffer hudStagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory hudStagingMemory = VK_NULL_HANDLE;
    void* hudStagingMapped = nullptr;
    VkCommandPool hudCmdPool = VK_NULL_HANDLE;

    if (hudOk) {
        VkBufferCreateInfo bufInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufInfo.size = (VkDeviceSize)hudWidth * hudHeight * 4;
        bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(vkDevice, &bufInfo, nullptr, &hudStagingBuffer) != VK_SUCCESS) {
            LOG_WARN("Failed to create HUD staging buffer");
            hudOk = false;
        }

        if (hudOk) {
            VkMemoryRequirements memReqs;
            vkGetBufferMemoryRequirements(vkDevice, hudStagingBuffer, &memReqs);

            VkPhysicalDeviceMemoryProperties memProps;
            vkGetPhysicalDeviceMemoryProperties(physDevice, &memProps);

            uint32_t memTypeIndex = UINT32_MAX;
            for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
                if ((memReqs.memoryTypeBits & (1 << i)) &&
                    (memProps.memoryTypes[i].propertyFlags &
                        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                    memTypeIndex = i;
                    break;
                }
            }

            if (memTypeIndex == UINT32_MAX) {
                LOG_WARN("No suitable memory type for HUD staging buffer");
                hudOk = false;
            } else {
                VkMemoryAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                allocInfo.allocationSize = memReqs.size;
                allocInfo.memoryTypeIndex = memTypeIndex;
                vkAllocateMemory(vkDevice, &allocInfo, nullptr, &hudStagingMemory);
                vkBindBufferMemory(vkDevice, hudStagingBuffer, hudStagingMemory, 0);
                vkMapMemory(vkDevice, hudStagingMemory, 0, bufInfo.size, 0, &hudStagingMapped);
            }
        }

        if (hudOk) {
            VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            poolInfo.queueFamilyIndex = queueFamilyIndex;
            if (vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &hudCmdPool) != VK_SUCCESS) {
                LOG_WARN("Failed to create HUD command pool");
                hudOk = false;
            }
        }

        if (hudOk) {
            LOG_INFO("HUD Vulkan resources created (%ux%u)", hudWidth, hudHeight);
        }
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    LOG_INFO("");
    LOG_INFO("=== Entering main loop ===");
    LOG_INFO("Controls: WASDEQ=Move  LMB-drag=Rotate  Scroll=Zoom  DblClick=Focus");
    LOG_INFO("          -/= Depth  Space=Reset  M=Auto-Orbit  V=Mode");
    LOG_INFO("          L=Load  Tab=HUD  F11=Fullscreen  ESC=Quit");
    LOG_INFO("");

    g_inputState.viewParams.virtualDisplayHeight = kFallbackVirtualDisplayHeightM;
    g_inputState.renderingModeCount = xr.renderingModeCount;
    g_inputState.hudVisible = false;     // hidden by default; toggle with Tab
    g_inputState.animateEnabled = true;  // auto-orbit always on after 10 s idle
    {
        using namespace std::chrono;
        g_inputState.lastInputTimeSec = (double)duration_cast<microseconds>(
            high_resolution_clock::now().time_since_epoch()).count() * 1e-6;
    }

    std::thread renderThread(RenderThreadFunc, hwnd, &xr, vkDevice, graphicsQueue,
        queueFamilyIndex, vkInstance, physDevice,
        &swapchainVkImages,
        hudOk ? &hudRenderer : nullptr, hudWidth, hudHeight,
        hudStagingBuffer, hudStagingMapped, hudCmdPool,
        hudOk ? &hudSwapImages : nullptr,
        (VkCommandPool)VK_NULL_HANDLE, (std::vector<XrSwapchainImageVulkanKHR>*)nullptr,
        (uint32_t)0, (uint32_t)0);

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

    g_gsRenderer.cleanup();

    if (hudCmdPool != VK_NULL_HANDLE) vkDestroyCommandPool(vkDevice, hudCmdPool, nullptr);
    if (hudStagingBuffer != VK_NULL_HANDLE) {
        vkUnmapMemory(vkDevice, hudStagingMemory);
        vkDestroyBuffer(vkDevice, hudStagingBuffer, nullptr);
    }
    if (hudStagingMemory != VK_NULL_HANDLE) vkFreeMemory(vkDevice, hudStagingMemory, nullptr);
    if (hudOk) CleanupHudRenderer(hudRenderer);

    g_xr = nullptr;
    CleanupOpenXR(xr);
    vkDestroyDevice(vkDevice, nullptr);
    vkDestroyInstance(vkInstance, nullptr);

    DestroyWindow(hwnd);
    UnregisterClass(WINDOW_CLASS, hInstance);

    LOG_INFO("Application shutdown complete");
    ShutdownLogging();

    return 0;
}
