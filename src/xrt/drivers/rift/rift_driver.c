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
#include "xrt/xrt_results.h"

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

#include <stdio.h>
#include <assert.h>

#include "rift_distortion.h"
#include "rift_internal.h"
#include "rift_usb.h"


/*
 *
 * Structs and defines.
 *
 */

DEBUG_GET_ONCE_LOG_OPTION(rift_log, "RIFT_LOG", U_LOGGING_WARN)
DEBUG_GET_ONCE_FLOAT_OPTION(rift_override_icd_mm, "RIFT_OVERRIDE_ICD", 0.0f)
DEBUG_GET_ONCE_BOOL_OPTION(rift_use_firmware_distortion, "RIFT_USE_FIRMWARE_DISTORTION", false)
DEBUG_GET_ONCE_BOOL_OPTION(rift_power_override, "RIFT_POWER_OVERRIDE", false)
DEBUG_GET_ONCE_FLOAT_OPTION(rift_startup_wait_time, "RIFT_STARTUP_WAIT_TIME", 5.0f)

/*
 *
 * Headset functions
 *
 */

static int
rift_sensor_thread_tick(struct rift_hmd *hmd)
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
	timepoint_ns recv_time_ns = os_monotonic_get_ns();

	if (result < 0) {
		HMD_ERROR(hmd, "Got error reading from device, assuming fatal, reason %d", result);
		return result;
	}

	if (result == 0) {
		HMD_WARN(hmd, "Timed out waiting for packet from headset, packets should come in at %dhz",
		         IMU_SAMPLE_RATE);
		return 0;
	}

	switch (hmd->variant) {
	case RIFT_VARIANT_CV1:
	case RIFT_VARIANT_DK2: {
		// skip unknown commands
		if (buf[0] != IN_REPORT_DK2) {
			HMD_WARN(hmd, "Skipping unknown IN command %d", buf[0]);
			return 0;
		}

		struct dk2_in_report report;

		// don't treat invalid IN reports as fatal, just ignore them
		if (result < (int)sizeof(report)) {
			HMD_WARN(hmd, "Got malformed DK2 IN report with size %d", result);
			return 0;
		}

		// TODO: handle endianness
		memcpy(&report, buf + 1, sizeof(report));

		// if there's no samples, just do nothing.
		if (report.num_samples == 0) {
			return 0;
		}

		if (!hmd->processed_sample_packet) {
			hmd->last_remote_sample_time_us = report.sample_timestamp;
			hmd->processed_sample_packet = true;
		}

		// wrap-around intentional and A-OK, given these are unsigned
		uint32_t remote_sample_delta_us = report.sample_timestamp - hmd->last_remote_sample_time_us;

		hmd->last_remote_sample_time_us = report.sample_timestamp;

		hmd->last_remote_sample_time_ns += (int64_t)remote_sample_delta_us * OS_NS_PER_USEC;

		m_clock_windowed_skew_tracker_push(hmd->clock_tracker, recv_time_ns, hmd->last_remote_sample_time_ns);

		int64_t local_timestamp_ns;
		// if we haven't synchronized our clocks, just do nothing
		if (!m_clock_windowed_skew_tracker_to_local(hmd->clock_tracker, hmd->last_remote_sample_time_ns,
		                                            &local_timestamp_ns)) {
			return 0;
		}

		if (report.num_samples > 1)
			HMD_TRACE(hmd,
			          "Had more than one sample queued! We aren't receiving IN reports fast enough, HMD "
			          "had %d samples in the queue! Having to work back that first sample...",
			          report.num_samples);

		for (int i = 0; i < MIN(DK2_MAX_SAMPLES, report.num_samples); i++) {
			struct rift_dk2_sample_pack latest_sample_pack = report.samples[i];

			struct xrt_vec3 accel, gyro;
			rift_unpack_float_sample(latest_sample_pack.accel.data, 0.0001f, &accel);
			rift_unpack_float_sample(latest_sample_pack.gyro.data, 0.0001f, &gyro);

			// If the HMD is not doing it's own calibration, we need to apply it now
			if (hmd->imu_needs_calibration) {
				math_matrix_3x3_transform_vec3(&hmd->imu_calibration.gyro_matrix, &gyro, &gyro);
				math_vec3_subtract(&hmd->imu_calibration.gyro_offset, &gyro);

				math_matrix_3x3_transform_vec3(&hmd->imu_calibration.accel_matrix, &accel, &accel);
				math_vec3_subtract(&hmd->imu_calibration.accel_offset, &accel);
			}

			// work back the likely timestamp of the current sample
			// if there's only one sample, then this will always be zero, if there's two or more samples,
			// the previous samples will be offset by the sample rate of the IMU
			int64_t sample_local_timestamp_ns =
			    local_timestamp_ns - ((MIN(report.num_samples, DK2_MAX_SAMPLES) - 1) * NS_PER_SAMPLE);

			// drop packets which are in the past (TODO: figure out why these happen..)
			if (sample_local_timestamp_ns < hmd->last_sample_local_timestamp_ns) {
				break;
			}

			hmd->last_sample_local_timestamp_ns = sample_local_timestamp_ns;

			// update the IMU for that sample
			m_imu_3dof_update(&hmd->fusion, sample_local_timestamp_ns, &accel, &gyro);

			// push the pose of the IMU for that sample, doing so per sample
			struct xrt_space_relation relation = XRT_SPACE_RELATION_ZERO;
			relation.relation_flags = (enum xrt_space_relation_flags)(
			    XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT | XRT_SPACE_RELATION_ORIENTATION_VALID_BIT);
			relation.pose.orientation = hmd->fusion.rot;
			m_relation_history_push(hmd->relation_hist, &relation, sample_local_timestamp_ns);
		}

		break;
	}
	case RIFT_VARIANT_DK1: {
		HMD_ERROR(hmd, "DK1 support not implemented yet");
		return -1;
	}
	}

	return 0;
}

static void *
rift_sensor_thread(void *ptr)
{
	U_TRACE_SET_THREAD_NAME("Rift sensor thread");

	struct rift_hmd *hmd = (struct rift_hmd *)ptr;

	os_thread_helper_lock(&hmd->sensor_thread);

	// uncomment this to be able to see if things are actually progressing as expected in a debugger, without having
	// to count yourself
	// #define TICK_DEBUG

	int result = 0;
#ifdef TICK_DEBUG
	int ticks = 0;
#endif

	while (os_thread_helper_is_running_locked(&hmd->sensor_thread) && result >= 0) {
		os_thread_helper_unlock(&hmd->sensor_thread);

		result = rift_sensor_thread_tick(hmd);

		os_thread_helper_lock(&hmd->sensor_thread);
#ifdef TICK_DEBUG
		ticks += 1;
#endif
	}

	os_thread_helper_unlock(&hmd->sensor_thread);

	if (result < 0) {
		HMD_ERROR(hmd, "Sensor thread exiting due to error %d", result);
	} else {
		HMD_DEBUG(hmd, "Sensor thread exiting cleanly");
	}

	return NULL;
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

	// Only free if we are definitely the ones that allocated it
	if (hmd->lens_distortions && debug_get_bool_option_rift_use_firmware_distortion())
		free((void *)hmd->lens_distortions);

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
                        enum xrt_view_type view_type,
                        uint32_t view_count,
                        struct xrt_space_relation *out_head_relation,
                        struct xrt_fov *out_fovs,
                        struct xrt_pose *out_poses)
{
	struct rift_hmd *hmd = rift_hmd(xdev);

	return u_device_get_view_poses(                                  //
	    xdev,                                                        //
	    &(struct xrt_vec3){hmd->extra_display_info.icd, 0.0f, 0.0f}, //
	    at_timestamp_ns,                                             //
	    view_type,                                                   //
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

int
rift_devices_create(struct os_hid_device *dev,
                    enum rift_variant variant,
                    const char *device_name,
                    const char *serial_number,
                    struct rift_hmd **out_hmd,
                    struct xrt_device **out_xdevs)
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

	if (variant == RIFT_VARIANT_CV1) {
		// On CV1, we need to send a command to enable headset features
		rift_enable_components(hmd, &(struct rift_enable_components_report){.flags = RIFT_COMPONENT_DISPLAY |
		                                                                             RIFT_COMPONENT_AUDIO |
		                                                                             RIFT_COMPONENT_LEDS});
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

	// Set a sane new config
	hmd->config.config_flags = RIFT_CONFIG_REPORT_USE_CALIBRATION | RIFT_CONFIG_REPORT_AUTO_CALIBRATION |
	                           RIFT_CONFIG_REPORT_COMMAND_KEEP_ALIVE | RIFT_CONFIG_REPORT_MOTION_KEEP_ALIVE;

	if (debug_get_bool_option_rift_power_override()) {
		hmd->config.config_flags |= RIFT_CONFIG_REPORT_OVERRIDE_POWER;
		HMD_INFO(hmd, "Enabling the override power config flag.");
	} else {
		hmd->config.config_flags &= ~RIFT_CONFIG_REPORT_OVERRIDE_POWER;
		HMD_DEBUG(hmd, "Disabling the override power config flag.");
	}

	// @todo figure out why we have to set these to zero
	hmd->config.interval = 0;
	hmd->config.command_id = 0;

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

	if ((hmd->config.config_flags & RIFT_CONFIG_REPORT_USE_CALIBRATION) == 0) {
		HMD_INFO(hmd,
		         "Headset calibration enabling ignored by the headset, "
		         "reading out calibration to do ourselves");

		result = rift_get_imu_calibration(hmd, &hmd->imu_calibration);
		if (result < 0) {
			HMD_ERROR(hmd,
			          "failed to get IMU calibration, this is non-fatal, but headset IMU might drift more "
			          "than expected, reason %d",
			          result);
			hmd->imu_needs_calibration = false;
		} else {
			hmd->imu_needs_calibration = true;
			HMD_DEBUG(hmd, "Got IMU calibration from headset");
		}
	}

	if (debug_get_bool_option_rift_use_firmware_distortion()) {
		// get the lens distortions
		struct rift_lens_distortion_report lens_distortion_report;
		result = rift_get_lens_distortion(hmd, &lens_distortion_report);
		if (result < 0) {
			HMD_ERROR(hmd, "Failed to get lens distortion, reason %d", result);
			goto error;
		}

		hmd->num_lens_distortions = lens_distortion_report.num_distortions;
		struct rift_lens_distortion *lens_distortions =
		    calloc(lens_distortion_report.num_distortions, sizeof(struct rift_lens_distortion));

		rift_parse_distortion_report(&lens_distortion_report,
		                             &lens_distortions[lens_distortion_report.distortion_idx]);
		// TODO: actually verify we initialize all the distortions. if the headset is working correctly, this
		// should have happened, but you never know.
		for (uint16_t i = 1; i < hmd->num_lens_distortions; i++) {
			result = rift_get_lens_distortion(hmd, &lens_distortion_report);
			if (result < 0) {
				HMD_ERROR(hmd, "Failed to get lens distortion idx %d, reason %d", i, result);
				free(lens_distortions);
				goto error;
			}

			rift_parse_distortion_report(&lens_distortion_report,
			                             &lens_distortions[lens_distortion_report.distortion_idx]);
		}

		hmd->lens_distortions = lens_distortions;

		// TODO: pick the correct distortion for the eye relief setting the user has picked
		hmd->distortion_in_use = 0;
	} else {
		rift_fill_in_default_distortions(hmd);
	}

	// fill in extra display info about the headset

	switch (hmd->variant) {
	case RIFT_VARIANT_CV1: // TODO: figure out the *real* values for CV1 by dumping them from LibOVR somehow
		hmd->extra_display_info.lens_diameter_meters = 0.05f;
		hmd->extra_display_info.screen_gap_meters = 0.0f;
		break;
	case RIFT_VARIANT_DK2:
		hmd->extra_display_info.lens_diameter_meters = 0.04f;
		hmd->extra_display_info.screen_gap_meters = 0.0f;
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

	m_relation_history_create(&hmd->relation_hist);

	// Setup input.
	hmd->base.name = XRT_DEVICE_GENERIC_HMD;
	hmd->base.device_type = XRT_DEVICE_TYPE_HMD;
	hmd->base.inputs[0].name = XRT_INPUT_GENERIC_HEAD_POSE;
	hmd->base.supported.orientation_tracking = true;
	hmd->base.supported.position_tracking = false; // set to true once we are trying to get the sensor 6dof to work

	// Set up display details
	switch (hmd->variant) {
	case RIFT_VARIANT_DK1: hmd->base.hmd->screens[0].nominal_frame_interval_ns = time_s_to_ns(1.0f / 60.0f); break;
	case RIFT_VARIANT_DK2: hmd->base.hmd->screens[0].nominal_frame_interval_ns = time_s_to_ns(1.0f / 75.0f); break;
	case RIFT_VARIANT_CV1: hmd->base.hmd->screens[0].nominal_frame_interval_ns = time_s_to_ns(1.0f / 90.0f); break;
	}

	hmd->extra_display_info.icd = MICROMETERS_TO_METERS(hmd->display_info.lens_separation);

	float icd_override_mm = debug_get_float_option_rift_override_icd_mm();
	if (icd_override_mm > 0.0f) {
		hmd->extra_display_info.icd = icd_override_mm / 1000.0f;
		HMD_INFO(hmd, "Forcing ICD to %f", hmd->extra_display_info.icd);
	} else {
		HMD_DEBUG(hmd, "Using default ICD of %f", hmd->extra_display_info.icd);
	}

	switch (hmd->variant) {
	case RIFT_VARIANT_CV1: {
		hmd->base.hmd->screens[0].w_pixels = hmd->display_info.resolution_x;
		hmd->base.hmd->screens[0].h_pixels = hmd->display_info.resolution_y;

		// TODO: properly apply using rift_extra_display_info.screen_gap_meters, but this isn't necessary, as
		//       observed gap is always zero
		uint16_t view_width = hmd->display_info.resolution_x / 2;
		uint16_t view_height = hmd->display_info.resolution_y;

		for (uint32_t i = 0; i < 2; ++i) {
			hmd->base.hmd->views[i].display.w_pixels = view_width;
			hmd->base.hmd->views[i].display.h_pixels = view_height;

			hmd->base.hmd->views[i].viewport.x_pixels = i * (hmd->display_info.resolution_x / 2);
			hmd->base.hmd->views[i].viewport.y_pixels = 0;
			hmd->base.hmd->views[i].viewport.w_pixels = view_width;
			hmd->base.hmd->views[i].viewport.h_pixels = view_height;
			hmd->base.hmd->views[i].rot = u_device_rotation_ident;
		}

		// TODO: figure out how to calculate this programmatically, right now this is hardcoded with data dumped
		//       from oculus' OpenXR runtime, some of the math for this is in rift_distortion.c, used for
		//       calculating distortion
		hmd->base.hmd->distortion.fov[0].angle_up = 0.7269826;
		hmd->base.hmd->distortion.fov[0].angle_down = -0.8378981;
		hmd->base.hmd->distortion.fov[0].angle_left = -0.76754993;
		hmd->base.hmd->distortion.fov[0].angle_right = 0.6208969;

		hmd->base.hmd->distortion.fov[1].angle_up = 0.7269826;
		hmd->base.hmd->distortion.fov[1].angle_down = -0.8378981;
		hmd->base.hmd->distortion.fov[1].angle_left = -0.6208969;
		hmd->base.hmd->distortion.fov[1].angle_right = 0.76754993;

		break;
	}
	case RIFT_VARIANT_DK1: // TODO: actually figure out if this is correct for DK1
	case RIFT_VARIANT_DK2: {
		// screen is rotated, so we need to undo that here
		hmd->base.hmd->screens[0].h_pixels = hmd->display_info.resolution_x;
		hmd->base.hmd->screens[0].w_pixels = hmd->display_info.resolution_y;

		// TODO: properly apply using rift_extra_display_info.screen_gap_meters, but this isn't necessary, as
		//       observed gap is always zero
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

	m_imu_3dof_init(&hmd->fusion, M_IMU_3DOF_USE_GRAVITY_DUR_20MS);
	hmd->clock_tracker = m_clock_windowed_skew_tracker_alloc(64);

	result = os_thread_helper_start(&hmd->sensor_thread, rift_sensor_thread, hmd);

	if (result < 0) {
		HMD_ERROR(hmd, "Failed to start sensor thread");
		goto error;
	}

	// Setup variable tracker: Optional but useful for debugging
	u_var_add_root(hmd, "Rift HMD", true);
	u_var_add_log_level(hmd, &hmd->log_level, "log_level");
	u_var_add_f32(hmd, &hmd->extra_display_info.icd, "ICD");
	m_imu_3dof_add_vars(&hmd->fusion, hmd, "3dof_");

	u_var_add_bool(hmd, &hmd->imu_needs_calibration, "imu_needs_calibration");
	u_var_add_gui_header(hmd, NULL, "IMU Calibration");
	u_var_add_vec3_f32(hmd, &hmd->imu_calibration.gyro_offset, "gyro_offset");
	u_var_add_vec3_f32(hmd, &hmd->imu_calibration.accel_offset, "accel_offset");
	u_var_add_vec3_f32(hmd, (struct xrt_vec3 *)&hmd->imu_calibration.gyro_matrix.v[0], "gyro_matrix[0..3]");
	u_var_add_vec3_f32(hmd, (struct xrt_vec3 *)&hmd->imu_calibration.gyro_matrix.v[3], "gyro_matrix[3..6]");
	u_var_add_vec3_f32(hmd, (struct xrt_vec3 *)&hmd->imu_calibration.gyro_matrix.v[6], "gyro_matrix[6..9]");
	u_var_add_vec3_f32(hmd, (struct xrt_vec3 *)&hmd->imu_calibration.accel_matrix.v[0], "accel_matrix[0..3]");
	u_var_add_vec3_f32(hmd, (struct xrt_vec3 *)&hmd->imu_calibration.accel_matrix.v[3], "accel_matrix[3..6]");
	u_var_add_vec3_f32(hmd, (struct xrt_vec3 *)&hmd->imu_calibration.accel_matrix.v[6], "accel_matrix[6..9]");
	u_var_add_f32(hmd, &hmd->imu_calibration.temperature, "temperature");

	// wait for display/controller init
	// @note once monado *has* the capabilities to signal to us when a display is ready, we should use them!
	os_nanosleep(time_s_to_ns(debug_get_float_option_rift_startup_wait_time()));

	*out_hmd = hmd;
	*out_xdevs = &hmd->base;

	return 1;
error:
	rift_hmd_destroy(&hmd->base);
	return -1;
}
