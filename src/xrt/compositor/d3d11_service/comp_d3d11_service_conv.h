// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The D3D11 service compositor's side of the conversion core
 *         (ADR-053 P1a, #2003): the GPU glue the core and the v3 route use,
 *         the `dp-lift` provider's backend, and the weave path's bridge to
 *         core-routed streams.
 *
 * A separate header (not comp_d3d11_service.h) because it pulls the
 * conversion core's types; only the IPC layer's glue installer includes it.
 *
 * Wiring (ipc_server_conv_d3d11.c, at service init, once the device exists):
 *
 *   comp_d3d11_service_conv_glue()        -> image allocation, S5 / S6 stages,
 *                                            v3 import / export, the dp-lift
 *                                            backend (the ONE lift thread)
 *   comp_d3d11_service_conv_set_bridge()  -> weave rects + pins on core ids
 *   comp_d3d11_service_set_conv_eyes_sink -> per-DP eyes into the core (G7)
 *
 * @ingroup comp_d3d11_service
 */

#pragma once

#include "xrt/xrt_compositor.h"
#include "xrt/xrt_display_metrics.h"
#include "xrt/xrt_handles.h"
#include "xrt/xrt_lift.h"

#include "conversion/conv_core.h" // CONV_ADAPTER_*
#include "conversion/conv_pipeline.h"
#include "conversion/conv_provider_legacy_dp.h"
#include "conversion/conv_stream.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct conv_core;

//! What publishing one result into a stream's export targets produced.
struct comp_d3d11_conv_published
{
	uint64_t fence_value;
	uint32_t width, height, format; //!< DXGI_FORMAT
	bool realloc;
	uint32_t depth_width, depth_height, depth_format;
	bool depth_realloc;
};

//! The glue. Every function takes @ref io_ud (the lift module).
struct comp_d3d11_conv_glue
{
	struct conv_image_alloc_ops alloc;
	struct conv_viewsyn_ops viewsyn;
	struct conv_depth_readback_ops readback;
	struct conv_dp_lift_backend dp_lift;

	xrt_result_t (*import_input)(void *ud,
	                             uint64_t stream_id,
	                             xrt_graphics_buffer_handle_t handle,
	                             bool is_dxgi,
	                             uint32_t width,
	                             uint32_t height,
	                             const struct conv_submit_slot *slot);
	//! The client's input as packed RGBA8 CPU pixels (malloc'd; the GAUSSIANS facade).
	xrt_result_t (*import_input_cpu)(void *ud,
	                                 uint64_t stream_id,
	                                 xrt_graphics_buffer_handle_t handle,
	                                 bool is_dxgi,
	                                 uint32_t width,
	                                 uint32_t height,
	                                 uint8_t **out_pixels);
	void (*release_input)(void *ud, xrt_graphics_buffer_handle_t handle, bool is_dxgi);
	//! #2030: the input's content key (PHOTO dedupe); 0 = unknown. Optional.
	uint64_t (*input_content_key)(void *ud, uint64_t stream_id, const struct conv_submit_slot *slot);
	xrt_result_t (*publish_result)(void *ud,
	                               uint64_t stream_id,
	                               const struct conv_result *res,
	                               struct comp_d3d11_conv_published *out);
	bool (*export_output)(void *ud,
	                      uint64_t stream_id,
	                      bool depth,
	                      xrt_graphics_buffer_handle_t *out_handle,
	                      uint32_t *out_width,
	                      uint32_t *out_height,
	                      uint32_t *out_format);
	bool (*export_fence)(void *ud, uint64_t stream_id, xrt_graphics_sync_handle_t *out_handle);
	void (*stream_gone)(void *ud, uint64_t stream_id);
	//! Spec 4 (#2054): any input kind over a region; CPU export.
	xrt_result_t (*import_input_ex)(void *ud,
	                                uint64_t stream_id,
	                                const struct conv_input_import *in,
	                                const struct conv_submit_slot *slot);
	bool (*export_cpu)(void *ud,
	                   uint64_t stream_id,
	                   bool copy,
	                   xrt_graphics_buffer_handle_t *out_handle,
	                   uint32_t *out_width,
	                   uint32_t *out_height,
	                   uint32_t *out_format,
	                   uint64_t *out_bytes,
	                   bool *out_realloc);
	void *io_ud;

	//! The display's nominal viewing distance, metres (the policy's straight-on viewer).
	float nominal_z_m;

	//! LUID of the adapter the glue's images live on (the service's ingest
	//! adapter); conversion-host satellites open their device on it (#2034).
	uint64_t adapter_luid;
	//! CONV_ADAPTER_* of that adapter (ADR-058 Amendment 1: the late output's
	//! default size; integrated = under 512 MiB of dedicated video memory).
	uint32_t adapter_class;
};

/*!
 * Build the glue for @p xsysc (creates the lift module if needed: cheap, its
 * device and DP come up on the lift thread on first demand). False when
 * @p xsysc is not the D3D11 service compositor or has no device.
 */
bool
comp_d3d11_service_conv_glue(struct xrt_system_compositor *xsysc, struct comp_d3d11_conv_glue *out);

//! The weave path's view of core-routed streams.
struct comp_d3d11_conv_bridge
{
	struct conv_core *core;
	bool (*resolve)(void *ud, uint64_t owner, uint64_t wire_id, uint64_t *out_core_id, uint32_t *out_mode);
	//! Spec 4: the weave's view count for a lifted rect's stream (may be NULL).
	void (*match_weave)(void *ud, uint64_t owner, uint64_t wire_id, uint32_t views);
	void *ud;
};

//! Install (NULL: clear) the bridge on @p xsysc's lift module.
void
comp_d3d11_service_conv_set_bridge(struct xrt_system_compositor *xsysc, const struct comp_d3d11_conv_bridge *bridge);

/*!
 * ADR-048: the lifted rect of a v3 submit — the session's window (centre +
 * size, display space, metres); the panel (valid = false) for a headless
 * connection (@p xc NULL). What svc_lift rebased viewpoints to.
 */
void
comp_d3d11_service_conv_rect_frame(struct xrt_system_compositor *xsysc,
                                   struct xrt_compositor *xc,
                                   struct xrt_lift_rect_frame *out);

/*!
 * G7: where the service publishes predicted eyes for the conversion core, one
 * snapshot per display processor (roadmap §5.2). Every eye query the service
 * answers publishes under the key of the DP that answered it (the DP's
 * address, never dereferenced by the core); @p retire runs when a client's DP
 * is destroyed. The same key reaches a stream through
 * xrt_lift_rect_frame::eyes_dp_key, so a stream reads the eyes of the DP that
 * owns its rect. NULL clears. Process-wide.
 */
void
comp_d3d11_service_set_conv_eyes_sink(void (*publish)(void *ud, uint64_t dp_key, const struct xrt_eye_positions *eyes),
                                      void (*retire)(void *ud, uint64_t dp_key),
                                      void *ud);

#ifdef __cplusplus
}
#endif
