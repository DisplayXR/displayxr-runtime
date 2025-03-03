// Copyright 2020-2025, Collabora, Ltd.
// Copyright 2025, Beyley Cardellio
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Driver for the Oculus Rift.
 *
 * Based largely on simulated_hmd.c, with reference to the DK1/DK2 firmware and OpenHMD's rift driver.
 *
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @ingroup drv_rift
 */

#include "os/os_time.h"
#include "xrt/xrt_defines.h"
#include "xrt/xrt_device.h"

#include "rift_interface.h"
#include "rift_distortion.h"

#include "math/m_relation_history.h"
#include "math/m_clock_tracking.h"
#include "math/m_api.h"
#include "math/m_vec2.h"
#include "math/m_mathinclude.h" // IWYU pragma: keep

#include "util/u_debug.h"
#include "util/u_device.h"
#include "util/u_distortion_mesh.h"
#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_time.h"
#include "util/u_var.h"
#include "util/u_visibility_mask.h"
#include "util/u_trace_marker.h"
#include "xrt/xrt_results.h"

#include <stdio.h>
#include <assert.h>

/*
 *
 * Structs and defines.
 *
 */

DEBUG_GET_ONCE_LOG_OPTION(rift_log, "RIFT_LOG", U_LOGGING_WARN)

#define HMD_TRACE(hmd, ...) U_LOG_XDEV_IFL_T(&hmd->base, hmd->log_level, __VA_ARGS__)
#define HMD_DEBUG(hmd, ...) U_LOG_XDEV_IFL_D(&hmd->base, hmd->log_level, __VA_ARGS__)
#define HMD_INFO(hmd, ...) U_LOG_XDEV_IFL_I(&hmd->base, hmd->log_level, __VA_ARGS__)
#define HMD_WARN(hmd, ...) U_LOG_XDEV_IFL_W(&hmd->base, hmd->log_level, __VA_ARGS__)
#define HMD_ERROR(hmd, ...) U_LOG_XDEV_IFL_E(&hmd->base, hmd->log_level, __VA_ARGS__)

/*
 *
 * Headset functions
 *
 */

static int
rift_send_report(struct rift_hmd *hmd, uint8_t report_id, void *data, size_t data_length)
{
	int result;

	if (data_length > REPORT_MAX_SIZE - 1) {
		return -1;
	}

	uint8_t buffer[REPORT_MAX_SIZE];
	buffer[0] = report_id;
	memcpy(buffer + 1, data, data_length);

	result = os_hid_set_feature(hmd->hid_dev, buffer, data_length + 1);
	if (result < 0) {
		return result;
	}

	return 0;
}

static int
rift_get_report(struct rift_hmd *hmd, uint8_t report_id, uint8_t *out, size_t out_len)
{
	return os_hid_get_feature(hmd->hid_dev, report_id, out, out_len);
}

static int
rift_send_keepalive(struct rift_hmd *hmd)
{
	struct dk2_report_keepalive_mux report = {0, IN_REPORT_DK2,
	                                          KEEPALIVE_INTERVAL_NS / 1000000}; // convert ns to ms

	int result = rift_send_report(hmd, FEATURE_REPORT_KEEPALIVE_MUX, &report, sizeof(report));

	if (result < 0) {
		return result;
	}

	hmd->last_keepalive_time = os_monotonic_get_ns();
	HMD_TRACE(hmd, "Sent keepalive at time %ld", hmd->last_keepalive_time);

	return 0;
}

static int
rift_get_config(struct rift_hmd *hmd, struct rift_config_report *config)
{
	uint8_t buf[REPORT_MAX_SIZE] = {0};

	int result = rift_get_report(hmd, FEATURE_REPORT_CONFIG, buf, sizeof(buf));
	if (result < 0) {
		return result;
	}

	// FIXME: handle endian differences
	memcpy(config, buf + 1, sizeof(*config));

	// this value is hardcoded in the DK1 and DK2 firmware
	if ((hmd->variant == RIFT_VARIANT_DK1 || hmd->variant == RIFT_VARIANT_DK2) &&
	    config->sample_rate != IMU_SAMPLE_RATE) {
		HMD_ERROR(hmd, "Got invalid config from headset, got sample rate %d when expected %d",
		          config->sample_rate, IMU_SAMPLE_RATE);
		return -1;
	}

	return 0;
}

static int
rift_get_display_info(struct rift_hmd *hmd, struct rift_display_info_report *display_info)
{
	uint8_t buf[REPORT_MAX_SIZE] = {0};

	int result = rift_get_report(hmd, FEATURE_REPORT_DISPLAY_INFO, buf, sizeof(buf));
	if (result < 0) {
		return result;
	}

	// FIXME: handle endian differences
	memcpy(display_info, buf + 1, sizeof(*display_info));

	return 0;
}

static int
rift_get_lens_distortion(struct rift_hmd *hmd, struct rift_lens_distortion_report *lens_distortion)
{
	uint8_t buf[REPORT_MAX_SIZE] = {0};

	int result = rift_get_report(hmd, FEATURE_REPORT_LENS_DISTORTION, buf, sizeof(buf));
	if (result < 0) {
		return result;
	}

	memcpy(lens_distortion, buf + 1, sizeof(*lens_distortion));

	return 0;
}

static int
rift_set_config(struct rift_hmd *hmd, struct rift_config_report *config)
{
	return rift_send_report(hmd, FEATURE_REPORT_CONFIG, config, sizeof(*config));
}

/*
 *
 * Driver functions
 *
 */

static void
rift_hmd_destroy(struct xrt_device *xdev)
{
	struct rift_hmd *hmd = rift_hmd(xdev);

	// Remove the variable tracking.
	u_var_remove_root(hmd);

	if (hmd->sensor_thread.initialized)
		os_thread_helper_stop_and_wait(&hmd->sensor_thread);

	if (hmd->clock_tracker)
		m_clock_windowed_skew_tracker_destroy(hmd->clock_tracker);

	m_relation_history_destroy(&hmd->relation_hist);

	if (hmd->lens_distortions)
		free(hmd->lens_distortions);

	u_device_free(&hmd->base);
}

static xrt_result_t
rift_hmd_get_tracked_pose(struct xrt_device *xdev,
                          enum xrt_input_name name,
                          int64_t at_timestamp_ns,
                          struct xrt_space_relation *out_relation)
{
	struct rift_hmd *hmd = rift_hmd(xdev);

	if (name != XRT_INPUT_GENERIC_HEAD_POSE) {
		U_LOG_XDEV_UNSUPPORTED_INPUT(&hmd->base, hmd->log_level, name);
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}

	struct xrt_space_relation relation = XRT_SPACE_RELATION_ZERO;

	enum m_relation_history_result history_result =
	    m_relation_history_get(hmd->relation_hist, at_timestamp_ns, &relation);
	if (history_result == M_RELATION_HISTORY_RESULT_INVALID) {
		// If you get in here, it means you did not push any poses into the relation history.
		// You may want to handle this differently.
		HMD_ERROR(hmd, "Internal error: no poses pushed?");
	}

	if ((relation.relation_flags & XRT_SPACE_RELATION_ORIENTATION_VALID_BIT) != 0) {
		// If we provide an orientation, make sure that it is normalized.
		math_quat_normalize(&relation.pose.orientation);
	}

	*out_relation = relation;
	return XRT_SUCCESS;
}

static xrt_result_t
rift_hmd_get_view_poses(struct xrt_device *xdev,
                        const struct xrt_vec3 *default_eye_relation,
                        int64_t at_timestamp_ns,
                        uint32_t view_count,
                        struct xrt_space_relation *out_head_relation,
                        struct xrt_fov *out_fovs,
                        struct xrt_pose *out_poses)
{
	struct rift_hmd *hmd = rift_hmd(xdev);

	return u_device_get_view_poses(xdev,                                                        //
	                               &(struct xrt_vec3){hmd->extra_display_info.icd, 0.0f, 0.0f}, //
	                               at_timestamp_ns,                                             //
	                               view_count,                                                  //
	                               out_head_relation,                                           //
	                               out_fovs,                                                    //
	                               out_poses);
}

static xrt_result_t
rift_hmd_get_visibility_mask(struct xrt_device *xdev,
                             enum xrt_visibility_mask_type type,
                             uint32_t view_index,
                             struct xrt_visibility_mask **out_mask)
{
	struct xrt_fov fov = xdev->hmd->distortion.fov[view_index];
	u_visibility_mask_get_default(type, &fov, out_mask);
	return XRT_SUCCESS;
}

static float
rift_decode_fixed_point_uint16(uint16_t value, uint16_t zero_value, int fractional_bits)
{
	float value_float = (float)value;
	value_float -= (float)zero_value;
	value_float *= 1.0f / (float)(1 << fractional_bits);
	return value_float;
}

static void
rift_parse_distortion_report(struct rift_lens_distortion_report *report, struct rift_lens_distortion *out)
{
	out->distortion_version = report->distortion_version;

	switch (report->distortion_version) {
	case RIFT_LENS_DISTORTION_LCSV_CATMULL_ROM_10_VERSION_1: {
		struct rift_catmull_rom_distortion_report_data report_data = report->data.lcsv_catmull_rom_10;
		struct rift_catmull_rom_distortion_data data;

		out->eye_relief = MICROMETERS_TO_METERS(report_data.eye_relief);

		for (uint16_t i = 0; i < CATMULL_COEFFICIENTS; i += 1) {
			data.k[i] = rift_decode_fixed_point_uint16(report_data.k[i], 0, 14);
		}
		data.max_r = rift_decode_fixed_point_uint16(report_data.max_r, 0, 14);
		data.meters_per_tan_angle_at_center =
		    rift_decode_fixed_point_uint16(report_data.meters_per_tan_angle_at_center, 0, 19);
		for (uint16_t i = 0; i < CHROMATIC_ABBERATION_COEFFEICENT_COUNT; i += 1) {
			data.chromatic_abberation[i] =
			    rift_decode_fixed_point_uint16(report_data.chromatic_abberation[i], 0x8000, 19);
		}

		out->data.lcsv_catmull_rom_10 = data;
		break;
	}
	default: return;
	}
}

/*
 * Decode 3 tightly packed 21 bit values from 4 bytes.
 * We unpack them in the higher 21 bit values first and then shift
 * them down to the lower in order to get the sign bits correct.
 *
 * Code taken/reformatted from OpenHMD's rift driver
 */
static void
rift_decode_sample(const uint8_t *in, int32_t *out)
{
	int x = (in[0] << 24) | (in[1] << 16) | ((in[2] & 0xF8) << 8);
	int y = ((in[2] & 0x07) << 29) | (in[3] << 21) | (in[4] << 13) | ((in[5] & 0xC0) << 5);
	int z = ((in[5] & 0x3F) << 26) | (in[6] << 18) | (in[7] << 10);

	out[0] = x >> 11;
	out[1] = y >> 11;
	out[2] = z >> 11;
}

static void
rift_sample_to_imu_space(const int32_t *in, struct xrt_vec3 *out)
{
	out->x = (float)in[0] * 0.0001f;
	out->y = (float)in[1] * 0.0001f;
	out->z = (float)in[2] * 0.0001f;
}

static int
sensor_thread_tick(struct rift_hmd *hmd)
{
	uint8_t buf[REPORT_MAX_SIZE];
	int result;

	int64_t now = os_monotonic_get_ns();

	if (now - hmd->last_keepalive_time > KEEPALIVE_SEND_RATE_NS) {
		result = rift_send_keepalive(hmd);

		if (result < 0) {
			HMD_ERROR(hmd, "Got error sending keepalive, assuming fatal, reason %d", result);
			return result;
		}
	}

	result = os_hid_read(hmd->hid_dev, buf, sizeof(buf), IMU_SAMPLE_RATE);

	if (result < 0) {
		HMD_ERROR(hmd, "Got error reading from device, assuming fatal, reason %d", result);
		return result;
	}

	if (result == 0) {
		HMD_WARN(hmd, "Timed out waiting for packet from headset, packet should come in every %d milliseconds",
		         IMU_SAMPLE_RATE);
		return 0;
	}

	switch (hmd->variant) {
	case RIFT_VARIANT_DK2: {
		// skip unknown commands
		if (buf[0] != IN_REPORT_DK2) {
			HMD_WARN(hmd, "Skipping unknown IN command %d", buf[0]);
			return 0;
		}

		struct dk2_in_report report;
		assert(result >= (int)sizeof(report));

		memcpy(&report, buf + 1, sizeof(report));

		// if there's no samples, just do nothing.
		if (report.num_samples == 0) {
			return 0;
		}

		int64_t remote_sample_timestamp_ns = (int64_t)report.sample_timestamp * 1000;

		// ignore samples that are behind the latest current sample
		if (remote_sample_timestamp_ns < hmd->last_sample_time_ns) {
			return 0;
		}

		hmd->last_sample_time_ns = remote_sample_timestamp_ns;

		m_clock_windowed_skew_tracker_push(hmd->clock_tracker, os_monotonic_get_ns(),
		                                   remote_sample_timestamp_ns);

		int64_t local_timestamp_ns;
		// if we haven't synchronized our clocks, just do nothing
		if (!m_clock_windowed_skew_tracker_to_local(hmd->clock_tracker, remote_sample_timestamp_ns,
		                                            &local_timestamp_ns)) {
			return 0;
		}

		struct dk2_sample_pack sample_pack = report.samples[report.num_samples >= 2 ? 1 : 0];

		int32_t accel_raw[3], gyro_raw[3];
		rift_decode_sample(sample_pack.accel.data, accel_raw);
		rift_decode_sample(sample_pack.gyro.data, gyro_raw);

		struct xrt_vec3 accel, gyro;
		rift_sample_to_imu_space(accel_raw, &accel);
		rift_sample_to_imu_space(gyro_raw, &gyro);

		m_imu_3dof_update(&hmd->fusion, local_timestamp_ns, &accel, &gyro);

		struct xrt_space_relation relation = XRT_SPACE_RELATION_ZERO;
		relation.relation_flags = (enum xrt_space_relation_flags)(XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |
		                                                          XRT_SPACE_RELATION_ORIENTATION_VALID_BIT);
		relation.pose.orientation = hmd->fusion.rot;
		m_relation_history_push(hmd->relation_hist, &relation, local_timestamp_ns);

		break;
	}
	case RIFT_VARIANT_DK1: return 0;
	}

	return 0;
}

static void *
sensor_thread(void *ptr)
{
	U_TRACE_SET_THREAD_NAME("Rift sensor thread");

	struct rift_hmd *hmd = (struct rift_hmd *)ptr;

	os_thread_helper_lock(&hmd->sensor_thread);

	int result = 0;
	int ticks = 0;

	while (os_thread_helper_is_running_locked(&hmd->sensor_thread) && result >= 0) {
		os_thread_helper_unlock(&hmd->sensor_thread);

		result = sensor_thread_tick(hmd);

		os_thread_helper_lock(&hmd->sensor_thread);
		ticks += 1;
	}

	os_thread_helper_unlock(&hmd->sensor_thread);

	return NULL;
}

struct rift_hmd *
rift_hmd_create(struct os_hid_device *dev, enum rift_variant variant, char *device_name, char *serial_number)
{
	int result;

	// This indicates you won't be using Monado's built-in tracking algorithms.
	enum u_device_alloc_flags flags =
	    (enum u_device_alloc_flags)(U_DEVICE_ALLOC_HMD | U_DEVICE_ALLOC_TRACKING_NONE);

	struct rift_hmd *hmd = U_DEVICE_ALLOCATE(struct rift_hmd, flags, 1, 0);

	hmd->variant = variant;
	hmd->hid_dev = dev;

	result = rift_send_keepalive(hmd);
	if (result < 0) {
		HMD_ERROR(hmd, "Failed to send keepalive to spin up headset, reason %d", result);
		goto error;
	}

	result = rift_get_display_info(hmd, &hmd->display_info);
	if (result < 0) {
		HMD_ERROR(hmd, "Failed to get device config, reason %d", result);
		goto error;
	}
	HMD_DEBUG(hmd, "Got display info from hmd, res: %dx%d", hmd->display_info.resolution_x,
	          hmd->display_info.resolution_y);

	result = rift_get_config(hmd, &hmd->config);
	if (result < 0) {
		HMD_ERROR(hmd, "Failed to get device config, reason %d", result);
		goto error;
	}
	HMD_DEBUG(hmd, "Got config from hmd, config flags: %X", hmd->config.config_flags);

	if (getenv("RIFT_POWER_OVERRIDE") != NULL) {
		hmd->config.config_flags |= RIFT_CONFIG_REPORT_OVERRIDE_POWER;
		HMD_INFO(hmd, "Enabling the override power config flag.");
	} else {
		hmd->config.config_flags &= ~RIFT_CONFIG_REPORT_OVERRIDE_POWER;
		HMD_DEBUG(hmd, "Disabling the override power config flag.");
	}

	// force enable calibration use and auto calibration
	// this is on by default according to the firmware on DK1 and DK2,
	// but OpenHMD forces them on, we should do the same, they probably had a reason
	hmd->config.config_flags |= RIFT_CONFIG_REPORT_USE_CALIBRATION;
	hmd->config.config_flags |= RIFT_CONFIG_REPORT_AUTO_CALIBRATION;

	hmd->config.interval = 0;

	// update the config
	result = rift_set_config(hmd, &hmd->config);
	if (result < 0) {
		HMD_ERROR(hmd, "Failed to set the device config, reason %d", result);
		goto error;
	}

	// read it back
	result = rift_get_config(hmd, &hmd->config);
	if (result < 0) {
		HMD_ERROR(hmd, "Failed to set the device config, reason %d", result);
		goto error;
	}
	HMD_DEBUG(hmd, "After writing, HMD has config flags: %X", hmd->config.config_flags);

	if (getenv("RIFT_USE_FIRMWARE_DISTORTION") != NULL) {
		// get the lens distortions
		struct rift_lens_distortion_report lens_distortion;
		result = rift_get_lens_distortion(hmd, &lens_distortion);
		if (result < 0) {
			HMD_ERROR(hmd, "Failed to get lens distortion, reason %d", result);
			goto error;
		}

		hmd->num_lens_distortions = lens_distortion.num_distortions;
		hmd->lens_distortions = calloc(lens_distortion.num_distortions, sizeof(struct rift_lens_distortion));

		rift_parse_distortion_report(&lens_distortion, &hmd->lens_distortions[lens_distortion.distortion_idx]);
		// TODO: actually verify we initialize all the distortions. if the headset is working correctly, this
		// should have happened, but you never know.
		for (uint16_t i = 1; i < hmd->num_lens_distortions; i++) {
			result = rift_get_lens_distortion(hmd, &lens_distortion);
			if (result < 0) {
				HMD_ERROR(hmd, "Failed to get lens distortion idx %d, reason %d", i, result);
				goto error;
			}

			rift_parse_distortion_report(&lens_distortion,
			                             &hmd->lens_distortions[lens_distortion.distortion_idx]);
		}

		// TODO: pick the correct distortion for the eye relief setting the user has picked
		hmd->distortion_in_use = 0;
	} else {
		rift_fill_in_default_distortions(hmd);
	}

	// fill in extra display info about the headset

	switch (hmd->variant) {
	case RIFT_VARIANT_DK2:
		hmd->extra_display_info.screen_gap_meters = 0.0f;
		hmd->extra_display_info.lens_diameter_meters = 0.04f;
		break;
	default: break;
	}

	// hardcode left eye, probably not ideal, but sure, why not
	struct rift_distortion_render_info distortion_render_info = rift_get_distortion_render_info(hmd, 0);
	hmd->extra_display_info.fov = rift_calculate_fov_from_hmd(hmd, &distortion_render_info, 0);
	hmd->extra_display_info.eye_to_source_ndc =
	    rift_calculate_ndc_scale_and_offset_from_fov(&hmd->extra_display_info.fov);
	hmd->extra_display_info.eye_to_source_uv =
	    rift_calculate_uv_scale_and_offset_from_ndc_scale_and_offset(hmd->extra_display_info.eye_to_source_ndc);

	size_t idx = 0;
	hmd->base.hmd->blend_modes[idx++] = XRT_BLEND_MODE_OPAQUE;
	hmd->base.hmd->blend_mode_count = idx;

	hmd->base.update_inputs = u_device_noop_update_inputs;
	hmd->base.get_tracked_pose = rift_hmd_get_tracked_pose;
	hmd->base.get_view_poses = rift_hmd_get_view_poses;
	hmd->base.get_visibility_mask = rift_hmd_get_visibility_mask;
	hmd->base.destroy = rift_hmd_destroy;

	hmd->base.hmd->distortion.models = XRT_DISTORTION_MODEL_COMPUTE;
	hmd->base.hmd->distortion.preferred = XRT_DISTORTION_MODEL_COMPUTE;
	hmd->base.compute_distortion = rift_hmd_compute_distortion;
	u_distortion_mesh_fill_in_compute(&hmd->base);

	hmd->pose = (struct xrt_pose)XRT_POSE_IDENTITY;
	hmd->log_level = debug_get_log_option_rift_log();

	// Print name.
	strncpy(hmd->base.str, device_name, XRT_DEVICE_NAME_LEN);
	strncpy(hmd->base.serial, serial_number, XRT_DEVICE_NAME_LEN);

	m_relation_history_create(&hmd->relation_hist, NULL);

	// Setup input.
	hmd->base.name = XRT_DEVICE_GENERIC_HMD;
	hmd->base.device_type = XRT_DEVICE_TYPE_HMD;
	hmd->base.inputs[0].name = XRT_INPUT_GENERIC_HEAD_POSE;
	hmd->base.supported.orientation_tracking = true;
	hmd->base.supported.position_tracking = false; // set to true once we are trying to get the sensor 6dof to work

	// Set up display details
	hmd->base.hmd->screens[0].nominal_frame_interval_ns = time_s_to_ns(1.0f / 75.0f);

	hmd->extra_display_info.icd = MICROMETERS_TO_METERS(hmd->display_info.lens_separation);

	char *icd_str = getenv("RIFT_OVERRIDE_ICD");
	if (icd_str != NULL) {
		// mm -> meters
		float icd = strtof(icd_str, NULL) / 1000.0f;

		// 0 is error, and less than zero is invalid
		if (icd > 0.0f) {
			hmd->extra_display_info.icd = icd;
			HMD_INFO(hmd, "Forcing ICD to %f", hmd->extra_display_info.icd);
		} else {
			HMD_ERROR(hmd, "Failed to parse ICD override, expected float in millimeters, got %s", icd_str);
		}
	} else {
		HMD_DEBUG(hmd, "Using default ICD of %f", hmd->extra_display_info.icd);
	}

	// screen is rotated, so we need to undo that here
	hmd->base.hmd->screens[0].h_pixels = hmd->display_info.resolution_x;
	hmd->base.hmd->screens[0].w_pixels = hmd->display_info.resolution_y;

	// TODO: properly apply using rift_extra_display_info.screen_gap_meters, but this isn't necessary on DK2, where
	// the gap is always 0
	uint16_t view_width = hmd->display_info.resolution_x / 2;
	uint16_t view_height = hmd->display_info.resolution_y;

	for (uint32_t i = 0; i < 2; ++i) {
		hmd->base.hmd->views[i].display.w_pixels = view_width;
		hmd->base.hmd->views[i].display.h_pixels = view_height;

		hmd->base.hmd->views[i].viewport.x_pixels = 0;
		hmd->base.hmd->views[i].viewport.y_pixels = (1 - i) * (hmd->display_info.resolution_x / 2);
		hmd->base.hmd->views[i].viewport.w_pixels = view_height; // screen is rotated, so swap w and h
		hmd->base.hmd->views[i].viewport.h_pixels = view_width;
		hmd->base.hmd->views[i].rot = u_device_rotation_left;
	}

	switch (hmd->variant) {
	default:
	case RIFT_VARIANT_DK2:
		// TODO: figure out how to calculate this programmatically, right now this is hardcoded with data dumped
		//       from oculus' OpenXR runtime, some of the math for this is in rift_distortion.c, used for
		//       calculating distortion
		hmd->base.hmd->distortion.fov[0].angle_up = 0.92667186;
		hmd->base.hmd->distortion.fov[0].angle_down = -0.92667186;
		hmd->base.hmd->distortion.fov[0].angle_left = -0.8138836;
		hmd->base.hmd->distortion.fov[0].angle_right = 0.82951474;

		hmd->base.hmd->distortion.fov[1].angle_up = 0.92667186;
		hmd->base.hmd->distortion.fov[1].angle_down = -0.92667186;
		hmd->base.hmd->distortion.fov[1].angle_left = -0.82951474;
		hmd->base.hmd->distortion.fov[1].angle_right = 0.8138836;
		break;
	}

	// Just put an initial identity value in the tracker
	struct xrt_space_relation identity = XRT_SPACE_RELATION_ZERO;
	identity.relation_flags = (enum xrt_space_relation_flags)(XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |
	                                                          XRT_SPACE_RELATION_ORIENTATION_VALID_BIT);
	uint64_t now = os_monotonic_get_ns();
	m_relation_history_push(hmd->relation_hist, &identity, now);

	result = os_thread_helper_init(&hmd->sensor_thread);

	if (result < 0) {
		HMD_ERROR(hmd, "Failed to init os thread helper");
		goto error;
	}

	m_imu_3dof_init(&hmd->fusion, M_IMU_3DOF_USE_GRAVITY_DUR_300MS);
	hmd->clock_tracker = m_clock_windowed_skew_tracker_alloc(64);

	result = os_thread_helper_start(&hmd->sensor_thread, sensor_thread, hmd);

	if (result < 0) {
		HMD_ERROR(hmd, "Failed to start sensor thread");
		goto error;
	}

	// Setup variable tracker: Optional but useful for debugging
	u_var_add_root(hmd, "Rift HMD", true);
	u_var_add_log_level(hmd, &hmd->log_level, "log_level");
	m_imu_3dof_add_vars(&hmd->fusion, hmd, "3dof_");

	return hmd;
error:
	rift_hmd_destroy(&hmd->base);
	return NULL;
}
