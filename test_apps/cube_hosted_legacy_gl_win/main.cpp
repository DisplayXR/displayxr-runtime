// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  SR Cube OpenXR GL - Legacy hosted mode (no XR_EXT_display_info)
 *
 * This application demonstrates OpenXR with OpenGL without the XR_EXT_win32_window_binding
 * extension. DisplayXR will create its own window for rendering.
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
#include <wingdi.h>

#include "logging.h"
#include "xr_session.h"
#include "gl_functions.h"
#include "gl_renderer.h"

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

// Hidden dummy window class for WGL context creation
static const wchar_t* DUMMY_WINDOW_CLASS = L"SRCubeGLDummyClass";

static LRESULT CALLBACK DummyWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// Create a hidden dummy window + WGL core profile 3.3 context
static bool CreateDummyOpenGLContext(HINSTANCE hInstance, HWND& hwnd, HDC& hDC, HGLRC& hGLRC) {
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = DummyWindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = DUMMY_WINDOW_CLASS;

    if (!RegisterClassEx(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            LOG_ERROR("Failed to register dummy window class, error: %lu", err);
            return false;
        }
    }

    hwnd = CreateWindowEx(0, DUMMY_WINDOW_CLASS, L"DummyGL", 0,
        0, 0, 1, 1, nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) {
        LOG_ERROR("Failed to create dummy window, error: %lu", GetLastError());
        return false;
    }

    hDC = GetDC(hwnd);
    if (!hDC) {
        LOG_ERROR("GetDC failed on dummy window");
        DestroyWindow(hwnd);
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
        ReleaseDC(hwnd, hDC);
        DestroyWindow(hwnd);
        return false;
    }

    if (!SetPixelFormat(hDC, pixelFormat, &pfd)) {
        LOG_ERROR("SetPixelFormat failed");
        ReleaseDC(hwnd, hDC);
        DestroyWindow(hwnd);
        return false;
    }

    // Create temporary legacy context to load WGL extensions
    HGLRC tempRC = wglCreateContext(hDC);
    if (!tempRC) {
        LOG_ERROR("wglCreateContext (temp) failed");
        ReleaseDC(hwnd, hDC);
        DestroyWindow(hwnd);
        return false;
    }

    if (!wglMakeCurrent(hDC, tempRC)) {
        LOG_ERROR("wglMakeCurrent (temp) failed");
        wglDeleteContext(tempRC);
        ReleaseDC(hwnd, hDC);
        DestroyWindow(hwnd);
        return false;
    }

    // Load wglCreateContextAttribsARB
    typedef HGLRC(APIENTRY* PFNWGLCREATECONTEXTATTRIBSARBPROC_LOCAL)(HDC hDC, HGLRC hShareContext, const int* attribList);
    PFNWGLCREATECONTEXTATTRIBSARBPROC_LOCAL wglCreateContextAttribsARB =
        (PFNWGLCREATECONTEXTATTRIBSARBPROC_LOCAL)wglGetProcAddress("wglCreateContextAttribsARB");

    if (!wglCreateContextAttribsARB) {
        LOG_ERROR("wglCreateContextAttribsARB not available");
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(tempRC);
        ReleaseDC(hwnd, hDC);
        DestroyWindow(hwnd);
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
        ReleaseDC(hwnd, hDC);
        DestroyWindow(hwnd);
        return false;
    }

    if (!wglMakeCurrent(hDC, hGLRC)) {
        LOG_ERROR("wglMakeCurrent (core profile) failed");
        wglDeleteContext(hGLRC);
        hGLRC = nullptr;
        ReleaseDC(hwnd, hDC);
        DestroyWindow(hwnd);
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

    LOG_INFO("=== SR Cube OpenXR GL Application ===");
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

    // Create hidden dummy window + WGL context for OpenGL init
    HWND dummyHwnd = nullptr;
    HDC hDC = nullptr;
    HGLRC hGLRC = nullptr;
    if (!CreateDummyOpenGLContext(hInstance, dummyHwnd, hDC, hGLRC)) {
        LOG_ERROR("OpenGL context creation failed");
        MessageBox(nullptr, L"Failed to create OpenGL context", L"Error", MB_OK | MB_ICONERROR);
        ShutdownLogging();
        return 1;
    }

    // Load GL function pointers (context must be current)
    if (!LoadGLFunctions()) {
        LOG_ERROR("Failed to load GL function pointers");
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(hGLRC);
        ReleaseDC(dummyHwnd, hDC);
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
        ReleaseDC(dummyHwnd, hDC);
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
        ReleaseDC(dummyHwnd, hDC);
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
        ReleaseDC(dummyHwnd, hDC);
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
        ReleaseDC(dummyHwnd, hDC);
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
        ReleaseDC(dummyHwnd, hDC);
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

    // Initialize GL renderer (shaders, geometry)
    GLRenderer glRenderer = {};
    if (!InitializeGLRenderer(glRenderer)) {
        LOG_ERROR("GL renderer initialization failed");
        CleanupOpenXR(xr);
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(hGLRC);
        ReleaseDC(dummyHwnd, hDC);
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
            ReleaseDC(dummyHwnd, hDC);
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

    // Main loop - no window, just process OpenXR frames
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
                            for (uint32_t eye = 0; eye < modeViewCount; eye++) {
                                uint32_t tileX = eye % tileColumns;
                                uint32_t tileY = eye / tileColumns;

                                XMMATRIX viewMatrix = xr.viewMatrices[eye];
                                XMMATRIX projMatrix = xr.projMatrices[eye];

                                // Non-ext app: render with default zoom (1.0)
                                RenderScene(glRenderer, imageIndex,
                                    tileX * tileW, tileY * tileH,
                                    tileW, tileH,
                                    viewMatrix, projMatrix,
                                    1.0f);

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

                            // DIAGNOSTIC: check GL state and readback pixel after render
                            {
                                static int diag_frame = 0;
                                if (diag_frame < 10) {
                                    GLenum err = glGetError();
                                    HGLRC curCtx = wglGetCurrentContext();
                                    HDC curDC = wglGetCurrentDC();
                                    // Read pixel from the FBO we just rendered to
                                    GLuint scTex = swapchainImages[imageIndex].image;
                                    glBindFramebuffer_(GL_FRAMEBUFFER, glRenderer.fbos[imageIndex]);
                                    uint8_t px_center[4] = {0}, px_quarter[4] = {0};
                                    // Center of left eye region
                                    glReadPixels(tileW/2, tileH/2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px_center);
                                    // Quarter way (more likely to hit cube/grid)
                                    glReadPixels(tileW/2, tileH/4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px_quarter);
                                    glBindFramebuffer_(GL_FRAMEBUFFER, 0);
                                    GLenum err2 = glGetError();
                                    LOG_INFO("DIAG[%d]: img=%u tex=%u ctx=%p dc=%p glErr=0x%x/0x%x "
                                             "tile=%ux%u center=(%u,%u,%u,%u) quarter=(%u,%u,%u,%u)",
                                             diag_frame, imageIndex, scTex, (void*)curCtx, (void*)curDC,
                                             err, err2, tileW, tileH,
                                             px_center[0], px_center[1], px_center[2], px_center[3],
                                             px_quarter[0], px_quarter[1], px_quarter[2], px_quarter[3]);
                                }
                                diag_frame++;
                            }
                            // DIAGNOSTIC: force GPU completion before releasing
                            // to test cross-context sync hypothesis
                            glFinish();
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
    UnregisterClass(DUMMY_WINDOW_CLASS, hInstance);

    LOG_INFO("Application shutdown complete");
    ShutdownLogging();

    return 0;
}
