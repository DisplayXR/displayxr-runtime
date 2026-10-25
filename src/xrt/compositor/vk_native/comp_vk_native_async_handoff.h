// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Which frames may take the non-blocking xrEndFrame hand-off (#1905, #2356).
 *
 * The in-process vk_native compositor on Android may return from xrEndFrame
 * before the frame's weave has run (#1905 ask 2). That is only safe when the
 * served weave reads nothing the app may overwrite once xrEndFrame returns: the
 * runtime atlas and compositor state are protected by the depth-1 drain at the
 * next layer_commit, but the app's own swapchain images are not.
 *
 * #2356: window-space layers used to be composited into the atlas AT WEAVE
 * TIME, straight from the app's layer swapchain image, so every frame carrying
 * one stayed blocking. They are now composited at layer_commit instead, into
 * the runtime-owned atlas: submitted on the serve's own queue (queue order puts
 * it before the weave) with no host wait, while the app's reuse of the layer
 * image is held at xrWaitSwapchainImage until that submit retires (the #2095
 * backpressure). A frame whose window-space layers were composited at commit
 * therefore reads only runtime-owned images at weave time and may go
 * non-blocking.
 *
 * Local2D, zones and an active zone mask still read app images from the served
 * weave (the flatten / masked composite), and a zero-copy frame weaves the
 * app's own image, so those stay blocking.
 *
 * Pure logic, no Vulkan: pinned by tests/tests_comp_vk_async_handoff.cpp.
 *
 * @ingroup comp_vk_native
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

//! The facts about one committed frame that decide its hand-off.
struct comp_vk_native_async_frame
{
	bool enabled;                //!< hand-off switch resolved ON (dxr_async_handoff_resolve)
	bool runtime_queue;          //!< a runtime-owned queue + pool exist for the serve
	bool zero_copy;              //!< the DP weaves the app's own swapchain image
	bool local_2d;               //!< the frame carries Local2D layers (flattened at weave time)
	bool zones;                  //!< the frame carries display zones
	bool zone_mask;              //!< an active zone mask (masked composite at weave time)
	bool has_window_space;       //!< the frame carries window-space layers
	bool window_space_at_commit; //!< ...and they were composited into the atlas at commit
};

/*!
 * Everything except window-space layers allows the non-blocking hand-off.
 * This is also the condition under which compositing the window-space layers
 * at commit is worth doing: when it is false the frame blocks anyway, and the
 * weave keeps compositing them as it always did.
 */
static inline bool
comp_vk_native_async_base_eligible(const struct comp_vk_native_async_frame *f)
{
	return f->enabled && f->runtime_queue && !f->zero_copy && !f->local_2d && !f->zones && !f->zone_mask;
}

//! Should layer_commit composite this frame's window-space layers itself?
static inline bool
comp_vk_native_async_composite_ws_at_commit(const struct comp_vk_native_async_frame *f)
{
	return f->has_window_space && comp_vk_native_async_base_eligible(f);
}

/*!
 * May THIS frame's weave run without the app waiting for it? A frame with
 * window-space layers qualifies only when they were composited at commit — the
 * served weave then never touches the app's layer swapchain images.
 */
static inline bool
comp_vk_native_async_frame_eligible(const struct comp_vk_native_async_frame *f)
{
	if (!comp_vk_native_async_base_eligible(f)) {
		return false;
	}
	return !f->has_window_space || f->window_space_at_commit;
}

#ifdef __cplusplus
}
#endif
