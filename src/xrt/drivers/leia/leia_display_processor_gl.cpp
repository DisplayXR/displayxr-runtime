// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Leia GL display processor: wraps SR SDK GL weaver
 *         as an @ref xrt_display_processor_gl.
 *
 * The display processor owns the leiasr_gl handle — it creates it
 * via the factory function and destroys it on cleanup.
 *
 * The SR SDK weaver expects side-by-side (SBS) stereo input. When the
 * compositor's atlas uses a different tiling layout (e.g. vertical stacking
 * with tile_columns=1, tile_rows=2), this DP rearranges the atlas into
 * SBS format via glCopyImageSubData before passing to the weaver.
 *
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "leia_display_processor_gl.h"
#include "leia_sr_gl.h"

#include "xrt/xrt_display_metrics.h"
#include "util/u_logging.h"

// GL types and functions — Leia GL DP is Windows-only
#include "xrt/xrt_windows.h"
#include <GL/gl.h>
#include <GL/glext.h>

#include <cstdlib>


/*!
 * Implementation struct wrapping leiasr_gl as xrt_display_processor_gl.
 */
struct leia_display_processor_gl_impl
{
	struct xrt_display_processor_gl base;
	struct leiasr_gl *leiasr; //!< Owned — destroyed in leia_dp_gl_destroy.

	//! @name SBS staging resources for non-SBS atlas layouts
	//! @{
	GLuint sbs_texture;    //!< Staging SBS texture (lazy-created).
	uint32_t sbs_width;    //!< Current staging texture width.
	uint32_t sbs_height;   //!< Current staging texture height.
	GLenum sbs_format;     //!< Current staging texture format.
	//! @}
};

static inline struct leia_display_processor_gl_impl *
leia_dp_gl(struct xrt_display_processor_gl *xdp)
{
	return (struct leia_display_processor_gl_impl *)xdp;
}


/*!
 * Ensure the SBS staging texture exists with the right dimensions/format.
 */
static bool
ensure_sbs_staging_gl(struct leia_display_processor_gl_impl *ldp,
                      uint32_t view_width,
                      uint32_t view_height,
                      GLenum format)
{
	uint32_t sbs_w = 2 * view_width;
	uint32_t sbs_h = view_height;

	if (ldp->sbs_texture != 0 && ldp->sbs_width == sbs_w &&
	    ldp->sbs_height == sbs_h && ldp->sbs_format == format) {
		return true;
	}

	if (ldp->sbs_texture != 0) {
		glDeleteTextures(1, &ldp->sbs_texture);
		ldp->sbs_texture = 0;
	}

	glGenTextures(1, &ldp->sbs_texture);
	glBindTexture(GL_TEXTURE_2D, ldp->sbs_texture);
	glTexStorage2D(GL_TEXTURE_2D, 1, format, sbs_w, sbs_h);
	glBindTexture(GL_TEXTURE_2D, 0);

	ldp->sbs_width = sbs_w;
	ldp->sbs_height = sbs_h;
	ldp->sbs_format = format;

	U_LOG_I("Leia GL DP: created SBS staging texture %ux%u", sbs_w, sbs_h);
	return true;
}


/*
 *
 * xrt_display_processor_gl interface methods.
 *
 */

static void
leia_dp_gl_process_atlas(struct xrt_display_processor_gl *xdp,
                           uint32_t atlas_texture,
                           uint32_t view_width,
                           uint32_t view_height,
                           uint32_t tile_columns,
                           uint32_t tile_rows,
                           uint32_t format,
                           uint32_t target_width,
                           uint32_t target_height)
{
	struct leia_display_processor_gl_impl *ldp = leia_dp_gl(xdp);

	uint32_t weaver_texture = atlas_texture;

	// If atlas is already SBS (tile_columns=2, tile_rows=1), pass directly.
	// Otherwise, rearrange to SBS via glCopyImageSubData.
	if (tile_columns != 2 || tile_rows != 1) {
		GLenum gl_format = static_cast<GLenum>(format);
		if (!ensure_sbs_staging_gl(ldp, view_width, view_height, gl_format)) {
			goto do_weave;
		}

		// Copy each view from tiled position to SBS position
		for (uint32_t i = 0; i < 2; i++) {
			uint32_t src_x = (i % tile_columns) * view_width;
			uint32_t src_y = (i / tile_columns) * view_height;
			uint32_t dst_x = i * view_width;

			glCopyImageSubData(atlas_texture, GL_TEXTURE_2D, 0,
			                   src_x, src_y, 0,
			                   ldp->sbs_texture, GL_TEXTURE_2D, 0,
			                   dst_x, 0, 0,
			                   view_width, view_height, 1);
		}

		weaver_texture = ldp->sbs_texture;
	}

do_weave:
	// Set input texture for weaving
	leiasr_gl_set_input_texture(ldp->leiasr, weaver_texture, view_width, view_height, format);

	// Perform weaving to the currently bound framebuffer
	leiasr_gl_weave(ldp->leiasr);
}

static bool
leia_dp_gl_get_predicted_eye_positions(struct xrt_display_processor_gl *xdp,
                                        struct xrt_eye_pair *out_eye_pair)
{
	struct leia_display_processor_gl_impl *ldp = leia_dp_gl(xdp);
	float left[3], right[3];
	if (!leiasr_gl_get_predicted_eye_positions(ldp->leiasr, left, right)) {
		return false;
	}
	out_eye_pair->left.x = left[0];
	out_eye_pair->left.y = left[1];
	out_eye_pair->left.z = left[2];
	out_eye_pair->right.x = right[0];
	out_eye_pair->right.y = right[1];
	out_eye_pair->right.z = right[2];
	out_eye_pair->valid = true;
	out_eye_pair->is_tracking = true;
	return true;
}

static bool
leia_dp_gl_get_window_metrics(struct xrt_display_processor_gl *xdp,
                               struct xrt_window_metrics *out_metrics)
{
	(void)xdp;
	(void)out_metrics;
	return false;
}

static bool
leia_dp_gl_request_display_mode(struct xrt_display_processor_gl *xdp, bool enable_3d)
{
	struct leia_display_processor_gl_impl *ldp = leia_dp_gl(xdp);
	return leiasr_gl_request_display_mode(ldp->leiasr, enable_3d);
}

static bool
leia_dp_gl_get_display_dimensions(struct xrt_display_processor_gl *xdp,
                                   float *out_width_m,
                                   float *out_height_m)
{
	struct leia_display_processor_gl_impl *ldp = leia_dp_gl(xdp);
	struct leiasr_display_dimensions dims = {};
	if (!leiasr_gl_get_display_dimensions(ldp->leiasr, &dims) || !dims.valid) {
		return false;
	}
	*out_width_m = dims.width_m;
	*out_height_m = dims.height_m;
	return true;
}

static bool
leia_dp_gl_get_display_pixel_info(struct xrt_display_processor_gl *xdp,
                                   uint32_t *out_pixel_width,
                                   uint32_t *out_pixel_height,
                                   int32_t *out_screen_left,
                                   int32_t *out_screen_top)
{
	struct leia_display_processor_gl_impl *ldp = leia_dp_gl(xdp);
	float w_m, h_m; // unused but required by API
	return leiasr_gl_get_display_pixel_info(ldp->leiasr, out_pixel_width, out_pixel_height, out_screen_left,
	                                        out_screen_top, &w_m, &h_m);
}

static void
leia_dp_gl_destroy(struct xrt_display_processor_gl *xdp)
{
	struct leia_display_processor_gl_impl *ldp = leia_dp_gl(xdp);

	if (ldp->sbs_texture != 0) {
		glDeleteTextures(1, &ldp->sbs_texture);
	}

	if (ldp->leiasr != NULL) {
		leiasr_gl_destroy(&ldp->leiasr);
	}
	free(ldp);
}


/*
 *
 * Helper to populate vtable entries on an impl struct.
 *
 */

static void
leia_dp_gl_init_vtable(struct leia_display_processor_gl_impl *ldp)
{
	ldp->base.process_atlas = leia_dp_gl_process_atlas;
	ldp->base.get_predicted_eye_positions = leia_dp_gl_get_predicted_eye_positions;
	ldp->base.get_window_metrics = leia_dp_gl_get_window_metrics;
	ldp->base.request_display_mode = leia_dp_gl_request_display_mode;
	ldp->base.get_display_dimensions = leia_dp_gl_get_display_dimensions;
	ldp->base.get_display_pixel_info = leia_dp_gl_get_display_pixel_info;
	ldp->base.destroy = leia_dp_gl_destroy;
}


/*
 *
 * Factory function — matches xrt_dp_factory_gl_fn_t signature.
 *
 */

extern "C" xrt_result_t
leia_dp_factory_gl(void *window_handle,
                    struct xrt_display_processor_gl **out_xdp)
{
	// Create weaver — view dimensions are set per-frame via setInputViewTexture,
	// so we pass 0,0 here.
	struct leiasr_gl *weaver = NULL;
	xrt_result_t ret = leiasr_gl_create(5.0, window_handle, 0, 0, &weaver);
	if (ret != XRT_SUCCESS || weaver == NULL) {
		U_LOG_W("Failed to create SR GL weaver");
		return ret != XRT_SUCCESS ? ret : XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	struct leia_display_processor_gl_impl *ldp =
	    (struct leia_display_processor_gl_impl *)calloc(1, sizeof(*ldp));
	if (ldp == NULL) {
		leiasr_gl_destroy(&weaver);
		return XRT_ERROR_ALLOCATION;
	}

	leia_dp_gl_init_vtable(ldp);
	ldp->leiasr = weaver;

	*out_xdp = &ldp->base;

	U_LOG_W("Created Leia SR GL display processor (factory, owns weaver)");

	return XRT_SUCCESS;
}
