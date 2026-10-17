// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Reference view synthesis (ADR-053 S5) on Vulkan: image + depth ->
 *         N views at explicit viewpoints. The Vulkan twin of
 *         d3d11_service/d3d11_conv_viewsyn.{h,cpp}.
 *
 * The contract and the math live in util/u_conv_viewsyn.h; this is the GPU
 * half, a pass-for-pass port of the D3D11 stage (itself a port of the
 * browser's live-dibr.js). The passes are GLSL compute shaders
 * (vk_conv/shaders/conv_viewsyn_*.comp), one invocation per output pixel at
 * the pixel's centre, so every `SV_Position.xy` of the D3D11 pixel shaders is
 * `gl_GlobalInvocationID.xy + 0.5` here. In order:
 *
 *  1. prep (depth res): raw sample -> decoded -> nearness -> normalised u;
 *  2. optional foreground dilation (separable max, `dilate`);
 *  3. snap cores (separable min / max, radius from `snap_core`);
 *  4. guide (colour box-filtered to depth res);
 *  5. joint-bilateral upsample + edge snap to colour res (long side <= 1280);
 *  6. optional max filter after the upsample (`dilate_hi`);
 *  7. one dispatch per view: ray march + disocclusion fill.
 *
 * Passes 1-6 re-run only when an input or a parameter they read changed (the
 * D3D11 stage's prepass key); pass 7 runs every call.
 *
 * Intermediates match the D3D11 stage's precision: the depth-res rasters are
 * RGBA16F, the guide RGBA8, and the upsampled raster is RGBA16F with only `.r`
 * used (D3D11: R16F; RGBA16F is the storage format every Vulkan device must
 * support, R16F needs shaderStorageImageExtendedFormats). Pass 7 renders into
 * an RGBA16F scratch the size of one view and each view is blitted into the
 * target (format conversion by vkCmdBlitImage), so the target may be any
 * format with BLIT_DST support: RGBA8 / BGRA8 UNORM or SRGB, RGBA16F. An SRGB
 * target is encoded on the blit, as a D3D11 SRGB render-target view encodes.
 *
 * Inputs: a colour view that samples UNORM values (NOT an sRGB view, for
 * parity with the browser, whose filters see the encoded values) and a depth
 * view whose `.r` is the raw sample the depth descriptor decodes. Row 0 =
 * image top everywhere.
 *
 * Recording only: the stage records into the caller's command buffer and owns
 * no queue, no thread, no lock, and logs nothing per frame. One recording may
 * be in flight at a time (its descriptor pool and intermediates are reused):
 * the caller waits for the previous submission before recording again.
 *
 * @ingroup comp_vk_conv
 */

#pragma once

#include "util/u_conv_viewsyn.h"

#include "xrt/xrt_vulkan_includes.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct vk_bundle;
struct vk_conv_viewsyn;

//! Where the N views go.
struct vk_conv_viewsyn_target
{
	VkImage image;   //!< TRANSFER_DST usage
	VkFormat format; //!< its format (must support BLIT_DST with optimal tiling)
	uint32_t width;  //!< whole image (ROW: all views side by side)
	uint32_t height;
	uint32_t array_layers; //!< ARRAY: >= view_count
	VkImageLayout layout;  //!< its layout on entry; it is left in VK_IMAGE_LAYOUT_GENERAL
	uint32_t layout_kind;  //!< XRT_CONV_VIEWS_ROW (width / N per view) or _ARRAY (layer i)
};

//! One synthesis' inputs (2D views, SAMPLED usage).
struct vk_conv_viewsyn_inputs
{
	VkImageView color; //!< samples UNORM values, row 0 = image top
	uint32_t color_width, color_height;
	VkImageView depth; //!< `.r` = raw sample, row 0 = image top
	uint32_t depth_width, depth_height;
	//! Layout both input images are in (GENERAL or SHADER_READ_ONLY_OPTIMAL);
	//! the stage does not transition them, it only orders its reads after
	//! every earlier write.
	VkImageLayout input_layout;
	//! True when the colour or depth CONTENT changed since the last call (a
	//! new frame / new depth). View, size and parameter changes are detected
	//! internally.
	bool new_content;
};

/*!
 * Create the pipelines and the fixed state on @p vk's device.
 * @return NULL on failure (logged once).
 */
struct vk_conv_viewsyn *
vk_conv_viewsyn_create(struct vk_bundle *vk);

//! Waits for nothing: the caller has waited for its last submission.
void
vk_conv_viewsyn_destroy(struct vk_conv_viewsyn **vs_ptr);

/*!
 * Record @p params->view_count views into @p target on @p cmd (a primary
 * command buffer of @p vk's device in the recording state, for a queue with
 * compute + transfer). Every pass orders itself after all earlier work on the
 * queue (one global barrier at the start), so cross-submission hazards on the
 * cached intermediates and the inputs are covered.
 *
 * @return false on invalid geometry / inputs / target or a resource failure;
 *         nothing (or only harmless barriers) was recorded then.
 */
bool
vk_conv_viewsyn_record(struct vk_conv_viewsyn *vs,
                       VkCommandBuffer cmd,
                       const struct vk_conv_viewsyn_inputs *in,
                       const struct u_conv_viewsyn_params *params,
                       const struct vk_conv_viewsyn_target *target);

/*!
 * Forget the cached passes 1-6 (the next record re-runs them). Call when the
 * last recording was NOT submitted, or when an input view is destroyed and a
 * new one could reuse its handle value.
 */
void
vk_conv_viewsyn_invalidate(struct vk_conv_viewsyn *vs);

/*! Diagnostics: how many times passes 1-6 ran (live-dibr.js `stats.upsamples`). */
uint64_t
vk_conv_viewsyn_prepass_count(const struct vk_conv_viewsyn *vs);

#ifdef __cplusplus
}
#endif
