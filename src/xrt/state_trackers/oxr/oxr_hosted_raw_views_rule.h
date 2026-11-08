// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #2322: which hosted sessions take the RAW view contract when the
 *         process opted in with DXR_HOSTED_RAW_VIEWS=1.
 *
 * A hosted session (the runtime owns the window) gets render-ready views by
 * default, extension app or not (ADR-024 Amendment 1, point 3). A launcher
 * that runs a display-aware Windows app under a translation layer - an app that
 * does its own view math but cannot hand over a native window - can opt that
 * session into the RAW contract of an external-window session. This header is
 * the eligibility rule, pure so it is host-testable
 * (tests/tests_oxr_hosted_raw_views_rule.cpp); oxr_session_create() feeds it.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * True when a session takes the RAW view contract through the hosted opt-in.
 *
 * @param opted_in             DXR_HOSTED_RAW_VIEWS is set.
 * @param has_external_window  The app bound its own window / surface / texture
 *                             (already RAW; the opt-in does not apply).
 * @param is_bridge_relay      A headless bridge relay (already RAW).
 * @param service_mode         The session runs over IPC: the server's per-view
 *                             poses for a hosted client are head-local, so the
 *                             client cannot pass them through as plane-relative.
 * @param display_info_enabled The app enabled XR_DXR_display_info. A legacy app
 *                             has no use for display-relative eyes.
 */
static inline bool
oxr_hosted_raw_views_eligible(
    bool opted_in, bool has_external_window, bool is_bridge_relay, bool service_mode, bool display_info_enabled)
{
	return opted_in && !has_external_window && !is_bridge_relay && !service_mode && display_info_enabled;
}

#ifdef __cplusplus
}
#endif
