// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  SR Cube OpenXR - Legacy hosted mode with OpenGL (no XR_EXT_display_info)
 *
 * Legacy variant: does NOT enable XR_EXT_display_info.
 * Uses recommendedImageRectWidth * 2 (compromise scaling).
 * Only V toggle (2D/3D) works — no 1/2/3 mode switching.
 *
 * Creates a hidden dummy window + GL context to satisfy the OpenXR GL binding
 * requirement. DisplayXR creates its own presentation window via the GL
 * compositor's own_window path.
 *
 * Input is handled by DisplayXR's qwerty driver:
 * - WASD: Move camera
 * - Mouse drag: Look around
 * - ESC: Close window and exit
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>

#include "logging.h"
#include "gl_renderer.h"
#include "gl_functions.h"
#include "xr_session.h"

#include <chrono>
#include <vector>

using namespace DirectX;

// Application name for logging
static const char* APP_NAME = "cube_hosted_legacy_gl_win";

// Global state
static bool g_running = true;

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

// Dummy window proc for the hidden GL context window
static LRESULT CALLBACK DummyWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// Create hidden dummy window with GL context (temp legacy → core profile 3.3)
static HWND CreateDummyWindow(HINSTANCE hInstance) {
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = DummyWindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"DummyGLContextClass";

    DWORD err = 0;
    if (!RegisterClassEx(&wc)) {
        err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            LOG_ERROR("Failed to register dummy window class, error: %lu", err);
            return nullptr;
        }
    }

    HWND hwnd = CreateWindowEx(0, L"DummyGLContextClass", L"DummyGL",
        0, 0, 0, 1, 1, nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) {
        LOG_ERROR("Failed to create dummy window, error: %lu", GetLastError());
        return nullptr;
    }

    return hwnd;
}

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

// Main entry point
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    // Initialize logging
    if (!InitializeLogging(APP_NAME)) {
        MessageBox(nullptr, L"Failed to initialize logging", L"Warning", MB_OK | MB_ICONWARNING);
    }

    LOG_INFO("=== SR Cube OpenXR Legacy GL Application ===");
    LOG_INFO("OpenXR standard mode (DisplayXR creates window)");
    LOG_INFO("Input handled by DisplayXR's qwerty driver");

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

    // Create hidden dummy window for GL context
    LOG_INFO("Creating dummy window for GL context...");
    HWND dummyHwnd = CreateDummyWindow(hInstance);
    if (!dummyHwnd) {
        LOG_ERROR("Failed to create dummy window");
        ShutdownLogging();
        return 1;
    }

    // Create OpenGL context (temp → core profile 3.3)
    HDC hDC = nullptr;
    HGLRC hGLRC = nullptr;
    if (!CreateOpenGLContext(dummyHwnd, hDC, hGLRC)) {
        LOG_ERROR("OpenGL context creation failed");
        DestroyWindow(dummyHwnd);
        ShutdownLogging();
        return 1;
    }

    // Load GL function pointers (context must be current)
    if (!LoadGLFunctions()) {
        LOG_ERROR("Failed to load GL function pointers");
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(hGLRC);
        DestroyWindow(dummyHwnd);
        ShutdownLogging();
        return 1;
    }

    // Initialize OpenXR
    LOG_INFO("Initializing OpenXR...");
    XrSessionManager xr = {};
    if (!InitializeOpenXR(xr)) {
        LOG_ERROR("OpenXR initialization failed");
        MessageBox(nullptr, L"Failed to initialize OpenXR", L"Error", MB_OK | MB_ICONERROR);
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(hGLRC);
        DestroyWindow(dummyHwnd);
        ShutdownLogging();
        return 1;
    }

    // Get OpenGL graphics requirements
    if (!GetOpenGLGraphicsRequirements(xr)) {
        LOG_ERROR("Failed to get OpenGL graphics requirements");
        CleanupOpenXR(xr);
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(hGLRC);
        DestroyWindow(dummyHwnd);
        ShutdownLogging();
        return 1;
    }

    // Create OpenXR session (DisplayXR creates window)
    LOG_INFO("Creating OpenXR session...");
    if (!CreateSession(xr, hDC, hGLRC)) {
        LOG_ERROR("OpenXR session creation failed");
        MessageBox(nullptr, L"Failed to create OpenXR session", L"Error", MB_OK | MB_ICONERROR);
        CleanupOpenXR(xr);
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(hGLRC);
        DestroyWindow(dummyHwnd);
        ShutdownLogging();
        return 1;
    }

    // Create reference spaces
    if (!CreateSpaces(xr)) {
        LOG_ERROR("Reference space creation failed");
        CleanupOpenXR(xr);
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(hGLRC);
        DestroyWindow(dummyHwnd);
        ShutdownLogging();
        return 1;
    }

    // Create single swapchain at native display resolution
    if (!CreateSwapchain(xr)) {
        LOG_ERROR("Swapchain creation failed");
        CleanupOpenXR(xr);
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(hGLRC);
        DestroyWindow(dummyHwnd);
        ShutdownLogging();
        return 1;
    }

    // Enumerate OpenGL swapchain images
    std::vector<XrSwapchainImageOpenGLKHR> swapchainImages;
    {
        uint32_t count = xr.swapchain.imageCount;
        swapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
        xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)swapchainImages.data());
        LOG_INFO("Enumerated %u OpenGL swapchain images", count);
    }

    // Initialize GL renderer (shaders, geometry, textures)
    GLRenderer glRenderer = {};
    if (!InitializeGLRenderer(glRenderer)) {
        LOG_ERROR("GL renderer initialization failed");
        CleanupOpenXR(xr);
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(hGLRC);
        DestroyWindow(dummyHwnd);
        ShutdownLogging();
        return 1;
    }

    // Create FBOs for swapchain images
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
            DestroyWindow(dummyHwnd);
            ShutdownLogging();
            return 1;
        }
    }

    // Performance tracking
    PerformanceStats perfStats = {};
    perfStats.lastTime = std::chrono::high_resolution_clock::now();

    LOG_INFO("");
    LOG_INFO("=== Entering main loop ===");
    LOG_INFO("Rendering in DisplayXR's window (input via qwerty driver)");
    LOG_INFO("Controls: WASD=Move, QE=Up/Down, Mouse=Look, ESC=Quit");
    LOG_INFO("");

    // Main loop - no visible window, just process OpenXR frames
    // Exit when OpenXR session ends (user closes DisplayXR window or presses ESC)
    while (g_running && !xr.exitRequested) {
        // Update performance stats
        UpdatePerformanceStats(perfStats);

        // Update scene (cube rotation)
        UpdateScene(glRenderer, perfStats.deltaTime);

        // Poll OpenXR events
        PollEvents(xr);

        // Only render if session is running
        if (xr.sessionRunning) {
            XrFrameState frameState;
            if (BeginFrame(xr, frameState)) {
                XrCompositionLayerProjectionView projectionViews[8] = {};
                uint32_t submitViewCount = 2;

                if (frameState.shouldRender) {
                    // Camera movement is handled by DisplayXR's qwerty driver
                    // Pass zeros for player transform - XR poses already include qwerty input
                    if (LocateViews(xr, frameState.predictedDisplayTime,
                        0.0f, 0.0f, 0.0f,  // playerPos (handled by qwerty)
                        0.0f, 0.0f)) {     // playerYaw/Pitch (handled by qwerty)

                        // Use current mode's view count (not xr.viewCount which is max across all modes)
                        uint32_t modeViewCount = (xr.currentModeIndex < xr.renderingModeCount)
                            ? xr.renderingModeViewCounts[xr.currentModeIndex] : xr.viewCount;
                        submitViewCount = modeViewCount;

                        // Get raw view poses (pre-player-transform) for projection views.
                        XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
                        locateInfo.viewConfigurationType = xr.viewConfigType;
                        locateInfo.displayTime = frameState.predictedDisplayTime;
                        locateInfo.space = xr.localSpace;

                        XrViewState viewState = {XR_TYPE_VIEW_STATE};
                        uint32_t rawViewCount = 8;
                        XrView rawViews[8];
                        for (uint32_t i = 0; i < 8; i++) rawViews[i] = {XR_TYPE_VIEW};
                        xrLocateViews(xr.session, &locateInfo, &viewState, 8, &rawViewCount, rawViews);

                        // Get tile layout from rendering mode, with fallback
                        uint32_t tileColumns = (xr.currentModeIndex < xr.renderingModeCount)
                            ? xr.renderingModeTileColumns[xr.currentModeIndex] : (modeViewCount >= 2 ? 2 : 1);
                        uint32_t tileRows = (xr.currentModeIndex < xr.renderingModeCount)
                            ? xr.renderingModeTileRows[xr.currentModeIndex] : ((modeViewCount + tileColumns - 1) / tileColumns);

                        uint32_t tileW = xr.swapchain.width / tileColumns;
                        uint32_t tileH = xr.swapchain.height / tileRows;

                        uint32_t imageIndex;
                        if (AcquireSwapchainImage(xr, imageIndex)) {
                            // Clear entire FBO once before eye loop
                            glBindFramebuffer_(GL_FRAMEBUFFER, glRenderer.fbos[imageIndex]);
                            glClearColor(0.05f, 0.05f, 0.25f, 1.0f);
                            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                            glBindFramebuffer_(GL_FRAMEBUFFER, 0);

                            for (uint32_t eye = 0; eye < modeViewCount; eye++) {
                                uint32_t tileX = eye % tileColumns;
                                uint32_t tileY = eye / tileColumns;

                                uint32_t vpX = tileX * tileW;
                                uint32_t vpY = tileY * tileH;

                                XMMATRIX viewMatrix = xr.viewMatrices[eye];
                                XMMATRIX projMatrix = xr.projMatrices[eye];

                                // Non-ext app: 0.3m cube at z=-2m, no zoom control
                                RenderScene(glRenderer, imageIndex,
                                    vpX, vpY,
                                    tileW, tileH,
                                    viewMatrix, projMatrix,
                                    1.0f, 1.6f, -2.0f, 0.3f);

                                // Set up projection view for this eye
                                projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                                projectionViews[eye].subImage.swapchain = xr.swapchain.swapchain;
                                projectionViews[eye].subImage.imageRect.offset = {
                                    (int32_t)(tileX * tileW), (int32_t)(tileY * tileH)
                                };
                                projectionViews[eye].subImage.imageRect.extent = {
                                    (int32_t)tileW,
                                    (int32_t)tileH
                                };
                                projectionViews[eye].subImage.imageArrayIndex = 0;

                                projectionViews[eye].pose = rawViews[eye].pose;
                                projectionViews[eye].fov = rawViews[eye].fov;
                            }

                            ReleaseSwapchainImage(xr);
                        }
                    }
                }

                // Submit frame (projection layer only)
                EndFrame(xr, frameState.predictedDisplayTime, projectionViews, submitViewCount);
            }
        } else {
            Sleep(100);
        }
    }

    // Cleanup
    LOG_INFO("");
    LOG_INFO("=== Shutting down ===");

    CleanupGLRenderer(glRenderer);
    CleanupOpenXR(xr);

    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(hGLRC);
    ReleaseDC(dummyHwnd, hDC);
    DestroyWindow(dummyHwnd);

    LOG_INFO("Application shutdown complete");
    ShutdownLogging();

    return 0;
}
