// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The conversion core's D3D11 GPU glue on the service device
 *         (ADR-053; split out of d3d11_lift in P1b, #2028).
 *
 * Runtime image allocation (shared NT-handle textures + keyed mutex), the v3
 * route's import / publish / export, the S6 depth readback, the S5 reference
 * view synthesis, and the weave path's submit / pin on core-routed streams
 * (through the bridge the IPC layer installs). The ipc_server_conv_io_ops the
 * service installs are thin wrappers over these.
 *
 * Threads: producers (IPC), the core's dispatcher and the render thread call
 * in; none of these calls a provider slot (those run on the lift thread or
 * in a host). Locks: the service's immediate_ctx_mutex around service-context
 * work only, then the glue's own leaf mutex.
 *
 * @ingroup comp_d3d11_service
 */

#pragma once

#include "xrt/xrt_results.h"
#include "xrt/xrt_dp_lift.h"
#include "xrt/xrt_display_metrics.h"
#include "xrt/xrt_lift.h"

#include "conversion/conv_pipeline.h"
#include "conversion/conv_stream.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include <stdint.h>

struct d3d11_lift;
struct conv_core;

//! Set in every v3 stream id the conversion core route minted (== IPC_SERVER_CONV_STREAM_TAG).
#define D3D11_LIFT_CORE_STREAM_TAG (1ull << 62)

/*!
 * A result pinned for reading on the SERVICE device (the weave's use). While
 * pinned, its core ring slot is not reused and its keyed mutex is held.
 */
struct d3d11_lift_pin
{
	struct d3d11_lift *lift;
	uint64_t stream_id;
	int32_t slot; //!< unused (-1); the core ring slot is core_ring_slot
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
 * G7, producer side: the service notes every eye query it answered (the eyes
 * themselves go to the conversion core's per-DP snapshot). A producer whose
 * last note is older than 50 ms calls the refresh first, on its own thread
 * (the refresh may take render_mutex; the lift thread never does).
 */
void
d3d11_lift_publish_eyes(struct d3d11_lift *lift, const struct xrt_eye_positions *eyes);

void
d3d11_lift_set_eyes_refresh(struct d3d11_lift *lift, void (*fn)(void *ud), void *ud);

//! The weave path's view of core-routed streams (installed by the IPC layer).
struct d3d11_lift_core_bridge
{
	struct conv_core *core;
	//! A v3 stream id the route minted -> the core stream and its v3 mode bit.
	bool (*resolve)(void *ud, uint64_t owner, uint64_t wire_id, uint64_t *out_core_id, uint32_t *out_mode);
	//! Spec 4: the weave's view count for a lifted rect's stream (may be NULL).
	void (*match_weave)(void *ud, uint64_t owner, uint64_t wire_id, uint32_t views);
	void *ud;
};

//! Install (or, with NULL, clear) the bridge. Copied.
void
d3d11_lift_set_core_bridge(struct d3d11_lift *lift, const struct d3d11_lift_core_bridge *bridge);

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

/*! #2030: the content key of v3 stream @p stream_id's input (still-image
 *  dedupe, PHOTO streams), sampled from @p slot's image after
 *  d3d11_lift_glue_import_input filled it. One submit late; 0 = unknown
 *  (see d3d11_conv_content_key.h). */
uint64_t
d3d11_lift_glue_input_content_key(struct d3d11_lift *lift, uint64_t stream_id, const struct conv_submit_slot *slot);

//! The spec-4 input kinds (#2054): D3D11 / D3D12 NT (+ a shared D3D fence),
//! legacy DXGI, CPU bytes in a file mapping; any sub-rect region.
#define D3D11_LIFT_INPUT_NT CONV_INPUT_NT
#define D3D11_LIFT_INPUT_DXGI CONV_INPUT_DXGI
#define D3D11_LIFT_INPUT_CPU CONV_INPUT_CPU

/*! S0 for a spec-4 submit: like d3d11_lift_glue_import_input, for any of the
 *  kinds above and a region. Takes ownership of both handles. */
xrt_result_t
d3d11_lift_glue_import_input_ex(struct d3d11_lift *lift,
                                uint64_t stream_id,
                                const struct conv_input_import *in,
                                const struct conv_submit_slot *slot);

/*! CPU export (#2054): the stream's current export read back, tightly
 *  packed, into a file mapping the service owns (re-created on a size or
 *  format change: @p out_realloc). Synchronous; call right after publish.
 *  @p copy false: only the current mapping (no copy, no realloc). */
bool
d3d11_lift_glue_export_cpu(struct d3d11_lift *lift,
                           uint64_t stream_id,
                           bool copy,
                           HANDLE *out_mapping,
                           uint32_t *out_w,
                           uint32_t *out_h,
                           uint32_t *out_format,
                           uint64_t *out_bytes,
                           bool *out_realloc);

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

/*! Spec 4: before a lifted rect is submitted, the view count the active mode
 *  weaves (2 on the batch SBS layout). A core-routed stream created with
 *  viewCount 0 follows it; anything else ignores it. Cheap. */
void
d3d11_lift_rect_match_views(struct d3d11_lift *lift, uint64_t owner, uint64_t stream_id, uint32_t views);


/*
 *
 * The weave path (comp_d3d11_service's lift-rect weave). Every lifted rect is
 * a core-routed v3 stream; any other id is refused (non-fatal).
 *
 */

//! The core-routed stream's v3 mode bit, 0 when @p id is not @p owner's live stream.
uint32_t
d3d11_lift_stream_mode(struct d3d11_lift *lift, uint64_t owner, uint64_t id);

/*!
 * The weave path's submit: snapshot region (@p x, @p y, @p w, @p h) of @p src
 * (an SRV on the service device, @p src_tw x @p src_th) into a core slot.
 * The CALLER holds the service context mutex and whatever keyed mutex guards
 * @p src. The snapshot's long edge is capped at DXR_LIFT_MAX_INPUT_EDGE
 * (default 1920, 0 = off; u_lift_cap_dims) — the result is stretched back into
 * the rect, so consumers must sample it, never assume result size == rect size.
 * The rect's letterbox bars are cropped first (DXR_LIFT_LETTERBOX). @p params
 * NULL = the rect's last options. @p view_control NULL = none carried with this
 * frame (conv_frame_submit); @p frame = the lifted rect's physical centre + size.
 * @p viewpoints / @p rig: a lifted rect's EXPLICIT viewpoints and app rig
 * (XR_DXR_lift v3).
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
