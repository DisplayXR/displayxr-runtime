// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Vulkan OpenXR test WITHOUT XR_EXT_session_target
 *
 * This application tests OpenXR with Vulkan WITHOUT using the XR_EXT_session_target
 * extension. OpenXR/Monado will create its own window for rendering.
 * This helps isolate whether issues are with the extension or base OpenXR.
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>

#include "logging.h"
#include "xr_session.h"
#include "vulkan_renderer.h"
#include "input_state.h"

#include <chrono>
#include <string>
#include <sstream>

// Window class name (for control window only - not used for rendering)
static const wchar_t* WINDOW_CLASS = L"VulkanTestNoSessionTargetClass";
static const wchar_t* WINDOW_TITLE = L"Vulkan Test (No Session Target) - Press ESC to exit";

// Global state
static InputState g_inputState;
static bool g_windowResized = false;
static UINT g_windowWidth = 1280;
static UINT g_windowHeight = 720;

// Window procedure
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Handle input
    UpdateInputState(g_inputState, msg, wParam, lParam);

    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            g_windowWidth = LOWORD(lParam);
            g_windowHeight = HIWORD(lParam);
            g_windowResized = true;
        }
        return 0;

    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            PostQuitMessage(0);
            return 0;
        }
        break;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// Create the application window
static HWND CreateAppWindow(HINSTANCE hInstance, int width, int height) {
    LOG_INFO("Creating application window (%dx%d)", width, height);

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
        LOG_ERROR("Failed to register window class, error: %lu", err);
        MessageBox(nullptr, L"Failed to register window class", L"Error", MB_OK | MB_ICONERROR);
        return nullptr;
    }
    LOG_INFO("Window class registered successfully");

    // Calculate window size to get desired client area
    RECT rect = { 0, 0, width, height };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindowEx(
        0,
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
        DWORD err = GetLastError();
        LOG_ERROR("Failed to create window, error: %lu", err);
        MessageBox(nullptr, L"Failed to create window", L"Error", MB_OK | MB_ICONERROR);
        return nullptr;
    }

    LOG_INFO("Window created successfully, HWND: 0x%p", hwnd);
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

// Main entry point
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    // Initialize logging first
    if (!InitializeLogging()) {
        MessageBox(nullptr, L"Failed to initialize logging", L"Warning", MB_OK | MB_ICONWARNING);
        // Continue anyway - just won't have file logging
    }

    LOG_INFO("=== Vulkan OpenXR Test (NO session_target extension) ===");
    LOG_INFO("OpenXR/Monado will create its own window for rendering");
    LOG_INFO("Starting initialization...");

    // Create window
    HWND hwnd = CreateAppWindow(hInstance, g_windowWidth, g_windowHeight);
    if (!hwnd) {
        LOG_ERROR("Window creation failed, exiting");
        ShutdownLogging();
        return 1;
    }

    // Initialize OpenXR and get required Vulkan instance extensions
    LOG_INFO("Initializing OpenXR and querying Vulkan requirements...");
    XrSessionManager xr = {};
    std::vector<const char*> vkInstanceExtensions;

    // Always need VK_KHR_surface and VK_KHR_win32_surface for window presentation
    vkInstanceExtensions.push_back("VK_KHR_surface");
    vkInstanceExtensions.push_back("VK_KHR_win32_surface");

    if (!GetVulkanInstanceExtensions(xr, vkInstanceExtensions)) {
        LOG_ERROR("Failed to get Vulkan instance extensions from OpenXR");
        MessageBox(hwnd, L"Failed to initialize OpenXR", L"Error", MB_OK | MB_ICONERROR);
        ShutdownLogging();
        return 1;
    }

    // Initialize Vulkan instance
    LOG_INFO("Initializing Vulkan instance...");
    VulkanRenderer renderer = {};
    if (!InitializeVulkanInstance(renderer, vkInstanceExtensions)) {
        LOG_ERROR("Vulkan instance creation failed");
        MessageBox(hwnd, L"Failed to create Vulkan instance", L"Error", MB_OK | MB_ICONERROR);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Get the physical device that OpenXR wants
    VkPhysicalDevice physicalDevice;
    if (!GetVulkanPhysicalDevice(xr, renderer.instance, physicalDevice)) {
        LOG_ERROR("Failed to get Vulkan physical device from OpenXR");
        MessageBox(hwnd, L"Failed to get Vulkan physical device", L"Error", MB_OK | MB_ICONERROR);
        CleanupVulkan(renderer);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Get required Vulkan device extensions
    std::vector<const char*> vkDeviceExtensions;
    if (!GetVulkanDeviceExtensions(xr, renderer.instance, physicalDevice, vkDeviceExtensions)) {
        LOG_ERROR("Failed to get Vulkan device extensions from OpenXR");
        MessageBox(hwnd, L"Failed to get Vulkan device extensions", L"Error", MB_OK | MB_ICONERROR);
        CleanupVulkan(renderer);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Initialize Vulkan device
    LOG_INFO("Initializing Vulkan device...");
    if (!InitializeVulkanDevice(renderer, physicalDevice, vkDeviceExtensions)) {
        LOG_ERROR("Vulkan device creation failed");
        MessageBox(hwnd, L"Failed to create Vulkan device", L"Error", MB_OK | MB_ICONERROR);
        CleanupVulkan(renderer);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }
    LOG_INFO("Vulkan initialized successfully");

    // Complete OpenXR initialization with the Vulkan instance
    LOG_INFO("Completing OpenXR initialization...");
    if (!InitializeOpenXR(xr, renderer.instance)) {
        LOG_ERROR("OpenXR initialization failed");
        MessageBox(hwnd, L"Failed to initialize OpenXR", L"Error", MB_OK | MB_ICONERROR);
        CleanupVulkan(renderer);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }
    LOG_INFO("OpenXR instance initialized successfully");

    // Create OpenXR session with Vulkan device - NO window handle (no session_target)
    // OpenXR/Monado will create its own window
    LOG_INFO("Creating OpenXR session WITHOUT XR_EXT_session_target (passing nullptr for HWND)...");
    if (!CreateSession(xr, renderer.instance, renderer.physicalDevice, renderer.device,
        renderer.graphicsQueueFamily, 0, nullptr)) {
        LOG_ERROR("OpenXR session creation failed");
        MessageBox(hwnd, L"Failed to create OpenXR session", L"Error", MB_OK | MB_ICONERROR);
        CleanupVulkan(renderer);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }
    LOG_INFO("OpenXR session created successfully");

    // Create reference spaces
    LOG_INFO("Creating reference spaces...");
    if (!CreateSpaces(xr)) {
        LOG_ERROR("Reference space creation failed");
        MessageBox(hwnd, L"Failed to create reference spaces", L"Error", MB_OK | MB_ICONERROR);
        CleanupVulkan(renderer);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }
    LOG_INFO("Reference spaces created successfully");

    // Create swapchains
    LOG_INFO("Creating swapchains...");
    if (!CreateSwapchains(xr)) {
        LOG_ERROR("Swapchain creation failed");
        MessageBox(hwnd, L"Failed to create swapchains", L"Error", MB_OK | MB_ICONERROR);
        CleanupVulkan(renderer);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }
    LOG_INFO("Swapchains created successfully (width: %u, height: %u)",
        xr.swapchains[0].width, xr.swapchains[0].height);

    // Create Vulkan rendering resources
    LOG_INFO("Creating Vulkan rendering resources...");
    if (!CreateRenderingResources(renderer)) {
        LOG_ERROR("Failed to create rendering resources");
        MessageBox(hwnd, L"Failed to create rendering resources", L"Error", MB_OK | MB_ICONERROR);
        CleanupVulkan(renderer);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Set up per-eye swapchain resources
    LOG_INFO("Setting up per-eye swapchain resources...");
    for (int eye = 0; eye < 2; eye++) {
        std::vector<VkImage> swapchainImages = GetSwapchainImages(xr, eye);
        VkFormat format = (VkFormat)xr.swapchains[eye].format;

        if (!SetupSwapchainResources(renderer, eye, swapchainImages,
            xr.swapchains[eye].width, xr.swapchains[eye].height, format)) {
            LOG_ERROR("Failed to set up swapchain resources for eye %d", eye);
            MessageBox(hwnd, L"Failed to set up swapchain resources", L"Error", MB_OK | MB_ICONERROR);
            CleanupVulkan(renderer);
            CleanupOpenXR(xr);
            ShutdownLogging();
            return 1;
        }
    }
    LOG_INFO("Swapchain resources created successfully");

    // Show window
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    LOG_INFO("Window shown");

    // Performance tracking
    PerformanceStats perfStats = {};
    perfStats.lastTime = std::chrono::high_resolution_clock::now();

    LOG_INFO("");
    LOG_INFO("=== Initialization complete, entering main loop ===");
    LOG_INFO("");

    // Main loop
    MSG msg = {};
    bool running = true;
    int frameNumber = 0;

    while (running && !xr.exitRequested) {
        frameNumber++;
        // Process Windows messages
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = false;
                RequestExit(xr);
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        if (!running) break;

        // Update performance stats
        UpdatePerformanceStats(perfStats);

        // Update input-based camera movement
        UpdateCameraMovement(g_inputState, perfStats.deltaTime);

        // Update scene (cube rotation)
        UpdateScene(renderer, perfStats.deltaTime);

        // Poll OpenXR events
        PollEvents(xr);

        // Only render if session is running
        if (xr.sessionRunning) {
            XrFrameState frameState;
            if (BeginFrame(xr, frameState)) {
                XrCompositionLayerProjectionView projectionViews[2] = {};

                if (frameState.shouldRender) {
                    // Get view matrices
                    Mat4 leftViewMatrix, leftProjMatrix;
                    Mat4 rightViewMatrix, rightProjMatrix;

                    if (LocateViews(xr, frameState.predictedDisplayTime,
                        leftViewMatrix, leftProjMatrix,
                        rightViewMatrix, rightProjMatrix)) {

                        // Render each eye
                        for (int eye = 0; eye < 2; eye++) {
                            uint32_t imageIndex;
                            if (AcquireSwapchainImage(xr, eye, imageIndex)) {
                                // Select view/projection matrices
                                Mat4& viewMatrix = (eye == 0) ? leftViewMatrix : rightViewMatrix;
                                Mat4& projMatrix = (eye == 0) ? leftProjMatrix : rightProjMatrix;

                                // Render scene
                                RenderScene(renderer, eye, imageIndex,
                                    viewMatrix, projMatrix,
                                    g_inputState.cameraPosX,
                                    g_inputState.cameraPosY,
                                    g_inputState.cameraPosZ,
                                    g_inputState.yaw,
                                    g_inputState.pitch);

                                ReleaseSwapchainImage(xr, eye);

                                // Set up projection view for this eye
                                projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                                projectionViews[eye].subImage.swapchain = xr.swapchains[eye].swapchain;
                                projectionViews[eye].subImage.imageRect.offset = {0, 0};
                                projectionViews[eye].subImage.imageRect.extent = {
                                    (int32_t)xr.swapchains[eye].width,
                                    (int32_t)xr.swapchains[eye].height
                                };
                                projectionViews[eye].subImage.imageArrayIndex = 0;

                                // Get pose from views (we need to re-query or store from LocateViews)
                                XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
                                locateInfo.viewConfigurationType = xr.viewConfigType;
                                locateInfo.displayTime = frameState.predictedDisplayTime;
                                locateInfo.space = xr.localSpace;

                                XrViewState viewState = {XR_TYPE_VIEW_STATE};
                                uint32_t viewCount = 2;
                                XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
                                xrLocateViews(xr.session, &locateInfo, &viewState, 2, &viewCount, views);

                                projectionViews[eye].pose = views[eye].pose;
                                projectionViews[eye].fov = views[eye].fov;
                            }
                        }
                    }
                }

                // End frame
                EndFrame(xr, frameState.predictedDisplayTime, projectionViews);
            }
        } else {
            // Not running - sleep to avoid spinning
            Sleep(100);
        }
    }

    // Cleanup
    LOG_INFO("");
    LOG_INFO("=== Shutting down ===");

    // Wait for GPU to finish
    if (renderer.device) {
        vkDeviceWaitIdle(renderer.device);
    }

    LOG_INFO("Cleaning up Vulkan...");
    CleanupVulkan(renderer);

    LOG_INFO("Cleaning up OpenXR...");
    CleanupOpenXR(xr);

    LOG_INFO("Destroying window...");
    DestroyWindow(hwnd);
    UnregisterClass(WINDOW_CLASS, hInstance);

    LOG_INFO("Application shutdown complete");
    ShutdownLogging();

    return 0;
}
