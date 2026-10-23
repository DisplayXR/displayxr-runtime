// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Viewer-tracking provider plug-in negotiation interface (ADR-054 §4.1, N6 Part 2).
 *
 * The fourth component class next to display processors (`xrt_plugin.h`),
 * input providers (`xrt_input_plugin.h`) and conversion providers
 * (`xrt_conversion_plugin.h`). A viewer-tracking provider measures where the
 * viewer is — head pose plus the left / right eye positions, in DISPLAY space
 * — from a sensor the display processor does not own (a colour webcam, a
 * depth camera, a wearable) and publishes timestamped samples. The trunk keeps
 * a short ring of them and samples it at the target time (the frame's display
 * time), exactly like the DP eye-pair ring (`util/u_eye_history.h`).
 *
 * What the trunk does with the samples is TRUNK POLICY, per screen
 * (`docs/specs/runtime/tracking-provider-discovery.md` §4):
 *  1. the display processor's own eyes when it tracks — always, even while its
 *     tracker has lost the face; a second source is never spliced into a
 *     screen whose weave uses tracked eyes;
 *  2. else the best enabled provider (`PreferredPlugin`, then `ProbeOrder`),
 *     but only when the display declares @ref XRT_PLUGIN_VIEWER_POLICY_EXTERNAL_OK
 *     (`xrt_plugin_iface::get_viewer_policy`);
 *  3. else the nominal viewer.
 * The trunk never pushes eyes into a display processor.
 *
 * Discovery: `HKLM\Software\DisplayXR\TrackingProviders\<id>` on Windows,
 * `NNN-<id>-tracking-provider.json` manifests in the shared POSIX roots. The
 * runtime resolves the single exported symbol `xrtTrackingPluginNegotiate`,
 * gates the ABI major (ADR-020 rule 3), and calls `probe()`.
 *
 * ## Hosting shape (why the ABI looks like this)
 *
 * The provider is **session-free and owns its own thread**. It never sees an
 * OpenXR session, a compositor or a graphics device: it is started with a
 * sink and pushes samples into it from its own thread whenever it has one.
 * That is the shape an out-of-process host needs later (the sink becomes the
 * N8 seqlock page, ADR-053 §5 hosting), and the shape a provider that is an
 * ADR-043 camera CONSUMER needs (frames arrive on the camera's thread, not on
 * a frame loop). In this release the runtime loads providers in-process
 * through the shared loader; the ABI does not change when hosting moves.
 *
 * ## Camera-space samples (room for calibration)
 *
 * A camera-based provider MAY also report what it measured in CAMERA space
 * (@ref xrt_tracking_camera_sample: camera key, per-eye unit direction and
 * estimated distance, the same capture time) beside the display-space sample.
 * Nothing consumes it in this release; it is the input of the mirror-alignment
 * camera-pose calibration (`displayxr-cli tracking calibrate`, a runtime-side
 * solver), which writes the solved pose into the RUNTIME's camera calibration
 * store, keyed by (camera, screen) — not into any provider's settings (spec
 * §6). A provider reads the pose back through
 * @ref xrt_tracking_plugin_host_iface::get_camera_pose, the same answer an
 * input provider using the same camera gets. The block is struct_size-gated,
 * so a provider that does not report it simply leaves
 * @ref XRT_TRACKING_SAMPLE_CAMERA_VALID clear.
 *
 * ABI policy per ADR-020: `struct_size` first on every struct, append-only
 * vtables, loader-side major reject.
 *
 * @author David Fattal
 * @ingroup xrt_iface
 */

#pragma once

#include "xrt/xrt_camera_pose.h"
#include "xrt/xrt_component_settings.h"
#include "xrt/xrt_results.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


/*
 *
 * API versioning.
 *
 */

//! Viewer-tracking provider ABI version (major). Independent of every other class's major.
#define XRT_TRACKING_PLUGIN_API_VERSION_1 1

//! The major this header describes; a provider returning another is rejected (ADR-020 rule 3).
#define XRT_TRACKING_PLUGIN_API_VERSION_CURRENT XRT_TRACKING_PLUGIN_API_VERSION_1

//! The single exported symbol every viewer-tracking provider binary provides (C linkage).
#define XRT_TRACKING_PLUGIN_ENTRYPOINT_NAME "xrtTrackingPluginNegotiate"

//! Linkage decoration for `xrtTrackingPluginNegotiate` (same semantics as `XRT_PLUGIN_EXPORT`).
#if defined(_WIN32) || defined(__CYGWIN__)
#define XRT_TRACKING_PLUGIN_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define XRT_TRACKING_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define XRT_TRACKING_PLUGIN_EXPORT
#endif

#ifndef XRT_TRACKING_PLUGIN_ABI_ASSERT
#if defined(__cplusplus)
#define XRT_TRACKING_PLUGIN_ABI_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define XRT_TRACKING_PLUGIN_ABI_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif
#endif

/*!
 * True only when the provider's reported `struct_size` covers ALL of
 * @p FIELD and the provider filled it in. Every runtime dispatch through a
 * slot appended after v1 goes through this (whole-field coverage, like
 * `XRT_INPUT_PLUGIN_IFACE_HAS`).
 */
#define XRT_TRACKING_PLUGIN_IFACE_HAS(IFACE, FIELD)                                                                    \
	((IFACE) != NULL &&                                                                                            \
	 (IFACE)->struct_size >= offsetof(struct xrt_tracking_plugin_iface, FIELD) + sizeof((IFACE)->FIELD) &&         \
	 (IFACE)->FIELD != NULL)


/*
 *
 * Samples.
 *
 */

//! Bits of @ref xrt_tracking_viewer_sample::flags.
//! The eye positions are meaningful (a fresh measurement or a provider-filtered hold).
#define XRT_TRACKING_SAMPLE_EYES_VALID (1u << 0)
//! The provider has lock on a viewer right now. Clear = "last known, not tracking".
#define XRT_TRACKING_SAMPLE_TRACKING (1u << 1)
//! @ref xrt_tracking_viewer_sample::head_position / head_orientation are meaningful.
#define XRT_TRACKING_SAMPLE_HEAD_POSE_VALID (1u << 2)
//! @ref xrt_tracking_viewer_sample::camera is filled (camera-based providers; see the file comment).
#define XRT_TRACKING_SAMPLE_CAMERA_VALID (1u << 3)

//! Size of a camera key, NUL included (a USB "vvvv:pppp", an OS device id, or a provider-chosen stable name).
#define XRT_TRACKING_CAMERA_KEY_MAX 128

/*!
 * What a camera-based provider measured, in its CAMERA's frame, for one
 * capture. Input of the camera-pose calibration (spec §6, `math/m_mirror_calib.h`).
 *
 * Camera frame: origin at the optical centre, +X right in the image as the
 * USER sees it (un-mirrored), +Y up, +Z out of the lens toward the viewer.
 */
struct xrt_tracking_camera_sample
{
	//! Stable key of the camera the measurement came from (what the camera calibration store is keyed by, spec §6).
	char camera_key[XRT_TRACKING_CAMERA_KEY_MAX];
	//! Unit direction from the optical centre to the left eye point (the face landmarker's accurate part).
	float left_dir[3];
	//! Estimated distance to the left eye point along @ref left_dir, metres (iris-size depth; ±5-10 %).
	float left_distance_m;
	//! Unit direction to the right eye point.
	float right_dir[3];
	//! Estimated distance to the right eye point, metres.
	float right_distance_m;
	//! Reserved, 0.
	uint32_t reserved[8];
};

/*!
 * One viewer measurement, pushed by the provider through
 * @ref xrt_tracking_sink::push.
 *
 * Display space (the eye-position convention of `xrt_display_metrics.h`):
 * metres, origin at the centre of the display surface, +X right, +Y up, +Z out
 * of the glass toward the viewer. The provider maps its sensor into this space
 * (through `host->get_camera_pose`, spec §6); the runtime never does.
 *
 * `struct_size` first (ADR-020): fields are appended only, and the runtime
 * reads no further than the provider's `struct_size`.
 */
struct xrt_tracking_viewer_sample
{
	uint32_t struct_size; //!< `sizeof(struct xrt_tracking_viewer_sample)` at the PROVIDER's compile time
	uint32_t flags;       //!< XRT_TRACKING_SAMPLE_* bits
	//! When the sensor captured this (monotonic, `os_monotonic_get_ns` domain) — NOT when it was pushed.
	int64_t capture_ns;
	float left_eye[3];         //!< Left eye, display space, metres
	float right_eye[3];        //!< Right eye, display space, metres
	float head_position[3];    //!< Head centre, display space, metres (HEAD_POSE_VALID)
	float head_orientation[4]; //!< Head orientation x, y, z, w (HEAD_POSE_VALID)
	float confidence;          //!< 0..1, provider's own scale; 0 = unknown
	uint32_t screen_hint;      //!< Reserved for per-screen providers: 0 = "the screen the provider is serving"
	struct xrt_tracking_camera_sample camera; //!< CAMERA_VALID
	                                          /* Append-only below this line (ADR-020). */
};

/*!
 * Where a running provider delivers its samples. Runtime-owned storage, valid
 * from `start()` until `stop()` returns. `push` is non-blocking, allocation
 * free and callable from any thread (the provider's own, typically); the
 * runtime copies the sample.
 */
struct xrt_tracking_sink
{
	uint32_t struct_size; //!< `sizeof(struct xrt_tracking_sink)` at the runtime's compile time
	uint32_t reserved_0;
	void (*push)(const struct xrt_tracking_sink *sink, const struct xrt_tracking_viewer_sample *sample);
	void *ctx; //!< Runtime-private
	void *reserved[4];
};


/*
 *
 * Description.
 *
 */

//! Bits of @ref xrt_tracking_provider_info::tracks.
#define XRT_TRACKING_TRACKS_EYE_PAIR (1u << 0)     //!< left + right eye positions (required for the viewer role)
#define XRT_TRACKING_TRACKS_HEAD_POSE (1u << 1)    //!< head centre + orientation
#define XRT_TRACKING_TRACKS_GAZE (1u << 2)         //!< reserved: gaze direction (no slot yet)
#define XRT_TRACKING_TRACKS_CAMERA_SPACE (1u << 3) //!< camera-space samples (calibration input)

/*!
 * What a provider tracks and from what, filled by
 * @ref xrt_tracking_plugin_iface::describe. The runtime sets `struct_size` and
 * zero-fills; the provider writes no further than `struct_size`.
 */
struct xrt_tracking_provider_info
{
	uint32_t struct_size;
	uint32_t tracks;            //!< XRT_TRACKING_TRACKS_* bits
	int64_t typical_latency_ns; //!< Capture-to-push latency; 0 = unknown
	char source[128];           //!< Human description of the source ("scripted sweep", "webcam 'FaceTime HD'")
	char camera_key[XRT_TRACKING_CAMERA_KEY_MAX]; //!< The camera it reads, "" = none / not a camera
	uint32_t reserved[8];
};

//! Live state of a provider, @ref xrt_tracking_plugin_iface::get_status.
enum xrt_tracking_provider_status
{
	XRT_TRACKING_PROVIDER_STATUS_UNKNOWN = 0,   //!< Cannot say (starting up)
	XRT_TRACKING_PROVIDER_STATUS_STOPPED = 1,   //!< Not started (or stopped)
	XRT_TRACKING_PROVIDER_STATUS_RUNNING = 2,   //!< Started and its source is producing
	XRT_TRACKING_PROVIDER_STATUS_NO_SOURCE = 3, //!< Started, but its source is gone (camera unplugged)
};


/*
 *
 * Host iface.
 *
 */

/*!
 * Nominal panel + viewer geometry of the active display, for a provider that
 * needs the panel size to place itself (a bezel camera prior). Display space.
 */
struct xrt_tracking_host_display_geometry
{
	uint32_t struct_size; //!< caller sets to sizeof before the call
	float display_width_m;
	float display_height_m;
	float nominal_viewer_x_m;
	float nominal_viewer_y_m;
	float nominal_viewer_z_m;
};

/*!
 * Host callbacks. Process-lifetime storage: a provider may keep the pointer.
 * Providers MUST NOT dereference past `host->struct_size`; reserved slots are
 * repurposed without a major bump (an older runtime leaves them NULL).
 */
struct xrt_tracking_plugin_host_iface
{
	uint32_t struct_size;      //!< `sizeof(struct xrt_tracking_plugin_host_iface)` at the runtime's compile time
	uint32_t host_api_version; //!< == the `runtime_api_version` negotiate received

	/*!
	 * The one options contract (ADR-054 decision 7): `DXR_TRACKING_<ID>_<KEY>`
	 * in the hosting process's environment, then the per-user store, then the
	 * row's `Settings\<KEY>` / manifest `settings`. Never `getenv`. Read at init.
	 */
	xrt_component_get_setting_fn_t get_setting;

	/*!
	 * The active display's nominal geometry. XRT_SUCCESS once the runtime knows
	 * it (from `start()` on); before that XRT_ERROR_INPUT_HOST_GEOMETRY_NOT_READY
	 * (the code the input host uses for the same state) with the struct untouched.
	 */
	xrt_result_t (*get_display_geometry)(struct xrt_tracking_host_display_geometry *inout);

	/*!
	 * Where camera `camera_key` sits relative to screen `screen_key`: the
	 * runtime's camera calibration store, resolved display-declared >
	 * user-calibration > placement-hint > default-prior, so the answer is
	 * never "not found" (`xrt/xrt_camera_pose.h`; spec §6). Pass this host
	 * iface as `host`. The SAME answer the input host iface's slot of the same
	 * name gives for the same camera and screen: one calibration serves every
	 * class. The answer also carries the viewer's per-user iris scale.
	 *
	 * Carved out of the former reserved[0]: size and every other offset
	 * unchanged, the API major stays 1. An older runtime leaves it NULL —
	 * providers MUST NULL-check (and cover-check `struct_size`). Announced by
	 * @ref XRT_TRACKING_PLUGIN_HOST_HAS_GET_CAMERA_POSE.
	 */
	xrt_camera_get_pose_fn_t get_camera_pose;

	//! Reserved for forward-compatible callbacks (an ADR-043 camera-consumer open, N8). MUST NOT be dereferenced.
	void *reserved[11];
};

/*!
 * Defined when this header carries the
 * @ref xrt_tracking_plugin_host_iface::get_camera_pose slot, so a provider
 * that must also compile against an older runtime header can `#ifdef`-guard
 * its use.
 */
#define XRT_TRACKING_PLUGIN_HOST_HAS_GET_CAMERA_POSE 1

/*!
 * True when @p HOST carries a filled @ref xrt_tracking_plugin_host_iface::get_camera_pose
 * (whole-field `struct_size` coverage plus the NULL test an older runtime needs).
 */
#define XRT_TRACKING_PLUGIN_HOST_HAS_CAMERA_POSE(HOST)                                                                 \
	((HOST) != NULL &&                                                                                             \
	 (HOST)->struct_size >=                                                                                        \
	     offsetof(struct xrt_tracking_plugin_host_iface, get_camera_pose) + sizeof((HOST)->get_camera_pose) &&     \
	 (HOST)->get_camera_pose != NULL)

// clang-format off
XRT_TRACKING_PLUGIN_ABI_ASSERT(offsetof(struct xrt_tracking_plugin_host_iface, get_camera_pose) ==
                                   offsetof(struct xrt_tracking_plugin_host_iface, get_display_geometry) +
                                       sizeof(void *),
                               "xrt_tracking_plugin_host_iface: get_camera_pose must occupy the former reserved[0]");
XRT_TRACKING_PLUGIN_ABI_ASSERT(sizeof(struct xrt_tracking_plugin_host_iface) ==
                                   2 * sizeof(uint32_t) + 14 * sizeof(void *),
                               "xrt_tracking_plugin_host_iface size changed - see ADR-020");
// clang-format on


/*
 *
 * Provider vtable.
 *
 */

//! Opaque per-provider instance, produced by `probe()`.
struct xrt_tracking_plugin_instance;

/*!
 * The provider's vtable, filled inside `xrtTrackingPluginNegotiate`. Provider
 * storage, valid until `destroy()`. Append-only below the marker (ADR-020).
 *
 * Threading: `probe`, `describe`, `start`, `stop`, `destroy` are called from
 * the runtime's system-create / teardown path (one thread at a time);
 * `get_status` from any thread and MUST NOT block.
 */
struct xrt_tracking_plugin_iface
{
	uint32_t struct_size; //!< `sizeof(struct xrt_tracking_plugin_iface)` at the provider's compile time
	uint32_t reserved_0;  //!< 0

	const char *id;           //!< Matches the registration id (e.g. "sim-tracking")
	const char *display_name; //!< Human name
	const char *vendor;       //!< May be NULL
	const char *version;      //!< May be NULL

	/*!
	 * Does this provider have a USABLE source on this system? Cheap and
	 * side-effect free: enumerate, never open a camera. XRT_SUCCESS (with
	 * `*out_inst`, NULL legal) = yes; XRT_ERROR_PROBER_NOT_SUPPORTED = cleanly
	 * no (no camera, opt-in setting off); any other error = a fault.
	 *
	 * This is the "usable source at system creation" the composed
	 * `XR_DXR_display_info` advertisement is decided on, so a provider must not
	 * say yes when it could not produce a sample.
	 */
	xrt_result_t (*probe)(struct xrt_tracking_plugin_instance **out_inst);

	//! Fill @p out (struct_size preset by the runtime). Optional (NULL = eye pair, unknown latency).
	xrt_result_t (*describe)(struct xrt_tracking_plugin_instance *inst, struct xrt_tracking_provider_info *out);

	/*!
	 * Start producing: open the source, start the provider's OWN thread, push
	 * samples into @p sink. Called on demand — the first time the trunk needs
	 * a viewer on a screen this provider serves — never at load. @p sink is
	 * valid until `stop()` returns. Idempotent from the runtime's side (it
	 * never calls start twice without a stop).
	 */
	xrt_result_t (*start)(struct xrt_tracking_plugin_instance *inst, const struct xrt_tracking_sink *sink);

	//! Stop producing and close the source; returns after the provider's thread stopped pushing.
	void (*stop)(struct xrt_tracking_plugin_instance *inst);

	//! Live state; non-blocking. Optional (NULL = RUNNING while started).
	enum xrt_tracking_provider_status (*get_status)(struct xrt_tracking_plugin_instance *inst);

	//! Free @p inst (after `stop()` when it was started).
	void (*destroy)(struct xrt_tracking_plugin_instance *inst);

	//! The settings it reads through `host->get_setting`, for `displayxr-cli tracking options <id>`. Optional.
	xrt_component_describe_settings_fn_t describe_settings;

	/* Append-only below this line, forever (ADR-020). */
};


/*
 *
 * Entry point.
 *
 */

/*!
 * `xrtTrackingPluginNegotiate`. The provider sets `*out_plugin_api_version`
 * to the major it implements (the loader rejects a mismatch before any
 * dispatch), `(*out_iface)->struct_size` to its own `sizeof`, and returns
 * XRT_SUCCESS, or XRT_ERROR_PROBER_NOT_SUPPORTED to decline cleanly.
 */
typedef xrt_result_t (*xrt_tracking_plugin_negotiate_fn_t)(uint32_t runtime_api_version,
                                                           const struct xrt_tracking_plugin_host_iface *host,
                                                           struct xrt_tracking_plugin_iface **out_iface,
                                                           uint32_t *out_plugin_api_version);

#ifdef __cplusplus
}
#endif
