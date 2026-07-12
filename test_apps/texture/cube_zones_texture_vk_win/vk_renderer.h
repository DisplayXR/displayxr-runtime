// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Vulkan rendering for cube and grid
 *
 * Cloned from cube_handle_vk_win's vk_renderer and extended for
 * XR_DXR_display_zones: the base single-swapchain path (framebuffers[2]) is
 * unchanged; a generalized ZoneFramebuffers set + RenderSceneToZone() let each
 * display zone render the same cube+grid scene into its OWN OpenXR Vulkan
 * swapchain with a per-zone clear color and spin-phase offset (mirrors the
 * macOS cube_zones_vk_macos RenderSceneToFramebuffer).
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

    // Render pass (clears attachments, single pass renders all eyes)
    VkRenderPass renderPass = VK_NULL_HANDLE;

    // Pipeline layouts + pipelines
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;      // Grid: 64-byte push constants
    VkPipelineLayout cubePipelineLayout = VK_NULL_HANDLE;   // Cube: descriptor set + 128-byte push constants
    VkPipeline cubePipeline = VK_NULL_HANDLE;
    VkPipeline gridPipeline = VK_NULL_HANDLE;

    // Zone content-alpha edge fade (#613 parity): a fullscreen per-tile pass that
    // ramps each zone view tile's RGBA to 0 near its borders (multiply blend).
    VkPipelineLayout zoneFeatherPipelineLayout = VK_NULL_HANDLE; // 12-byte frag push const
    VkPipeline zoneFeatherPipeline = VK_NULL_HANDLE;

    // Textures (basecolor, normal, AO)
    VkImage texImages[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceMemory texMemory[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImageView texViews[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkSampler texSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    bool texturesLoaded = false;

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

// Push constant data for grid: MVP matrix + color (64 bytes)
struct VkPushConstants {
    DirectX::XMFLOAT4X4 transform;
    float color[4];
};

// Push constant data for textured cube: MVP + model matrices (128 bytes)
struct VkCubePushConstants {
    DirectX::XMFLOAT4X4 mvp;
    DirectX::XMFLOAT4X4 model;
};

// Initialize Vulkan renderer (pipelines, geometry)
bool InitializeVkRenderer(VkRenderer& renderer, VkDevice device, VkPhysicalDevice physDevice,
    VkQueue queue, uint32_t queueFamilyIndex, VkFormat colorFormat);

// Create framebuffers for swapchain images
bool CreateSwapchainFramebuffers(VkRenderer& renderer, int eye,
    const VkImage* images, uint32_t count,
    uint32_t width, uint32_t height, VkFormat colorFormat);

// Update scene. spinSpeed (rad/s) is agent-settable via the set_spin
// MCP tool (#457).
void UpdateScene(VkRenderer& renderer, float deltaTime, float spinSpeed = 0.5f);

// Per-eye render parameters for single-pass SBS rendering
struct EyeRenderParams {
    uint32_t viewportX, viewportY, width, height;
    DirectX::XMMATRIX viewMatrix;
    DirectX::XMMATRIX projMatrix;
};

// Render all eyes in a single render pass (avoids LOAD_OP_LOAD issues).
// eyes: array of per-eye viewport/matrix params, eyeCount: number of eyes.
void RenderScene(
    VkRenderer& renderer,
    uint32_t imageIndex,
    uint32_t framebufferWidth, uint32_t framebufferHeight,
    const EyeRenderParams* eyes, int eyeCount,
    float zoomScale = 1.0f
);

// Cleanup
void CleanupVkRenderer(VkRenderer& renderer);

// ---------------------------------------------------------------------------
// XR_DXR_display_zones: generalized per-zone framebuffer set
// ---------------------------------------------------------------------------
//
// The base RenderScene() above is hardwired to renderer.framebuffers[0] (the
// single window swapchain). Each display zone owns its OWN OpenXR Vulkan
// swapchain (horizontally tiled per view), so it needs its own framebuffer set
// over those images + a shared depth image. This mirrors the macOS app's
// SwapchainFramebuffers / CreateFramebuffersInto / RenderSceneToFramebuffer.
struct ZoneFramebuffers {
    std::vector<VkImageView> colorViews;
    std::vector<VkImageView> depthViews;
    std::vector<VkFramebuffer> framebuffers;
    VkImage depthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
};

// Build a framebuffer set over an arbitrary array of swapchain images (the
// zone's full tiled extent). The renderer's renderPass is reused (CLEAR load
// op, same as the window path).
bool CreateZoneFramebuffers(VkRenderer& renderer, ZoneFramebuffers& fb,
    const VkImage* images, uint32_t count,
    uint32_t width, uint32_t height, VkFormat colorFormat);

// Render the cube + grid scene into one zone framebuffer with an explicit
// premultiplied clear color and a per-zone rotation offset (added to
// renderer.cubeRotation for this submission only — the caller does NOT need to
// save/restore the shared rotation).
void RenderSceneToZone(VkRenderer& renderer, ZoneFramebuffers& fb,
    uint32_t imageIndex, const EyeRenderParams* eyes, int eyeCount,
    const float clearColor[4], float rotationOffset);

// Destroy a zone framebuffer set (call before xrDestroySwapchain).
void DestroyZoneFramebuffers(VkRenderer& renderer, ZoneFramebuffers& fb);
