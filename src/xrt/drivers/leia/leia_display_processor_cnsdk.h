// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Leia CNSDK display processor (Android): wraps the CNSDK
 *         Vulkan interlacer as an @ref xrt_display_processor.
 *
 * POC scope: factory + lifecycle + hardcoded display metrics + IPD-only
 * eye positions. process_atlas is a no-op until #126 (self_submitting
 * DP flag) lands — CNSDK's interlacer records and submits its own
 * command buffer, which doesn't fit the compositor's "record into my
 * cmd_buffer" contract.
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
