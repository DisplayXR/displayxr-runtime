// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Leia CNSDK display processor (Android): wraps the CNSDK
 *         Vulkan interlacer as an @ref xrt_display_processor.
 *
 * Advertises `is_self_submitting = true`. The compositor flushes its
 * pre-DP cmd buffer before calling process_atlas, which blits the SBS
 * atlas tiles into per-view VkImages and then calls leia_cnsdk_weave —
 * CNSDK records and submits its own command buffer internally.
 *
 * Display metrics + face-tracked eye positions come from CNSDK once the
 * async core init completes (lazy on first query). Falls back to hardcoded
 * Lume Pad 2 metrics and IPD-only eyes while the core is still booting.
 *
 * @author David Fattal
 * @ingroup drv_leia
 */

#pragma once

#include "xrt/xrt_display_processor.h"
#include "xrt/xrt_results.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Factory matching @ref xrt_dp_factory_vk_fn_t. Creates and owns a
 * leia_cnsdk handle for the lifetime of the display processor.
 */
xrt_result_t
leia_dp_factory_cnsdk(void *vk_bundle,
                      void *vk_cmd_pool,
                      void *window_handle,
                      int32_t target_format,
                      struct xrt_display_processor **out_xdp);

#ifdef __cplusplus
}
#endif
