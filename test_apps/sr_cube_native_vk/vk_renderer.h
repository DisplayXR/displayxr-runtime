// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Vulkan rendering helpers for native SR cube app
 */

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <vector>

// Push constant data: MVP matrix (64 bytes)
struct VkPushConstants {
    float transform[16];  // column-major 4x4 matrix
    float color[4];
};

// Per-frame synchronization resources
struct FrameSync {
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkSemaphore renderFinished = VK_NULL_HANDLE;
    VkFence inFlight = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
};

// Vulkan state
struct VulkanState {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue presentQueue = VK_NULL_HANDLE;
    uint32_t graphicsFamily = 0;
    uint32_t presentFamily = 0;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;

    // Swapchain
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapchainFormat = VK_FORMAT_B8G8R8A8_SRGB;
    VkExtent2D swapchainExtent = {};
    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainImageViews;

    // Swapchain framebuffers (color-only, for weaver output)
    VkRenderPass swapchainRenderPass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> swapchainFramebuffers;

    // Stereo view texture (side-by-side: viewWidth*2 x viewHeight)
    VkImage viewImage = VK_NULL_HANDLE;
    VkDeviceMemory viewImageMemory = VK_NULL_HANDLE;
    VkImageView viewImageView = VK_NULL_HANDLE;
    uint32_t viewWidth = 0;   // single-eye width
    uint32_t viewHeight = 0;

    // Depth buffer (same SBS dimensions as view texture)
    VkImage depthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthImageMemory = VK_NULL_HANDLE;
    VkImageView depthImageView = VK_NULL_HANDLE;

    // Scene render pass (color + depth) and SBS framebuffer
    VkRenderPass sceneRenderPass = VK_NULL_HANDLE;
    VkFramebuffer sceneFramebuffer = VK_NULL_HANDLE;

    // Pipeline
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline cubePipeline = VK_NULL_HANDLE;

    // Cube geometry
    VkBuffer cubeVertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory cubeVertexMemory = VK_NULL_HANDLE;
    VkBuffer cubeIndexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory cubeIndexMemory = VK_NULL_HANDLE;

    // Per-frame sync
    std::vector<FrameSync> frames;
    uint32_t currentFrame = 0;

    // Layout tracking (matches reference app pattern)
    VkImageLayout viewImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout depthImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // Scene state
    float cubeRotation = 0.0f;
};

// Create Vulkan instance with required extensions
bool CreateVulkanInstance(VulkanState& vk);

// Create Win32 surface
bool CreateVulkanSurface(VulkanState& vk, HWND hwnd, HINSTANCE hInstance);

// Pick physical device and create logical device
bool CreateVulkanDevice(VulkanState& vk);

// Create command pool
bool CreateCommandPool(VulkanState& vk);

// Create swapchain
bool CreateSwapchain(VulkanState& vk, uint32_t width, uint32_t height);

// Create swapchain framebuffers (color-only for weaver output)
bool CreateSwapchainFramebuffers(VulkanState& vk);

// Create stereo view texture and depth buffer
bool CreateViewTexture(VulkanState& vk, uint32_t singleEyeWidth, uint32_t singleEyeHeight);

// Create scene render pass (color + depth) and pipeline
bool CreateSceneRenderPass(VulkanState& vk);
bool CreateCubePipeline(VulkanState& vk);

// Create cube vertex/index buffers
bool CreateCubeGeometry(VulkanState& vk);

// Create per-frame synchronization objects
bool CreateFrameSync(VulkanState& vk);

// Transition image layout using a command buffer
void TransitionImageLayout(VkCommandBuffer cmd, VkImage image,
    VkImageLayout oldLayout, VkImageLayout newLayout,
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);

// Cleanup everything
void CleanupVulkan(VulkanState& vk);
