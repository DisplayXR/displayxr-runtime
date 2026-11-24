// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The rule behind "the compositor moved the rendering mode": what an
 *         in-process session is told when the head device's active mode is
 *         not the one the session last reported (#2801).
 *
 * The runtime's mode keys (V, '0', '1', ADR-065) are applied by the in-process
 * compositor that owns the window: it writes the head device's
 * `active_rendering_mode_index` and asks the display processor for the
 * hardware state. An in-process native compositor owns no session event sink,
 * so the state tracker is the one that tells the app, by comparing that index
 * with the one it last reported, from xrPollEvent. This file is that
 * comparison as a pure function, so it can be pinned without an instance, a
 * device and a compositor (tests_oxr_mode_observed_rule.cpp).
 *
 * A service-mode session is not covered here: its head device is the IPC
 * proxy and the service tells it with XRT_SESSION_EVENT_RENDERING_MODE_CHANGE.
 *
 * @ingroup oxr_main
 */

#pragma once

#include "xrt/xrt_device.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! What the session does about the device's active mode, see
//! @ref oxr_mode_observed_decide.
struct oxr_mode_observed_decision
{
	//! Record @ref current as the last reported mode (false = nothing moved).
	bool sync;
	//! Send XrEventDataRenderingModeChangedDXR (@ref previous -> @ref current).
	bool push_mode_event;
	uint32_t previous;
	uint32_t current;
	//! Record @ref hardware_3d as the session's hardware 2D/3D state.
	bool sync_hardware;
	//! Send XrEventDataHardwareDisplayStateChangedDXR (@ref hardware_3d).
	bool push_hardware_event;
	bool hardware_3d;
};

/*!
 * @param modes, count        the head device's mode table (NULL / 0 = none).
 * @param last                the mode index the session last reported.
 * @param active              the device's active mode index now.
 * @param session_hardware_3d the session's hardware 2D/3D state.
 * @param in_process          false for a service-mode session: nothing is
 *                            decided, the service's events own it.
 * @param running             the session is between xrBeginSession and
 *                            xrEndSession. A mode that moves outside that
 *                            (the end-of-session drop to 2D, a restore at
 *                            begin) is recorded and not reported.
 * @param events_enabled      the app enabled XR_DXR_display_info; without it
 *                            the state is recorded and no event is sent.
 * @param hardware_held_2d    the runtime holds the panel 2D for its own reason
 *                            (window hidden, auto-2D): the hardware state is
 *                            recorded, and reported when that hold ends.
 */
static inline struct oxr_mode_observed_decision
oxr_mode_observed_decide(const struct xrt_rendering_mode *modes,
                         uint32_t count,
                         uint32_t last,
                         uint32_t active,
                         bool session_hardware_3d,
                         bool in_process,
                         bool running,
                         bool events_enabled,
                         bool hardware_held_2d)
{
	struct oxr_mode_observed_decision d = {false, false, 0, 0, false, false, false};
	if (!in_process || active == last) {
		return d; // never an event with previous == current
	}
	d.sync = true;
	d.previous = last;
	d.current = active;
	if (!running) {
		return d;
	}
	d.push_mode_event = events_enabled;
	if (modes == NULL || active >= count) {
		return d; // no table entry to read the hardware state from
	}
	d.hardware_3d = modes[active].hardware_display_3d;
	if (d.hardware_3d != session_hardware_3d) {
		d.sync_hardware = true;
		d.push_hardware_event = events_enabled && !hardware_held_2d;
	}
	return d;
}

#ifdef __cplusplus
}
#endif
