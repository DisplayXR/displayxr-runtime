// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Non-Linux-desktop stub: the X11 placement quantum is not a question
 *         on this platform, so the answer is always "unknown".
 * @ingroup aux_os
 */

#include "os/os_display_scale.h"

#include <string.h>

bool
os_display_scale_query(struct os_display_scale_report *out)
{
	if (out != NULL) {
		memset(out, 0, sizeof(*out));
		out->verdict.culprit = -1;
	}
	return false;
}

const char *
os_display_scale_state_str(enum u_x11_scale_state state)
{
	switch (state) {
	case U_X11_SCALE_DEVICE_PIXELS: return "device pixels";
	case U_X11_SCALE_QUANTIZED: return "quantized";
	case U_X11_SCALE_UNKNOWN:
	default: return "unknown";
	}
}
