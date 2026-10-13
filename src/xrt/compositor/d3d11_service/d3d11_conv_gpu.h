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

#include "conversion/conv_pipeline.h"
#include "conversion/conv_stream.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>

struct d3d11_lift;
struct conv_core;

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
