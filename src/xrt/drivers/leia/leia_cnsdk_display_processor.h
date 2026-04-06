// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  CNSDK display processor: wraps CNSDK Vulkan interlacer
 *         as an @ref xrt_display_processor.
 * @author David Fattal
 * @ingroup drv_leia
 */

#pragma once

#include "xrt/xrt_display_processor.h"
#include "xrt/xrt_results.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Factory function for creating a CNSDK Vulkan display processor.
 *
 * Matches the @ref xrt_dp_factory_vk_fn_t signature.
 * Creates a CNSDK core + interlacer internally and owns them
 * for the lifetime of the display processor.
 *
 * This DP sets self_submitting = true because CNSDK manages
 * its own Vulkan command buffers and queue submission.
 */
xrt_result_t
leia_cnsdk_dp_factory_vk(void *vk_bundle,
                          void *vk_cmd_pool,
                          void *window_handle,
                          int32_t target_format,
                          struct xrt_display_processor **out_xdp);

#ifdef __cplusplus
}
#endif
