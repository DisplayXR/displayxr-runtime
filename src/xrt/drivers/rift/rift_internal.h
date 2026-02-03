// Copyright 2025, Beyley Cardellio
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Interface to Oculus Rift driver code.
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @ingroup drv_rift
 */

#pragma once

#include "xrt/xrt_byte_order.h"

#include "util/u_device.h"
#include "util/u_logging.h"

#include "math/m_imu_3dof.h"
#include "math/m_api.h"
#include "math/m_mathinclude.h"
#include "math/m_clock_tracking.h"

#include "tracking/t_imu.h"

#include "os/os_hid.h"
#include "os/os_threading.h"

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#include "rift_interface.h"


#define HMD_TRACE(hmd, ...) U_LOG_XDEV_IFL_T(&hmd->base, hmd->log_level, __VA_ARGS__)
#define HMD_DEBUG(hmd, ...) U_LOG_XDEV_IFL_D(&hmd->base, hmd->log_level, __VA_ARGS__)
#define HMD_INFO(hmd, ...) U_LOG_XDEV_IFL_I(&hmd->base, hmd->log_level, __VA_ARGS__)
#define HMD_WARN(hmd, ...) U_LOG_XDEV_IFL_W(&hmd->base, hmd->log_level, __VA_ARGS__)
#define HMD_ERROR(hmd, ...) U_LOG_XDEV_IFL_E(&hmd->base, hmd->log_level, __VA_ARGS__)

#define REPORT_MAX_SIZE 69                // max size of a feature report (FEATURE_REPORT_CALIBRATE)
#define KEEPALIVE_INTERVAL_NS 10000000000 // 10 seconds
// give a 5% breathing room (at 10 seconds, this is 500 milliseconds of breathing room)
#define KEEPALIVE_SEND_RATE_NS ((KEEPALIVE_INTERVAL_NS * 19) / 20)
#define IMU_SAMPLE_RATE (1000)      // 1000hz
#define NS_PER_SAMPLE (1000 * 1000) // 1ms (1,000,000 ns) per sample
#define SERIAL_NUMBER_LENGTH 14

#define CALIBRATION_HASH_BYTE_OFFSET 0x1bf0
#define CALIBRATION_HASH_BYTE_LENGTH 0x10

#define RIFT_CONFIG_SUBDIR "rift"

#define CALIBRATION_HEADER_BYTE_OFFSET 0x0
#define CALIBRATION_HEADER_BYTE_LENGTH 0x4

#define CALIBRATION_BODY_BYTE_OFFSET 0x4
#define CALIBRATION_BODY_BYTE_CHUNK_LENGTH 0x14

#define MICROMETERS_TO_METERS(microns) ((float)microns / 1000000.0f)

// value taken from LibOVR 0.4.4
#define DEFAULT_EXTRA_EYE_ROTATION DEG_TO_RAD(30.0f)

#define IN_REPORT_DK2 11                 // sent on the HMD HID interface
#define IN_REPORT_RADIO_DATA 12          // sent on the radio HID interface
#define IN_REPORT_CV1_RADIO_KEEPALIVE 13 // sent on the HMD HID interface when no devices are connected

#define IN_REPORT_RADIO_DATA_SIZE 64

#ifdef __cplusplus
extern "C" {
#endif

// asserts the size of a type is equal to the byte size provided
#define SIZE_ASSERT(type, size)                                                                                        \
	static_assert(sizeof(type) == (size), "Size of " #type " is not " #size " bytes as was expected")

enum rift_feature_reports
{
	// DK1
	FEATURE_REPORT_CONFIG = 2,         // get + set
	FEATURE_REPORT_CALIBRATE = 3,      // get + set
	FEATURE_REPORT_RANGE = 4,          // get + set
	FEATURE_REPORT_REGISTER = 5,       // get + set
	FEATURE_REPORT_DFU = 6,            // get + set
	FEATURE_REPORT_DK1_KEEP_ALIVE = 8, // get + set
	FEATURE_REPORT_DISPLAY_INFO = 9,   // get + set
	FEATURE_REPORT_SERIAL = 10,        // get + set

	// DK2
	FEATURE_REPORT_TRACKING = 12,        // get + set
	FEATURE_REPORT_DISPLAY = 13,         // get + set
	FEATURE_REPORT_MAG_CALIBRATION = 14, // get + set
	FEATURE_REPORT_POS_CALIBRATION = 15, // get + set
	FEATURE_REPORT_CUSTOM_PATTERN = 16,  // get + set
	FEATURE_REPORT_KEEPALIVE_MUX = 17,   // get + set
	FEATURE_REPORT_MANUFACTURING = 18,   // get + set
	FEATURE_REPORT_UUID = 19,            // get + set
	FEATURE_REPORT_TEMPERATURE = 20,     // get + set
	FEATURE_REPORT_GYROOFFSET = 21,      // get only
	FEATURE_REPORT_LENS_DISTORTION = 22, // get + set

	// CV1
	FEATURE_REPORT_RADIO_CONTROL = 26,       // get + set
	FEATURE_REPORT_RADIO_READ_DATA_CMD = 27, // @todo: get + ???
	FEATURE_REPORT_ENABLE_COMPONENTS = 29,   // @todo: ??? + set
};

enum rift_config_report_flags
{
	// output the sample data raw from the sensors without converting them to known units
	RIFT_CONFIG_REPORT_USE_RAW = 1,
	// internal test mode for calibrating zero rate drift on gyro
	RIFT_CONFIG_REPORT_INTERNAL_CALIBRATION = 1 << 1,
	// use the calibration parameters stored on the device
	RIFT_CONFIG_REPORT_USE_CALIBRATION = 1 << 2,
	// recalibrate the gyro zero rate offset when the device is stationary
	RIFT_CONFIG_REPORT_AUTO_CALIBRATION = 1 << 3,
	// stop sending IN reports when the device has stopped moving for Interval milliseconds
	RIFT_CONFIG_REPORT_MOTION_KEEP_ALIVE = 1 << 4,
	// stop sending IN reports when the device has stopped receiving feature reports for Interval milliseconds
	RIFT_CONFIG_REPORT_COMMAND_KEEP_ALIVE = 1 << 5,
	// output the IN report data in the coordinate system used by LibOVR relative to the tracker, otherwise, report
	// in the coordinate system of the device
	RIFT_CONFIG_REPORT_USE_SENSOR_COORDINATES = 1 << 6,
	// override the power state of the USB hub, forcing it to act as if the external power source is connected (DK2
	// only, does nothing on DK1)
	RIFT_CONFIG_REPORT_OVERRIDE_POWER = 1 << 7,
};

enum rift_distortion_type
{
	RIFT_DISTORTION_TYPE_DIMS = 1,
	RIFT_DISTORTION_TYPE_K = 2,
};

enum rift_lens_type
{
	// firmware indirectly states lens type A is 0
	RIFT_LENS_TYPE_A = 0,
	// firmware does not state what lens type B is, 1 is an educated guess
	RIFT_LENS_TYPE_B = 1,
};

enum rift_lens_distortion_version
{
	// no distortion data is stored
	RIFT_LENS_DISTORTION_NONE = 0,
	// standard distortion matrix
	RIFT_LENS_DISTORTION_LCSV_CATMULL_ROM_10_VERSION_1 = 1,
};

enum rift_component_flags
{
	RIFT_COMPONENT_DISPLAY = 1 << 0,
	RIFT_COMPONENT_AUDIO = 1 << 1,
	RIFT_COMPONENT_LEDS = 1 << 2,
};

/*
 *
 * Packed structs for USB communication
 *
 */

#pragma pack(push, 1)

struct rift_config_report
{
	uint16_t command_id;
	uint8_t config_flags;
	// the IN report rate of the headset, rate is calculated as `sample_rate / (1 + interval)`
	uint8_t interval;
	// sample rate of the IMU, always 1000hz on DK1/DK2, read-only
	uint16_t sample_rate;
};

SIZE_ASSERT(struct rift_config_report, 6);

struct rift_display_info_report
{
	uint16_t command_id;
	uint8_t distortion_type;
	// the horizontal resolution of the display, in pixels
	uint16_t resolution_x;
	// the vertical resolution of the display, in pixels
	uint16_t resolution_y;
	// width in micrometers
	uint32_t display_width;
	// height in micrometers
	uint32_t display_height;
	// the vertical center of the display, in micrometers
	uint32_t center_v;
	// the separation between the two lenses, in micrometers
	uint32_t lens_separation;
	uint32_t lens_distance[2];
	float distortion[6];
};

SIZE_ASSERT(struct rift_display_info_report, 55);

#define CATMULL_COEFFICIENTS 11
#define CHROMATIC_ABBERATION_COEFFEICENT_COUNT 4

struct rift_catmull_rom_distortion_report_data
{
	// eye relief setting, in micrometers from front surface of lens
	uint16_t eye_relief;
	// the k coeffecients of the distortion
	uint16_t k[CATMULL_COEFFICIENTS];
	uint16_t max_r;
	uint16_t meters_per_tan_angle_at_center;
	uint16_t chromatic_abberation[CHROMATIC_ABBERATION_COEFFEICENT_COUNT];
	uint8_t unused[14];
};

SIZE_ASSERT(struct rift_catmull_rom_distortion_report_data, 50);

struct rift_lens_distortion_report
{
	uint16_t command_id;
	// the amount of distortions on this device
	uint8_t num_distortions;
	// the index of this distortion in the devices array
	uint8_t distortion_idx;
	// unused bitmask field
	uint8_t bitmask;
	// the type of the lenses
	uint16_t lens_type;
	// the version of the lens distortion data
	uint16_t distortion_version;

	union {
		struct rift_catmull_rom_distortion_report_data lcsv_catmull_rom_10;
	} data;
};

SIZE_ASSERT(struct rift_lens_distortion_report, 9 + sizeof(struct rift_catmull_rom_distortion_report_data));

struct rift_dk2_keepalive_mux_report
{
	uint16_t command;
	uint8_t in_report;
	uint16_t interval;
};

SIZE_ASSERT(struct rift_dk2_keepalive_mux_report, 5);

enum rift_display_mode
{
	RIFT_DISPLAY_MODE_GLOBAL,
	RIFT_DISPLAY_MODE_ROLLING_TOP_BOTTOM,
	RIFT_DISPLAY_MODE_ROLLING_LEFT_RIGHT,
	RIFT_DISPLAY_MODE_ROLLING_RIGHT_LEFT,
};

enum rift_display_limit
{
	RIFT_DISPLAY_LIMIT_ACL_OFF = 0,
	RIFT_DISPLAY_LIMIT_ACL_30 = 1,
	RIFT_DISPLAY_LIMIT_ACL_25 = 2,
	RIFT_DISPLAY_LIMIT_ACL_50 = 3,
};

enum rift_display_flags
{
	RIFT_DISPLAY_USE_ROLLING = 1 << 6,
	RIFT_DISPLAY_REVERSE_ROLLING = 1 << 7,
	RIFT_DISPLAY_HIGH_BRIGHTNESS = 1 << 8,
	RIFT_DISPLAY_SELF_REFRESH = 1 << 9,
	RIFT_DISPLAY_READ_PIXEL = 1 << 10,
	RIFT_DISPLAY_DIRECT_PENTILE = 1 << 11,
};

struct rift_display_report
{
	uint16_t command_id;
	// relative brightness setting independent of pixel persistence, only effective when high brightness is disabled
	uint8_t brightness;
	// a set of flags, ordered from LSB -> MSB
	// - panel mode/shutter type (4 bits), read only, see rift_display_mode
	// - current limit (2 bits), see rift_display_limit
	// - use rolling (1 bit)
	// - reverse rolling (1 bit), unavailable on released DK2 firmware for unknown reason
	// - high brightness (1 bit), unavailable on released DK2 firmware for unpublished reason
	// - self refresh (1 bit)
	// - read pixel (1 bit)
	// - direct pentile (1 bit)
	uint32_t flags;
	// the length of time in rows that the display is lit each frame, defaults to the full size of the display, full
	// persistence
	uint16_t persistence;
	// the offset in rows from vsync that the panel is lit when using global shutter, no effect in rolling shutter,
	// disabled on released DK2 firmware for unknown reason
	uint16_t lighting_offset;
	// the time in microseconds it is estimated for a pixel to settle to one value after it is set, read only
	uint16_t pixel_settle;
	// the number of rows including active area and blanking period used with persistence and lightingoffset, read
	// only
	uint16_t total_rows;
};

SIZE_ASSERT(struct rift_display_report, 15);

struct rift_dk2_sensor_sample
{
	uint8_t data[8];
};

SIZE_ASSERT(struct rift_dk2_sensor_sample, 8);

struct rift_dk2_sample_pack
{
	struct rift_dk2_sensor_sample accel;
	struct rift_dk2_sensor_sample gyro;
};

SIZE_ASSERT(struct rift_dk2_sample_pack, sizeof(struct rift_dk2_sensor_sample) * 2);

struct rift_dk2_version_data
{
	int16_t mag_x;
	int16_t mag_y;
	int16_t mag_z;
};

SIZE_ASSERT(struct rift_dk2_version_data, 6);

struct rift_cv1_version_data
{
	uint16_t presence_sensor;
	uint16_t iad_adc_value;
	uint16_t unk;
};

SIZE_ASSERT(struct rift_cv1_version_data, 6);
static_assert(sizeof(struct rift_cv1_version_data) == sizeof(struct rift_dk2_version_data),
              "Incorrect version data size");

#define DK2_MAX_SAMPLES 2
struct dk2_in_report
{
	uint16_t command_id;
	uint8_t num_samples;
	uint16_t sample_count;
	uint16_t temperature;
	uint32_t sample_timestamp;
	struct rift_dk2_sample_pack samples[DK2_MAX_SAMPLES];
	union {
		struct rift_dk2_version_data dk2;
		struct rift_cv1_version_data cv1;
	};
	uint16_t frame_count;
	uint32_t frame_timestamp;
	uint8_t frame_id;
	uint8_t tracking_pattern;
	uint16_t tracking_count;
	uint32_t tracking_timestamp;
};

SIZE_ASSERT(struct dk2_in_report, 63);

struct rift_enable_components_report
{
	uint16_t command_id;
	// which components to enable, see rift_component_flags
	uint8_t flags;
};

SIZE_ASSERT(struct rift_enable_components_report, 3);

struct rift_imu_calibration_report
{
	uint16_t command_id;
	struct rift_dk2_sample_pack offset;
	struct rift_dk2_sample_pack matrix_samples[3];
	uint16_t temperature;
};

SIZE_ASSERT(struct rift_imu_calibration_report, sizeof(struct rift_dk2_sample_pack) * 4 + 4);

enum rift_radio_read_cmd
{
	RIFT_RADIO_READ_CMD_FLASH_CONTROL = 0x0a,
	RIFT_RADIO_READ_CMD_SERIAL = 0x88,
};

struct rift_radio_cmd_report
{
	uint16_t command_id;
	uint8_t a;
	uint8_t b;
	uint8_t c;
};

SIZE_ASSERT(struct rift_radio_cmd_report, 5);

struct rift_radio_data_read_cmd
{
	uint16_t command_id;
	uint16_t offset;
	uint16_t length;
	uint8_t unk[28];
};

SIZE_ASSERT(struct rift_radio_data_read_cmd, 34);

struct rift_radio_flash_read_response_header
{
	uint8_t unk[5];
	__le16 data_length;
};
SIZE_ASSERT(struct rift_radio_flash_read_response_header, 7);

struct rift_radio_address_radio_report
{
	uint16_t command_id;
	uint8_t radio_address[5];
};

SIZE_ASSERT(struct rift_radio_address_radio_report, 7);

enum rift_radio_report_remote_button_masks
{
	RIFT_REMOTE_BUTTON_MASK_DPAD_UP = 0x001,
	RIFT_REMOTE_BUTTON_MASK_DPAD_DOWN = 0x002,
	RIFT_REMOTE_BUTTON_MASK_DPAD_LEFT = 0x004,
	RIFT_REMOTE_BUTTON_MASK_DPAD_RIGHT = 0x008,
	RIFT_REMOTE_BUTTON_MASK_SELECT = 0x010,
	RIFT_REMOTE_BUTTON_MASK_VOLUME_UP = 0x020,
	RIFT_REMOTE_BUTTON_MASK_VOLUME_DOWN = 0x040,
	RIFT_REMOTE_BUTTON_MASK_OCULUS = 0x080,
	RIFT_REMOTE_BUTTON_MASK_BACK = 0x100,
};

struct rift_radio_report_remote_message
{
	// the button state of the controller, see rift_radio_report_remote_button_masks
	uint16_t buttons;
};

SIZE_ASSERT(struct rift_radio_report_remote_message, 2);

enum rift_radio_report_touch_buttons
{
	RIFT_TOUCH_CONTROLLER_BUTTON_A = 0x01,
	RIFT_TOUCH_CONTROLLER_BUTTON_X = 0x01,
	RIFT_TOUCH_CONTROLLER_BUTTON_B = 0x02,
	RIFT_TOUCH_CONTROLLER_BUTTON_Y = 0x02,
	RIFT_TOUCH_CONTROLLER_BUTTON_MENU = 0x04,
	RIFT_TOUCH_CONTROLLER_BUTTON_OCULUS = 0x04,
	RIFT_TOUCH_CONTROLLER_BUTTON_STICK = 0x08,
};

enum rift_radio_report_adc_channel
{
	RIFT_TOUCH_CONTROLLER_ADC_STICK = 0x01,
	RIFT_TOUCH_CONTROLLER_ADC_B_Y = 0x02,
	RIFT_TOUCH_CONTROLLER_ADC_TRIGGER = 0x03,
	RIFT_TOUCH_CONTROLLER_ADC_A_X = 0x04,
	RIFT_TOUCH_CONTROLLER_ADC_THUMBREST = 0x08,
	// seen with values varying per controller, maybe power draw? temperature? my left controller while powered on
	// had the value slowly rise from 2800 to 3000 over the span of a couple minutes, dunno what that could be tbh
	RIFT_TOUCH_CONTROLLER_ADC_UNK1 = 0x20,
	RIFT_TOUCH_CONTROLLER_ADC_BATTERY = 0x21,
	RIFT_TOUCH_CONTROLLER_ADC_HAPTIC_COUNTER = 0x23,
};

struct rift_radio_report_touch_message
{
	uint32_t timestamp;
	int16_t accel[3];
	int16_t gyro[3];
	uint8_t buttons;
	uint8_t touch_grip_stick_state[5];
	// see rift_radio_report_adc_channel
	uint8_t adc_channel;
	uint16_t adc_value;
};

SIZE_ASSERT(struct rift_radio_report_touch_message, 25);

enum rift_radio_device_type
{
	RIFT_RADIO_DEVICE_REMOTE = 1,
	RIFT_RADIO_DEVICE_LEFT_TOUCH = 2,
	RIFT_RADIO_DEVICE_RIGHT_TOUCH = 3,
	RIFT_RADIO_DEVICE_TRACKED_OBJECT = 6,
};

struct rift_radio_report_message
{
	uint16_t flags;
	// the type of device sending the message, see rift_radio_device_type
	uint8_t device_type;
	union {
		struct rift_radio_report_remote_message remote;
		struct rift_radio_report_touch_message touch;
	};
};

SIZE_ASSERT(struct rift_radio_report_message, 3 + sizeof(struct rift_radio_report_touch_message));

struct rift_radio_report
{
	uint16_t command_id;
	struct rift_radio_report_message messages[2];
};

#pragma pack(pop)

/*
 *
 * Parsed structs for internal use
 *
 */

struct rift_catmull_rom_distortion_data
{
	// the k coeffecients of the distortion
	float k[CATMULL_COEFFICIENTS];
	float max_r;
	float meters_per_tan_angle_at_center;
	float chromatic_abberation[CHROMATIC_ABBERATION_COEFFEICENT_COUNT];
};

struct rift_lens_distortion
{
	// the version of the lens distortion data
	uint16_t distortion_version;
	// eye relief setting, in meters from surface of lens
	float eye_relief;

	union {
		struct rift_catmull_rom_distortion_data lcsv_catmull_rom_10;
	} data;
};

struct rift_scale_and_offset
{
	struct xrt_vec2 scale;
	struct xrt_vec2 offset;
};

struct rift_viewport_fov_tan
{
	float up_tan;
	float down_tan;
	float left_tan;
	float right_tan;
};

struct rift_extra_display_info
{
	// gap left between the two eyes
	float screen_gap_meters;
	// the diameter of the lenses, may need to be extended to an array
	float lens_diameter_meters;
	// ipd of the headset
	float icd;

	// the fov of the headset
	struct rift_viewport_fov_tan fov;
	// mapping from tan-angle space to target NDC space
	struct rift_scale_and_offset eye_to_source_ndc;
	struct rift_scale_and_offset eye_to_source_uv;
};

struct rift_imu_calibration
{
	struct xrt_vec3 gyro_offset;
	struct xrt_vec3 accel_offset;
	struct xrt_matrix_3x3 gyro_matrix;
	struct xrt_matrix_3x3 accel_matrix;
	float temperature;
};

enum rift_touch_controller_input
{
	// left
	RIFT_TOUCH_CONTROLLER_INPUT_X_CLICK = 0,
	RIFT_TOUCH_CONTROLLER_INPUT_X_TOUCH = 1,
	RIFT_TOUCH_CONTROLLER_INPUT_Y_CLICK = 2,
	RIFT_TOUCH_CONTROLLER_INPUT_Y_TOUCH = 3,
	RIFT_TOUCH_CONTROLLER_INPUT_SYSTEM_CLICK = 4,
	// right
	RIFT_TOUCH_CONTROLLER_INPUT_A_CLICK = 0,
	RIFT_TOUCH_CONTROLLER_INPUT_A_TOUCH = 1,
	RIFT_TOUCH_CONTROLLER_INPUT_B_CLICK = 2,
	RIFT_TOUCH_CONTROLLER_INPUT_B_TOUCH = 3,
	RIFT_TOUCH_CONTROLLER_INPUT_MENU_CLICK = 4,
	// both
	RIFT_TOUCH_CONTROLLER_INPUT_SQUEEZE_VALUE = 5,
	RIFT_TOUCH_CONTROLLER_INPUT_TRIGGER_TOUCH = 6,
	RIFT_TOUCH_CONTROLLER_INPUT_TRIGGER_VALUE = 7,
	RIFT_TOUCH_CONTROLLER_INPUT_THUMBSTICK_CLICK = 8,
	RIFT_TOUCH_CONTROLLER_INPUT_THUMBSTICK_TOUCH = 9,
	RIFT_TOUCH_CONTROLLER_INPUT_THUMBSTICK = 10,
	RIFT_TOUCH_CONTROLLER_INPUT_THUMBREST_TOUCH = 11,
	RIFT_TOUCH_CONTROLLER_INPUT_GRIP_POSE = 12,
	RIFT_TOUCH_CONTROLLER_INPUT_AIM_POSE = 13,
	RIFT_TOUCH_CONTROLLER_INPUT_TRIGGER_PROXIMITY = 14,
	RIFT_TOUCH_CONTROLLER_INPUT_THUMB_PROXIMITY = 15,
	RIFT_TOUCH_CONTROLLER_INPUT_COUNT = 16,
};

struct rift_touch_controller_led
{
	struct xrt_vec3 position;
	struct xrt_vec3 normal;
	struct xrt_vec3 angles;
};

struct rift_touch_controller_calibration
{
	uint16_t joy_x_range[2];
	uint16_t joy_x_dead[2];
	uint16_t joy_y_range[2];
	uint16_t joy_y_dead[2];

	// min - mid - max
	uint16_t trigger_range[3];

	// min - mid - max
	uint16_t middle_range[3];
	bool middle_flipped;

	uint16_t cap_sense_min[8];
	uint16_t cap_sense_touch[8];

	float gyro_calibration[3][3];
	struct xrt_vec3 gyro_offset;
	float accel_calibration[3][3];
	struct xrt_vec3 accel_offset;

	struct xrt_vec3 imu_position;

	size_t num_leds;
	struct rift_touch_controller_led *leds;
};

struct rift_touch_controller_input_state
{
	uint8_t buttons;
	float trigger;
	float grip;
	struct xrt_vec2 stick;
	uint8_t haptic_counter;
	float cap_stick;
	float cap_b_y;
	float cap_a_x;
	float cap_trigger;
	float cap_thumbrest;
};

/*!
 * A Rift Touch controller device.
 *
 * @implements xrt_device
 */
struct rift_touch_controller
{
	struct xrt_device base;

	struct rift_hmd *hmd;

	enum rift_radio_device_type device_type;

	struct
	{
		bool mutex_created;
		struct os_mutex mutex;

		struct rift_touch_controller_input_state state;

		xrt_atomic_s32_t battery_status;

		uint32_t last_device_remote_us;
		timepoint_ns device_remote_ns;
		timepoint_ns device_local_ns;

		struct imu_fusion *imu_fusion;
		struct xrt_imu_sample last_imu_sample;

		struct m_clock_windowed_skew_tracker *clock_tracker;

		bool calibration_read;
		struct rift_touch_controller_calibration calibration;
	} input;

	//! Locked by radio_state.thread
	struct
	{
		bool serial_valid;

		uint8_t calibration_hash[CALIBRATION_HASH_BYTE_LENGTH];

		uint8_t calibration_data_buffer[CALIBRATION_BODY_BYTE_CHUNK_LENGTH];

		uint8_t *calibration_body_json;
		uint16_t calibration_body_json_length;

	} radio_data;
};

enum rift_remote_inputs
{
	RIFT_REMOTE_INPUT_DPAD_UP,
	RIFT_REMOTE_INPUT_DPAD_DOWN,
	RIFT_REMOTE_INPUT_DPAD_LEFT,
	RIFT_REMOTE_INPUT_DPAD_RIGHT,
	RIFT_REMOTE_INPUT_SELECT,
	RIFT_REMOTE_INPUT_VOLUME_UP,
	RIFT_REMOTE_INPUT_VOLUME_DOWN,
	RIFT_REMOTE_INPUT_BACK,
	RIFT_REMOTE_INPUT_OCULUS,
	RIFT_REMOTE_INPUT_COUNT,
};

/*!
 * A Rift Remote device.
 *
 * @implements xrt_device
 */
struct rift_remote
{
	struct xrt_device base;

	//! The button state of the remote, stored as an atomic to avoid needing a mutex.
	xrt_atomic_s32_t buttons;

	//! Locked by radio_state.thread
	bool serial_valid;
};

enum rift_radio_command
{
	RIFT_RADIO_COMMAND_NONE = 0,
	RIFT_RADIO_COMMAND_READ_SERIAL,
	RIFT_RADIO_COMMAND_READ_FLASH,
};

struct rift_radio_command_data_read_serial
{
	//! A pointer to store the serial string. Must contain at least SERIAL_NUMBER_LENGTH bytes.
	char *serial;
	//! A pointer to store when reading the serial was successful.
	bool *serial_valid;
};

typedef int (*flash_read_callback_t)(void *user_data, uint16_t address, uint16_t length);

struct rift_radio_command_data_read_flash
{
	void *user_data;

	uint16_t address;
	uint16_t length;

	uint8_t *buffer;

	flash_read_callback_t read_callback;
};

union rift_radio_command_data {
	struct rift_radio_command_data_read_serial read_serial;
	struct rift_radio_command_data_read_flash read_flash;
};

/*!
 * A rift HMD device.
 *
 * @implements xrt_device
 */
struct rift_hmd
{
	struct xrt_device base;

	enum u_logging_level log_level;

	// has built-in mutex so thread safe
	struct m_relation_history *relation_hist;

	struct os_hid_device *hmd_dev;
	struct os_hid_device *radio_dev;

	struct os_thread_helper sensor_thread;
	bool processed_sample_packet;
	uint32_t last_remote_sample_time_us;
	timepoint_ns last_remote_sample_time_ns;
	timepoint_ns last_sample_local_timestamp_ns;

	struct m_imu_3dof fusion;
	struct m_clock_windowed_skew_tracker *clock_tracker;

	timepoint_ns last_keepalive_time;
	enum rift_variant variant;
	struct rift_config_report config;
	struct rift_display_info_report display_info;

	const struct rift_lens_distortion *lens_distortions;
	uint16_t num_lens_distortions;
	uint16_t distortion_in_use;

	struct rift_extra_display_info extra_display_info;
	float icd_override_m;

	bool presence;

	bool imu_needs_calibration;
	struct rift_imu_calibration imu_calibration;

	uint8_t radio_address[5];

	//! Mutex to protect access to the device array, device count == -1 means uninitialized
	struct os_mutex device_mutex;

	int device_count;
	int added_devices;
	struct xrt_device *devices[4]; // left touch, right touch, tracked object, remote

	//! Generic state for the radio state machine
	struct
	{
		struct os_thread_helper thread;

		struct rift_touch_controller *touch_controllers[3];
		struct rift_remote *remote;

		enum rift_radio_command current_command;
		union rift_radio_command_data command_data;
	} radio_state;
};

/// Casting helper function
static inline struct rift_hmd *
rift_hmd(struct xrt_device *xdev)
{
	return (struct rift_hmd *)xdev;
}

static inline struct rift_touch_controller *
rift_touch_controller(struct xrt_device *xdev)
{
	return (struct rift_touch_controller *)xdev;
}

static inline struct rift_remote *
rift_remote(struct xrt_device *xdev)
{
	return (struct rift_remote *)xdev;
}

static inline size_t
rift_radio_device_type_to_touch_index(enum rift_radio_device_type device_type)
{
	switch (device_type) {
	case RIFT_RADIO_DEVICE_LEFT_TOUCH: return 0;
	case RIFT_RADIO_DEVICE_RIGHT_TOUCH: return 1;
	case RIFT_RADIO_DEVICE_TRACKED_OBJECT: return 2;
	default: assert(false);
	}

	return -1;
}

static inline enum rift_radio_device_type
rift_radio_touch_index_to_device_type(size_t index)
{
	switch (index) {
	case 0: return RIFT_RADIO_DEVICE_LEFT_TOUCH;
	case 1: return RIFT_RADIO_DEVICE_RIGHT_TOUCH;
	case 2: return RIFT_RADIO_DEVICE_TRACKED_OBJECT;
	default: assert(false);
	}

	return (enum rift_radio_device_type)0;
}

static inline float
rift_min_mid_max_cap(struct rift_touch_controller_calibration *calibration, size_t index, float value)
{
	return (value - calibration->cap_sense_min[index]) /
	       (calibration->cap_sense_touch[index] - calibration->cap_sense_min[index]);
}

static inline float
rift_min_mid_max_range_to_float(uint16_t range[3], uint16_t value)
{
	if (value < range[1]) {
		return 1.0f - ((float)value - range[0]) / (range[1] - range[0]) * 0.5f;
	} else {
		return 0.5f - ((float)value - range[1]) / (range[2] - range[1]) * 0.5f;
	}
}

bool
rift_touch_calibration_parse(const char *calibration_data,
                             size_t calibration_size,
                             struct rift_touch_controller_calibration *out_calibration);

#ifdef __cplusplus
} // extern "C"
#endif
