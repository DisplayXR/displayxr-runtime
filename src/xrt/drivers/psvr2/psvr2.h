// Copyright 2020-2021, Collabora, Ltd.
// Copyright 2023, Jan Schmidt
// Copyright 2024, Joel Valenciano
// Copyright 2025, Beyley Cardellio
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  PSVR2 HMD device
 *
 * @author Jan Schmidt <jan@centricular.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Ryan Pavlik <ryan.pavlik@collabora.com>
 * @author Joel Valenciano <joelv1907@gmail.com>
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @ingroup drv_psvr2
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <xrt/xrt_defines.h>
#include <xrt/xrt_byte_order.h>
#include "xrt/xrt_device.h"
#include "xrt/xrt_prober.h"
#include "xrt/xrt_tracking.h"

#include "os/os_threading.h"
#include "os/os_time.h"

#include "math/m_api.h"
#include "math/m_clock_tracking.h"
#include "math/m_mathinclude.h"
#include "math/m_relation_history.h"
#include "math/m_filter_one_euro.h"
#include "math/m_filter_fifo.h"

#include "util/u_misc.h"
#include "util/u_debug.h"
#include "util/u_device.h"
#include "util/u_distortion_mesh.h"
#include "util/u_frame.h"
#include "util/u_logging.h"
#include "util/u_sink.h"
#include "util/u_time.h"
#include "util/u_trace_marker.h"
#include "util/u_var.h"
#include "util/u_debug.h"

#include "psvr2_protocol.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <libusb.h>


#define NUM_CAM_XFERS 1

#define PSVR2_TRACE(p, ...) U_LOG_XDEV_IFL_T(&p->base, p->log_level, __VA_ARGS__)
#define PSVR2_TRACE_HEX(p, data, data_size) U_LOG_XDEV_IFL_T_HEX(&p->base, p->log_level, data, data_size)
#define PSVR2_DEBUG(p, ...) U_LOG_XDEV_IFL_D(&p->base, p->log_level, __VA_ARGS__)
#define PSVR2_DEBUG_HEX(p, data, data_size) U_LOG_XDEV_IFL_D_HEX(&p->base, p->log_level, data, data_size)
#define PSVR2_WARN(p, ...) U_LOG_XDEV_IFL_W(&p->base, p->log_level, __VA_ARGS__)
#define PSVR2_ERROR(p, ...) U_LOG_XDEV_IFL_E(&p->base, p->log_level, __VA_ARGS__)

#define TIMESTAMP_SAMPLES 100

/*!
 * PSVR2 HMD device
 *
 * @implements xrt_device
 */
struct psvr2_hmd
{
	struct xrt_device base;

	struct xrt_pose pose;

	enum u_logging_level log_level;

	struct os_mutex data_lock;

	/* Device status */
	uint8_t dprx_status;               //< DisplayPort receiver status
	xrt_atomic_s32_t proximity_sensor; //< Atomic state for whether the proximity sensor is triggered
	bool function_button;              //< Boolean state for whether the function button is pressed

	bool ipd_updated; //< Whether the IPD has been updated, and an HMD info refresh is needed
	uint8_t ipd_mm;   //< IPD dial value in mm, from 59 to 72mm

	bool camera_enable;                 //< Whether the camera is enabled
	enum psvr2_camera_mode camera_mode; //< The current camera mode
	struct u_var_button camera_enable_btn;
	struct u_var_button camera_mode_btn;

	struct u_var_button brightness_btn;
	float brightness;

	/* IMU input data */
	uint32_t last_imu_vts_us;   //< Last VTS timestamp, in microseconds
	uint16_t last_imu_ts;       //< Last IMU timestamp, in microseconds
	struct xrt_vec3 last_gyro;  //< Last gyro reading, in rad/s
	struct xrt_vec3 last_accel; //< Last accel reading, in m/s²

	/* SLAM input data */
	uint32_t last_slam_vts_us;      //< Last slam timestamp, in microseconds
	struct xrt_pose last_slam_pose; //< Last SLAM pose reading

	struct xrt_pose slam_correction_pose;
	struct u_var_button slam_correction_set_btn;
	struct u_var_button slam_correction_reset_btn;

	struct xrt_pose T_imu_head; //< Constant transform from SLAM tracker pose to head pose

	/* Display parameters */
	struct u_device_simple_info info;

	/* Camera debug sinks */
	struct u_sink_debug debug_sinks[4];

	/* USB communication */
	libusb_context *ctx;
	libusb_device_handle *dev;

	struct os_thread_helper usb_thread;
	int usb_complete;
	int usb_active_xfers;

	/* Status report */
	struct libusb_transfer *status_xfer;
	/* SLAM (bulk) transfer */
	struct libusb_transfer *slam_xfer;
	/* Camera (bulk) transfers */
	struct libusb_transfer *camera_xfers[NUM_CAM_XFERS];
	/* LD EP9 (bulk) transfer */
	struct libusb_transfer *led_detector_xfer;
	/* RP EP10 (bulk) transfer */
	struct libusb_transfer *relocalizer_xfer;
	/* VD EP11 (bulk) transfer */
	struct libusb_transfer *vd_xfer;
	/* Gaze transfer */
	struct libusb_transfer *gaze_xfer;

	/* Distortion calibration parameters, to be used with
	 * psvr2_compute_distortion_asymmetric. Very specific to
	 * PS VR2. */
	float distortion_calibration[8];

	/* Timing data */
	int timestamp_samples;

	timepoint_ns last_imu_vts_ns;
	timepoint_ns last_slam_vts_ns;
	timepoint_ns system_zero_ns;
	timepoint_ns last_imu_ns;

	time_duration_ns hw2mono_vts;
	time_duration_ns hw2mono_imu;

	/* Tracking state */
	struct m_relation_history *slam_relation_history;
	struct m_ff_vec3_f32 *ff_gyro;
};

/// Casting helper function
static inline struct psvr2_hmd *
psvr2_hmd(struct xrt_device *xdev)
{
	return (struct psvr2_hmd *)xdev;
}

enum psvr2_hmd_input_name
{
	PSVR2_HMD_INPUT_HEAD_POSE,
	PSVR2_HMD_INPUT_FUNCTION_BUTTON,
	PSVR2_HMD_INPUT_COUNT,
};

void
psvr2_compute_distortion_asymmetric(
    float *calibration, struct xrt_uv_triplet *distCoords, int eEye, float fU, float fV);

bool
psvr2_usb_xfer_continue(struct libusb_transfer *xfer, const char *type);

bool
send_psvr2_control(struct psvr2_hmd *hmd, uint16_t report_id, uint8_t subcmd, uint8_t *pkt_data, uint32_t pkt_len);

#ifdef __cplusplus
}
#endif
