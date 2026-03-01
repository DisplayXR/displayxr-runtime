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
 * Factory function for creating a Leia SR D3D11 display processor.
 *
 * Matches the @ref xrt_dp_factory_d3d11_fn_t signature.
 * Creates an SR D3D11 weaver internally and owns it for the lifetime
 * of the display processor.
 *
 * Set this as dp_factory_d3d11 in xrt_system_compositor_info from
 * the Leia target builder.
 */
xrt_result_t
leia_dp_factory_d3d11(void *d3d11_device,
                      void *d3d11_context,
                      void *window_handle,
                      struct xrt_display_processor_d3d11 **out_xdp);

/*!
 * Create an @ref xrt_display_processor_d3d11 that wraps a Leia SR SDK
 * D3D11 weaver (leiasr_d3d11_set_input_texture + leiasr_d3d11_weave).
 *
 * Legacy API — the processor takes ownership of the leiasr_d3d11 instance
 * and will destroy it when the processor is destroyed.
 *
 * @param leiasr  Existing SR D3D11 weaver (ownership transferred to processor).
 * @param[out] out_xdp  Receives the created display processor.
 * @return XRT_SUCCESS on success.
 */
xrt_result_t
leia_display_processor_d3d11_create(struct leiasr_d3d11 *leiasr,
                                    struct xrt_display_processor_d3d11 **out_xdp);

#ifdef __cplusplus
}
#endif
