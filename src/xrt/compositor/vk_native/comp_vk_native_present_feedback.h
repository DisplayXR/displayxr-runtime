// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Presentation feedback from the window system — the desktop-Linux
 *         vblank-grid source for late weave (#902).
 *
 * Desktop Linux Vulkan drivers mostly expose no `VK_GOOGLE_display_timing`, so
 * the vblank grid (util/comp_vblank_grid.h) never anchored and late weave was
 * dormant on Linux. The window system reports the same two facts per present:
 *
 *  - **native Wayland** — `wp_presentation` (stable presentation-time): one
 *    `wp_presentation.feedback` is requested on the surface immediately before
 *    the WSI's `wl_surface.commit` (inside `vkQueuePresentKHR`), and its
 *    `presented` event carries the presentation timestamp, the refresh period,
 *    the vblank counter and the `vsync` / `hw_clock` flags. The protocol runs
 *    on the APP's `wl_display` (the connection the surface belongs to) through
 *    a private event queue — no second connection, and none of our events can
 *    reach the app's listeners. Same arrangement as comp_vk_native_wl_move_sync.
 *  - **X11** — the Present extension: `PresentSelectInput(CompleteNotify)` on
 *    the window the swapchain presents to, received as XGE special events on
 *    their own queue. Every swapchain present produces a `CompleteNotify` with
 *    `ust` (µs, CLOCK_MONOTONIC) and `msc` (the vblank counter); mode `SKIP` is
 *    a discard. The two requests are encoded here directly (the protocol is
 *    two fixed-size structs), so the runtime takes no libxcb-present
 *    dependency and the .deb no new soname.
 *
 * Nothing here blocks the frame thread after creation: requests are
 * fire-and-forget, and @ref comp_vk_native_present_feedback_poll reads only
 * what the socket already holds. Creation does one bounded round trip.
 *
 * @ingroup comp_vk_native
 */

#pragma once

#include "util/comp_present_feedback.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct comp_vk_native_present_feedback;

//! One sample plus the caller's tag (Wayland only; 0 on X11).
struct comp_vk_native_present_feedback_event
{
	struct comp_present_feedback_sample sample;
	//! The value passed to @ref comp_vk_native_present_feedback_request for
	//! this present — the caller uses it for the weave→scanout residual.
	uint64_t tag;
};

/*!
 * Bind `wp_presentation` on the app's @p wl_display for @p wl_surface.
 *
 * @return NULL when built without Wayland protocol support, when the
 *         compositor has no `wp_presentation`, or on any failure. The reason
 *         is logged once with the `#902` prefix.
 */
struct comp_vk_native_present_feedback *
comp_vk_native_present_feedback_create_wayland(void *wl_display, void *wl_surface);

/*!
 * Select Present `CompleteNotify` events for @p window on @p xcb_connection.
 *
 * @return NULL when the server has no Present extension or the selection
 *         fails. The reason is logged once with the `#902` prefix.
 */
struct comp_vk_native_present_feedback *
comp_vk_native_present_feedback_create_xcb(void *xcb_connection, uint32_t window);

//! "wp_presentation" or "x11-present", for log lines.
const char *
comp_vk_native_present_feedback_kind(const struct comp_vk_native_present_feedback *pf);

/*!
 * Ask for feedback on the NEXT commit of the surface. Call immediately before
 * `vkQueuePresentKHR`. A no-op on X11, where every present is reported anyway.
 */
void
comp_vk_native_present_feedback_request(struct comp_vk_native_present_feedback *pf, uint64_t tag);

/*!
 * Drain the samples that have arrived, without blocking.
 *
 * @return the number written to @p out (at most @p max). Call again if it
 *         returns @p max.
 */
uint32_t
comp_vk_native_present_feedback_poll(struct comp_vk_native_present_feedback *pf,
                                     struct comp_vk_native_present_feedback_event *out,
                                     uint32_t max);

/*!
 * #2068 feedback pacing: can @ref comp_vk_native_present_feedback_wait block on
 * a specific commit? Wayland only — X11 CompleteNotify carries no tag of ours.
 */
bool
comp_vk_native_present_feedback_can_pace(const struct comp_vk_native_present_feedback *pf);

/*!
 * #2068 feedback pacing: block until the feedback for the request tagged
 * @p tag (or a later one) has arrived — presented or discarded — or until
 * @p timeout_ns passes. Arrived samples stay queued for
 * @ref comp_vk_native_present_feedback_poll.
 *
 * @return true if it arrived in time.
 */
bool
comp_vk_native_present_feedback_wait(struct comp_vk_native_present_feedback *pf, uint64_t tag, uint64_t timeout_ns);

void
comp_vk_native_present_feedback_destroy(struct comp_vk_native_present_feedback **pf_ptr);

#ifdef __cplusplus
}
#endif
