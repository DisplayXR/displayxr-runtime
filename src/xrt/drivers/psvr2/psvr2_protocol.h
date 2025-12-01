// Copyright 2023, Jan Schmidt
// Copyright 2024, Joel Valenciano
// Copyright 2025, Beyley Cardellio
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  PSVR2 HMD protocol defines
 *
 * @author Jan Schmidt <jan@centricular.com>
 * @author Joel Valenciano <joelv1907@gmail.com>
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @ingroup drv_psvr2
 */
#pragma once


#define PSVR2_SLAM_INTERFACE 3
#define PSVR2_SLAM_ENDPOINT 3

#define PSVR2_GAZE_INTERFACE 5
#define PSVR2_GAZE_ENDPOINT 0x85

#define PSVR2_CAMERA_INTERFACE 6
#define PSVR2_CAMERA_ENDPOINT 7

#define PSVR2_STATUS_INTERFACE 7
#define PSVR2_STATUS_ENDPOINT 8

#define PSVR2_LD_INTERFACE 8
#define PSVR2_LD_ENDPOINT 9

#define PSVR2_RP_INTERFACE 9
#define PSVR2_RP_ENDPOINT 10

#define PSVR2_VD_INTERFACE 10
#define PSVR2_VD_ENDPOINT 11

#define USB_SLAM_XFER_SIZE 1024
#define USB_STATUS_XFER_SIZE 1024
#define USB_GAZE_XFER_SIZE 32768
#define USB_CAM_MODE10_XFER_SIZE 1040640
#define USB_CAM_MODE1_XFER_SIZE 819456
#define USB_LD_XFER_SIZE 36944
#define USB_RP_XFER_SIZE 821120
#define USB_VD_XFER_SIZE 32768

#define SERIAL_LENGTH 14

#define GYRO_SCALE (2000.0 / 32767.0)
#define ACCEL_SCALE (4.0 * MATH_GRAVITY_M_S2 / 32767.0)

#define IMU_FREQ 2000.0f
#define IMU_PERIOD_NS ((time_duration_ns)(1000000000.0f / IMU_FREQ))

enum psvr2_report_id
{
	PSVR2_REPORT_ID_SET_PERIPHERAL = 0x8,
	PSVR2_REPORT_ID_SET_CAMERA_MODE = 0xB,
	PSVR2_REPORT_ID_SET_GAZE_STREAM = 0xC,
	PSVR2_REPORT_ID_SET_GAZE_USER_CALIBRATION = 0xD,
	PSVR2_REPORT_ID_SET_BRIGHTNESS = 0x12,
};

enum psvr2_gaze_stream_subcommand
{
	PSVR2_GAZE_STREAM_SUBCMD_ENABLE = 0x01,
	PSVR2_GAZE_STREAM_SUBCMD_DISABLE = 0x02,
};

enum psvr2_set_peripheral_subcommand
{
	PSVR2_SET_PERIPHERAL_SUBCMD_MOTOR = 0x01,
};

struct imu_record
{
	uint32_t vts_us;
	int16_t accel[3];
	int16_t gyro[3];
	uint16_t dp_frame_cnt;
	uint16_t dp_line_cnt;
	uint16_t imu_ts_us;
	uint16_t status;
};

struct imu_usb_record
{
	__le32 vts_us;
	__le16 accel[3];
	__le16 gyro[3];
	__le16 dp_frame_cnt;
	__le16 dp_line_cnt;
	__le16 imu_ts_us;
	__le16 status;
} __attribute__((packed));

struct status_record_hdr
{
	uint8_t dprx_status;      //< 0 = not ready. 2 = cinematic? and 1 = unknown. HDCP? Other?
	uint8_t prox_sensor_flag; //< 0 = not triggered. 1 = triggered?
	uint8_t function_button;  //< 0 = not pressed, 1 = pressed
	uint8_t empty0[2];
	uint8_t ipd_dial_mm; //< 59 to 72mm

	uint8_t remainder[26];
} __attribute__((packed));

struct slam_usb_record
{
	char SLAhdr[3];    //< "SLA"
	uint8_t const1;    //< Constant 0x01?
	__le32 pkt_size;   //< 0x0200 = 512 bytes;
	__le32 vts_ts_us;  //< Timestamp
	__le32 unknown1;   //< Unknown. Constant 3?
	__lef32 pos[3];    //< 32-bit floats
	__lef32 orient[4]; //< Orientation quaternion
	uint8_t remainder[468];
} __attribute__((packed));

struct sie_ctrl_pkt
{
	__le16 report_id;
	__le16 subcmd;
	__le32 len;
	uint8_t data[512 - 8];
} __attribute__((packed));

enum psvr2_camera_mode
{
	PSVR2_CAMERA_MODE_OFF = 0,
	// 819456 byte 640x640x2 SBS bottom cameras
	PSVR2_CAMERA_MODE_BOTTOM_SBS_CROPPED = 1,
	// 819456 byte 640x640x2 SBS (mode 1) + 409856 byte 640x640 frames (from top cameras alternately) interleaved
	PSVR2_CAMERA_MODE_2 = 2,
	// 819456 byte 640x640x2 SBS interleaved bottom and top camera paired images
	PSVR2_CAMERA_MODE_3 = 3,
	// 520448 byte 512x508x2 Top-Bottom fisheye, *Controller Tracking* interleaved top and bottom camera pairs
	PSVR2_CAMERA_MODE_4 = 4,
	// 80256 byte 400x200 nearly black (no value higher than 0x0f)
	PSVR2_CAMERA_MODE_400_200_DARK = 5,
	// no packets / off
	PSVR2_CAMERA_MODE_EYE_CAMERAS = 6,
	// 819456 byte 640x640x2 + 520448 byte 512x508x2 fisheye *Controller Tracking* alternating bottom camera
	PSVR2_CAMERA_MODE_7 = 7,
	// 819456 byte 640x640x2 SBS bottom cameras + 80256 byte 400x200 nearly black packets like mode 5
	PSVR2_CAMERA_MODE_8 = 8,
	// interleaved mode 2 + mode 5 packets
	PSVR2_CAMERA_MODE_9 = 9,
	// 640x640x2 SBS interleaved bottom and top cameras + 80256 byte mode 5 packets
	PSVR2_CAMERA_MODE_10 = 0xa,
	// Mode 4 + Mode 5 packets interleaved
	PSVR2_CAMERA_MODE_11 = 0xb,
	// 409856 byte 320x640x4 vertical stack of all 4 cameras + 260352 byte 256x254x4 vertical stack fisheye
	// controller-tracking all-4-cameras packets interleaved
	PSVR2_CAMERA_MODE_12 = 0xc,
	// mode 1, but upside down
	PSVR2_CAMERA_MODE_13 = 0xd,
	// mode 1 upside down + mode 4 bottom cameras only
	PSVR2_CAMERA_MODE_14 = 0xe,
	// mode 0xc + mode 5
	PSVR2_CAMERA_MODE_15 = 0xf,
	// 1024x1024x2 BC4 compressed images (really 1000x1000, with 24 padding pixels you can ignore on the
	// right/bottom)
	PSVR2_CAMERA_MODE_BOTTOM_SBS_BC4 = 0x10,
};
