// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  3DGS renderer adapter for OpenXR stereo rendering
 *
 * Wraps a Gaussian splatting compute pipeline for use with OpenXR swapchain
 * images.  Accepts an external Vulkan device (provided by the OpenXR runtime)
 * and renders stereo views into SBS swapchain regions.
 *
 * Phase 1: Placeholder renderer using Vulkan clear/blit operations.
 * Phase 2: Integrate 3DGS.cpp compute pipeline via FetchContent.
 */

#pragma once

#include <vulkan/vulkan.h>
#include <string>

struct GsRenderer {
    // Initialize with the OpenXR runtime's Vulkan resources.
    bool init(VkInstance instance,
              VkPhysicalDevice physicalDevice,
              VkDevice device,
              VkQueue queue,
              uint32_t queueFamilyIndex,
              uint32_t renderWidth,
              uint32_t renderHeight);

    // Load a .ply Gaussian splatting scene file.
    bool loadScene(const char* plyPath);

    // Returns true if a scene is currently loaded.
    bool hasScene() const;

    // Returns the loaded scene file path.
    const std::string& scenePath() const;

    // Returns the number of Gaussians in the loaded scene (0 for placeholder).
    uint32_t gaussianCount() const;

    // Render one eye's view to a region of a Vulkan swapchain image.
    // viewMatrix and projMatrix are column-major float[16].
    void renderEye(VkCommandBuffer cmd,
                   VkImage swapchainImage,
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
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool cmdPool_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool initialized_ = false;
    bool sceneLoaded_ = false;
    std::string loadedScenePath_;
    uint32_t numGaussians_ = 0;
};
