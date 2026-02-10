// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  SR Cube OpenXR Ext GL - OpenXR with XR_EXT_win32_window_binding (OpenGL)
 *
 * OpenGL port of sr_cube_openxr_ext. Projection layer + window-space HUD overlay.
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

#include <atomic>
#include <string>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

using namespace DirectX;

static const char* APP_NAME = "sr_cube_openxr_ext_gl";

// HUD overlay size as fraction of window dimensions (for window-space layer)
static const float HUD_WIDTH_FRACTION = 0.30f;
static const float HUD_HEIGHT_FRACTION = 0.35f;

static const wchar_t* WINDOW_CLASS = L"SRCubeOpenXRExtGLClass";
static const wchar_t* WINDOW_TITLE = L"SR Cube OpenXR Ext OpenGL (Press ESC to exit)";

// Global state (shared between main thread and render thread)
static InputState g_inputState;
static std::mutex g_inputMutex;
static std::atomic<bool> g_running{true};
static UINT g_windowWidth = 1280;
static UINT g_windowHeight = 720;

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
        g_running.store(false);
        PostQuitMessage(0);
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            g_running.store(false);
            PostQuitMessage(0);
            return 0;
        }
        break;
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
        uint32_t windowW, windowH;
        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            inputSnapshot = g_inputState;
            resetRequested = g_inputState.resetViewRequested;
            g_inputState.resetViewRequested = false;
            g_inputState.fullscreenToggleRequested = false;
            windowW = g_windowWidth;
            windowH = g_windowHeight;
        }

        UpdatePerformanceStats(perfStats);
        UpdateCameraMovement(inputSnapshot, perfStats.deltaTime);

        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_inputState.cameraPosX = inputSnapshot.cameraPosX;
            g_inputState.cameraPosY = inputSnapshot.cameraPosY;
            g_inputState.cameraPosZ = inputSnapshot.cameraPosZ;
            if (resetRequested) {
                g_inputState.yaw = inputSnapshot.yaw;
                g_inputState.pitch = inputSnapshot.pitch;
                g_inputState.zoomScale = inputSnapshot.zoomScale;
            }
        }

        UpdateScene(*renderer, perfStats.deltaTime);
        PollEvents(*xr);

        if (xr->sessionRunning) {
            XrFrameState frameState;
            if (BeginFrame(*xr, frameState)) {
                XrCompositionLayerProjectionView projectionViews[2] = {};
                bool rendered = false;
                bool hudSubmitted = false;

                if (frameState.shouldRender) {
                    XMMATRIX leftViewMatrix, leftProjMatrix;
                    XMMATRIX rightViewMatrix, rightProjMatrix;

                    if (LocateViews(*xr, frameState.predictedDisplayTime,
                        leftViewMatrix, leftProjMatrix,
                        rightViewMatrix, rightProjMatrix,
                        inputSnapshot.cameraPosX, inputSnapshot.cameraPosY, inputSnapshot.cameraPosZ,
                        inputSnapshot.yaw, inputSnapshot.pitch)) {

                        // Get raw view poses for projection views
                        XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
                        locateInfo.viewConfigurationType = xr->viewConfigType;
                        locateInfo.displayTime = frameState.predictedDisplayTime;
                        locateInfo.space = xr->localSpace;

                        XrViewState viewState = {XR_TYPE_VIEW_STATE};
                        uint32_t viewCount = 2;
                        XrView rawViews[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
                        xrLocateViews(xr->session, &locateInfo, &viewState, 2, &viewCount, rawViews);

                        // Compute render dims from window size and display scale factors
                        uint32_t renderW = (uint32_t)(windowW * xr->recommendedViewScaleX);
                        uint32_t renderH = (uint32_t)(windowH * xr->recommendedViewScaleY);
                        if (renderW > xr->swapchains[0].width) renderW = xr->swapchains[0].width;
                        if (renderH > xr->swapchains[0].height) renderH = xr->swapchains[0].height;

                        rendered = true;
                        for (int eye = 0; eye < 2; eye++) {
                            uint32_t imageIndex;
                            if (AcquireSwapchainImage(*xr, eye, imageIndex)) {
                                XMMATRIX viewMatrix = (eye == 0) ? leftViewMatrix : rightViewMatrix;
                                XMMATRIX projMatrix = (eye == 0) ? leftProjMatrix : rightProjMatrix;

                                RenderScene(*renderer, eye, imageIndex,
                                    renderW, renderH,
                                    viewMatrix, projMatrix,
                                    inputSnapshot.zoomScale);

                                ReleaseSwapchainImage(*xr, eye);

                                projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                                projectionViews[eye].subImage.swapchain = xr->swapchains[eye].swapchain;
                                projectionViews[eye].subImage.imageRect.offset = {0, 0};
                                projectionViews[eye].subImage.imageRect.extent = {
                                    (int32_t)renderW,
                                    (int32_t)renderH
                                };
                                projectionViews[eye].subImage.imageArrayIndex = 0;
                                projectionViews[eye].pose = rawViews[eye].pose;
                                projectionViews[eye].fov = rawViews[eye].fov;
                            } else {
                                rendered = false;
                            }
                        }

                        // Render HUD to window-space layer swapchain
                        if (rendered && inputSnapshot.hudVisible && hud && xr->hasHudSwapchain && hudSwapchainImages) {
                            uint32_t hudImageIndex;
                            if (AcquireHudSwapchainImage(*xr, hudImageIndex)) {
                                std::wstring sessionText = L"Session: ";
                                sessionText += FormatSessionState((int)xr->sessionState);
                                std::wstring modeText = xr->hasWin32WindowBindingExt ?
                                    L"XR_EXT_win32_window_binding: ACTIVE (OpenGL)" :
                                    L"XR_EXT_win32_window_binding: NOT AVAILABLE (OpenGL)";
                                uint32_t dispRenderW = (uint32_t)(windowW * xr->recommendedViewScaleX);
                                uint32_t dispRenderH = (uint32_t)(windowH * xr->recommendedViewScaleY);
                                if (dispRenderW > xr->swapchains[0].width) dispRenderW = xr->swapchains[0].width;
                                if (dispRenderH > xr->swapchains[0].height) dispRenderH = xr->swapchains[0].height;
                                std::wstring perfText = FormatPerformanceInfo(perfStats.fps, perfStats.frameTimeMs,
                                    dispRenderW, dispRenderH,
                                    windowW, windowH);
                                std::wstring eyeText = FormatEyeTrackingInfo(
                                    xr->eyePosX, xr->eyePosY, xr->eyePosZ, xr->eyeTrackingActive);

                                uint32_t srcRowPitch = 0;
                                const void* pixels = RenderHudAndMap(*hud, &srcRowPitch,
                                    sessionText, modeText, perfText, eyeText);
                                bool uploadOk = false;
                                if (pixels) {
                                    // Clear any prior GL errors
                                    while (glGetError() != GL_NO_ERROR) {}

                                    GLuint hudTexId = (*hudSwapchainImages)[hudImageIndex].image;
                                    glBindTexture(GL_TEXTURE_2D, hudTexId);
                                    if (srcRowPitch == hudWidth * 4) {
                                        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, hudWidth, hudHeight,
                                            GL_RGBA, GL_UNSIGNED_BYTE, pixels);
                                    } else {
                                        const uint8_t* src = (const uint8_t*)pixels;
                                        for (uint32_t row = 0; row < hudHeight; row++) {
                                            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row, hudWidth, 1,
                                                GL_RGBA, GL_UNSIGNED_BYTE, src + row * srcRowPitch);
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

                if (hudSubmitted) {
                    LOG_DEBUG("[Frame] Submitting EndFrame with HUD (layerCount=2)");
                    if (!EndFrameWithWindowSpaceHud(*xr, frameState.predictedDisplayTime, projectionViews,
                        0.0f, 0.0f, HUD_WIDTH_FRACTION, HUD_HEIGHT_FRACTION, 0.0f)) {
                        LOG_WARN("[Frame] EndFrameWithWindowSpaceHud FAILED — disabling HUD for this session");
                        hud = nullptr;  // Disable HUD for subsequent frames
                    }
                    LOG_DEBUG("[Frame] EndFrame with HUD returned");
                } else {
                    EndFrame(*xr, frameState.predictedDisplayTime, projectionViews);
                }
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

    HWND hwnd = CreateAppWindow(hInstance, g_windowWidth, g_windowHeight);
    if (!hwnd) {
        LOG_ERROR("Failed to create window");
        ShutdownLogging();
        return 1;
    }

    // Add SRMonado to DLL search path
    {
        HKEY hKey;
        char installPath[MAX_PATH] = {0};
        DWORD pathSize = sizeof(installPath);
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\LeiaSR\\SRMonado", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            if (RegQueryValueExA(hKey, "InstallPath", nullptr, nullptr, (LPBYTE)installPath, &pathSize) == ERROR_SUCCESS) {
                LOG_INFO("Adding SRMonado install path to DLL search: %s", installPath);
                SetDllDirectoryA(installPath);
            }
            RegCloseKey(hKey);
        }
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
    if (!InitializeOpenXR(xr)) {
        LOG_ERROR("OpenXR initialization failed");
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

    if (!CreateSwapchains(xr)) {
        LOG_ERROR("Swapchain creation failed");
        CleanupOpenXR(xr);
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(hGLRC);
        ShutdownLogging();
        return 1;
    }

    // Enumerate OpenGL swapchain images
    std::vector<XrSwapchainImageOpenGLKHR> swapchainImages[2];
    for (int eye = 0; eye < 2; eye++) {
        uint32_t count = xr.swapchains[eye].imageCount;
        swapchainImages[eye].resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
        xrEnumerateSwapchainImages(xr.swapchains[eye].swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)swapchainImages[eye].data());
        LOG_INFO("Eye %d: enumerated %u OpenGL swapchain images", eye, count);
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

    // Create FBOs for swapchain images
    for (int eye = 0; eye < 2; eye++) {
        uint32_t count = xr.swapchains[eye].imageCount;
        std::vector<GLuint> textures(count);
        for (uint32_t i = 0; i < count; i++) {
            textures[i] = swapchainImages[eye][i].image;
        }

        if (!CreateSwapchainFBOs(glRenderer, eye, textures.data(), count,
            xr.swapchains[eye].width, xr.swapchains[eye].height)) {
            LOG_ERROR("Failed to create FBOs for eye %d", eye);
            CleanupGLRenderer(glRenderer);
            CleanupOpenXR(xr);
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(hGLRC);
            ShutdownLogging();
            return 1;
        }
    }

    // Initialize HUD renderer for window-space layer overlay
    uint32_t hudWidth = (uint32_t)(xr.swapchains[0].width * HUD_WIDTH_FRACTION);
    uint32_t hudHeight = (uint32_t)(xr.swapchains[0].height * HUD_HEIGHT_FRACTION);

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
    LOG_INFO("Controls: WASD=Fly, QE=Up/Down, Mouse=Look, Space/DblClick=Reset, TAB=HUD, ESC=Quit");
    LOG_INFO("");

    // Release GL context from main thread before handing to render thread
    wglMakeCurrent(nullptr, nullptr);

    std::thread renderThread(RenderThreadFunc, hwnd, hDC, hGLRC, &xr, &glRenderer,
        swapchainImages,
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
