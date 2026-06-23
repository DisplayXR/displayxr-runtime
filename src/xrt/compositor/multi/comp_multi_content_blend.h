// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Rounded-corner + edge-feather content composite pipeline (macOS).
 * @author David Fattal
 * @ingroup comp_multi
 *
 * macOS-only dedicated Vulkan pipeline that composites each spatial window's
 * client content into the shared-surface atlas with rounded corners and
 * feathered edges — the faithful analogue of the Windows D3D11 content-blit
 * pixel shader (d3d11_service_shaders.h, rounded-rect SDF coverage). It
 * replaces the hard vkCmdBlitImage content copy in render_shared_surface_locked
 * so window corners match the shell's rounded focus-glow ring instead of
 * spilling past it as sharp rectangles.
 *
 * Modeled on vk_hud_blend (aux_vk) but with: a rounded-rect/feather fragment
 * shader, a fragment push-constant block carrying the per-draw source sub-rect,
 * window-local UV domain and corner/feather parameters, and an array-layer-aware
 * image/descriptor cache (content projection swapchains can be multiview array
 * textures). It does NOT extend vk_hud_blend, which is shared by the
 * chrome/cursor/HUD decorations and uses hand-embedded SPIR-V.
 *
 * The whole module is compiled only on macOS (XRT_OS_MACOS); elsewhere it is an
 * empty translation unit so Windows/Android binaries stay byte-identical.
 */

#pragma once

#include "xrt/xrt_config_os.h"

#ifdef XRT_OS_MACOS

#include "vk/vk_helpers.h"

#ifdef __cplusplus
extern "C" {
#endif

//! Max distinct (image, array-layer) sources cached for descriptor sets.
#define COMP_MULTI_CONTENT_BLEND_MAX_IMAGES 64

/*!
 * Per-draw push constants. Layout (std430 push_constant) MUST match
 * content.frag exactly — offsets 0,8,16,24,32,36,40,44.
 */
struct comp_multi_content_pc
{
	float src_uv_off[2];   //!< Source sample sub-rect origin (normalized to full image).
	float src_uv_scale[2]; //!< Source sample sub-rect extent (normalized).
	float win_uv_off[2];   //!< Window-local UV [0,1] domain origin for the SDF.
	float win_uv_scale[2]; //!< Window-local UV [0,1] domain extent for the SDF.
	float corner_radius;   //!< |radius| as a fraction of window HEIGHT (0 = sharp).
	float corner_aspect;   //!< win_w_m / win_h_m (aspect-correct corners + feather).
	float edge_feather;    //!< Feather width as a fraction of window HEIGHT (0 = off).
	float _pad;            //!< Padding to 48 bytes.
};

/*!
 * Per-draw push constants for the ROTATED (quad) path. Layout (std430
 * push_constant) MUST match quad.vert / quad.frag exactly. Unlike the
 * axis-aligned struct above, the window-local UV domain isn't derived from a
 * dynamic viewport — the 4 pre-projected corner positions carry both the screen
 * placement (NDC + perspective W) and, via a fixed TL/BL/TR/BR vertex layout,
 * the canonical [0,1] window-local UV the rounded-rect SDF runs in.
 */
struct comp_multi_content_pc_quad
{
	float corners[4][4];   //!< Per corner (NDC.x, NDC.y, depth_ndc, W), order TL,BL,TR,BR.
	float src_uv_off[2];   //!< Source sample sub-rect origin (normalized; flip baked into scale.y).
	float src_uv_scale[2]; //!< Source sample sub-rect extent (normalized).
	float corner_radius;   //!< |radius| as a fraction of window HEIGHT (0 = sharp).
	float corner_aspect;   //!< win_w_m / win_h_m (aspect-correct corners + feather).
	float edge_feather;    //!< Feather width as a fraction of window HEIGHT (0 = off).
	float use_src_alpha;   //!< >0.5 = modulate by texture alpha (chrome pill); else opaque coverage.
};

struct comp_multi_content_blend
{
	VkRenderPass render_pass;
	VkPipeline pipeline;
	VkPipelineLayout pipe_layout;
	VkDescriptorSetLayout desc_layout;
	VkDescriptorPool desc_pool;
	VkSampler sampler;
	VkShaderModule vert_mod;
	VkShaderModule frag_mod;

	//! Rotated-window (quad) variant: a second pipeline sharing the render pass,
	//! sampler and descriptor cache, with its own VERTEX|FRAGMENT push-constant
	//! layout + TRIANGLE_STRIP quad shaders. Built lazily on first rotated draw.
	VkPipeline quad_pipeline;
	VkPipelineLayout quad_pipe_layout;
	VkShaderModule quad_vert_mod;
	VkShaderModule quad_frag_mod;
	bool quad_initialized;

	//! Cache of (VkImage, array-layer) → VkImageView + VkDescriptorSet.
	struct
	{
		VkImage image;
		uint32_t array_layer;
		VkImageView view;
		VkDescriptorSet desc_set;
	} cached_images[COMP_MULTI_CONTENT_BLEND_MAX_IMAGES];
	uint32_t image_count;

	bool initialized;
};

/*!
 * Create the pipeline. @p atlas_fmt is the format of the atlas color attachment
 * the content is composited into (the render pass uses LOAD/STORE,
 * COLOR_ATTACHMENT_OPTIMAL in/out, matching vk_hud_blend's atlas pass).
 */
bool
comp_multi_content_blend_init(struct comp_multi_content_blend *blend, struct vk_bundle *vk, VkFormat atlas_fmt);

/*!
 * Begin ONE render pass over @p fb (the atlas framebuffer) and bind the
 * pipeline. The framebuffer's attachment must be in COLOR_ATTACHMENT_OPTIMAL on
 * entry and is left in COLOR_ATTACHMENT_OPTIMAL on exit. All content draws issued
 * between begin and end share this render pass instance, so overlapping windows
 * blend in submission (painter, far→near) order.
 */
void
comp_multi_content_blend_begin(
    struct comp_multi_content_blend *blend, struct vk_bundle *vk, VkCommandBuffer cmd, VkFramebuffer fb, uint32_t fb_w, uint32_t fb_h);

/*!
 * Composite one (clipped) content rect. @p src_image / @p array_layer / @p
 * src_fmt identify the content source (sampled via a cached view of that layer);
 * @p pc carries the source sub-rect, window-local UV domain and corner/feather
 * params; @p dst_* is the atlas-px destination rect (already tile-clipped).
 * Must be called between begin and end.
 */
void
comp_multi_content_blend_draw(struct comp_multi_content_blend *blend,
                              struct vk_bundle *vk,
                              VkCommandBuffer cmd,
                              VkImage src_image,
                              uint32_t array_layer,
                              VkFormat src_fmt,
                              const struct comp_multi_content_pc *pc,
                              int32_t dst_x,
                              int32_t dst_y,
                              uint32_t dst_w,
                              uint32_t dst_h);

/*!
 * Composite one ROTATED (quad) content/chrome rect. @p pc carries the 4
 * pre-projected corner positions (NDC + per-corner W), the source sub-rect and
 * corner/feather params. @p scissor_* clips the rasterized quad to the eye tile
 * (a tilted quad can spill past its tile/atlas edges). The quad pipeline + its
 * push-constant layout are lazily built on first call. Must be called between
 * begin and end; binds the quad pipeline itself, so it freely interleaves with
 * comp_multi_content_blend_draw in the same render pass (painter order).
 */
void
comp_multi_content_blend_draw_quad(struct comp_multi_content_blend *blend,
                                   struct vk_bundle *vk,
                                   VkCommandBuffer cmd,
                                   VkImage src_image,
                                   uint32_t array_layer,
                                   VkFormat src_fmt,
                                   const struct comp_multi_content_pc_quad *pc,
                                   int32_t scissor_x,
                                   int32_t scissor_y,
                                   uint32_t scissor_w,
                                   uint32_t scissor_h,
                                   uint32_t atlas_w,
                                   uint32_t atlas_h);

//! End the render pass begun by comp_multi_content_blend_begin.
void
comp_multi_content_blend_end(struct comp_multi_content_blend *blend, struct vk_bundle *vk, VkCommandBuffer cmd);

//! Destroy all pipeline objects and cached views; zero the struct.
void
comp_multi_content_blend_fini(struct comp_multi_content_blend *blend, struct vk_bundle *vk);

#ifdef __cplusplus
}
#endif

#endif // XRT_OS_MACOS
