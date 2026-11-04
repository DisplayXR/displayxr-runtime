// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-061 arm 2: the process-wide hand-off between an in-process
 *         compositor's share tap and the session's service link.
 *
 * The share tap lives in the compositor (compositor/xbridge/
 * comp_xbridge_share_tap.h); the service link lives in the IPC client and is
 * owned by the session (ipc_client_service_link.h). The compositor layer may
 * not depend on IPC, so the session installs a SINK here at xrBeginSession and
 * removes it at xrEndSession / session destroy, and the tap reaches the link
 * only through the forwarding calls below. Every call holds this unit's mutex
 * for its duration, so after u_share_link_uninstall() returns no call is
 * still inside the sink.
 *
 * One sink per process: an in-process OpenXR app runs one session at a time.
 * A second install replaces the first (logged).
 *
 * The tap state (set_tap) is remembered across installs: the compositor
 * decides it at create, before xrBeginSession installs the sink.
 *
 * Portable C, no graphics API.
 *
 * @ingroup aux_util
 */

#pragma once

#include "xrt/xrt_handles.h"
#include "xrt/xrt_results.h"
#include "xrt/xrt_share_link.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! What the session's service link offers the compositor (all required).
struct u_share_link_sink
{
	void (*set_tap)(
	    void *ctx, bool available, uint32_t unavailable_reason, const uint8_t luid[XRT_LUID_SIZE], bool luid_valid);
	bool (*poll)(void *ctx, struct xrt_share_link_poll *out);
	xrt_result_t (*publish_ring)(void *ctx,
	                             const struct xrt_share_link_ring_desc *desc,
	                             const xrt_graphics_buffer_handle_t *textures,
	                             uint32_t texture_count,
	                             xrt_graphics_sync_handle_t fence);
	xrt_result_t (*frame_ready)(void *ctx,
	                            const struct xrt_share_link_frame *frame,
	                            struct xrt_share_link_poll *out_poll);
};

//! Install @p sink (copied) with @p ctx; replays the remembered tap state into it.
void
u_share_link_install(const struct u_share_link_sink *sink, void *ctx);

//! Remove the sink installed with @p ctx (no-op for another ctx). Waits out an in-flight call.
void
u_share_link_uninstall(void *ctx);

//! Is a sink installed?
bool
u_share_link_installed(void);

/*!
 * The compositor's tap flag and adapter (@p unavailable_reason: enum
 * xrt_app_share_availability when not available). Remembered, and forwarded
 * when a sink is installed.
 */
void
u_share_link_set_tap(bool available, uint32_t unavailable_reason, const uint8_t luid[XRT_LUID_SIZE], bool luid_valid);

//! The last set_tap state (for tests and diagnostics).
void
u_share_link_get_tap(bool *out_available, uint32_t *out_unavailable_reason);

//! The link's answer (see ipc_client_service_link_share_poll); false / NONE without a sink.
bool
u_share_link_poll(struct xrt_share_link_poll *out);

//! XRT_ERROR_FEATURE_NOT_SUPPORTED without a sink.
xrt_result_t
u_share_link_publish_ring(const struct xrt_share_link_ring_desc *desc,
                          const xrt_graphics_buffer_handle_t *textures,
                          uint32_t texture_count,
                          xrt_graphics_sync_handle_t fence);

//! XRT_ERROR_FEATURE_NOT_SUPPORTED without a sink.
xrt_result_t
u_share_link_frame_ready(const struct xrt_share_link_frame *frame, struct xrt_share_link_poll *out_poll);

/*!
 * The single-report rule of the share poll (ADR-061 arm 2), shared by the
 * client link and its tests: answers from the service (polls and every
 * frame_ready reply) are MERGED in, the consumer TAKES them out. A
 * SHARE_STOPPED is reported to the consumer exactly once: a later NONE never
 * hides it before it was taken, taking it turns the cache to NONE, and the
 * same STOPPED arriving again (by poll AND reply) is not reported twice. A new
 * SHARE_REQUESTED (a new stream) supersedes an unread STOPPED — the consumer
 * re-creates its ring for the new stream generation either way.
 */
struct u_share_link_poll_cache
{
	struct xrt_share_link_poll poll;
	bool valid;
	//! The stream generation of the last STOPPED taken: a repeat of it is never reported again.
	uint32_t stopped_taken_generation;
	bool stopped_taken;
};

void
u_share_link_poll_cache_reset(struct u_share_link_poll_cache *c);

void
u_share_link_poll_cache_merge(struct u_share_link_poll_cache *c, const struct xrt_share_link_poll *in);

//! false (and @p out NONE) until the first merge.
bool
u_share_link_poll_cache_take(struct u_share_link_poll_cache *c, struct xrt_share_link_poll *out);

//! Tests: forget the sink and the tap state.
void
u_share_link_reset(void);

#ifdef __cplusplus
}
#endif
