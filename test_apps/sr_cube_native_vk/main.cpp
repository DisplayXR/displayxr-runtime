// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  SR Cube Native Vulkan - Direct SR SDK test with Vulkan
 *
 * This application demonstrates direct SR SDK usage with the Vulkan weaver.
 * It uses Kooima projection for off-axis stereo rendering.
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shellscalingapi.h>

#include "logging.h"
#include "input_handler.h"
#include "window_manager.h"
#include "leia_math.h"
#include "vk_renderer.h"

#include <atomic>
#include <chrono>
#include <string>
#include <mutex>
#include <thread>

// Library for SetProcessDpiAwareness
#pragma comment(lib, "shcore.lib")

// LeiaSR SDK headers
#if defined(HAVE_LEIA_SR) && defined(HAVE_VK_WEAVER)
#include "sr/utility/exception.h"
#include "sr/sense/core/inputstream.h"
#include "sr/sense/system/systemsense.h"
#include "sr/sense/eyetracker/eyetracker.h"
#include "sr/world/display/display.h"
#include "sr/weaver/vkweaver.h"
#endif

// Application name for logging
static const char* APP_NAME = "sr_cube_native_vk";

// Window settings
static const wchar_t* WINDOW_CLASS = L"SRCubeNativeVKClass";
static const wchar_t* WINDOW_TITLE = L"SR Cube (Native Vulkan)";

// Global state (shared between main thread and render thread)
static InputState g_inputState;              // Protected by g_mutex
static WindowInfo g_windowInfo;
static std::atomic<bool> g_running{true};
static UINT g_windowWidth = 2560;
static UINT g_windowHeight = 1600;
static bool g_windowResized = false;
static std::mutex g_mutex;

#if defined(HAVE_LEIA_SR) && defined(HAVE_VK_WEAVER)
// SR SDK global state
static SR::SRContext*        g_srContext = nullptr;
static SR::IDisplayManager*  g_displayManager = nullptr;
static SR::IDisplay*         g_display = nullptr;
static SR::IVulkanWeaver1*   g_srWeaver = nullptr;

// Display properties
static float g_screenWidthMM = 0.0f;
static float g_screenHeightMM = 0.0f;
static leia::vec3f g_defaultViewingPosition = leia::vec3f(0.0f, 0.0f, 600.0f);
static int g_viewTextureWidth = 0;
static int g_viewTextureHeight = 0;

// Display pixel geometry
static int g_displayPixelWidth = 0;
static int g_displayPixelHeight = 0;
static int g_displayScreenLeft = 0;
static int g_displayScreenTop = 0;
#endif

// Window procedure (runs on main thread)
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        UpdateInputState(g_inputState, msg, wParam, lParam);
    }

    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_windowWidth = LOWORD(lParam);
            g_windowHeight = HIWORD(lParam);
            g_windowResized = true;
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

    case WM_GETMINMAXINFO:
    {
        MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        mmi->ptMinTrackSize.x = 100;
        mmi->ptMinTrackSize.y = 100;
        return 0;
    }
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
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

#if defined(HAVE_LEIA_SR) && defined(HAVE_VK_WEAVER)

static bool CreateSRContext(double maxTimeSeconds) {
    LOG_INFO("Creating SR context (timeout: %.1fs)...", maxTimeSeconds);

    const double startTime = (double)GetTickCount64() / 1000.0;

    while (g_srContext == nullptr) {
        try {
            g_srContext = SR::SRContext::create();
            break;
        }
        catch (SR::ServerNotAvailableException& e) {
            (void)e;
        }
        catch (...) {
            LOG_ERROR("Unknown exception while creating SR context");
        }

        LOG_INFO("Waiting for SR context...");
        Sleep(100);

        double curTime = (double)GetTickCount64() / 1000.0;
        if ((curTime - startTime) > maxTimeSeconds) {
            LOG_ERROR("Timeout waiting for SR context");
            break;
        }
    }

    if (g_srContext == nullptr) {
        return false;
    }
    LOG_INFO("SR context created: 0x%p", g_srContext);

    // Get display manager
    try {
        g_displayManager = SR::GetDisplayManagerInstance(*g_srContext);
        if (g_displayManager != nullptr) {
            g_display = g_displayManager->getPrimaryActiveSRDisplay();
        }
    }
    catch (...) {
        LOG_ERROR("Failed to get DisplayManager - requires runtime version 1.34.8 or later");
        return false;
    }

    if (g_display == nullptr) {
        LOG_ERROR("No SR display found");
        return false;
    }

    // Wait for display to be ready
    bool displayReady = false;
    while (!displayReady) {
        if (g_display->isValid()) {
            SR_recti displayLocation = g_display->getLocation();
            int64_t width = displayLocation.right - displayLocation.left;
            int64_t height = displayLocation.bottom - displayLocation.top;
            if (width > 0 && height > 0) {
                displayReady = true;
                LOG_INFO("SR display ready: %lldx%lld at (%lld,%lld)",
                    width, height, displayLocation.left, displayLocation.top);
                break;
            }
        }

        LOG_INFO("Waiting for display to be ready...");
        Sleep(100);

        double curTime = (double)GetTickCount64() / 1000.0;
        if ((curTime - startTime) > maxTimeSeconds) {
            LOG_ERROR("Timeout waiting for display");
            break;
        }
    }

    return displayReady;
}

static void CleanupLeiaSR() {
    LOG_INFO("Cleaning up LeiaSR...");

    if (g_srWeaver) {
        g_srWeaver->destroy();
        g_srWeaver = nullptr;
    }

    if (g_srContext) {
        SR::SRContext::deleteSRContext(g_srContext);
        g_srContext = nullptr;
    }

    g_displayManager = nullptr;
    g_display = nullptr;

    LOG_INFO("LeiaSR cleanup complete");
}

// Render thread function
static void RenderThreadFunc(HWND hwnd, VulkanState* vk) {
    LOG_INFO("[RenderThread] Started");

    PerformanceStats perfStats = {};
    perfStats.lastTime = std::chrono::high_resolution_clock::now();

    while (g_running.load()) {
        if (IsIconic(hwnd)) {
            Sleep(10);
            continue;
        }

        // Snapshot input state under lock
        InputState inputSnapshot;
        UINT snapWidth, snapHeight;
        bool resetRequested = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            inputSnapshot = g_inputState;
            snapWidth = g_windowWidth;
            snapHeight = g_windowHeight;
            resetRequested = g_inputState.resetViewRequested;
            g_inputState.resetViewRequested = false;
            g_windowResized = false;
        }

        UpdatePerformanceStats(perfStats);
        UpdateCameraMovement(inputSnapshot, perfStats.deltaTime);

        // Write back camera position
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_inputState.cameraPosX = inputSnapshot.cameraPosX;
            g_inputState.cameraPosY = inputSnapshot.cameraPosY;
            g_inputState.cameraPosZ = inputSnapshot.cameraPosZ;
            if (resetRequested) {
                g_inputState.yaw = inputSnapshot.yaw;
                g_inputState.pitch = inputSnapshot.pitch;
                g_inputState.zoomScale = inputSnapshot.zoomScale;
            }
        }

        // Update cube rotation
        vk->cubeRotation += perfStats.deltaTime * 0.5f;
        if (vk->cubeRotation > 6.28318530f) {
            vk->cubeRotation -= 6.28318530f;
        }

        // --- Acquire swapchain image ---
        FrameSync& frame = vk->frames[vk->currentFrame];
        vkWaitForFences(vk->device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX);
        vkResetFences(vk->device, 1, &frame.inFlight);

        uint32_t imageIndex;
        VkResult acquireResult = vkAcquireNextImageKHR(vk->device, vk->swapchain, UINT64_MAX,
            frame.imageAvailable, VK_NULL_HANDLE, &imageIndex);

        if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR || acquireResult == VK_SUBOPTIMAL_KHR) {
            // Swapchain needs recreation - skip this frame
            continue;
        }

        // --- Viewport scale and eye offset for windowed mode ---
        float pixelSizeX = g_screenWidthMM / (float)g_displayPixelWidth;
        float pixelSizeY = g_screenHeightMM / (float)g_displayPixelHeight;

        float winWidthMM  = (float)snapWidth * pixelSizeX;
        float winHeightMM = (float)snapHeight * pixelSizeY;

        float minDisp = fminf(g_screenWidthMM, g_screenHeightMM);
        float minWin  = fminf(winWidthMM, winHeightMM);
        float vs = (minWin > 0.0f) ? (minDisp / minWin) : 1.0f;

        float effectiveWidthMM  = winWidthMM * vs;
        float effectiveHeightMM = winHeightMM * vs;

        // Eye offset for off-center windows
        POINT clientOrigin = {0, 0};
        ClientToScreen(hwnd, &clientOrigin);

        float winCenterPxX = (float)(clientOrigin.x - g_displayScreenLeft) + (float)snapWidth / 2.0f;
        float winCenterPxY = (float)(clientOrigin.y - g_displayScreenTop) + (float)snapHeight / 2.0f;
        float dispCenterPxX = (float)g_displayPixelWidth / 2.0f;
        float dispCenterPxY = (float)g_displayPixelHeight / 2.0f;

        float offsetX_mm = (winCenterPxX - dispCenterPxX) * pixelSizeX;
        float offsetY_mm = -((winCenterPxY - dispCenterPxY) * pixelSizeY);

        // Get predicted eye positions from weaver
        leia::vec3f leftEye, rightEye;
        {
            float leftPos[3], rightPos[3];
            g_srWeaver->getPredictedEyePositions(leftPos, rightPos);
            leftEye = leia::vec3f(leftPos[0], leftPos[1], leftPos[2]);
            rightEye = leia::vec3f(rightPos[0], rightPos[1], rightPos[2]);
        }

        // Apply parallax toggle
        if (!inputSnapshot.parallaxEnabled) {
            leia::vec3f midEye = (leftEye + rightEye) * 0.5f;
            leia::vec3f translation = g_defaultViewingPosition - midEye;
            leftEye = leftEye + translation;
            rightEye = rightEye + translation;
        }

        // Apply eye offset for off-center window
        leia::vec3f eyeOffset = leia::vec3f(offsetX_mm, offsetY_mm, 0.0f);
        leia::vec3f adjLeftEye  = leftEye  - eyeOffset;
        leia::vec3f adjRightEye = rightEye - eyeOffset;

        // --- Begin command buffer ---
        VkCommandBuffer cmd = frame.commandBuffer;
        vkResetCommandBuffer(cmd, 0);

        VkCommandBufferBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &beginInfo);

        // Cube parameters
        const float cubeSize = 60.0f;
        const float znear = 0.1f;
        const float zfar = 10000.0f;

        VkClearValue clearValues[2] = {};
        clearValues[0].color = {{0.05f, 0.05f, 0.15f, 1.0f}};
        clearValues[1].depthStencil = {1.0f, 0};

        // Render each eye to its own framebuffer (separate images for weaver)
        for (int eye = 0; eye < 2; eye++) {
            // Transition per-eye color + depth to attachment layouts
            TransitionImageLayout(cmd, vk->viewImages[eye],
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            TransitionImageLayout(cmd, vk->depthImages[eye],
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                VK_IMAGE_ASPECT_DEPTH_BIT);

            VkRenderPassBeginInfo rpBegin = {};
            rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rpBegin.renderPass = vk->sceneRenderPass;
            rpBegin.framebuffer = vk->sceneFramebuffers[eye];
            rpBegin.renderArea.extent = {vk->viewWidth, vk->viewHeight};
            rpBegin.clearValueCount = 2;
            rpBegin.pClearValues = clearValues;

            vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk->cubePipeline);

            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &vk->cubeVertexBuffer, &offset);
            vkCmdBindIndexBuffer(cmd, vk->cubeIndexBuffer, 0, VK_INDEX_TYPE_UINT16);

            // Full viewport per eye (Y-flip via negative height)
            VkViewport viewport = {};
            viewport.x = 0.0f;
            viewport.y = (float)vk->viewHeight;
            viewport.width = (float)vk->viewWidth;
            viewport.height = -(float)vk->viewHeight;
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            vkCmdSetViewport(cmd, 0, 1, &viewport);

            VkRect2D scissor = {};
            scissor.offset = {0, 0};
            scissor.extent = {vk->viewWidth, vk->viewHeight};
            vkCmdSetScissor(cmd, 0, 1, &scissor);

            leia::vec3f eyePos = (eye == 0) ? adjLeftEye : adjRightEye;

            leia::mat4f mvp = leia::CalculateMVP(
                eyePos,
                effectiveWidthMM, effectiveHeightMM,
                vk->cubeRotation,
                cubeSize,
                znear, zfar
            );

            VkPushConstants pc = {};
            memcpy(pc.transform, mvp.m, sizeof(float) * 16);
            pc.color[0] = 1.0f; pc.color[1] = 1.0f; pc.color[2] = 1.0f; pc.color[3] = 1.0f;

            vkCmdPushConstants(cmd, vk->pipelineLayout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

            vkCmdDrawIndexed(cmd, 36, 1, 0, 0, 0);

            vkCmdEndRenderPass(cmd);

            // Transition per-eye image to shader read for weaver
            TransitionImageLayout(cmd, vk->viewImages[eye],
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }

        // Transition swapchain image to color attachment for weaver output
        TransitionImageLayout(cmd, vk->swapchainImages[imageIndex],
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        // Call the SR Vulkan weaver with separate left/right image views
        RECT weaverRect = {};
        weaverRect.left = 0;
        weaverRect.top = 0;
        weaverRect.right = (LONG)vk->swapchainExtent.width;
        weaverRect.bottom = (LONG)vk->swapchainExtent.height;

        g_srWeaver->setViewport(weaverRect);
        g_srWeaver->setScissorRect(weaverRect);
        g_srWeaver->setCommandBuffer(cmd);
        g_srWeaver->setInputViewTexture(vk->viewImageViews[0], vk->viewImageViews[1],
            (int)(vk->viewWidth), (int)(vk->viewHeight), vk->swapchainFormat);
        g_srWeaver->setOutputFrameBuffer(vk->swapchainFramebuffers[imageIndex],
            (int)vk->swapchainExtent.width, (int)vk->swapchainExtent.height,
            vk->swapchainFormat);
        g_srWeaver->weave();

        // Transition swapchain image to present
        TransitionImageLayout(cmd, vk->swapchainImages[imageIndex],
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

        vkEndCommandBuffer(cmd);

        // Submit
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submitInfo = {};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &frame.imageAvailable;
        submitInfo.pWaitDstStageMask = &waitStage;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &frame.renderFinished;

        vkQueueSubmit(vk->graphicsQueue, 1, &submitInfo, frame.inFlight);

        // Present
        VkPresentInfoKHR presentInfo = {};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &frame.renderFinished;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &vk->swapchain;
        presentInfo.pImageIndices = &imageIndex;

        vkQueuePresentKHR(vk->presentQueue, &presentInfo);

        vk->currentFrame = (vk->currentFrame + 1) % (uint32_t)vk->frames.size();
    }

    // Wait for device idle before exiting
    vkDeviceWaitIdle(vk->device);

    LOG_INFO("[RenderThread] Exiting");
}

#endif // HAVE_LEIA_SR && HAVE_VK_WEAVER

// Main entry point
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);

    if (!InitializeLogging(APP_NAME)) {
        MessageBox(nullptr, L"Failed to initialize logging", L"Warning", MB_OK | MB_ICONWARNING);
    }

    LOG_INFO("=== SR Cube Native Vulkan Application ===");
    LOG_INFO("Direct SR SDK Vulkan rendering without OpenXR");

#if !defined(HAVE_LEIA_SR) || !defined(HAVE_VK_WEAVER)
    LOG_ERROR("LeiaSR Vulkan SDK not available - this application requires the LeiaSR SDK with Vulkan weaver");
    MessageBox(nullptr,
        L"This application requires the LeiaSR SDK with Vulkan weaver.\n\n"
        L"Please ensure LeiaSR SDK is installed and LEIASR_SDKROOT is set.",
        L"LeiaSR SDK Required", MB_OK | MB_ICONERROR);
    ShutdownLogging();
    return 1;
#else

    // Create SR context first to get display location
    LOG_INFO("Creating SR context to find display...");
    if (!CreateSRContext(10.0)) {
        LOG_ERROR("Failed to create SR context");
        MessageBox(nullptr,
            L"Failed to connect to SR Service.\n\n"
            L"Please ensure SR Service is running.",
            L"SR Service Required", MB_OK | MB_ICONERROR);
        ShutdownLogging();
        return 1;
    }

    // Get display location for window positioning
    SR_recti displayLocation = g_display->getLocation();
    g_displayScreenLeft = (int)displayLocation.left;
    g_displayScreenTop = (int)displayLocation.top;
    g_displayPixelWidth = (int)(displayLocation.right - displayLocation.left);
    g_displayPixelHeight = (int)(displayLocation.bottom - displayLocation.top);
    LOG_INFO("SR display at (%d,%d) size %dx%d", g_displayScreenLeft, g_displayScreenTop,
        g_displayPixelWidth, g_displayPixelHeight);

    g_windowWidth = g_displayPixelWidth;
    g_windowHeight = g_displayPixelHeight;

    // Get recommended view texture size
    g_viewTextureWidth = g_display->getRecommendedViewsTextureWidth();
    g_viewTextureHeight = g_display->getRecommendedViewsTextureHeight();
    LOG_INFO("Recommended view texture size: %dx%d", g_viewTextureWidth, g_viewTextureHeight);

    // Get physical screen dimensions (cm to mm)
    g_screenWidthMM = g_display->getPhysicalSizeWidth() * 10.0f;
    g_screenHeightMM = g_display->getPhysicalSizeHeight() * 10.0f;
    LOG_INFO("Screen physical size: %.1fmm x %.1fmm", g_screenWidthMM, g_screenHeightMM);

    // Get default viewing position
    float x_mm, y_mm, z_mm;
    g_display->getDefaultViewingPosition(x_mm, y_mm, z_mm);
    g_defaultViewingPosition = leia::vec3f(x_mm, y_mm, z_mm);
    LOG_INFO("Default viewing position: (%.1f, %.1f, %.1f) mm", x_mm, y_mm, z_mm);

    // Register window class
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = WINDOW_CLASS;

    if (!RegisterClassEx(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            LOG_ERROR("Failed to register window class, error: %lu", err);
            CleanupLeiaSR();
            ShutdownLogging();
            return 1;
        }
    }

    // Create borderless fullscreen window on SR display
    LOG_INFO("Creating window on SR display...");
    HWND hwnd = CreateWindowEx(
        WS_EX_APPWINDOW,
        WINDOW_CLASS,
        WINDOW_TITLE,
        WS_POPUP,
        g_displayScreenLeft, g_displayScreenTop,
        g_displayPixelWidth, g_displayPixelHeight,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!hwnd) {
        LOG_ERROR("Failed to create window, error: %lu", GetLastError());
        CleanupLeiaSR();
        ShutdownLogging();
        return 1;
    }
    LOG_INFO("Window created: 0x%p", hwnd);
    g_windowInfo.hwnd = hwnd;

    // Initialize Vulkan
    LOG_INFO("Initializing Vulkan...");
    VulkanState vk = {};

    if (!CreateVulkanInstance(vk)) {
        LOG_ERROR("Vulkan instance creation failed");
        goto cleanup;
    }

    if (!CreateVulkanSurface(vk, hwnd, hInstance)) {
        LOG_ERROR("Vulkan surface creation failed");
        goto cleanup;
    }

    if (!CreateVulkanDevice(vk)) {
        LOG_ERROR("Vulkan device creation failed");
        goto cleanup;
    }

    if (!CreateCommandPool(vk)) {
        LOG_ERROR("Command pool creation failed");
        goto cleanup;
    }

    // Create SR Vulkan weaver
    LOG_INFO("Creating SR Vulkan weaver...");
    {
        WeaverErrorCode createWeaverResult = SR::CreateVulkanWeaver(
            *g_srContext, vk.device, vk.physicalDevice,
            vk.graphicsQueue, vk.commandPool, hwnd, &g_srWeaver);

        if (createWeaverResult != WeaverErrorCode::WeaverSuccess) {
            LOG_ERROR("Failed to create Vulkan weaver, error: %d", (int)createWeaverResult);
            MessageBox(hwnd, L"Failed to create SR Vulkan weaver", L"Error", MB_OK | MB_ICONERROR);
            goto cleanup;
        }
    }
    LOG_INFO("SR Vulkan weaver created: 0x%p", g_srWeaver);

    // Initialize SR context after creating weaver
    g_srContext->initialize();
    LOG_INFO("SR context initialized");

    if (!CreateSwapchain(vk, g_windowWidth, g_windowHeight)) {
        LOG_ERROR("Swapchain creation failed");
        goto cleanup;
    }

    if (!CreateSwapchainFramebuffers(vk)) {
        LOG_ERROR("Swapchain framebuffer creation failed");
        goto cleanup;
    }

    if (!CreateViewTexture(vk, (uint32_t)g_viewTextureWidth, (uint32_t)g_viewTextureHeight)) {
        LOG_ERROR("View texture creation failed");
        goto cleanup;
    }

    if (!CreateSceneRenderPass(vk)) {
        LOG_ERROR("Scene render pass creation failed");
        goto cleanup;
    }

    if (!CreateCubePipeline(vk)) {
        LOG_ERROR("Cube pipeline creation failed");
        goto cleanup;
    }

    if (!CreateCubeGeometry(vk)) {
        LOG_ERROR("Cube geometry creation failed");
        goto cleanup;
    }

    if (!CreateFrameSync(vk)) {
        LOG_ERROR("Frame sync creation failed");
        goto cleanup;
    }

    // Show window
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    LOG_INFO("");
    LOG_INFO("=== Entering main loop ===");
    LOG_INFO("Controls: WASD=Move, Mouse=Look, P=Toggle Parallax, ESC=Quit");
    LOG_INFO("");

    // Launch render thread
    {
        std::thread renderThread(RenderThreadFunc, hwnd, &vk);

        MSG msg = {};
        while (GetMessage(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        g_running.store(false);

        LOG_INFO("Main thread: waiting for render thread to finish...");
        renderThread.join();
        LOG_INFO("Main thread: render thread joined");
    }

cleanup:
    LOG_INFO("");
    LOG_INFO("=== Shutting down ===");

    if (g_srWeaver) {
        g_srWeaver->destroy();
        g_srWeaver = nullptr;
    }

    CleanupVulkan(vk);

    DestroyWindow(hwnd);
    UnregisterClass(WINDOW_CLASS, hInstance);

    if (g_srContext) {
        SR::SRContext::deleteSRContext(g_srContext);
        g_srContext = nullptr;
    }

    LOG_INFO("Application shutdown complete");
    ShutdownLogging();

    return 0;
#endif // HAVE_LEIA_SR && HAVE_VK_WEAVER
}
