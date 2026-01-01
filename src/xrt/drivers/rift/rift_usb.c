// Copyright 2025, Beyley Cardellio
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  USB communications for the Oculus Rift.
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @ingroup drv_rift
 */

#include "rift_usb.h"


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

int
rift_send_keepalive(struct rift_hmd *hmd)
{
	struct rift_dk2_keepalive_mux_report report = {0, IN_REPORT_DK2,
	                                               KEEPALIVE_INTERVAL_NS / 1000000}; // convert ns to ms

	int result = rift_send_report(hmd, FEATURE_REPORT_KEEPALIVE_MUX, &report, sizeof(report));

	if (result < 0) {
		return result;
	}

	hmd->last_keepalive_time = os_monotonic_get_ns();
	HMD_TRACE(hmd, "Sent keepalive at time %ld", hmd->last_keepalive_time);

	return 0;
}

int
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

int
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

int
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

int
rift_set_config(struct rift_hmd *hmd, struct rift_config_report *config)
{
	return rift_send_report(hmd, FEATURE_REPORT_CONFIG, config, sizeof(*config));
}

static float
rift_decode_fixed_point_uint16(uint16_t value, uint16_t zero_value, int fractional_bits)
{
	float value_float = (float)value;
	value_float -= (float)zero_value;
	value_float *= 1.0f / (float)(1 << fractional_bits);
	return value_float;
}

void
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

int
rift_enable_components(struct rift_hmd *hmd, struct rift_enable_components_report *enable_components)
{
	return rift_send_report(hmd, FEATURE_REPORT_ENABLE_COMPONENTS, enable_components, sizeof(*enable_components));
}

int
rift_get_imu_calibration(struct rift_hmd *hmd, struct rift_imu_calibration *imu_calibration)
{
	uint8_t buf[REPORT_MAX_SIZE] = {0};

	int result = rift_get_report(hmd, FEATURE_REPORT_CALIBRATE, buf, sizeof buf);
	if (result < 0) {
		return result;
	}

	struct rift_imu_calibration_report report;
	if ((size_t)result < sizeof report) {
		HMD_ERROR(hmd, "Got invalid size for calibration report");
		return -1;
	}

	memcpy(&report, buf + 1, sizeof report);

	imu_calibration->temperature = report.temperature * 0.01f;
	rift_unpack_float_sample(report.offset.gyro.data, 1e-4f, &imu_calibration->gyro_offset);
	rift_unpack_float_sample(report.offset.accel.data, 1e-4f, &imu_calibration->accel_offset);
	for (int i = 0; i < 3; i++) {
		const float scale = 1.0f / ((1 << 20) - 1);

		rift_unpack_float_sample(report.matrix_samples[i].gyro.data, scale,
		                         (struct xrt_vec3 *)&imu_calibration->gyro_matrix.v[3 * i]);
		imu_calibration->gyro_matrix.v[(3 * i) + (i)] += 1.0f;

		rift_unpack_float_sample(report.matrix_samples[i].accel.data, scale,
		                         (struct xrt_vec3 *)&imu_calibration->accel_matrix.v[3 * i]);
		imu_calibration->accel_matrix.v[(3 * i) + (i)] += 1.0f;
	}

	return 0;
}

/*
 * Decode 3 tightly packed 21 bit values from 4 bytes.
 * We unpack them in the higher 21 bit values first and then shift
 * them down to the lower in order to get the sign bits correct.
 *
 * Code taken/reformatted from OpenHMD's rift driver
 */
void
rift_unpack_int_sample(const uint8_t *in, struct xrt_vec3_i32 *out)
{
	int x = (in[0] << 24) | (in[1] << 16) | ((in[2] & 0xF8) << 8);
	int y = ((in[2] & 0x07) << 29) | (in[3] << 21) | (in[4] << 13) | ((in[5] & 0xC0) << 5);
	int z = ((in[5] & 0x3F) << 26) | (in[6] << 18) | (in[7] << 10);

	out->x = x >> 11;
	out->y = y >> 11;
	out->z = z >> 11;
}

void
rift_unpack_float_sample(const uint8_t *in, float scale, struct xrt_vec3 *out)
{
	int x = (in[0] << 24) | (in[1] << 16) | ((in[2] & 0xF8) << 8);
	int y = ((in[2] & 0x07) << 29) | (in[3] << 21) | (in[4] << 13) | ((in[5] & 0xC0) << 5);
	int z = ((in[5] & 0x3F) << 26) | (in[6] << 18) | (in[7] << 10);

	out->x = scale * (float)(x >> 11);
	out->y = scale * (float)(y >> 11);
	out->z = scale * (float)(z >> 11);
}
