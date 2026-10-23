// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_tracking's scripted viewer: a pure function of time (no camera).
 *
 * What the hardware-free CI gate drives `xrLocateViews` with. Pure so the
 * script can be pinned without a thread (tests/tests_viewer_arbitration.cpp).
 *
 * @ingroup drv_sim_tracking
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Which script runs.
enum sim_tracking_script_kind
{
	SIM_TRACKING_SCRIPT_SWEEP = 0,  //!< the head sweeps left-right: x = offset_x + amplitude * sin(2 pi t / period)
	SIM_TRACKING_SCRIPT_OFFSET = 1, //!< the head holds still at nominal + offset (deterministic)
};

struct sim_tracking_script
{
	enum sim_tracking_script_kind kind;
	float nominal[3];       //!< the display's nominal viewer (host geometry), display space, metres
	float offset[3];        //!< added to the nominal viewer, metres
	float amplitude_m;      //!< SWEEP half-width
	int64_t period_ns;      //!< SWEEP period (> 0)
	float ipd_m;            //!< eye separation
	int64_t loss_period_ns; //!< > 0: tracking square-waves off for half of every period (loss edges); 0 = always
	                        //!< tracking
	int64_t epoch_ns;       //!< t = 0 of the script
};

//! Parse "sweep" / "offset" (case-insensitive). False (out untouched) on anything else.
bool
sim_tracking_parse_script(const char *s, enum sim_tracking_script_kind *out);

/*!
 * The viewer at @p t_ns: left / right eye and head centre, display space,
 * metres; returns whether the script reports "tracking" at that instant.
 */
bool
sim_tracking_script_eval(
    const struct sim_tracking_script *s, int64_t t_ns, float left[3], float right[3], float head[3]);

#ifdef __cplusplus
}
#endif
