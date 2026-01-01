// Copyright 2025, Beyley Cardellio
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  USB communications for the Oculus Rift.
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @ingroup drv_rift
 */

#include "rift_internal.h"


int
rift_get_lens_distortion(struct rift_hmd *hmd, struct rift_lens_distortion_report *lens_distortion);

void
rift_parse_distortion_report(struct rift_lens_distortion_report *report, struct rift_lens_distortion *out);

int
rift_send_keepalive(struct rift_hmd *hmd);

int
rift_get_config(struct rift_hmd *hmd, struct rift_config_report *config);

int
rift_set_config(struct rift_hmd *hmd, struct rift_config_report *config);

int
rift_get_display_info(struct rift_hmd *hmd, struct rift_display_info_report *display_info);

int
rift_enable_components(struct rift_hmd *hmd, struct rift_enable_components_report *enable_components);

int
rift_get_imu_calibration(struct rift_hmd *hmd, struct rift_imu_calibration *imu_calibration);

void
rift_unpack_int_sample(const uint8_t *in, struct xrt_vec3_i32 *out);

void
rift_unpack_float_sample(const uint8_t *in, float scale, struct xrt_vec3 *out);


void
rift_sample_to_imu_space(const int32_t *in, struct xrt_vec3 *out);
