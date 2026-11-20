// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The weave-rect cursor of the Vulkan XR_DXR_weave engines (desktop
 *         Linux, macOS vk backend) - the GPU pass.
 * @author David Fattal
 * @ingroup comp_multi
 *
 * Draws what comp_multi_weave_cursor_plan() returned into the atlas the
 * display processor is about to consume: one premultiplied, alpha-blended,
 * scissored quad per view tile, softened against crosstalk by the same 13-tap
 * kernel as the in-process Vulkan compositor's cursor (vk_native's
 * zone_blit_soft.frag; ADR-046 Amendment 2 B.7). The in-process pass rides
 * that compositor's compose pipeline; a weave engine has none (it un-squeezes
 * with blits), so this is the same quad as a self-contained pass: its own
 * render pass, LOAD, over the engine's scratch.
 *
 * ZERO COST UNLESS REQUESTED (ADR-046 section 0): nothing is created until the
 * first draw; a client that never chains a request holds a zeroed struct.
 *
 * Contract: the caller holds the engine lock and has waited the previous
 * frame's GPU work (both engines do before they record), so the sprite image,
 * its staging buffer and the framebuffer may be recreated while recording.
 */

#pragma once

#include "xrt/xrt_vulkan_includes.h"

#include "comp_multi_weave_cursor.h"

#ifdef __cplusplus
extern "C" {
#endif

struct vk_bundle;

struct comp_multi_weave_cursor_vk
{
	bool initialized;
	bool failed; //!< creation failed once: never retried (one WARN)

	VkFormat target_format;
	VkRenderPass render_pass;
	VkShaderModule vert, frag;
	VkSampler sampler;
	VkDescriptorSetLayout dsl;
	VkDescriptorPool pool;
	VkDescriptorSet set;
	VkPipelineLayout layout;
	VkPipeline pipeline;

	//! The sprite: RGBA8 UNORM (the stored bytes are sRGB-encoded and are
	//! written as they are into the encoded atlas).
	VkImage sprite;
	VkDeviceMemory sprite_memory;
	VkImageView sprite_view;
	uint32_t sprite_w, sprite_h;
	bool sprite_ready;      //!< holds pixels (in SHADER_READ_ONLY_OPTIMAL)
	uint64_t sprite_serial; //!< the image_serial those pixels are

	VkBuffer staging;
	VkDeviceMemory staging_memory;
	VkDeviceSize staging_size;

	//! Framebuffer over the last target view (dropped by _invalidate_target).
	VkFramebuffer fb;
	VkImageView fb_view;
	uint32_t fb_w, fb_h;
};

//! A caller's texture carrier the engine imported: copied into the sprite
//! image by the draw that first needs it. In GENERAL, owned by this queue
//! family, for the duration of the record.
struct comp_multi_weave_cursor_vk_source
{
	VkImage image;
	uint32_t w, h;
};

/*!
 * Would a draw of @p d need its pixels brought in (a CPU upload, or a copy of
 * the engine's imported carrier)? The engine acquires the carrier from its
 * producer only then.
 */
bool
comp_multi_weave_cursor_vk_needs_pixels(const struct comp_multi_weave_cursor_vk *cv,
                                        const struct comp_multi_weave_cursor_draw *d);

/*!
 * Record the cursor into @p target (@p target_format, with
 * VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT), which is in @p layout on entry and on
 * exit (@p access / @p stage: its last use before and its next use after).
 *
 * @param source the engine's imported carrier for a draw whose rgba is NULL
 *               (may be NULL when the sprite image already holds that serial)
 * @return false if nothing was drawn (the caller reports it not drawn).
 */
bool
comp_multi_weave_cursor_vk_draw(struct comp_multi_weave_cursor_vk *cv,
                                struct vk_bundle *vk,
                                VkCommandBuffer cmd,
                                const struct comp_multi_weave_cursor_draw *d,
                                const struct comp_multi_weave_cursor_vk_source *source,
                                VkImage target,
                                VkImageView target_view,
                                VkFormat target_format,
                                uint32_t target_w,
                                uint32_t target_h,
                                VkImageLayout layout,
                                VkAccessFlags access,
                                VkPipelineStageFlags stage);

//! The engine is about to destroy (or has recreated) its atlas images: drop
//! the framebuffer over them. GPU idle on it.
void
comp_multi_weave_cursor_vk_invalidate_target(struct comp_multi_weave_cursor_vk *cv, struct vk_bundle *vk);

//! Release the sprite (the 1 s idle release); the pipeline stays. GPU idle on it.
void
comp_multi_weave_cursor_vk_release_sprite(struct comp_multi_weave_cursor_vk *cv, struct vk_bundle *vk);

//! Release everything. GPU idle.
void
comp_multi_weave_cursor_vk_fini(struct comp_multi_weave_cursor_vk *cv, struct vk_bundle *vk);

#ifdef __cplusplus
}
#endif
