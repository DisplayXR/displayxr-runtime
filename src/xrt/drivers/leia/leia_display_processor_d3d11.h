// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Leia D3D11 display processor: wraps SR SDK D3D11 weaver
 *         as an @ref xrt_display_processor_d3d11 implementation.
 * @author David Fattal
 * @ingroup drv_leia
 */

#pragma once

#include "xrt/xrt_display_processor_d3d11.h"
#include "xrt/xrt_results.h"

#ifdef __cplusplus
extern "C" {
#endif

struct leiasr_d3d11;

/*!
 * Create an @ref xrt_display_processor_d3d11 that wraps a Leia SR SDK
 * D3D11 weaver (leiasr_d3d11_set_input_texture + leiasr_d3d11_weave).
 *
 * The processor does NOT own the leiasr_d3d11 instance; the caller is
 * responsible for destroying it separately after the processor.
 *
 * @param leiasr  Existing SR D3D11 weaver (must outlive the processor).
 * @param[out] out_xdp  Receives the created display processor.
 * @return XRT_SUCCESS on success.
 */
xrt_result_t
leia_display_processor_d3d11_create(struct leiasr_d3d11 *leiasr,
                                    struct xrt_display_processor_d3d11 **out_xdp);

#ifdef __cplusplus
}
#endif
