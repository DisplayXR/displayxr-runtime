#pragma once

#include "xrt/xrt_results.h"
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

xrt_result_t leiasr_create(double maxTime, VkDevice device, VkPhysicalDevice physicalDevice, VkQueue graphicsQueue, VkCommandPool commandPool, void *windowHandle, struct leiasr **out);
void leiasr_destroy(struct leiasr* leiasr);
void leiasr_weave(struct leiasr* leiasr, VkCommandBuffer commandBuffer, VkImageView leftImageView, VkImageView rightImageView, VkRect2D viewport, int imageWidth, int imageHeight, VkFormat imageFormat, VkFramebuffer framebuffer, int framebufferWidth, int framebufferHeight, VkFormat framebufferFormat);

#ifdef __cplusplus
}
#endif
