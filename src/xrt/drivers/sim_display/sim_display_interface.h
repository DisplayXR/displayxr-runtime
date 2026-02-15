// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Public interface for the simulation 3D display driver.
 * @author David Fattal
 * @ingroup drv_sim_display
 */

#pragma once

#include "xrt/xrt_results.h"

#ifdef __cplusplus
extern "C" {
#endif

struct xrt_device;
struct xrt_display_processor;

/*!
 * @defgroup drv_sim_display Simulation 3D Display Driver
 * @ingroup drv
 *
 * @brief Simulates a tracked 3D display on any 2D screen.
 *
 * Supports three output modes selected via SIM_DISPLAY_OUTPUT env var:
 * - "sbs" (default): side-by-side left/right views
 * - "anaglyph": red-cyan anaglyph stereoscopy
 * - "blend": 50/50 alpha blend of both views
 *
 * Activated via SIM_DISPLAY_ENABLE=1 env var.
 */

/*!
 * Output mode for the simulation display processor.
 * @ingroup drv_sim_display
 */
enum sim_display_output_mode
{
	SIM_DISPLAY_OUTPUT_SBS = 0,      //!< Side-by-side stereo
	SIM_DISPLAY_OUTPUT_ANAGLYPH = 1, //!< Red-cyan anaglyph
	SIM_DISPLAY_OUTPUT_BLEND = 2,    //!< 50/50 alpha blend
};

/*!
 * Create a simulated 3D display HMD device.
 *
 * Display properties are configurable via environment variables:
 * - SIM_DISPLAY_WIDTH_M (default: 0.344)
 * - SIM_DISPLAY_HEIGHT_M (default: 0.194)
 * - SIM_DISPLAY_NOMINAL_Z_M (default: 0.65)
 * - SIM_DISPLAY_PIXEL_W (default: 1920)
 * - SIM_DISPLAY_PIXEL_H (default: 1080)
 *
 * @return A new xrt_device acting as a 3D display HMD, or NULL on failure.
 * @ingroup drv_sim_display
 */
struct xrt_device *
sim_display_hmd_create(void);

/*!
 * Create a simulation display processor.
 *
 * The processor implements side-by-side, anaglyph, or alpha-blend
 * output via simple Vulkan viewport blits (no custom shaders needed).
 *
 * @param mode  Output mode (SBS, anaglyph, or blend).
 * @param[out] out_xdp  Receives the created display processor.
 * @return XRT_SUCCESS on success.
 * @ingroup drv_sim_display
 */
xrt_result_t
sim_display_processor_create(enum sim_display_output_mode mode,
                             struct xrt_display_processor **out_xdp);

/*!
 * Create the simulation display system builder.
 *
 * Enabled via SIM_DISPLAY_ENABLE=1 environment variable.
 *
 * @return A new xrt_builder, or NULL on failure.
 * @ingroup drv_sim_display
 */
struct xrt_builder *
t_builder_sim_display_create(void);

#ifdef __cplusplus
}
#endif
