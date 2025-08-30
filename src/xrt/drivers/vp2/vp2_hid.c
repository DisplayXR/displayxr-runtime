// Copyright 2025, Beyley Cardellio
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Implementation of the Vive Pro 2 HID interface.
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @ingroup drv_vp2
 */

#include "os/os_hid.h"
#include "os/os_time.h"

#include "util/u_logging.h"
#include "util/u_debug.h"
#include "util/u_misc.h"

#include "xrt/xrt_byte_order.h"

#include "vp2_hid.h"


DEBUG_GET_ONCE_LOG_OPTION(vp2_log, "VP2_LOG", U_LOGGING_WARN)

// NOTE: VP2_RESOLUTION_3680_1836_90_02 is the safest resolution that works without needing the kernel patches.
DEBUG_GET_ONCE_NUM_OPTION(vp2_resolution, "VP2_RESOLUTION", VP2_RESOLUTION_3680_1836_90_02)

#define VP2_TRACE(vp2, ...) U_LOG_IFL_T(vp2->log_level, __VA_ARGS__)
#define VP2_DEBUG(vp2, ...) U_LOG_IFL_D(vp2->log_level, __VA_ARGS__)
#define VP2_INFO(vp2, ...) U_LOG_IFL_I(vp2->log_level, __VA_ARGS__)
#define VP2_WARN(vp2, ...) U_LOG_IFL_W(vp2->log_level, __VA_ARGS__)
#define VP2_ERROR(vp2, ...) U_LOG_IFL_E(vp2->log_level, __VA_ARGS__)

struct vp2_hid
{
	enum u_logging_level log_level;

	struct os_hid_device *hid;

	enum vp2_resolution resolution;
};

#define VP2_DATA_SIZE 64

#pragma pack(push, 1)

struct vp2_feature_header
{
	uint8_t id;
	__le16 sub_id;
	uint8_t size;
};

#pragma pack(pop)

static int
vp2_write(struct vp2_hid *vp2, uint8_t id, const uint8_t *data, size_t size)
{
	uint8_t buffer[VP2_DATA_SIZE] = {0};
	buffer[0] = id;
	if (size > sizeof(buffer) - 1) {
		VP2_ERROR(vp2, "Data size too large to write.");
		return -1;
	}

	memcpy(&buffer[1], data, size);
	return os_hid_write(vp2->hid, buffer, sizeof(buffer));
}

static int
vp2_write_feature(struct vp2_hid *vp2, uint8_t id, uint16_t sub_id, const uint8_t *data, size_t size)
{
	struct vp2_feature_header header = {
	    .id = id,
	    .sub_id = __cpu_to_le16(sub_id),
	    .size = (uint8_t)size,
	};

	uint8_t buffer[VP2_DATA_SIZE] = {0};
	memcpy(buffer, &header, sizeof(header));
	if (size > sizeof(buffer) - sizeof(header)) {
		VP2_ERROR(vp2, "Data size too large to write.");
		return -1;
	}

	memcpy(&buffer[sizeof(header)], data, size);
	return os_hid_set_feature(vp2->hid, buffer, sizeof(buffer));
}

int
vp2_set_resolution(struct vp2_hid *vp2, enum vp2_resolution resolution)
{
	const uint8_t *wireless_command = (const uint8_t *)"wireless,0";
	int ret = vp2_write_feature(vp2, 0x04, 0x2970, wireless_command, strlen((const char *)wireless_command));
	if (ret < 0) {
		VP2_ERROR(vp2, "Failed to write no wireless command.");
		return ret;
	}

	uint8_t resolution_command[16];
	int command_length =
	    snprintf((char *)resolution_command, sizeof(resolution_command), "dtd,%d", (uint8_t)resolution);

	ret = vp2_write_feature(vp2, 0x04, 0x2970, resolution_command, command_length);
	if (ret < 0) {
		VP2_ERROR(vp2, "Failed to write resolution command.");
		return ret;
	}

	return 0;
}

static int
vp2_read(struct vp2_hid *vp2, uint8_t id, const uint8_t *prefix, size_t prefix_size, uint8_t *out_data, size_t out_size)
{
	uint8_t buffer[VP2_DATA_SIZE] = {0};
	int ret = os_hid_read(vp2->hid, buffer, sizeof(buffer), 500);
	if (ret < 0) {
		VP2_ERROR(vp2, "Failed to read from HID device.");
		return ret;
	}
	if (ret == 0) {
		VP2_WARN(vp2, "Timeout reading from HID device.");
		return -1;
	}

	if (buffer[0] != id) {
		VP2_ERROR(vp2, "Unexpected report ID: got %02x, expected %02x", buffer[0], id);
		return -1;
	}

	if (prefix_size > 0 && memcmp(&buffer[1], prefix, prefix_size) != 0) {
		VP2_ERROR(vp2, "Unexpected report prefix.");
		U_LOG_IFL_D_HEX(vp2->log_level, &buffer[1], prefix_size);
		U_LOG_IFL_D_HEX(vp2->log_level, prefix, prefix_size);
		return -1;
	}

	uint8_t size = buffer[1 + prefix_size];
	if (size > out_size) {
		VP2_ERROR(vp2, "Output buffer too small: got %zu, need %u", out_size, size);
		return -1;
	}

	memcpy(out_data, &buffer[2 + prefix_size], size);

	return size;
}

static int
vp2_read_int(struct vp2_hid *vp2, const char *command, int *out_value)
{
	int ret;

	ret = vp2_write(vp2, 0x02, (const uint8_t *)command, strlen((const char *)command));
	if (ret < 0) {
		VP2_ERROR(vp2, "Failed to write IPD read command.");
		return ret;
	}

	uint8_t response[VP2_DATA_SIZE] = {0};
	ret = vp2_read(vp2, 0x02, NULL, 0, response, sizeof(response));
	if (ret < 0) {
		VP2_ERROR(vp2, "Failed to read IPD response.");
		return ret;
	}

	// Null-terminate the response string
	response[ret] = '\0';

	*out_value = strtol((const char *)response, NULL, 10);

	return 0;
}

static int
vp2_read_ipd(struct vp2_hid *vp2, int *out, int *out_min, int *out_max, int *out_lap)
{
	int ret;

	ret = vp2_read_int(vp2, "mfg-r-ipdadc", out);
	if (ret < 0) {
		VP2_ERROR(vp2, "Failed to read IPD.");
		return ret;
	}

	ret = vp2_read_int(vp2, "mfg-r-ipdmin", out_min);
	if (ret < 0) {
		VP2_ERROR(vp2, "Failed to read IPD min.");
		return ret;
	}

	ret = vp2_read_int(vp2, "mfg-r-ipdmax", out_max);
	if (ret < 0) {
		VP2_ERROR(vp2, "Failed to read IPD max.");
		return ret;
	}

	ret = vp2_read_int(vp2, "mfg-r-ipdlap", out_lap);
	if (ret < 0) {
		VP2_ERROR(vp2, "Failed to read IPD lap.");
		return ret;
	}

	return 0;
}

int
vp2_hid_open(struct os_hid_device *hid_dev, struct vp2_hid **out_hid)
{
	int ret;

	struct vp2_hid *vp2 = U_TYPED_CALLOC(struct vp2_hid);

	vp2->log_level = debug_get_log_option_vp2_log();
	vp2->hid = hid_dev;
	vp2->resolution = (enum vp2_resolution)debug_get_num_option_vp2_resolution();

	*out_hid = vp2;

	VP2_INFO(vp2, "Opened Vive Pro 2 HID device.");

	ret = vp2_set_resolution(vp2, vp2->resolution);
	if (ret < 0) {
		VP2_WARN(vp2, "Failed to set resolution.");
	}
	os_nanosleep(U_TIME_1S_IN_NS * 3llu); // wait 3s for headset to reset

	// @todo: Figure out how to compute this into a mm value.
	int ipd, ipd_min, ipd_max, ipd_lap;
	ret = vp2_read_ipd(vp2, &ipd, &ipd_min, &ipd_max, &ipd_lap);
	if (ret == 0) {
		VP2_DEBUG(vp2, "IPD: %d (min: %d, max: %d, lap: %d)", ipd, ipd_min, ipd_max, ipd_lap);
	} else {
		VP2_WARN(vp2, "Failed to read IPD values.");
	}

	return 0;
}

enum vp2_resolution
vp2_get_resolution(struct vp2_hid *vp2)
{
	assert(vp2 != NULL);

	return vp2->resolution;
}

void
vp2_hid_destroy(struct vp2_hid *vp2)
{
	if (vp2->hid != NULL) {
		os_hid_destroy(vp2->hid);
		vp2->hid = NULL;
	}

	VP2_INFO(vp2, "Destroyed Vive Pro 2 HID device.");

	free(vp2);
}