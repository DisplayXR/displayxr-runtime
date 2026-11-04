// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-061 arm 2: the in-process share tap hook surface (see
 *         comp_xbridge_share_tap.h). Portable; the Windows ring is pending.
 * @ingroup comp_xbridge
 */

#include "comp_xbridge_share_tap.h"

#include "xrt/xrt_config_os.h"
#include "xrt/xrt_stereo_camera.h"

#include "util/u_share_link.h"

#include <stddef.h>

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
#ifdef XRT_OS_WINDOWS
	// TODO(ADR-061 arm 2, Windows): the fill arm's ring (comp_xbridge_share_tap.h
	// "Contract"). Until it lands this session is "no tap", like every other
	// platform — the service lists it UNAVAILABLE with the reason.
#endif
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
	// TODO(ADR-061 arm 2, Windows): steps 1-6 of the contract.
	return XRT_ERROR_FEATURE_NOT_SUPPORTED;
}

void
xbridge_share_tap_end(struct comp_xbridge_share_tap **tap_ptr)
{
	if (tap_ptr == NULL) {
		return;
	}
	// TODO(ADR-061 arm 2, Windows): release the ring and fence, free the tap.
	*tap_ptr = NULL;
	u_share_link_set_tap(false, XRT_APP_SHARE_UNAVAILABLE_NO_TAP, NULL, false);
}
