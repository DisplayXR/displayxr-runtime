// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Public interface for the Leia 3D display driver.
 * @author David Fattal
 * @ingroup drv_leia
 */

#pragma once

#include "xrt/xrt_defines.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @defgroup drv_leia Leia 3D Display Driver
 * @ingroup drv
 *
 * @brief Driver for Leia light field displays using SR SDK (Windows)
 *        and CNSDK (Android).
 */

/*!
 * Cached results from an SR hardware probe.
 *
 * @ingroup drv_leia
 */
struct leiasr_probe_result
{
	bool hw_found;        //!< True if SR hardware was detected
	uint32_t pixel_w;     //!< Native display width in pixels
	uint32_t pixel_h;     //!< Native display height in pixels
	float refresh_hz;     //!< Display refresh rate in Hz
	float display_w_m;    //!< Physical display width in meters
	float display_h_m;    //!< Physical display height in meters
	float nominal_z_m;    //!< Nominal viewer distance in meters
};

/*!
 * Probe for SR display hardware.
 *
 * Creates a temporary SR context and checks for an active SR display.
 * Results are cached in statics for later retrieval via
 * leiasr_get_probe_results().
 *
 * On non-SR builds this always returns false.
 *
 * @param timeout_seconds Maximum time to wait for SR context creation.
 * @return true if SR hardware is present and responsive.
 *
 * @ingroup drv_leia
 */
bool
leiasr_probe_display(double timeout_seconds);

/*!
 * Retrieve cached probe results from a prior leiasr_probe_display() call.
 *
 * @param[out] out Probe result struct to fill in.
 * @return true if cached results are available.
 *
 * @ingroup drv_leia
 */
bool
leiasr_get_probe_results(struct leiasr_probe_result *out);

/*!
 * Create a Leia system builder.
 *
 * The builder will detect Leia 3D display hardware and create the
 * appropriate devices and display processors for the platform:
 * - Windows: SR SDK for Vulkan/D3D11 weaving + eye tracking
 * - Android: CNSDK for Vulkan interlacing
 *
 * @return A new xrt_builder, or NULL on failure.
 *
 * @ingroup drv_leia
 */
struct xrt_builder *
t_builder_leia_create(void);

/*!
 * Create a Leia 3D display HMD device.
 *
 * Uses cached probe results from leiasr_probe_display() when available,
 * otherwise falls back to hardcoded defaults.
 *
 * @return A new xrt_device, or NULL on failure.
 *
 * @ingroup drv_leia
 */
struct xrt_device *
leia_hmd_create(void);

#ifdef __cplusplus
}
#endif
