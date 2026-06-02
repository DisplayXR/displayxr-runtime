// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  windowspace_handle_gl_win — submit N XR_EXT window-space layers, no 3D scene.
 *
 * OpenGL handle-class app for issue #389. Flat-cleared projection layer (via a
 * per-swapchain-image FBO + glClear) + N window-space layer swapchains, each
 * filled with a distinct solid color via glTexSubImage2D — the same upload path
 * the cube_handle_gl app uses for its HUD. Single-threaded (GL context current
 * on the main thread). Pass the layer count as argv[1] (default 6, clamped [1,12]).
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>

#include "logging.h"
#include "xr_session.h"
#include "gl_functions.h"
#include "xr_window_space_hud.h"
#include "windowspace_layers.h"

#include <vector>

static const char* APP_NAME = "windowspace_handle_gl_win";
static const wchar_t* WINDOW_CLASS = L"DXRWindowSpaceGLClass";
static const wchar_t* WINDOW_TITLE = L"Window-Space Layers — GL Native Compositor";

static bool g_running = true;
static XrSessionManager* g_xr = nullptr;
static UINT g_windowWidth = 1280;
static UINT g_windowHeight = 720;

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) { g_windowWidth = LOWORD(lParam); g_windowHeight = HIWORD(lParam); }
        return 0;
    case WM_CLOSE:
        if (g_xr && g_xr->session != XR_NULL_HANDLE && g_xr->sessionRunning) {
            xrRequestExitSession(g_xr->session); return 0;
        }
        g_running = false; PostQuitMessage(0); return 0;
    case WM_SYSKEYDOWN:
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) { PostMessage(hwnd, WM_CLOSE, 0, 0); return 0; }
        break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static HWND CreateAppWindow(HINSTANCE hInstance, int width, int height) {
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = WINDOW_CLASS;
    if (!RegisterClassEx(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) { LOG_ERROR("RegisterClassEx err %lu", err); return nullptr; }
    }
    RECT rect = { 0, 0, width, height };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowEx(0, WINDOW_CLASS, WINDOW_TITLE, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) { LOG_ERROR("CreateWindowEx err %lu", GetLastError()); return nullptr; }
    return hwnd;
}

// temp legacy context -> wglCreateContextAttribsARB -> core profile 3.3
static bool CreateOpenGLContext(HWND hwnd, HDC& hDC, HGLRC& hGLRC) {
    hDC = GetDC(hwnd);
    if (!hDC) { LOG_ERROR("GetDC failed"); return false; }
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd); pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32; pfd.cDepthBits = 24; pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;
    int pixelFormat = ChoosePixelFormat(hDC, &pfd);
    if (!pixelFormat || !SetPixelFormat(hDC, pixelFormat, &pfd)) { LOG_ERROR("pixel format failed"); return false; }
    HGLRC tempRC = wglCreateContext(hDC);
    if (!tempRC || !wglMakeCurrent(hDC, tempRC)) { LOG_ERROR("temp GL context failed"); if (tempRC) wglDeleteContext(tempRC); return false; }
    PFNWGLCREATECONTEXTATTRIBSARBPROC wglCreateContextAttribsARB =
        (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress("wglCreateContextAttribsARB");
    if (!wglCreateContextAttribsARB) { LOG_ERROR("wglCreateContextAttribsARB missing"); wglMakeCurrent(nullptr, nullptr); wglDeleteContext(tempRC); return false; }
    int attribs[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 3,
        WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0 };
    wglMakeCurrent(nullptr, nullptr);
    hGLRC = wglCreateContextAttribsARB(hDC, nullptr, attribs);
    wglDeleteContext(tempRC);
    if (!hGLRC || !wglMakeCurrent(hDC, hGLRC)) { LOG_ERROR("core GL context failed"); if (hGLRC) wglDeleteContext(hGLRC); hGLRC = nullptr; return false; }
    return true;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance; (void)lpCmdLine;
    if (!InitializeLogging(APP_NAME)) {
        MessageBox(nullptr, L"Failed to initialize logging", L"Warning", MB_OK | MB_ICONWARNING);
    }

    const uint32_t layerCount = wsl::ParseLayerCount(__argc, __argv);
    LOG_INFO("=== windowspace_handle_gl_win === N=%u window-space layers", layerCount);

    HWND hwnd = CreateAppWindow(hInstance, g_windowWidth, g_windowHeight);
    if (!hwnd) { ShutdownLogging(); return 1; }

    HDC hDC = nullptr; HGLRC hGLRC = nullptr;
    if (!CreateOpenGLContext(hwnd, hDC, hGLRC)) { LOG_ERROR("GL context failed"); ShutdownLogging(); return 1; }
    if (!LoadGLFunctions()) { LOG_ERROR("LoadGLFunctions failed"); wglMakeCurrent(nullptr, nullptr); wglDeleteContext(hGLRC); ShutdownLogging(); return 1; }

    XrSessionManager xr = {};
    g_xr = &xr;
    if (!InitializeOpenXR(xr)) { LOG_ERROR("OpenXR init failed"); g_xr = nullptr; wglMakeCurrent(nullptr, nullptr); wglDeleteContext(hGLRC); ShutdownLogging(); return 1; }
    if (!GetOpenGLGraphicsRequirements(xr)) { LOG_ERROR("GL graphics req failed"); CleanupOpenXR(xr); wglMakeCurrent(nullptr, nullptr); wglDeleteContext(hGLRC); ShutdownLogging(); return 1; }
    if (!CreateSession(xr, hDC, hGLRC, hwnd)) { LOG_ERROR("Session failed"); CleanupOpenXR(xr); wglMakeCurrent(nullptr, nullptr); wglDeleteContext(hGLRC); ShutdownLogging(); return 1; }
    if (!CreateSpaces(xr)) { LOG_ERROR("Spaces failed"); CleanupOpenXR(xr); wglMakeCurrent(nullptr, nullptr); wglDeleteContext(hGLRC); ShutdownLogging(); return 1; }
    if (!CreateSwapchain(xr)) { LOG_ERROR("Swapchain failed"); CleanupOpenXR(xr); wglMakeCurrent(nullptr, nullptr); wglDeleteContext(hGLRC); ShutdownLogging(); return 1; }

    // Enumerate projection swapchain images + create one FBO per image for clearing.
    std::vector<XrSwapchainImageOpenGLKHR> swapchainImages;
    std::vector<GLuint> projFbos;
    {
        uint32_t count = xr.swapchain.imageCount;
        swapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
        xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)swapchainImages.data());
        LOG_INFO("Enumerated %u OpenGL projection swapchain images", count);
        projFbos.resize(count, 0);
        glGenFramebuffers_(count, projFbos.data());
        for (uint32_t i = 0; i < count; i++) {
            glBindFramebuffer_(GL_FRAMEBUFFER, projFbos[i]);
            glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                swapchainImages[i].image, 0);
        }
        glBindFramebuffer_(GL_FRAMEBUFFER, 0);
    }

    // Create N window-space layer swapchains + enumerate + solid-color buffers.
    std::vector<XrHudSwapchain> layerSC(layerCount);
    std::vector<std::vector<XrSwapchainImageOpenGLKHR>> layerImages(layerCount);
    std::vector<std::vector<uint8_t>> layerPixels(layerCount);
    for (uint32_t i = 0; i < layerCount; ++i) {
        if (!CreateHudSwapchain(xr.session, wsl::kLayerPxWidth, wsl::kLayerPxHeight, layerSC[i])) {
            LOG_WARN("Window-space swapchain %u create failed", i); continue;
        }
        uint32_t cnt = layerSC[i].imageCount;
        layerImages[i].resize(cnt, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
        xrEnumerateSwapchainImages(layerSC[i].swapchain, cnt, &cnt,
            (XrSwapchainImageBaseHeader*)layerImages[i].data());
        uint8_t rgba[4];
        wsl::LayerColor(i, layerCount, rgba);
        wsl::FillSolid(layerPixels[i], wsl::kLayerPxWidth, wsl::kLayerPxHeight, rgba);
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    XrEnvironmentBlendMode blendMode = (xr.envBlendModeCount > 0) ? xr.envBlendModes[0] : XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

    MSG msg = {};
    while (g_running && !xr.exitRequested) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_running = false; break; }
            TranslateMessage(&msg); DispatchMessage(&msg);
        }
        if (!g_running) break;

        PollEvents(xr);
        if (!xr.sessionRunning) { Sleep(50); continue; }

        XrFrameState frameState;
        if (!BeginFrame(xr, frameState)) continue;

        bool monoMode = (xr.renderingModeCount > 0 && !xr.renderingModeDisplay3D[xr.currentModeIndex]);
        uint32_t modeViewCount = (xr.renderingModeCount > 0 && xr.currentModeIndex < xr.renderingModeCount)
            ? xr.renderingModeViewCounts[xr.currentModeIndex] : 2;
        uint32_t tileColumns = (xr.renderingModeCount > 0 && xr.currentModeIndex < xr.renderingModeCount)
            ? xr.renderingModeTileColumns[xr.currentModeIndex] : 2;
        uint32_t tileRows = (xr.renderingModeCount > 0 && xr.currentModeIndex < xr.renderingModeCount)
            ? xr.renderingModeTileRows[xr.currentModeIndex] : 1;
        if (xr.renderingModeCount > 0 && xr.currentModeIndex < xr.renderingModeCount) {
            xr.recommendedViewScaleX = xr.renderingModeScaleX[xr.currentModeIndex];
            xr.recommendedViewScaleY = xr.renderingModeScaleY[xr.currentModeIndex];
        }
        int eyeCount = monoMode ? 1 : (int)modeViewCount;
        std::vector<XrCompositionLayerProjectionView> projectionViews(eyeCount, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});

        bool rendered = false;
        if (frameState.shouldRender) {
            XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
            locateInfo.viewConfigurationType = xr.viewConfigType;
            locateInfo.displayTime = frameState.predictedDisplayTime;
            locateInfo.space = xr.localSpace;
            XrViewState viewState = {XR_TYPE_VIEW_STATE};
            uint32_t viewCount = 8;
            XrView rawViews[8];
            for (uint32_t vi = 0; vi < 8; vi++) rawViews[vi] = {XR_TYPE_VIEW};
            xrLocateViews(xr.session, &locateInfo, &viewState, 8, &viewCount, rawViews);

            uint32_t maxTileW = tileColumns > 0 ? xr.swapchain.width / tileColumns : xr.swapchain.width;
            uint32_t maxTileH = tileRows > 0 ? xr.swapchain.height / tileRows : xr.swapchain.height;
            uint32_t renderW, renderH;
            if (monoMode) {
                renderW = g_windowWidth; renderH = g_windowHeight;
                if (renderW > xr.swapchain.width) renderW = xr.swapchain.width;
                if (renderH > xr.swapchain.height) renderH = xr.swapchain.height;
            } else {
                renderW = (uint32_t)(g_windowWidth * xr.recommendedViewScaleX);
                renderH = (uint32_t)(g_windowHeight * xr.recommendedViewScaleY);
                if (renderW > maxTileW) renderW = maxTileW;
                if (renderH > maxTileH) renderH = maxTileH;
            }

            uint32_t imageIndex;
            if (AcquireSwapchainImage(xr, imageIndex)) {
                // Flat dark background fill — no 3D scene.
                if (imageIndex < projFbos.size()) {
                    glBindFramebuffer_(GL_FRAMEBUFFER, projFbos[imageIndex]);
                    glViewport(0, 0, (GLsizei)xr.swapchain.width, (GLsizei)xr.swapchain.height);
                    glClearColor(wsl::kBgR / 255.0f, wsl::kBgG / 255.0f, wsl::kBgB / 255.0f, wsl::kBgA / 255.0f);
                    glClear(GL_COLOR_BUFFER_BIT);
                    glFlush();
                    glBindFramebuffer_(GL_FRAMEBUFFER, 0);
                }

                for (int eye = 0; eye < eyeCount; eye++) {
                    uint32_t tileX = monoMode ? 0 : (eye % tileColumns);
                    uint32_t tileY = monoMode ? 0 : (eye / tileColumns);
                    projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                    projectionViews[eye].subImage.swapchain = xr.swapchain.swapchain;
                    projectionViews[eye].subImage.imageRect.offset = {
                        (int32_t)(tileX * renderW), (int32_t)(tileY * renderH) };
                    projectionViews[eye].subImage.imageRect.extent = { (int32_t)renderW, (int32_t)renderH };
                    projectionViews[eye].subImage.imageArrayIndex = 0;
                    int safeIdx = (eye < (int)viewCount) ? eye : 0;
                    projectionViews[eye].pose = rawViews[safeIdx].pose;
                    projectionViews[eye].fov = rawViews[safeIdx].fov;
                }
                ReleaseSwapchainImage(xr);
                rendered = true;
            }

            // Fill each window-space layer image via glTexSubImage2D (distinct color).
            for (uint32_t i = 0; i < layerCount; ++i) {
                if (layerSC[i].swapchain == XR_NULL_HANDLE) continue;
                uint32_t idx = 0;
                if (!AcquireHudSwapchainImage(layerSC[i], idx)) continue;
                if (idx < layerImages[i].size() && layerImages[i][idx].image && !layerPixels[i].empty()) {
                    while (glGetError() != GL_NO_ERROR) {}
                    glBindTexture(GL_TEXTURE_2D, layerImages[i][idx].image);
                    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, wsl::kLayerPxWidth, wsl::kLayerPxHeight,
                        GL_RGBA, GL_UNSIGNED_BYTE, layerPixels[i].data());
                    glFlush();
                    glBindTexture(GL_TEXTURE_2D, 0);
                }
                ReleaseHudSwapchainImage(layerSC[i]);
            }
        }

        if (rendered) {
            std::vector<WindowSpaceLayerDesc> descs;
            descs.reserve(layerCount);
            for (uint32_t i = 0; i < layerCount; ++i) {
                if (layerSC[i].swapchain == XR_NULL_HANDLE) continue;
                float y = wsl::kLayerYStart + i * wsl::kLayerYStride;
                if (y + wsl::kLayerH > 1.0f) continue;
                WindowSpaceLayerDesc d;
                d.sc = &layerSC[i];
                d.x = wsl::kLayerX; d.y = y; d.width = wsl::kLayerW; d.height = wsl::kLayerH;
                d.disparity = -0.01f * (float)i;
                descs.push_back(d);
            }
            SubmitWindowSpaceLayersFrame(xr.session, xr.localSpace,
                frameState.predictedDisplayTime, blendMode,
                projectionViews.data(), (uint32_t)eyeCount,
                descs.data(), (uint32_t)descs.size());
        } else {
            EndFrame(xr, frameState.predictedDisplayTime, projectionViews.data(), (uint32_t)eyeCount);
        }
    }

    LOG_INFO("=== Shutting down ===");
    if (!projFbos.empty()) glDeleteFramebuffers_((GLsizei)projFbos.size(), projFbos.data());
    g_xr = nullptr;
    CleanupOpenXR(xr);
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(hGLRC);
    ReleaseDC(hwnd, hDC);
    DestroyWindow(hwnd);
    UnregisterClass(WINDOW_CLASS, hInstance);
    ShutdownLogging();
    return 0;
}
