// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The S7 convergence observer (ADR-053 decision 4, roadmap
 *         conversion-providers.md §5.1): rt#1978's runtime auto-convergence
 *         (util/u_lift_autoconv.h) behind the conv_observer interface.
 *
 * Only the CALL SITE moved (#2003): d3d11_lift's lift thread fed the
 * estimator from its own staging readback; the conversion core now feeds it
 * from S6's CPU view of the same-inference depth. The estimator, its
 * parameters and its filter are u_lift_autoconv's, untouched:
 *
 *  - observe: decode the samples (inverse depth for the *_INVERSE /
 *    *_DISPARITY kinds, 1/d otherwise), grid them, find the centre-weighted
 *    foreground mode (with the subject hysteresis the lift thread used), and
 *    feed the filter;
 *  - value: the filter's current plane, [0,1] relative depth at the glass
 *    (convergence = 1 − h(subject)), false until the first measurement;
 *  - reset: FRAME_RESET, a rebind, stream create (the core calls it).
 *
 * The core's conv_policy_observe already drops anything that is not aux depth
 * of the same inference (same_inference = 1).
 *
 * @ingroup aux_conversion
 */

#pragma once

#include "conv_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Create one observer (one per stream). Returns false when
 * DXR_LIFT_AUTO_CONVERGENCE selects the module's own auto-convergence
 * (u_lift_autoconv_source_parse: "module" / "off" / "0"), or on allocation
 * failure. Read the environment once per call: cheap, a stream create.
 */
bool
conv_observer_autoconv_create(struct conv_observer *out);

/*! conv_core_config::create_convergence_observer adapter (@p ud unused). */
bool
conv_observer_autoconv_factory(void *ud, struct conv_observer *out);

#ifdef __cplusplus
}
#endif
