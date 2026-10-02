// Copyright 2025, The DisplayXR Project
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
 * images doesn't pay setup cost beyond first sight of each image. A
 * destroyed source image must be removed with vk_hud_blend_forget_image().
 *
 * @author David Fattal
 * @ingroup aux_vk
 */

#pragma once

#include "vk/vk_helpers.h"

#ifdef __cplusplus
extern "C" {
#endif

// Each window-space layer is its own OpenXR swapchain, and swapchains rotate
// through ~3 images; the image cache is keyed by VkImage and only evicts what
// vk_hud_blend_forget_image() is told about. So
// the bound must cover (max concurrent window-space layers) × (images/swapchain),
// not just the 1-2 layers an undersized cache happened to allow. 64 ≈ 21 layers ×
// 3 images — generous headroom for realistic per-depth-plane 2D UI (see issue #389
// and the layering guidance in docs/specs/extensions/XR_DXR_win32_window_binding.md).
#define VK_HUD_BLEND_MAX_FBS    64 //!< Cached framebuffers (per swapchain image).
#define VK_HUD_BLEND_MAX_IMAGES 64 //!< Cached HUD source images (descriptor sets).

/*!
 * Vulkan resources for alpha-blended HUD overlay.
 * @ingroup aux_vk
 */
struct vk_hud_blend
{
	VkRenderPass render_pass;
	VkPipeline pipeline;        //!< Straight-alpha colour blend (src SRC_ALPHA).
	VkPipeline pipeline_premul; //!< Premultiplied colour blend (src ONE), #1786.
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
 * Like vk_hud_blend_init(), choosing whether the HUD's alpha is composited
 * into the target too (#1780).
 *
 * With @p write_alpha false (vk_hud_blend_init) the target's alpha channel is
 * left untouched. With it true the HUD is composited "over" in alpha as well
 * (src ONE, dst ONE_MINUS_SRC_ALPHA, all channels written), so the stamped
 * region gets at least the HUD's coverage. Use that when the target's alpha
 * is the window's transparency (a transparent session's atlas); it matches
 * the D3D11 window-space blend (#225).
 *
 * @ingroup aux_vk
 */
bool
vk_hud_blend_init_ex(struct vk_hud_blend *blend,
                     struct vk_bundle *vk,
                     VkFormat target_fmt,
                     bool write_alpha);

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
 * vk_hud_blend_draw_no_layout(), choosing the colour blend per draw (#1786).
 *
 * @p premultiplied false is the straight-alpha blend every other draw uses
 * (colour src SRC_ALPHA, dst ONE_MINUS_SRC_ALPHA). True treats the source as
 * premultiplied (colour src ONE, dst ONE_MINUS_SRC_ALPHA). The alpha blend is
 * whatever the blend was initialized with, in both cases.
 *
 * @ingroup aux_vk
 */
void
vk_hud_blend_draw_no_layout_ex(struct vk_hud_blend *blend,
                               struct vk_bundle *vk,
                               VkCommandBuffer cmd,
                               VkFramebuffer fb,
                               uint32_t fb_w,
                               uint32_t fb_h,
                               VkImage hud_image,
                               int32_t dst_x,
                               int32_t dst_y,
                               uint32_t dst_w,
                               uint32_t dst_h,
                               bool premultiplied);

/*!
 * Draw without managing target image layout transitions or framebuffer
 * caching. Use this when blending into a non-swapchain target (e.g. an
 * atlas image) where the caller manages layout transitions outside the
 * render pass and pre-creates the framebuffer.
 *
 * Target must be in COLOR_ATTACHMENT_OPTIMAL on entry; left in
 * COLOR_ATTACHMENT_OPTIMAL on exit. The framebuffer's attachment must
 * use the same format the blend was initialized with.
 *
 * @param blend         Initialized HUD blend resources.
 * @param vk            Vulkan bundle.
 * @param cmd           Open command buffer.
 * @param fb            Caller-managed framebuffer (color attachment is the target).
 * @param fb_w, fb_h    Framebuffer width/height in pixels.
 * @param hud_image     HUD source image (in SHADER_READ_ONLY_OPTIMAL).
 * @param dst_x, dst_y  Top-left of destination rect in target px.
 * @param dst_w, dst_h  Size of destination rect in target px.
 * @ingroup aux_vk
 */
void
vk_hud_blend_draw_no_layout(struct vk_hud_blend *blend,
                              struct vk_bundle *vk,
                              VkCommandBuffer cmd,
                              VkFramebuffer fb,
                              uint32_t fb_w,
                              uint32_t fb_h,
                              VkImage hud_image,
                              int32_t dst_x,
                              int32_t dst_y,
                              uint32_t dst_w,
                              uint32_t dst_h);

/*!
 * Drop the cached view + descriptor set for a HUD source image (#1782).
 *
 * The cache is keyed by the raw VkImage handle, and drivers hand a destroyed
 * image's handle value to the next image they create. An entry that outlives
 * its image is then returned for an unrelated image, and the draw samples a
 * view of freed memory (on NVIDIA: VK_ERROR_DEVICE_LOST, Xid 109). Whoever
 * destroys a HUD source image must forget it here before the next draw that
 * could see the reused handle.
 *
 * The caller must ensure no submitted command buffer still uses the entry.
 * No-op if @p hud_image is not cached.
 *
 * @ingroup aux_vk
 */
void
vk_hud_blend_forget_image(struct vk_hud_blend *blend, struct vk_bundle *vk, VkImage hud_image);

/*!
 * Destroy HUD blend resources.
 * @ingroup aux_vk
 */
void
vk_hud_blend_fini(struct vk_hud_blend *blend, struct vk_bundle *vk);

#ifdef __cplusplus
}
#endif
