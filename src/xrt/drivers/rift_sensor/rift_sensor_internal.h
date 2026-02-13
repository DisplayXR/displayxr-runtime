// Copyright 2025-2026, Beyley Cardellio
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Internal structures for Oculus Rift sensor probing/initialization
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @ingroup drv_rift_sensor
 */

#include "os/os_threading.h"

#include "rift_sensor_interface.h"

#include <libusb.h>


struct rift_sensor_context
{
	enum u_logging_level log_level;

	struct libusb_context *usb_ctx;
	struct xrt_frame_context *xfctx;

	struct rift_sensor *sensors;
	size_t num_sensors;

	struct os_thread_helper usb_thread;
};

struct rift_sensor
{
	enum rift_sensor_variant variant;
	struct xrt_fs *frame_server;
	struct t_camera_calibration calibration;
	struct libusb_device_handle *hid_dev;
	bool usb2;

	time_duration_ns frame_interval;
};

#define SIZE_ASSERT(type, size)                                                                                        \
	static_assert(sizeof(type) == (size), "Size of " #type " is not " #size " bytes as was expected")

#pragma pack(push, 1)

struct rift_sensor_dk2_calib
{
	uint8_t unk1[18]; // 00
	__lef64 fx;       // 18
	uint8_t unk2[4];  // 26
	__lef64 fy;       // 30
	uint8_t unk3[4];  // 38
	__lef64 cx;       // 42
	uint8_t unk4[4];  // 50
	__lef64 cy;       // 54
	uint8_t unk5[4];  // 62
	__lef64 k1;       // 66
	uint8_t unk6[4];  // 74
	__lef64 k2;       // 78
	uint8_t unk7[4];  // 86
	__lef64 p1;       // 90
	uint8_t unk8[4];  // 98
	__lef64 p2;       // 102
	uint8_t unk9[4];  // 110
	__lef64 k3;       // 114
	uint8_t pad[6];   // 122
};
SIZE_ASSERT(struct rift_sensor_dk2_calib, 128);

struct rift_sensor_cv1_calib
{
	uint8_t unk1[0x30];      // 0x00
	__lef32 fxy;             // 0x30
	__lef32 cx;              // 0x34
	__lef32 cy;              // 0x38
	uint8_t unk2[0xC];       // 0x3c
	__lef32 k1;              // 0x48
	__lef32 k2;              // 0x4c
	__lef32 k3;              // 0x50
	__lef32 k4;              // 0x54
	uint8_t pad[128 - 0x58]; // 0x58
};
SIZE_ASSERT(struct rift_sensor_cv1_calib, 128);

#pragma pack(pop)
