// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_lift (ADR-042) on the D3D11 service: the lift thread, its
 *         device, the vendor module's display processor, and every stream.
 *
 * ## Shape
 *
 * One @ref d3d11_lift per service system, created lazily on the first lift
 * call. It owns:
 *
 *  - the LIFT THREAD, the only thread that ever touches the lift display
 *    processor (the vendor module's contract, xrt_display_processor_d3d11.h);
 *  - a dedicated LIFT DEVICE on the service's adapter, with ID3D11Multithread
 *    protection on its immediate context (a module may flush / signal it from
 *    its own worker). A conversion takes milliseconds to seconds, so it must
 *    never run on the service's shared immediate context — that context is
 *    the one every weave, commit and render sequence serializes on;
 *  - per stream: a latest-wins MAILBOX (two input slots) and an output RING
 *    (two slots), both shared textures with keyed mutexes that cross between
 *    the service device and the lift device, driven by the platform-neutral
 *    state machine in util/u_lift_mailbox.h;
 *  - per stream, for xrAcquireLiftResultDXR: a client-facing export texture +
 *    fence on the service device (the weave output pattern).
 *
 * ## The conversion core (ADR-053 P1a, #2003)
 *
 * The lift thread is also where the conversion core's `dp-lift` provider
 * (conversion/conv_provider_legacy_dp.h) runs: the provider's exec thread
 * hands each of its [S] calls to this thread as a command
 * (d3d11_lift_get_dp_backend) and waits, so the vendor module keeps its one
 * thread and there is one module instance per service. The pre-core path
 * (the streams below) is a branch inside the same thread, kept until P1b for
 * what the v3 routing rule still lets fall through.
 *
 * The same file carries the core's D3D11 GLUE on the service device
 * (d3d11_lift_glue_*): runtime image allocation (shared NT-handle textures +
 * keyed mutex), the v3 route's import / export, the depth readback for S6 /
 * S7 and the reference view synthesis for S5. The weave path's submit / pin
 * calls below take core-routed stream ids (D3D11_LIFT_CORE_STREAM_TAG) too,
 * through the bridge the IPC layer installs (d3d11_lift_set_core_bridge).
 *
 * Eyes (G7): the lift thread never calls back into the compositor; the
 * service publishes the panel's predicted eyes (d3d11_lift_publish_eyes) from
 * where it reads them, and producers refresh a stale snapshot first.
 *
 * ## Threads and locks
 *
 *  - `mtx` (inside d3d11_lift) guards the stream table and every mailbox. It is
 *    never held across a GPU wait, a keyed-mutex acquire, or a module call.
 *  - The service's `immediate_ctx_mutex` is passed in and taken ONLY around
 *    draws/copies on the service context (snapshot blit, export copy). Order:
 *    immediate_ctx_mutex → mtx, never the reverse. The lift thread never takes
 *    it.
 *  - Producers (IPC threads) never wait for the module: a submit is one
 *    snapshot blit on the service context. Consumers (the weave, an acquire)
 *    pin a result slot for the duration of one GPU copy issue.
 *
 * @ingroup comp_d3d11_service
 */

#pragma once

#include "xrt/xrt_results.h"
#include "xrt/xrt_dp_lift.h"
#include "xrt/xrt_display_metrics.h"
#include "xrt/xrt_lift.h"

#include "conversion/conv_pipeline.h"
#include "conversion/conv_provider_legacy_dp.h"
#include "conversion/conv_stream.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include <mutex>
#include <stdint.h>

struct d3d11_lift;
struct conv_core;

//! Set in every v3 stream id the conversion core route minted (== IPC_SERVER_CONV_STREAM_TAG).
#define D3D11_LIFT_CORE_STREAM_TAG (1ull << 62)

//! One acquired texture result (xrAcquireLiftResultDXR).
struct d3d11_lift_result_info
{
	uint64_t frame_id;
	int64_t source_time;
	uint64_t fence_value;
	uint64_t latency_ns;
	uint32_t width;
	uint32_t height;
	uint32_t format; //!< DXGI_FORMAT
	uint32_t view_count;
	bool output_realloc; //!< the export texture changed: the caller must re-import it
	//! ADR-048 echo: viewpoints the result was synthesized for (rect-relative).
	uint32_t viewpoint_count;
	float viewpoints[3 * XRT_LIFT_MAX_VIEWS];
	float rect_center[3];
	float rect_size[2];
	//! XR_DXR_lift v3: the auxiliary depth (valid = 0 when none).
	struct xrt_lift_depth_info depth;
};

//! One acquired blob result (xrAcquireLiftBlobDXR).
struct d3d11_lift_blob_info
{
	uint64_t frame_id;
	int64_t source_time;
	uint32_t format; //!< XRT_DP_LIFT_BLOB_*
	uint64_t byte_count;
};

/*!
 * A result pinned for reading on the SERVICE device (the weave's use). While
 * pinned, the lift thread will not overwrite it and its keyed mutex is held.
 */
struct d3d11_lift_pin
{
	struct d3d11_lift *lift;
	uint64_t stream_id;
	int32_t slot;
	IDXGIKeyedMutex *km;
	ID3D11ShaderResourceView *srv; //!< service-device SRV of the result
	uint32_t width;                //!< whole result (all views)
	uint32_t height;
	uint32_t view_count;
	//! The part of the lifted rect this result covers (letterbox crop):
	//! x0, y0, x1, y1 normalised to the rect as it was snapshotted. Outside it
	//! (the bars) the caller weaves the 2D input FLAT, identical in every view.
	float active[4];
	bool valid;

	/*
	 * ADR-046 Amendment 1 (depth cursor on lifted content).
	 */

	//! The viewpoints this result was synthesized for, relative to
	//! @c rect_center (display axes, metres; ADR-048). 0 = none known.
	uint32_t vp_count;
	float vps[3 * XRT_LIFT_MAX_VIEWS];
	float rect_center[3]; //!< display space, metres
	float rect_size[2];   //!< metres
	//! The result's auxiliary depth on the service device (NULL = none). Its
	//! keyed mutex is held only after d3d11_lift_pin_depth returned true.
	ID3D11Texture2D *depth_tex;
	IDXGIKeyedMutex *depth_km;
	bool depth_held;
	uint32_t depth_w, depth_h, depth_format;
	uint32_t depth_encoding; //!< XRT_DP_LIFT_DEPTH_ENCODING_*
	float depth_value_scale;
	float depth_value_offset;
	//! z in front of the screen = relief_scale / depth + relief_offset
	//! (xrt_dp_lift_depth::relief_*); false = the module reported none.
	bool relief_valid;
	float relief_scale;
	float relief_offset;

	//! #2003: a result of a core-routed stream (its ring slot stays pinned in
	//! the core until d3d11_lift_unpin).
	bool core;
	uint64_t core_id;
	uint32_t core_ring_slot;
};

/*!
 * Re-resolve the plug-in's factory pair (G5): its LIFT-ONLY factory
 * (xrt_plugin_iface::create_dp_d3d11_lift) and, from the SAME plug-in, its
 * ordinary D3D11 factory as the fallback (used with a NULL window when there
 * is no lift-only one). Called at every activation attempt and by the
 * provider's snapshot; must not block. False / both NULL = no factory.
 */
typedef bool (*d3d11_lift_resolve_factories_fn)(void *ud, void **out_lift_factory, void **out_fallback_factory);

/*!
 * Create the lift module for a service. Cheap: the lift device and the vendor
 * display processor are brought up on the lift thread, on first demand.
 *
 * @param svc_device       the service's render device (ID3D11Device1-capable).
 * @param svc_context      its immediate context.
 * @param svc_ctx_mutex    the service's immediate_ctx_mutex.
 * @param resolve          re-resolves the plug-in's lift-only factory and its
 *                         fallback (see d3d11_lift_resolve_factories_fn) at
 *                         every activation attempt. NULL / nothing ⟹ UNAVAILABLE.
 * @param resolve_ud       user data for @p resolve.
 * @param nominal_z        the display's nominal viewing distance, metres
 *                         (<= 0 = 0.5 m) — the viewpoint policy's straight-on
 *                         viewer (ADR-048).
 */
struct d3d11_lift *
d3d11_lift_create(ID3D11Device *svc_device,
                  ID3D11DeviceContext *svc_context,
                  std::mutex *svc_ctx_mutex,
                  d3d11_lift_resolve_factories_fn resolve,
                  void *resolve_ud,
                  float nominal_z);

/*!
 * G7: the panel's predicted eyes (display space, metres), published by the
 * service from wherever it reads them (never pulled by the lift thread, so a
 * conversion never takes the compositor's render_mutex). TRACKED viewpoints
 * use the latest snapshot; one older than 1 s counts as no eyes.
 */
void
d3d11_lift_publish_eyes(struct d3d11_lift *lift, const struct xrt_eye_positions *eyes);

/*!
 * A PRODUCER-side refresh: called (on the submitting thread, never the lift
 * thread) before a submit when the snapshot is older than 50 ms, so eyes stay
 * fresh while nothing else queries them. @p fn may take the render_mutex.
 */
void
d3d11_lift_set_eyes_refresh(struct d3d11_lift *lift, void (*fn)(void *ud), void *ud);

/*!
 * The `dp-lift` provider's backend (conv_provider_legacy_dp.h): every [S] call
 * is one command on this module's lift thread.
 */
void
d3d11_lift_get_dp_backend(struct d3d11_lift *lift, struct conv_dp_lift_backend *out);

//! The weave path's view of core-routed streams (installed by the IPC layer).
struct d3d11_lift_core_bridge
{
	struct conv_core *core;
	//! A v3 stream id the route minted -> the core stream and its v3 mode bit.
	bool (*resolve)(void *ud, uint64_t owner, uint64_t wire_id, uint64_t *out_core_id, uint32_t *out_mode);
	void *ud;
};

//! Install (or, with NULL, clear) the bridge. Copied.
void
d3d11_lift_set_core_bridge(struct d3d11_lift *lift, const struct d3d11_lift_core_bridge *bridge);


/*
 *
 * The conversion core's D3D11 glue (#2003; d3d11_conv_gpu.cpp after P1b).
 *
 */

//! What publishing one result into a stream's export targets produced.
struct d3d11_lift_published
{
	uint64_t fence_value;
	uint32_t width, height, format; //!< DXGI_FORMAT
	bool realloc;
	uint32_t depth_width, depth_height, depth_format;
	bool depth_realloc;
};

//! Runtime image allocation on the service device (conv_image_alloc_ops).
void
d3d11_lift_glue_alloc_ops(struct d3d11_lift *lift, struct conv_image_alloc_ops *out);

//! S5 / S6 stages on the service device.
void
d3d11_lift_glue_gpu_ops(struct d3d11_lift *lift,
                        struct conv_viewsyn_ops *viewsyn,
                        struct conv_depth_readback_ops *readback);

/*! S0 for the v3 route: snapshot the client's shared input (@p w x @p h)
 *  into @p slot's images (downscaled when the slot is smaller: inputScale).
 *  Takes ownership of @p handle. */
xrt_result_t
d3d11_lift_glue_import_input(struct d3d11_lift *lift,
                             uint64_t stream_id,
                             HANDLE handle,
                             bool is_dxgi,
                             uint32_t w,
                             uint32_t h,
                             const struct conv_submit_slot *slot);

/*! The client's input read back as tightly packed RGBA8 (GAUSSIANS facade:
 *  a job takes decoded CPU pixels). *out_pixels is malloc'd. Takes ownership
 *  of @p handle. */
xrt_result_t
d3d11_lift_glue_import_input_cpu(struct d3d11_lift *lift,
                                 uint64_t stream_id,
                                 HANDLE handle,
                                 bool is_dxgi,
                                 uint32_t w,
                                 uint32_t h,
                                 uint8_t **out_pixels);

void
d3d11_lift_glue_release_input(HANDLE handle, bool is_dxgi);

//! Copy @p res into the stream's export texture(s), signal its fence.
xrt_result_t
d3d11_lift_glue_publish(struct d3d11_lift *lift,
                        uint64_t stream_id,
                        const struct conv_result *res,
                        struct d3d11_lift_published *out);

bool
d3d11_lift_glue_export_output(struct d3d11_lift *lift,
                              uint64_t stream_id,
                              bool depth,
                              HANDLE *out_handle,
                              uint32_t *out_w,
                              uint32_t *out_h,
                              uint32_t *out_format);

bool
d3d11_lift_glue_export_fence(struct d3d11_lift *lift, uint64_t stream_id, HANDLE *out_handle);

void
d3d11_lift_glue_stream_gone(struct d3d11_lift *lift, uint64_t stream_id);

//! Stop the lift thread, destroy every stream and the lift DP/device.
void
d3d11_lift_destroy(struct d3d11_lift **lift_ptr);

//! Current caps (cached; never blocks). Kicks activation on first call.
void
d3d11_lift_get_caps(struct d3d11_lift *lift, struct xrt_dp_lift_caps *out);

//! Create a stream owned by @p owner (an IPC connection token).
xrt_result_t
d3d11_lift_stream_create(struct d3d11_lift *lift,
                         uint64_t owner,
                         const struct xrt_dp_lift_stream_info *info,
                         uint64_t *out_id);

void
d3d11_lift_stream_destroy(struct d3d11_lift *lift, uint64_t owner, uint64_t id);

//! Destroy every stream @p owner created (client teardown).
void
d3d11_lift_release_owner(struct d3d11_lift *lift, uint64_t owner);

//! The stream's mode bit, 0 when @p id is not @p owner's live stream.
uint32_t
d3d11_lift_stream_mode(struct d3d11_lift *lift, uint64_t owner, uint64_t id);

/*!
 * xrSubmitLiftFrameDXR: open the caller's shared texture (cached per stream),
 * acquire its keyed mutex (4 ms), snapshot @p w x @p h into the mailbox, post.
 * Takes the service context mutex internally — call WITHOUT it held.
 * The handle is the service's to close (a duplicated NT handle) unless
 * @p is_dxgi. @p params NULL = the stream's last parameters.
 * @p view_control NULL = the stream's last policy (initially the ADR-048
 * defaults). @p frame = the submitting window (centre + size) the viewpoints
 * are rebased to; NULL / invalid = the panel centre. @p rig (XR_DXR_lift v3)
 * rides with the options: replaced (NULL = cleared) whenever @p params or
 * @p rig is non-NULL, kept otherwise.
 */
xrt_result_t
d3d11_lift_submit_handle(struct d3d11_lift *lift,
                         uint64_t owner,
                         uint64_t id,
                         HANDLE handle,
                         bool is_dxgi,
                         uint32_t w,
                         uint32_t h,
                         int64_t source_time,
                         const struct xrt_dp_lift_params *params,
                         const float *viewpoints,
                         uint32_t viewpoint_floats,
                         const struct xrt_lift_view_control *view_control,
                         const struct xrt_lift_rig *rig,
                         const struct xrt_lift_rect_frame *frame,
                         uint64_t *out_frame_id);

/*!
 * The weave path's submit: snapshot region (@p x, @p y, @p w, @p h) of @p src
 * (an SRV on the service device, @p src_tw x @p src_th) into the mailbox.
 * The CALLER holds the service context mutex and whatever keyed mutex guards
 * @p src. The snapshot's long edge is capped at DXR_LIFT_MAX_INPUT_EDGE
 * (default 1920, 0 = off; u_lift_cap_dims) — the result is stretched back into
 * the rect, so consumers must sample it, never assume result size == rect size.
 * @p view_control / @p frame as for d3d11_lift_submit_handle (@p frame = the
 * lifted rect's physical centre + size). @p viewpoints / @p rig: a lifted
 * rect's EXPLICIT viewpoints and app rig (XR_DXR_lift v3), same rules as
 * d3d11_lift_submit_handle.
 */
xrt_result_t
d3d11_lift_submit_srv_locked(struct d3d11_lift *lift,
                             uint64_t owner,
                             uint64_t id,
                             ID3D11ShaderResourceView *src,
                             uint32_t src_tw,
                             uint32_t src_th,
                             uint32_t x,
                             uint32_t y,
                             uint32_t w,
                             uint32_t h,
                             int64_t source_time,
                             const struct xrt_dp_lift_params *params,
                             const float *viewpoints,
                             uint32_t viewpoint_floats,
                             const struct xrt_lift_view_control *view_control,
                             const struct xrt_lift_rig *rig,
                             const struct xrt_lift_rect_frame *frame,
                             uint64_t *out_frame_id);

//! Pin the stream's latest result for a service-device read (the weave).
bool
d3d11_lift_pin_latest(struct d3d11_lift *lift, uint64_t owner, uint64_t id, struct d3d11_lift_pin *out);

//! ADR-046 Amendment 1: also take the pinned result's depth map (its keyed
//! mutex, without waiting). False = the result has none, or it is busy this
//! frame. Released by d3d11_lift_unpin.
bool
d3d11_lift_pin_depth(struct d3d11_lift_pin *pin);

void
d3d11_lift_unpin(struct d3d11_lift_pin *pin);

/*!
 * xrAcquireLiftResultDXR: copy the newest result not yet acquired into the
 * stream's export texture, signal its fence. @p out_ready false = nothing new.
 * Takes the service context mutex internally — call WITHOUT it held.
 */
xrt_result_t
d3d11_lift_acquire_result(
    struct d3d11_lift *lift, uint64_t owner, uint64_t id, bool *out_ready, struct d3d11_lift_result_info *out);

//! The export texture's NT handle (service-owned; the IPC layer duplicates it).
bool
d3d11_lift_export_output(struct d3d11_lift *lift,
                         uint64_t owner,
                         uint64_t id,
                         HANDLE *out_handle,
                         uint32_t *out_w,
                         uint32_t *out_h,
                         uint32_t *out_format);

bool
d3d11_lift_export_fence(struct d3d11_lift *lift, uint64_t owner, uint64_t id, HANDLE *out_handle);

//! XR_DXR_lift v3: the auxiliary depth export texture's NT handle
//! (service-owned; the IPC layer duplicates it). Signalled by the same fence.
bool
d3d11_lift_export_depth(struct d3d11_lift *lift,
                        uint64_t owner,
                        uint64_t id,
                        HANDLE *out_handle,
                        uint32_t *out_w,
                        uint32_t *out_h,
                        uint32_t *out_format);

/*!
 * xrAcquireLiftBlobDXR with the two-call latch (see XrLiftBlobDXR). On
 * delivery (@p capacity >= byte_count) @p out_bytes receives a malloc'd copy
 * the caller frees; otherwise it stays NULL and the blob stays latched (the
 * caller reports XR_ERROR_SIZE_INSUFFICIENT for 0 < capacity < byte_count).
 */
xrt_result_t
d3d11_lift_acquire_blob(struct d3d11_lift *lift,
                        uint64_t owner,
                        uint64_t id,
                        uint64_t capacity,
                        bool *out_ready,
                        struct d3d11_lift_blob_info *info,
                        uint8_t **out_bytes);

//! Set a stream's scheduling priority (XrLiftPriorityDXR: 0 paused .. 3 high).
xrt_result_t
d3d11_lift_set_priority(struct d3d11_lift *lift, uint64_t owner, uint64_t id, uint32_t priority);

//! A stream's counters + effective conversion rate.
xrt_result_t
d3d11_lift_get_stats(struct d3d11_lift *lift, uint64_t owner, uint64_t id, struct xrt_lift_stream_stats *out);
