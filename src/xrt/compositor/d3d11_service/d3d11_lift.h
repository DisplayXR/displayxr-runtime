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

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include <mutex>
#include <stdint.h>

struct d3d11_lift;

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
};

/*!
 * Create the lift module for a service. Cheap: the lift device and the vendor
 * display processor are brought up on the lift thread, on first demand.
 *
 * @param svc_device       the service's render device (ID3D11Device1-capable).
 * @param svc_context      its immediate context.
 * @param svc_ctx_mutex    the service's immediate_ctx_mutex.
 * @param lift_factory     the plug-in's LIFT-ONLY factory
 *                         (xrt_plugin_iface::create_dp_d3d11_lift), or NULL.
 * @param fallback_factory its ordinary D3D11 factory, used with a NULL window
 *                         when @p lift_factory is NULL. Both NULL ⟹ UNAVAILABLE.
 * @param eyes_fn          returns the panel's predicted tracked eyes (display
 *                         space, metres) — the viewpoints the lift thread passes
 *                         to every TRACKED conversion. Called from the lift
 *                         thread with no lift lock held. May be NULL.
 * @param eyes_ud          user data for @p eyes_fn.
 */
struct d3d11_lift *
d3d11_lift_create(ID3D11Device *svc_device,
                  ID3D11DeviceContext *svc_context,
                  std::mutex *svc_ctx_mutex,
                  void *lift_factory,
                  void *fallback_factory,
                  bool (*eyes_fn)(void *ud, struct xrt_eye_positions *out),
                  void *eyes_ud);

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
                         uint64_t *out_frame_id);

/*!
 * The weave path's submit: snapshot region (@p x, @p y, @p w, @p h) of @p src
 * (an SRV on the service device, @p src_tw x @p src_th) into the mailbox.
 * The CALLER holds the service context mutex and whatever keyed mutex guards
 * @p src. The snapshot's long edge is capped at DXR_LIFT_MAX_INPUT_EDGE
 * (default 1920, 0 = off; u_lift_cap_dims) — the result is stretched back into
 * the rect, so consumers must sample it, never assume result size == rect size.
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
                             uint64_t *out_frame_id);

//! Pin the stream's latest result for a service-device read (the weave).
bool
d3d11_lift_pin_latest(struct d3d11_lift *lift, uint64_t owner, uint64_t id, struct d3d11_lift_pin *out);

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
