// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Leia display processor: wraps SR SDK and CNSDK weavers
 *         as @ref xrt_display_processor implementations.
 * @author David Fattal
 * @ingroup drv_leia
 */

#pragma once

#include "xrt/xrt_display_processor.h"
#include "xrt/xrt_results.h"

#ifdef __cplusplus
extern "C" {
#endif

struct leiasr;

/*!
 * Create an @ref xrt_display_processor that wraps a Leia SR SDK
 * Vulkan weaver (leiasr_weave).
 *
 * The processor does NOT own the leiasr instance; the caller is
 * responsible for destroying it separately after the processor.
 *
 * @param leiasr  Existing SR Vulkan weaver (must outlive the processor).
 * @param[out] out_xdp  Receives the created display processor.
 * @return XRT_SUCCESS on success.
 */
xrt_result_t
leia_display_processor_create(struct leiasr *leiasr,
                               struct xrt_display_processor **out_xdp);

#ifdef __cplusplus
}
#endif
