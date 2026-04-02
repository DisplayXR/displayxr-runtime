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
 * EDID-based probe result for Leia/Dimenco display identification.
 *
 * Three-layer detection:
 * - hw_found: EDID manufacturer+product ID matched a known display panel
 * - sdk_installed: SR SDK registry key exists (HKLM\SOFTWARE\Dimenco\Simulated Reality)
 * - service_running: SRService shared memory is active (Global\sharedDeviceSerialMemory)
 *
 * @ingroup drv_leia
 */
struct leia_display_probe_result
{
	bool hw_found;        //!< EDID matched a known Leia/Dimenco 3D display
	bool sdk_installed;   //!< SR SDK is installed on this machine
	bool service_running; //!< SRService is running with devices connected
	uint16_t manufacturer_id; //!< EDID manufacturer ID of matched display
	uint16_t product_id;      //!< EDID product ID of matched display
	uint32_t pixel_w;     //!< Display width in pixels
	uint32_t pixel_h;     //!< Display height in pixels
	float refresh_hz;     //!< Display refresh rate in Hz
	int32_t screen_left;  //!< Monitor left edge in virtual screen coords
	int32_t screen_top;   //!< Monitor top edge in virtual screen coords
	void *hmonitor;       //!< HMONITOR handle (Windows only, NULL elsewhere)
};

/*!
 * Probe for Leia/Dimenco 3D displays using EDID matching.
 *
 * Does NOT require the SR SDK. Uses Windows SetupAPI to read EDID from
 * the monitor registry and matches against a table of known display panels.
 * Also checks for SR runtime availability (registry + shared memory).
 *
 * Results are cached for later retrieval via leia_edid_get_cached_result().
 *
 * @param[out] out Probe result struct to fill in.
 * @return true if a known Leia/Dimenco 3D display was found.
 *
 * @ingroup drv_leia
 */
bool
leia_edid_probe_display(struct leia_display_probe_result *out);

/*!
 * Retrieve cached EDID probe results from a prior leia_edid_probe_display().
 *
 * @param[out] out Probe result struct to fill in.
 * @return true if cached results are available and a display was found.
 *
 * @ingroup drv_leia
 */
bool
leia_edid_get_cached_result(struct leia_display_probe_result *out);

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

/*!
 * Set an optional external pose source for the Leia HMD.
 *
 * When set, leia_hmd_get_tracked_pose() delegates to this device
 * (e.g. qwerty HMD for WASD/mouse camera control).
 *
 * @param leia_dev  The Leia HMD device returned by leia_hmd_create().
 * @param source    The pose source device, or NULL to disable delegation.
 *
 * @ingroup drv_leia
 */
void
leia_hmd_set_pose_source(struct xrt_device *leia_dev, struct xrt_device *source);

#ifdef __cplusplus
}
#endif
