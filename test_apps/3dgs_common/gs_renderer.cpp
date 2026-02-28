// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  3DGS renderer adapter implementation
 */

#include "gs_renderer.h"

#include "vulkan/VulkanContext.h"
#include "Renderer.h"
#include "GSScene.h"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <spdlog/spdlog.h>

#include <cstring>

bool GsRenderer::init(VkInstance instance,
                      VkPhysicalDevice physicalDevice,
                      VkDevice device,
                      VkQueue queue,
                      uint32_t queueFamilyIndex,
                      uint32_t renderWidth,
                      uint32_t renderHeight)
{
    width = renderWidth;
    height = renderHeight;

    // Create VulkanContext from external handles (OpenXR owns lifetime)
    context = VulkanContext::createFromExternal(
        instance, physicalDevice, device, queue, queueFamilyIndex, 0);

    if (!context) {
        spdlog::error("GsRenderer: failed to create VulkanContext from external handles");
        return false;
    }

    initialized = true;
    spdlog::info("GsRenderer: initialized ({}x{} render target)", width, height);
    return true;
}

bool GsRenderer::loadScene(const char* plyPath)
{
    if (!initialized) {
        spdlog::error("GsRenderer: not initialized");
        return false;
    }

    try {
        // Create a new Renderer in headless mode for each scene
        renderer = std::make_unique<Renderer>(VulkanSplatting::RendererConfiguration{});
        renderer->initializeHeadless(context, plyPath, width, height);

        loadedScenePath = plyPath;
        sceneLoaded = true;

        spdlog::info("GsRenderer: loaded scene '{}'", plyPath);
        return true;

    } catch (const std::exception& e) {
        spdlog::error("GsRenderer: failed to load '{}': {}", plyPath, e.what());
        renderer.reset();
        sceneLoaded = false;
        return false;
    }
}

bool GsRenderer::hasScene() const
{
    return sceneLoaded && renderer != nullptr;
}

const std::string& GsRenderer::scenePath() const
{
    return loadedScenePath;
}

uint32_t GsRenderer::gaussianCount() const
{
    return numGaussians;
}

void GsRenderer::renderEye(VkImage swapchainImage,
                            VkImageView swapchainImageView,
                            VkFormat format,
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

    // Convert float[16] column-major to glm::mat4
    glm::mat4 view = glm::make_mat4(viewMatrix);
    glm::mat4 proj = glm::make_mat4(projMatrix);

    renderer->renderToExternal(
        swapchainImage, swapchainImageView, format,
        imageWidth, imageHeight,
        viewportX, viewportY, viewportWidth, viewportHeight,
        view, proj);
}

void GsRenderer::cleanup()
{
    if (renderer) {
        renderer->stop();
        renderer.reset();
    }
    sceneLoaded = false;
    initialized = false;
}

GsRenderer::~GsRenderer()
{
    cleanup();
}
