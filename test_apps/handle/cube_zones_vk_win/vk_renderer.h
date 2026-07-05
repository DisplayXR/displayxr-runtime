// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Vulkan rendering for cube and grid
 *
 * Cloned from cube_handle_vk_win's vk_renderer and extended for
 * XR_EXT_display_zones with the ARRAY / single-pass-instanced (SPI) stereo
 * layout: the base single-swapchain path (framebuffers[2]) is unchanged; a
 * ZoneArrayFramebuffers set + RenderSceneToZoneLayer() let each display zone
 * render into its OWN arraySize=2 OpenXR Vulkan swapchain, one view per array
 * LAYER (baseArrayLayer=vi), with a per-zone clear color and spin-phase offset.
 * This mirrors the D3D12 zones app's per-slice TEXTURE2DARRAY RTVs.
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
// XR_EXT_display_zones: per-zone ARRAY / single-pass-instanced framebuffer set
// ---------------------------------------------------------------------------
//
// Each display zone owns ONE OpenXR Vulkan swapchain created with arraySize = 2
// (the two views as array LAYERS). Every swapchain image is a VkImage with 2
// array layers; view vi is rendered full-viewport into layer vi. To bind a
// single layer as a 2D render target we build a VkImageView with
// subresourceRange.baseArrayLayer = vi, layerCount = 1 (VK_IMAGE_VIEW_TYPE_2D),
// and one framebuffer per (image, layer). This is the Vulkan analogue of the
// D3D12 zones app's per-slice TEXTURE2DARRAY RTVs (imageArrayIndex per view).
struct ZoneArrayFramebuffers {
    uint32_t imageCount = 0;
    uint32_t layers = 0;                        //!< array slices per image (== 2)
    uint32_t width = 0;                         //!< per-view tile width
    uint32_t height = 0;                        //!< per-view tile height
    // One color view + framebuffer per (image, layer): index [img*layers+layer].
    std::vector<VkImageView> colorViews;
    std::vector<VkFramebuffer> framebuffers;
    // Single shared depth image/view (per-view tile sized); the render pass
    // CLEARs it at the start of every layer submission, so serial reuse is safe.
    VkImage depthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;
};

// Build a framebuffer set over an array of arraySize=layerCount swapchain
// images. The renderer's renderPass is reused (CLEAR load op). colorFormat MUST
// equal the format the renderer/renderPass was initialized with.
bool CreateZoneArrayFramebuffers(VkRenderer& renderer, ZoneArrayFramebuffers& fb,
    const VkImage* images, uint32_t imageCount, uint32_t layerCount,
    uint32_t width, uint32_t height, VkFormat colorFormat);

// Render the cube + grid scene for ONE view into (imageIndex, layer) with an
// explicit premultiplied clear color and a per-zone rotation offset (added to
// renderer.cubeRotation for this submission only). Full-viewport into the tile.
void RenderSceneToZoneLayer(VkRenderer& renderer, ZoneArrayFramebuffers& fb,
    uint32_t imageIndex, uint32_t layer, const EyeRenderParams& eye,
    const float clearColor[4], float rotationOffset);

// Destroy a zone array framebuffer set (call before xrDestroySwapchain).
void DestroyZoneArrayFramebuffers(VkRenderer& renderer, ZoneArrayFramebuffers& fb);
