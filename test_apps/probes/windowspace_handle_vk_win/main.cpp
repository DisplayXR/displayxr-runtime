// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  windowspace_handle_vk_win — submit N XR_EXT window-space layers, no 3D scene.
 *
 * Vulkan handle-class app for issue #389. Flat-cleared projection layer + N
 * window-space layer swapchains, each filled with a distinct solid color via
 * a staging buffer + vkCmdCopyBufferToImage and the same
 * UNDEFINED->TRANSFER_DST->COLOR_ATTACHMENT_OPTIMAL barrier sequence the
 * cube_handle_vk app uses for its HUD (final layout = COLOR_ATTACHMENT_OPTIMAL).
 * Pass the layer count as argv[1] (default 6, clamped [1,12]).
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>

#include "logging.h"
#include "xr_session.h"
#include "xr_window_space_hud.h"
#include "windowspace_layers.h"

#include <string>
#include <vector>

static const char* APP_NAME = "windowspace_handle_vk_win";
static const wchar_t* WINDOW_CLASS = L"DXRWindowSpaceVKClass";
static const wchar_t* WINDOW_TITLE = L"Window-Space Layers — VK Native Compositor";

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
    wc.style = CS_HREDRAW | CS_VREDRAW;
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

// Find a host-visible+coherent memory type.
static uint32_t FindHostVisibleMemType(VkPhysicalDevice phys, uint32_t typeBits) {
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(phys, &props);
    const VkMemoryPropertyFlags need =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if ((typeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & need) == need) return i;
    }
    return UINT32_MAX;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance; (void)lpCmdLine;
    if (!InitializeLogging(APP_NAME)) {
        MessageBox(nullptr, L"Failed to initialize logging", L"Warning", MB_OK | MB_ICONWARNING);
    }

    const uint32_t layerCount = wsl::ParseLayerCount(__argc, __argv);
    LOG_INFO("=== windowspace_handle_vk_win === N=%u window-space layers", layerCount);

    HWND hwnd = CreateAppWindow(hInstance, g_windowWidth, g_windowHeight);
    if (!hwnd) { ShutdownLogging(); return 1; }

    XrSessionManager xr = {};
    g_xr = &xr;
    if (!InitializeOpenXR(xr)) { LOG_ERROR("OpenXR init failed"); g_xr = nullptr; ShutdownLogging(); return 1; }
    if (!GetVulkanGraphicsRequirements(xr)) { LOG_ERROR("VK graphics req failed"); CleanupOpenXR(xr); ShutdownLogging(); return 1; }

    VkInstance vkInstance = VK_NULL_HANDLE;
    if (!CreateVulkanInstance(xr, vkInstance)) { LOG_ERROR("VK instance failed"); CleanupOpenXR(xr); ShutdownLogging(); return 1; }
    VkPhysicalDevice physDevice = VK_NULL_HANDLE;
    if (!GetVulkanPhysicalDevice(xr, vkInstance, physDevice)) { LOG_ERROR("VK phys device failed"); vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); ShutdownLogging(); return 1; }
    std::vector<const char*> deviceExtensions;
    std::vector<std::string> extensionStorage;
    if (!GetVulkanDeviceExtensions(xr, vkInstance, physDevice, deviceExtensions, extensionStorage)) {
        LOG_ERROR("VK device ext failed"); vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); ShutdownLogging(); return 1;
    }
    uint32_t queueFamilyIndex = 0;
    if (!FindGraphicsQueueFamily(physDevice, queueFamilyIndex)) {
        LOG_ERROR("No graphics queue"); vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); ShutdownLogging(); return 1;
    }
    VkDevice vkDevice = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    if (!CreateVulkanDevice(physDevice, queueFamilyIndex, deviceExtensions, vkDevice, graphicsQueue)) {
        LOG_ERROR("VK device failed"); vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); ShutdownLogging(); return 1;
    }
    if (!CreateSession(xr, vkInstance, physDevice, vkDevice, queueFamilyIndex, 0, hwnd)) {
        LOG_ERROR("Session failed"); vkDestroyDevice(vkDevice, nullptr); vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); ShutdownLogging(); return 1;
    }
    if (!CreateSpaces(xr)) { LOG_ERROR("Spaces failed"); CleanupOpenXR(xr); vkDestroyDevice(vkDevice, nullptr); vkDestroyInstance(vkInstance, nullptr); ShutdownLogging(); return 1; }
    if (!CreateSwapchain(xr)) { LOG_ERROR("Swapchain failed"); CleanupOpenXR(xr); vkDestroyDevice(vkDevice, nullptr); vkDestroyInstance(vkInstance, nullptr); ShutdownLogging(); return 1; }

    // Enumerate projection swapchain images.
    std::vector<XrSwapchainImageVulkanKHR> swapchainImages;
    {
        uint32_t count = xr.swapchain.imageCount;
        swapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)swapchainImages.data());
        LOG_INFO("Enumerated %u Vulkan projection swapchain images", count);
    }

    // Create N window-space layer swapchains + enumerate + solid-color buffers.
    std::vector<XrHudSwapchain> layerSC(layerCount);
    std::vector<std::vector<XrSwapchainImageVulkanKHR>> layerImages(layerCount);
    std::vector<std::vector<uint8_t>> layerPixels(layerCount);
    for (uint32_t i = 0; i < layerCount; ++i) {
        if (!CreateHudSwapchain(xr.session, wsl::kLayerPxWidth, wsl::kLayerPxHeight, layerSC[i])) {
            LOG_WARN("Window-space swapchain %u create failed", i); continue;
        }
        uint32_t cnt = layerSC[i].imageCount;
        layerImages[i].resize(cnt, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        xrEnumerateSwapchainImages(layerSC[i].swapchain, cnt, &cnt,
            (XrSwapchainImageBaseHeader*)layerImages[i].data());
        uint8_t rgba[4];
        wsl::LayerColor(i, layerCount, rgba);
        wsl::FillSolid(layerPixels[i], wsl::kLayerPxWidth, wsl::kLayerPxHeight, rgba);
    }

    // Shared staging buffer (host-visible, persistently mapped) + command pool.
    const VkDeviceSize stagingSize = (VkDeviceSize)wsl::kLayerPxWidth * wsl::kLayerPxHeight * 4;
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    void* stagingMapped = nullptr;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    bool vkOk = true;
    {
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = stagingSize; bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(vkDevice, &bi, nullptr, &stagingBuffer) != VK_SUCCESS) { LOG_WARN("staging buffer failed"); vkOk = false; }
        if (vkOk) {
            VkMemoryRequirements mr; vkGetBufferMemoryRequirements(vkDevice, stagingBuffer, &mr);
            uint32_t mt = FindHostVisibleMemType(physDevice, mr.memoryTypeBits);
            if (mt == UINT32_MAX) { LOG_WARN("no host-visible mem"); vkOk = false; }
            else {
                VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                ai.allocationSize = mr.size; ai.memoryTypeIndex = mt;
                vkAllocateMemory(vkDevice, &ai, nullptr, &stagingMemory);
                vkBindBufferMemory(vkDevice, stagingBuffer, stagingMemory, 0);
                vkMapMemory(vkDevice, stagingMemory, 0, stagingSize, 0, &stagingMapped);
            }
        }
        if (vkOk) {
            VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pi.queueFamilyIndex = queueFamilyIndex;
            if (vkCreateCommandPool(vkDevice, &pi, nullptr, &cmdPool) != VK_SUCCESS) { LOG_WARN("cmd pool failed"); vkOk = false; }
        }
    }
    if (!vkOk) LOG_WARN("Window-space layer upload disabled (VK resource creation failed)");

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
                // Clear the projection image to a flat dark background (no 3D scene).
                if (vkOk) {
                    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
                    ai.commandPool = cmdPool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
                    VkCommandBuffer cb; vkAllocateCommandBuffers(vkDevice, &ai, &cb);
                    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                    vkBeginCommandBuffer(cb, &bi);
                    VkImage img = swapchainImages[imageIndex].image;
                    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                    b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    b.image = img; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        0, 0, nullptr, 0, nullptr, 1, &b);
                    VkClearColorValue clear = {};
                    clear.float32[0] = wsl::kBgR / 255.0f; clear.float32[1] = wsl::kBgG / 255.0f;
                    clear.float32[2] = wsl::kBgB / 255.0f; clear.float32[3] = wsl::kBgA / 255.0f;
                    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
                    // TRANSFER_DST -> COLOR_ATTACHMENT_OPTIMAL (final layout the compositor expects).
                    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        0, 0, nullptr, 0, nullptr, 1, &b);
                    vkEndCommandBuffer(cb);
                    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
                    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
                    vkQueueSubmit(graphicsQueue, 1, &si, VK_NULL_HANDLE);
                    vkQueueWaitIdle(graphicsQueue);
                    vkFreeCommandBuffers(vkDevice, cmdPool, 1, &cb);
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

            // Fill each window-space layer image: staging buffer -> vkCmdCopyBufferToImage,
            // ending in COLOR_ATTACHMENT_OPTIMAL.
            if (vkOk) {
                for (uint32_t i = 0; i < layerCount; ++i) {
                    if (layerSC[i].swapchain == XR_NULL_HANDLE) continue;
                    uint32_t idx = 0;
                    if (!AcquireHudSwapchainImage(layerSC[i], idx)) continue;
                    if (idx < layerImages[i].size() && layerImages[i][idx].image && !layerPixels[i].empty()) {
                        memcpy(stagingMapped, layerPixels[i].data(), (size_t)stagingSize);

                        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
                        ai.commandPool = cmdPool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
                        VkCommandBuffer cb; vkAllocateCommandBuffers(vkDevice, &ai, &cb);
                        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                        vkBeginCommandBuffer(cb, &bi);
                        VkImage img = layerImages[i][idx].image;
                        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                        b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        b.image = img; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            0, 0, nullptr, 0, nullptr, 1, &b);
                        VkBufferImageCopy region = {};
                        region.bufferRowLength = wsl::kLayerPxWidth;
                        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                        region.imageOffset = {0, 0, 0};
                        region.imageExtent = {wsl::kLayerPxWidth, wsl::kLayerPxHeight, 1};
                        vkCmdCopyBufferToImage(cb, stagingBuffer, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            0, 0, nullptr, 0, nullptr, 1, &b);
                        vkEndCommandBuffer(cb);
                        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
                        si.commandBufferCount = 1; si.pCommandBuffers = &cb;
                        vkQueueSubmit(graphicsQueue, 1, &si, VK_NULL_HANDLE);
                        vkQueueWaitIdle(graphicsQueue);
                        vkFreeCommandBuffers(vkDevice, cmdPool, 1, &cb);
                    }
                    ReleaseHudSwapchainImage(layerSC[i]);
                }
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
    if (cmdPool != VK_NULL_HANDLE) vkDestroyCommandPool(vkDevice, cmdPool, nullptr);
    if (stagingBuffer != VK_NULL_HANDLE) {
        if (stagingMapped) vkUnmapMemory(vkDevice, stagingMemory);
        vkDestroyBuffer(vkDevice, stagingBuffer, nullptr);
    }
    if (stagingMemory != VK_NULL_HANDLE) vkFreeMemory(vkDevice, stagingMemory, nullptr);
    g_xr = nullptr;
    CleanupOpenXR(xr);
    vkDestroyDevice(vkDevice, nullptr);
    vkDestroyInstance(vkInstance, nullptr);
    DestroyWindow(hwnd);
    UnregisterClass(WINDOW_CLASS, hInstance);
    ShutdownLogging();
    return 0;
}
