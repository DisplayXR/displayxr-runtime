// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Masked 2D-over-3D composite (Vulkan) — #439 Phase 3.
 *
 * Vulkan analog of the D3D11 masked composite (`masked_composite_ps` /
 * `local2d_flatten_ps` in `comp_d3d11_renderer.cpp`). Runs POST-weave: the
 * display processor has already woven the 3D atlas into the target; this pass
 * overlays the frame's 2D content where the zone mask says "2D" by lerping
 * `final = M*weave + (1-M)*twod` per pixel (M=1 keep weave, M=0 show 2D).
 *
 * Three pipelines:
 *   - masked composite (blend OFF / opaque, LOAD_OP_LOAD so the rect-discard
 *     path keeps the loaded weave): lerps weave/twod through the mask, or on the
 *     Phase-0 rect path discards inside the canvas and writes 2D outside.
 *   - flatten premultiplied-over (One / OneMinusSrcAlpha): draws one Local2D
 *     layer into the runtime-owned `twod` scratch.
 *   - flatten straight-alpha-over (SrcAlpha / OneMinusSrcAlpha): same, for
 *     XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT.
 *
 * Plus a render pass for rasterizing the R8 zone mask (whole-clear + per-rect
 * vkCmdClearAttachments — the VK analog of D3D11's ClearView-with-rects).
 *
 * The caller owns all images, image views, framebuffers, and layout
 * transitions (this mirrors `vk_hud_blend_draw_no_layout`). Descriptor sets are
 * allocated from an internal pool that the caller resets once per frame via
 * vk_local2d_composite_begin_frame() — valid because the vk_native compositor
 * drains the GPU (vkQueueWaitIdle) at the end of every frame.
 *
 * @author David Fattal
 * @ingroup aux_vk
 */

#pragma once

#include "vk/vk_helpers.h"

#ifdef __cplusplus
extern "C" {
#endif

//! Per-frame descriptor sets: 1 composite (3 samplers) + N flatten (1 each).
//! Bounded by XRT_MAX_LAYERS Local2D layers + a little headroom; the pool is
//! reset each frame so this is a per-frame, not lifetime, ceiling.
#define VK_LOCAL2D_MAX_SETS 64

/*!
 * Vulkan resources for the masked 2D-over-3D composite.
 * @ingroup aux_vk
 */
struct vk_local2d_composite
{
	VkSampler sampler_linear; //!< Flatten: sub-rect sampling.
	VkSampler sampler_point;  //!< Composite: 1:1 region sampling (byte-exact).

	VkShaderModule vert_mod;           //!< Fullscreen triangle.
	VkShaderModule composite_frag_mod; //!< masked_composite.frag.
	VkShaderModule flatten_frag_mod;   //!< local2d_flatten.frag.

	VkFormat target_fmt;  //!< Display/target image format (composite RT).
	VkFormat scratch_fmt; //!< twod scratch format (flatten RT, composite sample).
	VkFormat mask_fmt;    //!< R8 zone-mask format.

	VkRenderPass composite_rp; //!< target_fmt, LOAD_OP_LOAD.
	VkRenderPass flatten_rp;   //!< scratch_fmt, LOAD_OP_LOAD.
	VkRenderPass mask_rp;      //!< mask_fmt, LOAD_OP_CLEAR (whole-clear base).

	VkDescriptorSetLayout composite_dsl; //!< bindings 0=twod,1=mask,2=weave.
	VkPipelineLayout composite_layout;
	VkPipeline composite_pipe; //!< blend OFF, writes RGBA.

	VkDescriptorSetLayout flatten_dsl; //!< binding 0=src.
	VkPipelineLayout flatten_layout;
	VkPipeline flatten_premul_pipe;   //!< One / OneMinusSrcAlpha.
	VkPipeline flatten_unpremul_pipe; //!< SrcAlpha / OneMinusSrcAlpha.

	VkDescriptorPool desc_pool; //!< Reset each frame by begin_frame.

	bool initialized;
};

/*!
 * Create the composite pipelines + render passes.
 *
 * @param lc          Output struct (zero-initialized by caller).
 * @param vk          Vulkan bundle.
 * @param target_fmt  Display/target image format the composite writes.
 * @param scratch_fmt Format of the runtime-owned `twod` flatten scratch.
 * @return true on success.
 * @ingroup aux_vk
 */
bool
vk_local2d_composite_init(struct vk_local2d_composite *lc,
                          struct vk_bundle *vk,
                          VkFormat target_fmt,
                          VkFormat scratch_fmt);

/*!
 * Reset the per-frame descriptor pool. Call once before the frame's flatten /
 * composite draws (the vk_native compositor drains the GPU each frame, so any
 * sets from the previous frame are no longer referenced).
 * @ingroup aux_vk
 */
void
vk_local2d_composite_begin_frame(struct vk_local2d_composite *lc, struct vk_bundle *vk);

/*!
 * Rasterize an R8 zone mask: LOAD_OP_CLEAR fills the whole attachment with
 * @p base_value, then per-rect vkCmdClearAttachments writes @p rect_value
 * inside each (clamped) rect. The VK analog of D3D11 ClearView-with-rects.
 *
 * The mask image must be in COLOR_ATTACHMENT_OPTIMAL on entry; left in
 * COLOR_ATTACHMENT_OPTIMAL on exit. @p mask_fb's attachment must be a view of
 * the mask image with the mask format.
 *
 * @ingroup aux_vk
 */
void
vk_local2d_composite_raster_mask(struct vk_local2d_composite *lc,
                                 struct vk_bundle *vk,
                                 VkCommandBuffer cmd,
                                 VkFramebuffer mask_fb,
                                 uint32_t w,
                                 uint32_t h,
                                 float base_value,
                                 const struct xrt_rect *rects,
                                 uint32_t rect_count,
                                 float rect_value);

/*!
 * Flatten one Local2D layer into the `twod` scratch (caller clears the scratch
 * transparent once before the first layer, and manages its layout).
 *
 * Target (scratch) must be in COLOR_ATTACHMENT_OPTIMAL; @p scratch_fb's
 * attachment must use the scratch format. @p src_view must be in
 * SHADER_READ_ONLY_OPTIMAL.
 *
 * @param dst_x,dst_y,dst_w,dst_h  Clipped dest rect in scratch px (viewport).
 * @param src_x,src_y,src_w,src_h  Source rect (normalized); src_h < 0 = flip_y.
 * @param unpremultiplied          true → straight-alpha over; false → premul.
 * @ingroup aux_vk
 */
void
vk_local2d_composite_flatten_draw(struct vk_local2d_composite *lc,
                                  struct vk_bundle *vk,
                                  VkCommandBuffer cmd,
                                  VkFramebuffer scratch_fb,
                                  uint32_t fb_w,
                                  uint32_t fb_h,
                                  VkImageView src_view,
                                  int32_t dst_x,
                                  int32_t dst_y,
                                  uint32_t dst_w,
                                  uint32_t dst_h,
                                  float src_x,
                                  float src_y,
                                  float src_w,
                                  float src_h,
                                  bool unpremultiplied);

/*!
 * Masked composite into the target. Samples @p twod_view / @p mask_view /
 * @p weave_view at 1:1 over the region and lerps M*weave + (1-M)*twod, or — when
 * @p mask_view is VK_NULL_HANDLE — runs the Phase-0 rect path (discard inside
 * the canvas, write twod outside).
 *
 * Target must be in COLOR_ATTACHMENT_OPTIMAL; @p target_fb's attachment must use
 * the target format. All sampled views must be in SHADER_READ_ONLY_OPTIMAL.
 *
 * @param region_w,region_h  Window region (uv [0,1] spans this).
 * @param cx,cy,cw,ch        Canvas rect (only used by the rect path).
 * @ingroup aux_vk
 */
void
vk_local2d_composite_draw(struct vk_local2d_composite *lc,
                          struct vk_bundle *vk,
                          VkCommandBuffer cmd,
                          VkFramebuffer target_fb,
                          uint32_t fb_w,
                          uint32_t fb_h,
                          VkImageView twod_view,
                          VkImageView mask_view,
                          VkImageView weave_view,
                          uint32_t region_w,
                          uint32_t region_h,
                          int32_t cx,
                          int32_t cy,
                          uint32_t cw,
                          uint32_t ch);

/*!
 * Destroy composite resources.
 * @ingroup aux_vk
 */
void
vk_local2d_composite_fini(struct vk_local2d_composite *lc, struct vk_bundle *vk);

#ifdef __cplusplus
}
#endif
