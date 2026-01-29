// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Vulkan rendering for cube and grid
 */

#pragma once

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <DirectXMath.h>
#include <vector>

struct VkRenderer {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    uint32_t queueFamilyIndex = 0;

    // Render pass
    VkRenderPass renderPass = VK_NULL_HANDLE;

    // Pipeline layout + pipelines
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline cubePipeline = VK_NULL_HANDLE;
    VkPipeline gridPipeline = VK_NULL_HANDLE;

    // Cube geometry
    VkBuffer cubeVertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory cubeVertexMemory = VK_NULL_HANDLE;
    VkBuffer cubeIndexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory cubeIndexMemory = VK_NULL_HANDLE;

    // Grid geometry
    VkBuffer gridVertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory gridVertexMemory = VK_NULL_HANDLE;
    int gridVertexCount = 0;

    // Per-eye depth images
    VkImage depthImages[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceMemory depthMemory[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImageView depthViews[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};

    // Per-eye per-swapchain-image framebuffers and image views
    std::vector<VkImageView> swapchainImageViews[2];
    std::vector<VkFramebuffer> framebuffers[2];

    // Command pool and buffers
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;

    // Fence for frame synchronization
    VkFence frameFence = VK_NULL_HANDLE;

    // Scene state
    float cubeRotation = 0.0f;
};

// Push constant data: MVP matrix + color
struct VkPushConstants {
    DirectX::XMFLOAT4X4 transform;
    float color[4];
};

// Initialize Vulkan renderer (pipelines, geometry)
bool InitializeVkRenderer(VkRenderer& renderer, VkDevice device, VkPhysicalDevice physDevice,
    VkQueue queue, uint32_t queueFamilyIndex, VkFormat colorFormat);

// Create framebuffers for swapchain images
bool CreateSwapchainFramebuffers(VkRenderer& renderer, int eye,
    const VkImage* images, uint32_t count,
    uint32_t width, uint32_t height, VkFormat colorFormat);

// Update scene
void UpdateScene(VkRenderer& renderer, float deltaTime);

// Render to a specific swapchain image
void RenderScene(
    VkRenderer& renderer,
    int eye, uint32_t imageIndex,
    uint32_t width, uint32_t height,
    const DirectX::XMMATRIX& viewMatrix,
    const DirectX::XMMATRIX& projMatrix,
    float zoomScale = 1.0f
);

// Cleanup
void CleanupVkRenderer(VkRenderer& renderer);
