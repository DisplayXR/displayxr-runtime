// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Vulkan rendering for cube, grid, and scene
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>

#include <vector>
#include <string>
#include <array>

// Simple math types (matching DirectXMath layout for compatibility)
struct Vec3 {
    float x, y, z;
};

struct Vec4 {
    float x, y, z, w;
};

struct Mat4 {
    float m[4][4];

    static Mat4 Identity();
    static Mat4 Translation(float x, float y, float z);
    static Mat4 RotationY(float angle);
    static Mat4 RotationRollPitchYaw(float pitch, float yaw, float roll);
    static Mat4 PerspectiveFov(float fovY, float aspect, float nearZ, float farZ);
    Mat4 operator*(const Mat4& other) const;
    Mat4 Transpose() const;
};

// Uniform buffer structure for shaders
struct UniformBufferObject {
    Mat4 worldViewProj;
    Vec4 color;
};

// Per-swapchain-image rendering resources
struct SwapchainImageResources {
    VkImage image = VK_NULL_HANDLE;           // From OpenXR swapchain
    VkImageView imageView = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkImage depthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;
};

struct VulkanRenderer {
    // Core Vulkan objects
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    uint32_t graphicsQueueFamily = 0;

    // Command resources
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkFence renderFence = VK_NULL_HANDLE;

    // Render pass
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkFormat colorFormat = VK_FORMAT_R8G8B8A8_SRGB;
    VkFormat depthFormat = VK_FORMAT_D24_UNORM_S8_UINT;

    // Cube pipeline
    VkPipelineLayout cubePipelineLayout = VK_NULL_HANDLE;
    VkPipeline cubePipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout cubeDescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool cubeDescriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet cubeDescriptorSet = VK_NULL_HANDLE;

    // Grid pipeline
    VkPipelineLayout gridPipelineLayout = VK_NULL_HANDLE;
    VkPipeline gridPipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout gridDescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool gridDescriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet gridDescriptorSet = VK_NULL_HANDLE;

    // Cube geometry
    VkBuffer cubeVertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory cubeVertexMemory = VK_NULL_HANDLE;
    VkBuffer cubeIndexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory cubeIndexMemory = VK_NULL_HANDLE;
    uint32_t cubeIndexCount = 0;

    // Grid geometry
    VkBuffer gridVertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory gridVertexMemory = VK_NULL_HANDLE;
    uint32_t gridVertexCount = 0;

    // Uniform buffers (one for cube, one for grid)
    VkBuffer cubeUniformBuffer = VK_NULL_HANDLE;
    VkDeviceMemory cubeUniformMemory = VK_NULL_HANDLE;
    void* cubeUniformMapped = nullptr;

    VkBuffer gridUniformBuffer = VK_NULL_HANDLE;
    VkDeviceMemory gridUniformMemory = VK_NULL_HANDLE;
    void* gridUniformMapped = nullptr;

    // Per-eye resources (indexed by eye, then by swapchain image)
    std::vector<SwapchainImageResources> eyeResources[2];

    // Scene state
    float cubeRotation = 0.0f;

    // Shader directory path
    std::string shaderDir;

    // Swapchain dimensions (per eye)
    uint32_t swapchainWidth[2] = {0, 0};
    uint32_t swapchainHeight[2] = {0, 0};
};

// Initialize Vulkan instance with required extensions for OpenXR
// Returns list of required instance extensions
bool InitializeVulkanInstance(VulkanRenderer& renderer, const std::vector<const char*>& requiredExtensions);

// Create Vulkan device with required extensions for OpenXR
bool InitializeVulkanDevice(
    VulkanRenderer& renderer,
    VkPhysicalDevice physicalDevice,
    const std::vector<const char*>& requiredExtensions
);

// Create rendering resources (pipelines, buffers, etc.)
// Must be called after swapchains are created
bool CreateRenderingResources(VulkanRenderer& renderer);

// Set up per-eye swapchain image resources
bool SetupSwapchainResources(
    VulkanRenderer& renderer,
    int eye,
    const std::vector<VkImage>& swapchainImages,
    uint32_t width,
    uint32_t height,
    VkFormat format
);

// Clean up Vulkan resources
void CleanupVulkan(VulkanRenderer& renderer);

// Update scene state (cube rotation, etc.)
void UpdateScene(VulkanRenderer& renderer, float deltaTime);

// Render the scene to a swapchain image
// viewMatrix and projMatrix come from OpenXR views
void RenderScene(
    VulkanRenderer& renderer,
    int eye,
    uint32_t imageIndex,
    const Mat4& viewMatrix,
    const Mat4& projMatrix,
    float cameraPosX,
    float cameraPosY,
    float cameraPosZ,
    float cameraYaw,
    float cameraPitch
);

// Get the Vulkan instance (for OpenXR)
VkInstance GetVulkanInstance(const VulkanRenderer& renderer);

// Get the Vulkan physical device (for OpenXR)
VkPhysicalDevice GetVulkanPhysicalDevice(const VulkanRenderer& renderer);

// Get the Vulkan device (for OpenXR)
VkDevice GetVulkanDevice(const VulkanRenderer& renderer);

// Get the graphics queue family index (for OpenXR)
uint32_t GetGraphicsQueueFamily(const VulkanRenderer& renderer);

// Get the graphics queue (for OpenXR)
VkQueue GetGraphicsQueue(const VulkanRenderer& renderer);
