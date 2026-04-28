// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Alpha-blended HUD overlay blit (Vulkan).
 *
 * Replaces vkCmdBlitImage with a render pass + pipeline that does
 * proper SrcAlpha/InvSrcAlpha blending so the HUD background can be
 * semi-transparent (and "transparent" pixels actually disappear instead
 * of painting opaque black).
 *
 * The pipeline is created once with a target format; per-call you pass
 * the source image (HUD swapchain image), its dimensions, and the
 * destination rect on the swapchain. Image views and descriptor sets
 * are cached by VkImage so an OpenXR swapchain with rotating per-frame
 * images doesn't pay setup cost beyond first sight of each image.
 *
 * @author David Fattal
 * @ingroup aux_vk
 */

#pragma once

#include "vk/vk_helpers.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VK_HUD_BLEND_MAX_FBS    8 //!< Cached framebuffers (per swapchain image).
#define VK_HUD_BLEND_MAX_IMAGES 8 //!< Cached HUD source images (descriptor sets).

/*!
 * Vulkan resources for alpha-blended HUD overlay.
 * @ingroup aux_vk
 */
struct vk_hud_blend
{
	VkRenderPass render_pass;
	VkPipeline pipeline;
	VkPipelineLayout pipe_layout;
	VkDescriptorSetLayout desc_layout;
	VkDescriptorPool desc_pool;
	VkSampler sampler;
	VkShaderModule vert_mod;
	VkShaderModule frag_mod;

	//! Cache of (VkImage → VkImageView, VkDescriptorSet) for HUD source images.
	//! Lookup is O(N) but N is bounded (≤ swapchain image count).
	struct {
		VkImage image;
		VkImageView view;
		VkDescriptorSet desc_set;
	} cached_images[VK_HUD_BLEND_MAX_IMAGES];
	uint32_t image_count;

	//! Cached framebuffers (one per swapchain target image, keyed by view).
	VkFramebuffer cached_fbs[VK_HUD_BLEND_MAX_FBS];
	VkImageView cached_views[VK_HUD_BLEND_MAX_FBS];
	uint32_t fb_count;
	uint32_t fb_w, fb_h; //!< Dimensions of cached framebuffers.

	bool initialized;
};

/*!
 * Create Vulkan resources for alpha-blended HUD overlay.
 *
 * @param blend       Output struct (zero-initialized by caller).
 * @param vk          Vulkan bundle.
 * @param target_fmt  Swapchain/target image format.
 * @return true on success.
 * @ingroup aux_vk
 */
bool
vk_hud_blend_init(struct vk_hud_blend *blend,
                   struct vk_bundle *vk,
                   VkFormat target_fmt);

/*!
 * Record alpha-blended HUD draw commands.
 *
 * Transitions: target PRESENT_SRC → COLOR_ATTACHMENT → PRESENT_SRC.
 * The HUD source image must be in SHADER_READ_ONLY_OPTIMAL when sampled.
 *
 * @param blend         Initialized HUD blend resources.
 * @param vk            Vulkan bundle.
 * @param cmd           Open command buffer.
 * @param target_view   Swapchain image view (for framebuffer).
 * @param target_image  Swapchain image (for layout transitions).
 * @param fb_w, fb_h    Framebuffer (swapchain) width/height in pixels.
 * @param hud_image     HUD source image (RGBA8). Cached on first sight.
 * @param hud_w, hud_h  HUD source image width/height in pixels.
 * @param dst_x, dst_y  Top-left of destination rect on the swapchain (px).
 * @param dst_w, dst_h  Size of destination rect on the swapchain (px).
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
                   VkImage hud_image,
                   uint32_t hud_w,
                   uint32_t hud_h,
                   int32_t dst_x,
                   int32_t dst_y,
                   uint32_t dst_w,
                   uint32_t dst_h);

/*!
 * Destroy HUD blend resources.
 * @ingroup aux_vk
 */
void
vk_hud_blend_fini(struct vk_hud_blend *blend, struct vk_bundle *vk);

#ifdef __cplusplus
}
#endif
