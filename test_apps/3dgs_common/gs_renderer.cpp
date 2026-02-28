// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  3DGS renderer adapter — Phase 1 placeholder implementation
 *
 * Renders a colored gradient placeholder per eye to verify the full
 * OpenXR → Vulkan → swapchain pipeline works.  Real 3DGS.cpp compute
 * pipeline integration will replace renderEye() in Phase 2.
 */

#include "gs_renderer.h"
#include <cstdio>
#include <cstring>

bool GsRenderer::init(VkInstance instance,
                      VkPhysicalDevice physicalDevice,
                      VkDevice device,
                      VkQueue queue,
                      uint32_t queueFamilyIndex,
                      uint32_t renderWidth,
                      uint32_t renderHeight)
{
    device_ = device;
    queue_ = queue;
    queueFamily_ = queueFamilyIndex;
    width_ = renderWidth;
    height_ = renderHeight;

    // Create a command pool for our render operations
    VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamilyIndex;
    VkResult res = vkCreateCommandPool(device, &poolInfo, nullptr, &cmdPool_);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "GsRenderer: failed to create command pool (%d)\n", res);
        return false;
    }

    initialized_ = true;
    printf("GsRenderer: initialized (%ux%u render target)\n", width_, height_);
    return true;
}

bool GsRenderer::loadScene(const char* plyPath)
{
    if (!initialized_) {
        fprintf(stderr, "GsRenderer: not initialized\n");
        return false;
    }

    // Phase 1: Just store the path — actual PLY loading comes in Phase 2
    loadedScenePath_ = plyPath;
    sceneLoaded_ = true;
    numGaussians_ = 0;  // Placeholder — no actual Gaussians loaded yet

    printf("GsRenderer: scene loaded (placeholder): %s\n", plyPath);
    return true;
}

bool GsRenderer::hasScene() const
{
    return sceneLoaded_;
}

const std::string& GsRenderer::scenePath() const
{
    return loadedScenePath_;
}

uint32_t GsRenderer::gaussianCount() const
{
    return numGaussians_;
}

void GsRenderer::renderEye(VkCommandBuffer cmd,
                            VkImage swapchainImage,
                            uint32_t imageWidth,
                            uint32_t imageHeight,
                            uint32_t viewportX,
                            uint32_t viewportY,
                            uint32_t viewportWidth,
                            uint32_t viewportHeight,
                            const float viewMatrix[16],
                            const float projMatrix[16])
{
    if (!hasScene()) return;

    // Phase 1 placeholder: clear the viewport region with a teal color
    // to confirm the rendering pipeline works end-to-end.
    //
    // The caller (main.cpp) is responsible for image layout transitions.
    // This function assumes the image is in VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL.

    VkClearColorValue clearColor = {{0.05f, 0.15f, 0.20f, 1.0f}};

    VkImageSubresourceRange range = {};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel = 0;
    range.levelCount = 1;
    range.baseArrayLayer = 0;
    range.layerCount = 1;

    // Clear the entire image (caller handles viewport-specific rendering)
    vkCmdClearColorImage(cmd, swapchainImage,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &clearColor, 1, &range);
}

void GsRenderer::cleanup()
{
    if (cmdPool_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, cmdPool_, nullptr);
        cmdPool_ = VK_NULL_HANDLE;
    }
    sceneLoaded_ = false;
    initialized_ = false;
}

GsRenderer::~GsRenderer()
{
    cleanup();
}
