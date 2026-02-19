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

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct xrt_device;
struct xrt_display_processor;
struct xrt_display_processor_d3d11;
struct vk_bundle;

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
 * Get the current runtime output mode.
 *
 * Thread-safe (atomic). The display processor reads this each frame
 * to select which shader pipeline to use.
 *
 * @return Current output mode.
 * @ingroup drv_sim_display
 */
enum sim_display_output_mode
sim_display_get_output_mode(void);

/*!
 * Set the output mode at runtime.
 *
 * Thread-safe (atomic). Call from the event pump (main thread) when
 * the user presses 1/2/3 to switch modes.
 *
 * @param mode The new output mode.
 * @ingroup drv_sim_display
 */
void
sim_display_set_output_mode(enum sim_display_output_mode mode);

/*!
 * Display info for a sim_display device.
 * Used by target_instance.c to populate xrt_system_compositor_info.
 * @ingroup drv_sim_display
 */
struct sim_display_info
{
	float display_width_m;
	float display_height_m;
	float nominal_y_m;
	float nominal_z_m;
	uint32_t display_pixel_width;
	uint32_t display_pixel_height;
};

/*!
 * Query display info from a sim_display device.
 *
 * @param xdev     The device to query (must be a sim_display HMD).
 * @param out_info Receives the display info.
 * @return true on success, false if xdev is not a sim_display device.
 * @ingroup drv_sim_display
 */
bool
sim_display_get_display_info(struct xrt_device *xdev, struct sim_display_info *out_info);

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
 * Create a simulation Vulkan display processor.
 *
 * For SBS mode, @p vk and @p target_format are ignored (no Vulkan resources needed).
 * For anaglyph and blend modes, creates a full Vulkan pipeline with fragment shaders.
 *
 * @param mode          Output mode (SBS, anaglyph, or blend).
 * @param vk            Vulkan bundle (ignored for SBS mode).
 * @param target_format Swapchain target format (ignored for SBS mode).
 * @param[out] out_xdp  Receives the created display processor.
 * @return XRT_SUCCESS on success.
 * @ingroup drv_sim_display
 */
xrt_result_t
sim_display_processor_create(enum sim_display_output_mode mode,
                             struct vk_bundle *vk,
                             int32_t target_format,
                             struct xrt_display_processor **out_xdp);

/*!
 * Create a simulation D3D11 display processor.
 *
 * For SBS mode, @p d3d11_device is ignored (no shader compilation needed).
 * For anaglyph and blend modes, compiles HLSL shaders for stereo compositing.
 *
 * @param mode          Output mode (SBS, anaglyph, or blend).
 * @param d3d11_device  D3D11 device for shader compilation (ignored for SBS).
 * @param[out] out_xdp  Receives the created display processor.
 * @return XRT_SUCCESS on success.
 * @ingroup drv_sim_display
 */
xrt_result_t
sim_display_processor_d3d11_create(enum sim_display_output_mode mode,
                                   void *d3d11_device,
                                   struct xrt_display_processor_d3d11 **out_xdp);

/*!
 * Set an external device as the pose source for a sim_display HMD.
 *
 * When set, the sim_display HMD delegates get_tracked_pose to the
 * source device (e.g. a qwerty HMD for WASD/mouse camera control).
 *
 * @param sim_hmd  The sim_display HMD device.
 * @param source   The device providing pose data, or NULL to use static pose.
 * @ingroup drv_sim_display
 */
void
sim_display_hmd_set_pose_source(struct xrt_device *sim_hmd, struct xrt_device *source);

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
