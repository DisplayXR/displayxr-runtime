// Copyright 2019-2023, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  File holding helper for @ref xrt_result_t results.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup oxr_main
 */

#pragma once

#include "xrt/xrt_results.h"
#include "oxr_objects.h"


/*!
 * Helper define to check results from 'xrt_` functions (@ref xrt_result_t) and
 * also set any needed state.
 *
 * @ingroup oxr_main
 */
#define OXR_CHECK_XRET(LOG, SESS, RESULTS, FUNCTION)                                                                   \
	do {                                                                                                           \
		xrt_result_t check_ret = (RESULTS);                                                                    \
		if (check_ret == XRT_ERROR_IPC_FAILURE) {                                                              \
			(SESS)->has_lost = true;                                                                       \
			return oxr_error(LOG, XR_ERROR_INSTANCE_LOST, "Call to " #FUNCTION " failed");                 \
		}                                                                                                      \
		if (check_ret != XRT_SUCCESS) {                                                                        \
			return oxr_error(LOG, XR_ERROR_RUNTIME_FAILURE, "Call to " #FUNCTION " failed");               \
		}                                                                                                      \
	} while (false)

/*!
 * Exactly @ref OXR_CHECK_XRET's semantics — @ref XRT_ERROR_IPC_FAILURE marks the
 * session lost and returns XR_ERROR_INSTANCE_LOST, any other error returns
 * XR_ERROR_RUNTIME_FAILURE — but with a caller-supplied printf message instead of
 * the stringified function name.
 *
 * Use this where the existing hand-rolled @ref oxr_error already carries
 * diagnostics worth keeping (the offending `xrt_result_t`, the parameter that was
 * rejected). Added for browser#103, where the XR_DXR_weave entry points had to
 * gain the has_lost semantics without losing their per-site messages.
 *
 * @ingroup oxr_main
 */
#define OXR_CHECK_XRET_MSG(LOG, SESS, RESULTS, ...)                                                                    \
	do {                                                                                                           \
		xrt_result_t check_ret = (RESULTS);                                                                    \
		if (check_ret == XRT_ERROR_IPC_FAILURE) {                                                              \
			(SESS)->has_lost = true;                                                                       \
			return oxr_error(LOG, XR_ERROR_INSTANCE_LOST, __VA_ARGS__);                                    \
		}                                                                                                      \
		if (check_ret != XRT_SUCCESS) {                                                                        \
			return oxr_error(LOG, XR_ERROR_RUNTIME_FAILURE, __VA_ARGS__);                                  \
		}                                                                                                      \
	} while (false)
