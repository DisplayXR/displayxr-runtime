// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Leia GL display processor: wraps SR SDK GL weaver
 *         as an @ref xrt_display_processor_gl implementation.
 * @author David Fattal
 * @ingroup drv_leia
 */

#pragma once

#include "xrt/xrt_display_processor_gl.h"
#include "xrt/xrt_results.h"

#ifdef __cplusplus
extern "C" {
#endif

struct leiasr_gl;

/*!
 * Factory function for creating a Leia SR GL display processor.
 *
 * Matches the @ref xrt_dp_factory_gl_fn_t signature.
 * Creates an SR GL weaver internally and owns it for the lifetime
 * of the display processor.
 *
 * Set this as dp_factory_gl in xrt_system_compositor_info from
 * the Leia target builder.
 */
xrt_result_t
leia_dp_factory_gl(void *window_handle,
                    struct xrt_display_processor_gl **out_xdp);

#ifdef __cplusplus
}
#endif
