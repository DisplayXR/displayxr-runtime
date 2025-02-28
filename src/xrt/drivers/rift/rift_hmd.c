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

#include "math/m_relation_history.h"
#include "math/m_api.h"
#include "math/m_mathinclude.h" // IWYU pragma: keep

#include "util/u_debug.h"
#include "util/u_device.h"
#include "util/u_distortion_mesh.h"
#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_time.h"
#include "util/u_var.h"
#include "util/u_visibility_mask.h"
#include "xrt/xrt_results.h"

#include <stdio.h>
#include <assert.h>

/*
 *
 * Structs and defines.
 *
 */

/// Casting helper function
static inline struct rift_hmd *
rift_hmd(struct xrt_device *xdev)
{
	return (struct rift_hmd *)xdev;
}

DEBUG_GET_ONCE_LOG_OPTION(rift_log, "RIFT_LOG", U_LOGGING_WARN)

#define HMD_TRACE(hmd, ...) U_LOG_XDEV_IFL_T(&hmd->base, hmd->log_level, __VA_ARGS__)
#define HMD_DEBUG(hmd, ...) U_LOG_XDEV_IFL_D(&hmd->base, hmd->log_level, __VA_ARGS__)
#define HMD_INFO(hmd, ...) U_LOG_XDEV_IFL_I(&hmd->base, hmd->log_level, __VA_ARGS__)
#define HMD_ERROR(hmd, ...) U_LOG_XDEV_IFL_E(&hmd->base, hmd->log_level, __VA_ARGS__)

/*
 *
 * Headset functions
 *
 */

static int
rift_send_report(struct rift_hmd *hmd, uint8_t report_id, void *data, size_t data_length)
{
#define REPORT_WRITE_DATA(ptr, ptr_len)                                                                                \
	if (ptr_len > sizeof(buffer) - length) {                                                                       \
		HMD_ERROR(hmd, "Tried to fit %ld bytes into buffer with only %ld bytes left", ptr_len,                 \
		          sizeof(buffer) - length);                                                                    \
		return -1;                                                                                             \
	}                                                                                                              \
	memcpy(buffer + length, ptr, ptr_len);                                                                         \
	length += ptr_len;

#define REPORT_WRITE_VALUE(value) REPORT_WRITE_DATA(&value, sizeof(value));

	int result;

	uint8_t buffer[REPORT_MAX_SIZE];
	size_t length = 0;

	REPORT_WRITE_VALUE(report_id)
	REPORT_WRITE_DATA(data, data_length)

	result = os_hid_set_feature(hmd->hid_dev, buffer, length);
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
	struct dk2_report_keepalive_mux report = {0, IN_REPORT_DK2, 10000};

	int result =
	    rift_send_report(hmd, FEATURE_REPORT_KEEPALIVE_MUX, &report, sizeof(struct dk2_report_keepalive_mux));

	if (result < 0) {
		return result;
	}

	hmd->last_keepalive_time = os_monotonic_get_ns();

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
	if ((hmd->variant == RIFT_VARIANT_DK1 || hmd->variant == RIFT_VARIANT_DK2) && config->sample_rate != 1000) {
		HMD_ERROR(hmd, "Got invalid config from headset, got sample rate %d when expected %d",
		          config->sample_rate, 1000);
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
	return rift_send_report(hmd, FEATURE_REPORT_CONFIG, &config, sizeof(*config));
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
	/*
	 * For HMDs you can call this function or directly set
	 * the `get_view_poses` function on the device to it.
	 */
	return u_device_get_view_poses(  //
	    xdev,                 //
	    default_eye_relation, //
	    at_timestamp_ns,      //
	    view_count,           //
	    out_head_relation,    //
	    out_fovs,             //
	    out_poses);           //
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
rift_catmull_rom_spline(struct rift_catmull_rom_distortion_data *catmull, float scaled_value)
{
	float scaled_value_floor = floorf(scaled_value);
	scaled_value_floor = CLAMP(scaled_value_floor, 0, CATMULL_COEFFICIENTS - 1);

	float t = scaled_value - scaled_value_floor;
	int k = (int)scaled_value_floor;

	float p0, p1, m0, m1;
	switch (k) {
	case 0:
		p0 = 1.0f;
		m0 = (catmull->k[1] - catmull->k[0]);
		p1 = catmull->k[1];
		m1 = 0.5f * (catmull->k[2] - catmull->k[0]);
		break;
	default:
		p0 = catmull->k[k];
		m0 = 0.5f * (catmull->k[k + 1] - catmull->k[k - 1]);
		p1 = catmull->k[k + 1];
		m1 = 0.5f * (catmull->k[k + 2] - catmull->k[k]);
		break;
	case CATMULL_COEFFICIENTS - 2:
		p0 = catmull->k[CATMULL_COEFFICIENTS - 2];
		m0 = 0.5f * (catmull->k[CATMULL_COEFFICIENTS - 1] - catmull->k[CATMULL_COEFFICIENTS - 2]);
		p1 = catmull->k[CATMULL_COEFFICIENTS - 1];
		m1 = catmull->k[CATMULL_COEFFICIENTS - 1] - catmull->k[CATMULL_COEFFICIENTS - 2];
		break;
	case CATMULL_COEFFICIENTS - 1:
		p0 = catmull->k[CATMULL_COEFFICIENTS - 1];
		m0 = catmull->k[CATMULL_COEFFICIENTS - 1] - catmull->k[CATMULL_COEFFICIENTS - 2];
		p1 = p0 + m0;
		m1 = m0;
		break;
	}

	float omt = 1.0f - t;

	float res = (p0 * (1.0f + 2.0f * t) + m0 * t) * omt * omt + (p1 * (1.0f + 2.0f * omt) - m1 * omt) * t * t;

	return res;
}

static float
rift_distortion_distance_squared(struct rift_lens_distortion *lens_distortion, float distance_squared)
{
	float scale = 1.0f;

	switch (lens_distortion->distortion_version) {
	case RIFT_LENS_DISTORTION_LCSV_CATMULL_ROM_10_VERSION_1: {
		struct rift_catmull_rom_distortion_data data = lens_distortion->data.lcsv_catmull_rom_10;

		float scaled_distance_squared =
		    (float)(CATMULL_COEFFICIENTS - 1) * distance_squared / (data.max_r * data.max_r);

		return rift_catmull_rom_spline(&data, scaled_distance_squared);
	}
	default: return scale;
	}
}

static struct xrt_vec3
rift_distortion_distance_squared_split_chroma(struct rift_lens_distortion *lens_distortion, float distance_squared)
{
	float scale = rift_distortion_distance_squared(lens_distortion, distance_squared);

	struct xrt_vec3 scale_split;
	scale_split.x = scale;
	scale_split.y = scale;
	scale_split.z = scale;

	switch (lens_distortion->distortion_version) {
	case RIFT_LENS_DISTORTION_LCSV_CATMULL_ROM_10_VERSION_1: {
		struct rift_catmull_rom_distortion_data data = lens_distortion->data.lcsv_catmull_rom_10;

		scale_split.x *= 1.0f + data.chromatic_abberation[0] + distance_squared * data.chromatic_abberation[1];
		scale_split.z *= 1.0f + data.chromatic_abberation[2] + distance_squared * data.chromatic_abberation[3];
		break;
	}
	}

	return scale_split;
}

static bool
rift_hmd_compute_distortion(struct xrt_device *dev, uint32_t view, float u, float v, struct xrt_uv_triplet *out_result)
{
#define ZERO_ONE_TO_N_ONE_ONE(x) ((x * 2) - 1)
#define N_ONE_ONE_TO_ZERO_ONE(x) ((x + 1) / 2)

	struct rift_hmd *hmd = rift_hmd(dev);

	struct rift_lens_distortion *distortion = &hmd->lens_distortions[0];

	float display_width_meters = (float)hmd->display_info.display_width / 1000000.0f;
	float display_height_meters = (float)hmd->display_info.display_height / 1000000.0f;

	float tan_eye_angle_scale_x =
	    display_width_meters / distortion->data.lcsv_catmull_rom_10.meters_per_tan_angle_at_center * 0.25f;
	float tan_eye_angle_scale_y =
	    display_height_meters / distortion->data.lcsv_catmull_rom_10.meters_per_tan_angle_at_center * 0.5f;

	u = ZERO_ONE_TO_N_ONE_ONE(u) * tan_eye_angle_scale_x;
	v = ZERO_ONE_TO_N_ONE_ONE(v) * tan_eye_angle_scale_y;

	float distance_squared = fabsf(u * u + v * v);

	struct xrt_vec3 chroma_distortions =
	    rift_distortion_distance_squared_split_chroma(distortion, distance_squared);

	out_result->r = (struct xrt_vec2){N_ONE_ONE_TO_ZERO_ONE(u * chroma_distortions.x),
	                                  N_ONE_ONE_TO_ZERO_ONE(v * chroma_distortions.x)};
	out_result->g = (struct xrt_vec2){N_ONE_ONE_TO_ZERO_ONE(u * chroma_distortions.y),
	                                  N_ONE_ONE_TO_ZERO_ONE(v * chroma_distortions.y)};
	out_result->b = (struct xrt_vec2){N_ONE_ONE_TO_ZERO_ONE(u * chroma_distortions.z),
	                                  N_ONE_ONE_TO_ZERO_ONE(v * chroma_distortions.z)};

	return true;
#undef ZERO_ONE_TO_N_ONE_ONE
#undef N_ONE_ONE_TO_ZERO_ONE
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
		u_device_free(&hmd->base);
		return NULL;
	}

	result = rift_get_config(hmd, &hmd->config);
	if (result < 0) {
		HMD_ERROR(hmd, "Failed to get device config, reason %d", result);
		u_device_free(&hmd->base);
		return NULL;
	}
	HMD_INFO(hmd, "Got config from hmd, config flags: %X", hmd->config.config_flags);

	result = rift_get_display_info(hmd, &hmd->display_info);
	if (result < 0) {
		HMD_ERROR(hmd, "Failed to get device config, reason %d", result);
		u_device_free(&hmd->base);
		return NULL;
	}
	HMD_INFO(hmd, "Got display info from hmd, res: %dx%d", hmd->display_info.resolution_x,
	         hmd->display_info.resolution_y);

	if (getenv("RIFT_POWER_OVERRIDE") != NULL) {
		hmd->config.config_flags |= RIFT_CONFIG_REPORT_OVERRIDE_POWER;
		HMD_INFO(hmd, "Force-enabling the override power config flag.");
	}

	// force enable calibration use and auto calibration
	// this is on by default according to the firmware on DK1 and DK2,
	// but OpenHMD forces them on, we should do the same, they probably had a reason
	hmd->config.config_flags |= RIFT_CONFIG_REPORT_USE_CALIBRATION;
	hmd->config.config_flags |= RIFT_CONFIG_REPORT_AUTO_CALIBRATION;

	// update the config
	result = rift_set_config(hmd, &hmd->config);
	if (result < 0) {
		HMD_ERROR(hmd, "Failed to set the device config, reason %d", result);
		u_device_free(&hmd->base);
		return NULL;
	}

	// get the lens distortions
	struct rift_lens_distortion_report lens_distortion;
	result = rift_get_lens_distortion(hmd, &lens_distortion);
	if (result < 0) {
		HMD_ERROR(hmd, "Failed to get lens distortion, reason %d", result);
		u_device_free(&hmd->base);
		return NULL;
	}

	hmd->num_lens_distortions = lens_distortion.num_distortions;
	hmd->lens_distortions = calloc(lens_distortion.num_distortions, sizeof(struct rift_lens_distortion));

	rift_parse_distortion_report(&lens_distortion, &hmd->lens_distortions[lens_distortion.distortion_idx]);
	// TODO: actually verify we initialize all the distortions. if the headset is working correctly, this should
	// have happened, but you never know.
	for (uint16_t i = 1; i < hmd->num_lens_distortions; i++) {
		result = rift_get_lens_distortion(hmd, &lens_distortion);
		if (result < 0) {
			HMD_ERROR(hmd, "Failed to get lens distortion idx %d, reason %d", i, result);
			u_device_free(&hmd->base);
			return NULL;
		}

		rift_parse_distortion_report(&lens_distortion, &hmd->lens_distortions[lens_distortion.distortion_idx]);
	}

	// This list should be ordered, most preferred first.
	size_t idx = 0;
	hmd->base.hmd->blend_modes[idx++] = XRT_BLEND_MODE_OPAQUE;
	hmd->base.hmd->blend_mode_count = idx;

	hmd->base.update_inputs = u_device_noop_update_inputs;
	hmd->base.get_tracked_pose = rift_hmd_get_tracked_pose;
	hmd->base.get_view_poses = rift_hmd_get_view_poses;
	hmd->base.get_visibility_mask = rift_hmd_get_visibility_mask;
	hmd->base.destroy = rift_hmd_destroy;

	// Distortion information, fills in xdev->compute_distortion().
	// u_distortion_mesh_set_none(&hmd->base);

	// populate this with something more complex if required
	hmd->base.compute_distortion = rift_hmd_compute_distortion;

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
	hmd->base.supported.position_tracking = false;

	// Set up display details
	// refresh rate
	hmd->base.hmd->screens[0].nominal_frame_interval_ns = time_s_to_ns(1.0f / 75.0f);

	struct u_device_simple_info info;
	info.display.w_pixels = hmd->display_info.resolution_x;
	info.display.h_pixels = hmd->display_info.resolution_y;
	info.display.w_meters = (float)hmd->display_info.display_width / 1000000.0f; // micrometers -> meters
	info.display.h_meters = (float)hmd->display_info.display_height / 1000000.0f;

	info.lens_horizontal_separation_meters = (float)hmd->display_info.lens_separation / 1000000.0f;

	// TODO: this is per eye on the headset, but we're just taking the left eye for this, we should be using both
	// eyes, ideally
	info.lens_vertical_position_meters = (float)hmd->display_info.lens_distance_l / 1000000.0f;

	// TODO: calculate this
	info.fov[0] = (float)(93.0 * (M_PI / 180.0));
	info.fov[1] = (float)(99.0 * (M_PI / 180.0));

	if (!u_device_setup_split_side_by_side(&hmd->base, &info)) {
		HMD_ERROR(hmd, "Failed to setup basic device info");
		rift_hmd_destroy(&hmd->base);
		return NULL;
	}

	// Just put an initial identity value in the tracker
	struct xrt_space_relation identity = XRT_SPACE_RELATION_ZERO;
	identity.relation_flags = (enum xrt_space_relation_flags)(XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |
	                                                          XRT_SPACE_RELATION_ORIENTATION_VALID_BIT);
	uint64_t now = os_monotonic_get_ns();
	m_relation_history_push(hmd->relation_hist, &identity, now);

	// Setup variable tracker: Optional but useful for debugging
	u_var_add_root(hmd, "Rift HMD", true);
	u_var_add_log_level(hmd, &hmd->log_level, "log_level");

	return hmd;
}
