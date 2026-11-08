// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-061 arm 2: the in-process share tap hook surface (see
 *         comp_xbridge_share_tap.h). The no-tap body for everything but
 *         Windows (comp_xbridge_share_tap_win.cpp).
 * @ingroup comp_xbridge
 */

#include "comp_xbridge_share_tap.h"

#include "xrt/xrt_config_os.h"
#include "xrt/xrt_stereo_camera.h"

#include "util/u_share_link.h"

#include <stddef.h>

#ifndef COMP_XBRIDGE_SHARE_TAP_WIN

xrt_result_t
xbridge_share_tap_begin(const struct comp_xbridge_share_tap_info *info, struct comp_xbridge_share_tap **out_tap)
{
	if (out_tap != NULL) {
		*out_tap = NULL;
	}
	uint32_t reason = XRT_APP_SHARE_UNAVAILABLE_NO_TAP;
	if (info != NULL && info->unavailable_reason != 0) {
		reason = info->unavailable_reason;
	}
	u_share_link_set_tap(false, reason, info != NULL ? info->luid : NULL, info != NULL && info->luid_valid);
	return XRT_ERROR_FEATURE_NOT_SUPPORTED;
}

xrt_result_t
xbridge_share_tap_commit(struct comp_xbridge_share_tap *tap, const struct comp_xbridge_share_tap_frame *frame)
{
	(void)frame;
	if (tap == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	return XRT_ERROR_FEATURE_NOT_SUPPORTED;
}

void
xbridge_share_tap_end(struct comp_xbridge_share_tap **tap_ptr)
{
	if (tap_ptr == NULL) {
		return;
	}
	*tap_ptr = NULL;
	u_share_link_set_tap(false, XRT_APP_SHARE_UNAVAILABLE_NO_TAP, NULL, false);
}

#endif // !COMP_XBRIDGE_SHARE_TAP_WIN
