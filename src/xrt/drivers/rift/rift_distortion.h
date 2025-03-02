// Copyright 2025, Beyley Cardellio
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Public functions for the Oculus Rift distortion correction.
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @ingroup drv_rift
 */

#include "rift_interface.h"

struct rift_distortion_render_info
{
	struct rift_lens_distortion *distortion;
	struct xrt_vec2 lens_center;
	struct xrt_vec2 tan_eye_angle_scale;
	struct xrt_vec2 pixels_per_tan_angle_at_center;
};

struct rift_distortion_render_info
rift_get_distortion_render_info(struct rift_hmd *hmd, uint32_t view);

struct xrt_vec3
rift_distortion_distance_scale_squared_split_chroma(struct rift_lens_distortion *lens_distortion,
                                                    float distance_squared);

struct rift_viewport_fov_tan
rift_calculate_fov_from_hmd(struct rift_hmd *hmd, struct rift_distortion_render_info *distortion, uint32_t view);

struct rift_scale_and_offset
rift_calculate_ndc_scale_and_offset_from_fov(struct rift_viewport_fov_tan *fov);

struct rift_scale_and_offset
rift_calculate_uv_scale_and_offset_from_ndc_scale_and_offset(struct rift_scale_and_offset eye_to_source_ndc);

void
rift_fill_in_default_distortions(struct rift_hmd *hmd);

bool
rift_hmd_compute_distortion(struct xrt_device *dev, uint32_t view, float u, float v, struct xrt_uv_triplet *out_result);
