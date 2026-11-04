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
//! xrt_stereo_camera_properties::frame_orientation_deg when not known
//! (= XR_STEREO_CAMERA_FRAME_ORIENTATION_UNKNOWN_DXR).
#define XRT_STEREO_CAMERA_FRAME_ORIENTATION_UNKNOWN 0xFFFFFFFFu

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

/*!
 * xrt_application_info::stereo_camera_client_flags — the same bit values as
 * XrStereoCameraClientFlagsDXR.
 */
//! XR_STEREO_CAMERA_CLIENT_CONSUMER_ONLY_BIT_DXR (mapped to the client class, not read from here).
#define XRT_STEREO_CAMERA_CLIENT_CONSUMER_ONLY (1ull << 0)
//! XR_STEREO_CAMERA_CLIENT_DECLINE_DELEGATION_BIT_DXR: skip consent delegation for this instance.
#define XRT_STEREO_CAMERA_CLIENT_DECLINE_DELEGATION (1ull << 1)
//! The public XrStereoCameraClientFlagsDXR bits occupy the low 32; the state
//! tracker passes only these through from the app's chained struct.
#define XRT_STEREO_CAMERA_CLIENT_PUBLIC_MASK 0xFFFFFFFFull
/*!
 * INTERNAL, never an XR bit (ADR-061 decision 3): the instance enabled
 * XR_DXR_app_share, so the service also lists running apps (APP_CONTENT
 * entries) to this connection. Set by the state tracker from the enabled
 * extension set, never from the app's flags. It only widens what is LISTED:
 * every app-share start still runs the share prompt, so a forged bit buys
 * nothing a consumer could not get by enabling the extension.
 */
#define XRT_STEREO_CAMERA_CLIENT_INTERNAL_APP_SHARE (1ull << 63)

/*!
 * XrStereoCameraFlagsDXR bit XR_STEREO_CAMERA_APP_CONTENT_BIT_DXR (0x40,
 * XR_DXR_app_share): the entry is a running app's pre-weave views, not a
 * camera. Set by the service only; a plug-in claiming it is ignored.
 */
#define XRT_STEREO_CAMERA_APP_CONTENT (1u << 6)

//! Format / transport sets are one bit per enum value: (1 << value).
#define XRT_STEREO_CAMERA_BIT(v) (1ull << (uint32_t)(v))

/*!
 * Where a camera's frames come from (diagnostics: `displayxr-cli camera
 * list`). The camera kinds are source-neutral in the OpenXR API; the app
 * kinds are not (ADR-061 decision 7): they surface as
 * XR_STEREO_CAMERA_APP_CONTENT_BIT_DXR, because a display-referred pair must
 * not be treated as a camera pair.
 */
enum xrt_stereo_camera_source
{
	//! The active display plug-in's stereo_camera_* slots (ADR-043).
	XRT_STEREO_CAMERA_SOURCE_PLUGIN = 0,
	//! The service's own vendor-neutral UVC side-by-side source (ADR-043
	//! Amendment 4): an opted-in plain stereo webcam.
	XRT_STEREO_CAMERA_SOURCE_UVC = 1,
	//! ADR-061: a SERVICE-COMPOSITED app's pre-weave views, tapped in the
	//! service compositor (or the service-side synthetic DXR_APP_SHARE_FAKE).
	XRT_STEREO_CAMERA_SOURCE_APP = 2,
	//! ADR-061 decision 9: an IN-PROCESS app's pre-weave views over its
	//! in-process service link (ipc_client_service_link, ADR-049 Amendment 1),
	//! imported through the platform's share_link_* hooks.
	XRT_STEREO_CAMERA_SOURCE_APP_LINK = 3,
};

/*!
 * Why an app entry cannot be shared right now (ADR-061 decision 9, spec §3 /
 * §9): untappable sessions are LISTED with a reason, never silently omitted.
 */
enum xrt_app_share_availability
{
	XRT_APP_SHARE_AVAILABLE = 0,
	//! No share tap on this platform / service build (macOS, Linux until P6).
	XRT_APP_SHARE_UNAVAILABLE_NO_TAP = 1,
	//! The app exited (listed for 10 s so a stream ends SOURCE_LOST visibly).
	XRT_APP_SHARE_UNAVAILABLE_EXITED = 2,
	//! The session has no projection compositor the tap can read.
	XRT_APP_SHARE_UNAVAILABLE_NO_COMPOSITOR = 3,
	//! The app's graphics API has no tap (OpenGL in-process).
	XRT_APP_SHARE_UNAVAILABLE_UNSUPPORTED_API = 4,
	//! A shared-texture session the fill arm declines (no DXR_HUB=1).
	XRT_APP_SHARE_UNAVAILABLE_SHARED_TEXTURE = 5,
	//! A kill-switched split (DXR_SPLIT_SAME_ADAPTER=0, DXR_WEAVE_ON_SCANOUT=0).
	XRT_APP_SHARE_UNAVAILABLE_SPLIT_DISABLED = 6,
	//! DXR_LEGACY_STANDALONE.
	XRT_APP_SHARE_UNAVAILABLE_LEGACY_STANDALONE = 7,
	//! The app renders on another adapter than the one the service publishes.
	XRT_APP_SHARE_UNAVAILABLE_OTHER_ADAPTER = 8,
	//! Reserved (was: the in-process share capability not implemented); not reported by this build.
	XRT_APP_SHARE_UNAVAILABLE_LINK_NOT_READY = 9,
	//! The platform tap declined for a reason of its own.
	XRT_APP_SHARE_UNAVAILABLE_OTHER = 10,
};

//! xrt_app_share_source_info::executable_name size (= XR_APP_SHARE_EXECUTABLE_NAME_MAX_SIZE_DXR).
#define XRT_APP_SHARE_EXECUTABLE_NAME_SIZE 260
//! xrt_app_share_source_info::application_name size (= XR_MAX_APPLICATION_NAME_SIZE).
#define XRT_APP_SHARE_APPLICATION_NAME_SIZE 128

/*!
 * One app entry as XR_DXR_app_share describes it (XrAppShareSourcePropertiesDXR
 * plus the service's diagnostics). Plain POD: rides the IPC wire as is
 * (`stereo_camera_get_app_source`).
 */
struct xrt_app_share_source_info
{
	uint64_t camera_id;
	//! OS-derived by the service, never app-asserted (0 for the synthetic fake).
	uint64_t pid;
	char executable_name[XRT_APP_SHARE_EXECUTABLE_NAME_SIZE]; //!< file name, no directory
	char application_name[XRT_APP_SHARE_APPLICATION_NAME_SIZE];
	//! The app's window in desktop pixels; width/height 0 when it has none.
	int32_t window_x, window_y;
	uint32_t window_width, window_height;
	/*!
	 * Per-view content size at the source: the active mode's content
	 * dimensions of ONE view (before scaling to the stream's eye extent). It
	 * is not the atlas tile: tiles sit at the atlas's slot stride (atlas size
	 * / grid), which is larger whenever the content is smaller than its slot.
	 */
	uint32_t content_eye_width, content_eye_height;
	uint32_t view_count;        //!< views the app submits (the fixed count, ADR-041)
	uint32_t active_view_count; //!< views active in its current mode (1 = mono)
	float zero_disparity_shift_frac;
	uint32_t encoding_is_linear; //!< the source atlas held LINEAR values (ADR-044); diagnostic
	uint32_t availability;       //!< enum xrt_app_share_availability
	uint32_t source;             //!< enum xrt_stereo_camera_source (APP / APP_LINK)
	uint32_t instance_ordinal;   //!< 0 = the first running copy of this executable
	uint32_t reserved;
};

/*!
 * Per-frame app metadata (XrAppShareFrameInfoDXR). Rides the
 * `stereo_camera_acquire` reply, never the ring (the section has no header).
 * All zero for a camera frame.
 */
struct xrt_app_share_frame_info
{
	/*!
	 * Where the delivered view sits inside ONE eye half of the DELIVERED frame,
	 * in that half's pixels (origin top-left of the left half; the right half
	 * is the same rect offset by the eye width): the view's content, scaled to
	 * fit the eye extent preserving aspect; outside is letterbox black. Never
	 * an atlas / slot / source coordinate — the tap cuts each view from its
	 * tile at the slot stride before scaling, and nothing of that reaches here.
	 */
	int32_t content_x, content_y;
	uint32_t content_width, content_height;
	uint32_t mono;               //!< one view in both halves
	uint32_t content_generation; //!< bumps when content rect / mono / layout changed
	float zero_disparity_shift_frac;
	uint32_t valid; //!< 1 = this is an app frame's metadata
	//! When the source app's frame was committed (os_monotonic ns; the state tracker converts).
	int64_t commit_time_ns;
};

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
	uint32_t source;               //!< enum xrt_stereo_camera_source (was reserved; same wire size)
	uint64_t supported_formats;    //!< XRT_STEREO_CAMERA_BIT(format)
	uint64_t supported_transports; //!< XRT_STEREO_CAMERA_BIT(transport)
	uint64_t display_id;           //!< xrt_screen::id of the camera's screen, 0 = unknown (#1961)
	//! Spec v7: clockwise degrees (0/90/180/270) that make the published frame
	//! upright in the device's natural orientation, or
	//! XRT_STEREO_CAMERA_FRAME_ORIENTATION_UNKNOWN.
	uint32_t frame_orientation_deg;
	uint32_t reserved1;
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
	uint32_t width; //!< full SBS
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

//! State of the service's online vertical-alignment refinement (R2).
enum xrt_stereo_camera_refine_state
{
	XRT_STEREO_CAMERA_REFINE_NONE = 0,      //!< not a service-rectified RECTIFIED stream
	XRT_STEREO_CAMERA_REFINE_OFF = 1,       //!< DXR_STEREO_CAMERA_REFINE=0
	XRT_STEREO_CAMERA_REFINE_MEASURING = 2, //!< no window fitted yet
	XRT_STEREO_CAMERA_REFINE_ALIGNED = 3,   //!< fitted, the calibration was already within the deadband
	XRT_STEREO_CAMERA_REFINE_APPLIED = 4,   //!< a correction is folded into the maps
};

struct xrt_stereo_camera_stream_stats
{
	float source_frame_rate;
	float delivered_frame_rate;
	uint64_t frames_published;
	uint64_t frames_skipped;
	uint64_t frames_acquired;
	uint64_t mean_latency_ns;

	// R2 refinement (diagnostics; the service's view of the rows).
	uint32_t refine_state; //!< enum xrt_stereo_camera_refine_state
	uint32_t refine_updates;
	uint32_t refine_windows;
	uint32_t refine_matches;      //!< inliers of the latest fitted window
	float refine_offset_px;       //!< applied a
	float refine_slope_per_100px; //!< applied b * 100
	float refine_initial_dy_px;   //!< signed median dy of the first window (uncorrected)
	float refine_residual_dy_px;  //!< signed median dy of the latest window
	float refine_measure_ms;      //!< mean cost of one measurement (worker thread)
	uint32_t reserved;
};

//! What a delivered event is (R3).
enum xrt_stereo_camera_event_kind
{
	XRT_STEREO_CAMERA_EVENT_CAMERA_STATE = 1,    //!< XrEventDataStereoCameraStateChangedDXR
	XRT_STEREO_CAMERA_EVENT_STREAM_ENDED = 2,    //!< XrEventDataStereoCameraStreamEndedDXR
	XRT_STEREO_CAMERA_EVENT_CAMERAS_CHANGED = 3, //!< XrEventDataStereoCamerasChangedDXR
};

//! Why the service ended a stream (= XrStereoCameraStreamEndReasonDXR).
enum xrt_stereo_camera_end_reason
{
	XRT_STEREO_CAMERA_END_USER_STOPPED = 1,
	XRT_STEREO_CAMERA_END_DISABLED = 2,
	XRT_STEREO_CAMERA_END_SOURCE_LOST = 3,
	//! The source could not start (held by another process / errored before its first
	//! frame / never delivered): UNAVAILABLE until the retry succeeds (#2221).
	XRT_STEREO_CAMERA_END_SOURCE_BUSY = 4,
};

/*!
 * One queued event for a connection (R3). The service queues them per
 * connection; the client drains them from xrPollEvent.
 */
struct xrt_stereo_camera_event
{
	uint32_t kind;  //!< enum xrt_stereo_camera_event_kind
	uint32_t value; //!< CAMERA_STATE: the state; STREAM_ENDED: the reason
	uint64_t camera_id;
	uint64_t stream_id; //!< STREAM_ENDED only
};

//! `stereo_camera_control` ops (DIAG clients and the service's own UI only).
enum xrt_stereo_camera_control_op
{
	//! End every started stream now (the tray's "Stop camera sharing").
	XRT_STEREO_CAMERA_CONTROL_STOP_ALL = 1,
	//! arg 0/1: the persistent "Share the 3D camera with apps" toggle.
	XRT_STEREO_CAMERA_CONTROL_SET_SHARING = 2,
	//! arg 0/1: simulate the OS session lock (diagnostics only; the real
	//! lock comes from the platform notification).
	XRT_STEREO_CAMERA_CONTROL_SET_LOCKED = 3,
	//! out = bit 0 sharing on, bit 1 locked, bits 8..15 started streams.
	XRT_STEREO_CAMERA_CONTROL_STATUS = 4,
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
	/*!
	 * @p out_ready false = nothing newer than the last acquire. @p out_app
	 * (may be NULL): the frame's XR_DXR_app_share metadata (valid = 0 for a
	 * camera frame).
	 */
	xrt_result_t (*acquire)(struct xrt_stereo_camera_client *c,
	                        uint64_t stream_id,
	                        bool *out_ready,
	                        struct xrt_stereo_camera_frame_info *out_frame,
	                        struct xrt_app_share_frame_info *out_app);
	//! ADR-061: the XR_DXR_app_share description of APP_CONTENT entry @p camera_id.
	xrt_result_t (*get_app_source)(struct xrt_stereo_camera_client *c,
	                               uint64_t camera_id,
	                               struct xrt_app_share_source_info *out_info);
	xrt_result_t (*stats)(struct xrt_stereo_camera_client *c,
	                      uint64_t stream_id,
	                      struct xrt_stereo_camera_stream_stats *out_stats);
	//! R3: pop one queued event; @p out_has false = none pending.
	xrt_result_t (*poll_event)(struct xrt_stereo_camera_client *c,
	                           bool *out_has,
	                           struct xrt_stereo_camera_event *out_event);
	//! R3: enum xrt_stereo_camera_control_op (DIAG clients only).
	xrt_result_t (*control)(struct xrt_stereo_camera_client *c, uint32_t op, uint32_t arg, uint32_t *out_value);
};

#ifdef __cplusplus
}
#endif
