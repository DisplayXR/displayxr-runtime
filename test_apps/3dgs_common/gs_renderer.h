// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  3DGS renderer adapter for OpenXR stereo rendering
 *
 * Wraps 3DGS.cpp's compute-based Gaussian splatting pipeline for use with
 * OpenXR swapchain images.  Accepts an external Vulkan device (provided by the
 * OpenXR runtime) and renders stereo views into SBS swapchain regions.
 */

#pragma once

#include <vulkan/vulkan.h>
#include <string>
#include <memory>

// Forward declarations — keep 3DGS.cpp headers out of this public header
namespace VulkanSplatting {
struct RendererConfiguration;
}
class Renderer;
class VulkanContext;

struct GsRenderer {
    // Initialize with the OpenXR runtime's Vulkan resources.
    // Must be called before any other methods.
    bool init(VkInstance instance,
              VkPhysicalDevice physicalDevice,
              VkDevice device,
              VkQueue queue,
              uint32_t queueFamilyIndex,
              uint32_t renderWidth,
              uint32_t renderHeight);

    // Load a .ply Gaussian splatting scene file.
    // Returns false if the file cannot be loaded.
    bool loadScene(const char* plyPath);

    // Returns true if a scene is currently loaded and ready to render.
    bool hasScene() const;

    // Returns the loaded scene file path (empty if no scene loaded).
    const std::string& scenePath() const;

    // Returns the number of Gaussians in the loaded scene.
    uint32_t gaussianCount() const;

    // Render one eye's view to a region of a Vulkan swapchain image.
    // The swapchain image must have VK_IMAGE_USAGE_STORAGE_BIT.
    // viewMatrix and projMatrix are column-major float[16].
    void renderEye(VkImage swapchainImage,
                   VkImageView swapchainImageView,
                   VkFormat format,
                   uint32_t imageWidth,
                   uint32_t imageHeight,
                   uint32_t viewportX,
                   uint32_t viewportY,
                   uint32_t viewportWidth,
                   uint32_t viewportHeight,
                   const float viewMatrix[16],
                   const float projMatrix[16]);

    // Clean up all resources.
    void cleanup();

    ~GsRenderer();

private:
    std::shared_ptr<VulkanContext> context;
    std::unique_ptr<Renderer> renderer;
    std::string loadedScenePath;
    uint32_t numGaussians = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool initialized = false;
    bool sceneLoaded = false;
};
