// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Alpha-blended HUD overlay blit (Vulkan).
 *
 * Replaces vkCmdBlitImage with a render pass + pipeline that does
 * proper alpha blending, so the HUD background can be semi-transparent.
 *
 * @author David Fattal
 * @ingroup aux_vk
 */

#pragma once

#include "vk/vk_helpers.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Vulkan resources for alpha-blended HUD overlay.
 * @ingroup aux_vk
 */
#define VK_HUD_BLEND_MAX_FBS 8

struct vk_hud_blend
{
	VkRenderPass render_pass;
	VkPipeline pipeline;
	VkPipelineLayout pipe_layout;
	VkDescriptorSetLayout desc_layout;
	VkDescriptorPool desc_pool;
	VkDescriptorSet desc_set;
	VkSampler sampler;
	VkShaderModule vert_mod;
	VkShaderModule frag_mod;
	VkImageView hud_view; //!< View of the HUD image (created internally)

	//! Cached framebuffers (one per swapchain image, keyed by view)
	VkFramebuffer cached_fbs[VK_HUD_BLEND_MAX_FBS];
	VkImageView cached_views[VK_HUD_BLEND_MAX_FBS];
	uint32_t fb_count;
	uint32_t fb_w, fb_h; //!< Dimensions of cached framebuffers

	bool initialized;
};

/*!
 * Create Vulkan resources for alpha-blended HUD overlay.
 *
 * @param blend       Output struct (zero-initialized by caller).
 * @param vk          Vulkan bundle.
 * @param target_fmt  Swapchain/target image format.
 * @param hud_image   HUD image (RGBA8, already created).
 * @param hud_w       HUD image width.
 * @param hud_h       HUD image height.
 * @return true on success.
 * @ingroup aux_vk
 */
bool
vk_hud_blend_init(struct vk_hud_blend *blend,
                   struct vk_bundle *vk,
                   VkFormat target_fmt,
                   VkImage hud_image,
                   uint32_t hud_w,
                   uint32_t hud_h);

/*!
 * Record alpha-blended HUD draw commands.
 *
 * Transitions: target PRESENT_SRC -> COLOR_ATTACHMENT -> PRESENT_SRC.
 * The HUD image must be in TRANSFER_SRC_OPTIMAL layout.
 *
 * @param blend       Initialized HUD blend resources.
 * @param vk          Vulkan bundle.
 * @param cmd         Open command buffer.
 * @param target_view Swapchain image view (for framebuffer).
 * @param target_image Swapchain image (for layout transitions).
 * @param fb_w        Framebuffer (swapchain) width.
 * @param fb_h        Framebuffer (swapchain) height.
 * @param hud_w       HUD image width.
 * @param hud_h       HUD image height.
 * @ingroup aux_vk
 */
void
vk_hud_blend_draw(struct vk_hud_blend *blend,
                   struct vk_bundle *vk,
                   VkCommandBuffer cmd,
                   VkImageView target_view,
                   VkImage target_image,
                   uint32_t fb_w,
                   uint32_t fb_h,
                   uint32_t hud_w,
                   uint32_t hud_h);

/*!
 * Destroy HUD blend resources.
 * @ingroup aux_vk
 */
void
vk_hud_blend_fini(struct vk_hud_blend *blend, struct vk_bundle *vk);

#ifdef __cplusplus
}
#endif
