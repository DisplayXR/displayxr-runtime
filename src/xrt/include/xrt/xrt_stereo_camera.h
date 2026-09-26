// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_stereo_camera (ADR-043) runtime-side value types and the
 *         client aspect of @ref xrt_instance.
 *
 * The value structs are plain fixed-size POD: they ride the IPC wire as-is
 * (proto.json) and are translated 1:1 to the XR structs by the state tracker.
 * Enum values equal the XR ones (static-asserted in oxr_stereo_camera.c).
 *
 * @ingroup xrt_iface
 */

#pragma once

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_handles.h"
#include "xrt/xrt_results.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Most cameras the service exposes (per plug-in).
#define XRT_STEREO_CAMERA_MAX_CAMERAS 4
//! Streams per camera, service-wide (spec §8: XR_ERROR_LIMIT_REACHED past it).
#define XRT_STEREO_CAMERA_MAX_STREAMS_PER_CAMERA 8
//! Slots in a stream's ring: latest + pinned + one to write.
#define XRT_STEREO_CAMERA_RING_SLOTS 3

enum xrt_stereo_camera_state
{
	XRT_STEREO_CAMERA_STATE_AVAILABLE = 1,
	XRT_STEREO_CAMERA_STATE_WAITING = 2,
	XRT_STEREO_CAMERA_STATE_SUSPENDED = 3,
	XRT_STEREO_CAMERA_STATE_UNAVAILABLE = 4,
};

enum xrt_stereo_camera_output
{
	XRT_STEREO_CAMERA_OUTPUT_RECTIFIED = 1,
	XRT_STEREO_CAMERA_OUTPUT_RAW = 2,
};

//! Same values as xrt_plugin_stereo_camera_format and XrStereoCameraFormatDXR.
enum xrt_stereo_camera_format
{
	XRT_STEREO_CAMERA_FORMAT_GRAY8 = 1,
	XRT_STEREO_CAMERA_FORMAT_NV12 = 2,
	XRT_STEREO_CAMERA_FORMAT_BGRA8 = 3,
};

enum xrt_stereo_camera_transport
{
	XRT_STEREO_CAMERA_TRANSPORT_SHARED_MEMORY = 1,
	XRT_STEREO_CAMERA_TRANSPORT_D3D11_TEXTURE = 2,
	XRT_STEREO_CAMERA_TRANSPORT_AHARDWAREBUFFER = 3,
};

//! Format / transport sets are one bit per enum value: (1 << value).
#define XRT_STEREO_CAMERA_BIT(v) (1ull << (uint32_t)(v))

/*!
 * One enumerated camera (XrStereoCameraPropertiesDXR).
 */
struct xrt_stereo_camera_properties
{
	uint64_t camera_id;
	char persistent_id[64];
	char display_name[128];
	char platform_device_hint[256];
	uint64_t flags; //!< XRT_PLUGIN_STEREO_CAMERA_* bits (= XrStereoCameraFlagsDXR)
	uint32_t state; //!< enum xrt_stereo_camera_state
	uint32_t view_count;
	uint32_t eye_width;
	uint32_t eye_height;
	float max_frame_rate;
	float baseline_mm;
	float horizontal_fov_deg;
	uint32_t reserved;
	uint64_t supported_formats;    //!< XRT_STEREO_CAMERA_BIT(format)
	uint64_t supported_transports; //!< XRT_STEREO_CAMERA_BIT(transport)
};

struct xrt_stereo_camera_intrinsics
{
	uint32_t width;
	uint32_t height;
	float fx, fy, cx, cy;
	uint32_t model; //!< xrt_plugin_stereo_camera_distortion
	float coefficients[8];
};

/*!
 * Calibration as a client sees it (XrStereoCameraCalibrationDXR).
 */
struct xrt_stereo_camera_calibration
{
	uint32_t output; //!< enum xrt_stereo_camera_output these numbers describe
	uint32_t reserved;
	struct xrt_stereo_camera_intrinsics eye[2];
	float orientation[4]; //!< right-from-left rotation, quaternion x y z w
	float position[3];    //!< right-from-left translation, metres
	float baseline_mm;
};

/*!
 * What a client asks for at stream create.
 */
struct xrt_stereo_camera_stream_request
{
	uint64_t camera_id;
	uint32_t output;    //!< enum xrt_stereo_camera_output
	uint32_t format;    //!< enum xrt_stereo_camera_format
	uint32_t transport; //!< enum xrt_stereo_camera_transport
	float max_frame_rate;
};

/*!
 * A started stream's layout (XrStereoCameraStreamInfoDXR + transport).
 */
struct xrt_stereo_camera_stream_layout
{
	uint32_t width;  //!< full SBS
	uint32_t height;
	uint32_t format;
	uint32_t output;
	uint32_t transport;
	float max_frame_rate; //!< effective delivery cap
	uint64_t section_size;
	uint64_t slot_stride;
	uint32_t slot_count;
	uint32_t reserved;
};

/*!
 * One acquired frame (XrStereoCameraFrameDXR). @ref capture_time_ns is on the
 * os_monotonic clock; the state tracker converts it to XrTime.
 */
struct xrt_stereo_camera_frame_info
{
	uint64_t frame_index;
	uint32_t slot;
	uint32_t width;
	uint32_t height;
	uint32_t format;
	uint32_t row_pitch[2];
	uint64_t plane_offset[2];
	int64_t capture_time_ns;
	uint32_t time_is_exposure;
	uint32_t output;
	uint32_t calibration_generation;
	uint32_t reserved;
};

struct xrt_stereo_camera_stream_stats
{
	float source_frame_rate;
	float delivered_frame_rate;
	uint64_t frames_published;
	uint64_t frames_skipped;
	uint64_t frames_acquired;
	uint64_t mean_latency_ns;
};

/*!
 * Client aspect of an @ref xrt_instance: set by the IPC client instance only
 * (the camera's single owner is the service); NULL for an in-process instance,
 * which therefore enumerates zero cameras. Streams are service-side ids owned
 * by the instance's connection.
 */
struct xrt_stereo_camera_client
{
	xrt_result_t (*count)(struct xrt_stereo_camera_client *c, uint32_t *out_count);
	xrt_result_t (*get_properties)(struct xrt_stereo_camera_client *c,
	                               uint32_t index,
	                               struct xrt_stereo_camera_properties *out_props);
	xrt_result_t (*get_calibration)(struct xrt_stereo_camera_client *c,
	                                uint64_t camera_id,
	                                uint32_t output,
	                                struct xrt_stereo_camera_calibration *out_calib);
	xrt_result_t (*stream_create)(struct xrt_stereo_camera_client *c,
	                              const struct xrt_stereo_camera_stream_request *req,
	                              uint64_t *out_stream_id);
	xrt_result_t (*stream_destroy)(struct xrt_stereo_camera_client *c, uint64_t stream_id);
	xrt_result_t (*stream_start)(struct xrt_stereo_camera_client *c, uint64_t stream_id);
	xrt_result_t (*stream_stop)(struct xrt_stereo_camera_client *c, uint64_t stream_id);
	//! @p out_section / @p out_wake are NEW handles owned by the caller.
	xrt_result_t (*stream_get_transport)(struct xrt_stereo_camera_client *c,
	                                     uint64_t stream_id,
	                                     struct xrt_stereo_camera_stream_layout *out_layout,
	                                     xrt_shmem_handle_t *out_section,
	                                     xrt_graphics_sync_handle_t *out_wake);
	//! @p out_ready false = nothing newer than the last acquire.
	xrt_result_t (*acquire)(struct xrt_stereo_camera_client *c,
	                        uint64_t stream_id,
	                        bool *out_ready,
	                        struct xrt_stereo_camera_frame_info *out_frame);
	xrt_result_t (*stats)(struct xrt_stereo_camera_client *c,
	                      uint64_t stream_id,
	                      struct xrt_stereo_camera_stream_stats *out_stats);
};

#ifdef __cplusplus
}
#endif
