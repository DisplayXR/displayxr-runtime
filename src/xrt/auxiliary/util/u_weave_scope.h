// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  One-time reporting of a display processor's weave scope.
 *
 * Header-only. Every compositor queries the DP's @ref xrt_dp_weave_scope once
 * at DP setup and routes it through here, so the log line a support engineer
 * greps for is identical on every backend.
 *
 * The second message is the point of the whole mechanism: a DP whose hardware
 * transforms the entire scanout cannot produce a correct windowed frame, and
 * before this existed the only symptom was crosstalk on a panel with nothing
 * in any log to explain it.
 *
 * @author David Fattal
 * @ingroup aux_util
 */

#pragma once

#include "xrt/xrt_display_scanout.h"

#include "util/u_logging.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Log @p scope once for a compositor backend and return it unchanged.
 *
 * @param scope         The scope the DP declared (already clamped).
 * @param ctx           Short backend name for the log line, e.g. "D3D11".
 * @param panel_scoped  True when this backend presents the whole panel
 *                      (fullscreen, panel-native composition), false when its
 *                      output is a window's client area.
 * @return @p scope, so call sites can cache in one statement.
 *
 * @ingroup aux_util
 */
static inline enum xrt_dp_weave_scope
u_weave_scope_report(enum xrt_dp_weave_scope scope, const char *ctx, bool panel_scoped)
{
	// One-off setup event — WARN per docs/reference/debug-logging.md.
	U_LOG_W("%s DP weave scope: %s (output is %s)", ctx, xrt_dp_weave_scope_name(scope),
	        panel_scoped ? "panel-scoped" : "canvas-scoped");

	if (xrt_dp_weave_scope_needs_panel(scope) && !panel_scoped) {
		U_LOG_W("%s DP declares scanout-scoped weave but this path presents a WINDOW. Its hardware "
		        "transforms the entire frame, so everything else on the panel is de-packed along with the "
		        "app and the 3D will not resolve. Run the panel under a workspace controller (fullscreen, "
		        "panel-native composition) or use a plug-in that declares region scope.",
		        ctx);
	}

	return scope;
}

#ifdef __cplusplus
}
#endif
