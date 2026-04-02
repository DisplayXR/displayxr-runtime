// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  EDID-based display identification: enumerate monitors and read
 *         manufacturer/product IDs for vendor display detection.
 * @ingroup aux_os
 *
 * Windows: reads EDID from the registry via SetupAPI, correlates with
 * EnumDisplayMonitors for HMONITOR handles and screen coordinates.
 *
 * Other platforms: stubs that return zero results.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OS_DISPLAY_EDID_MAX_MONITORS 16

/*!
 * EDID-derived identity for a connected monitor.
 */
struct os_display_edid_monitor
{
	uint16_t manufacturer_id; //!< EDID bytes 8-9 (raw, as stored in EDID)
	uint16_t product_id;      //!< EDID bytes 10-11 (raw, as stored in EDID)
	int32_t screen_left;      //!< Monitor left edge in virtual screen coords
	int32_t screen_top;       //!< Monitor top edge in virtual screen coords
	uint32_t pixel_width;     //!< Monitor width in pixels (current mode)
	uint32_t pixel_height;    //!< Monitor height in pixels (current mode)
	uint32_t refresh_hz;      //!< Current refresh rate in Hz
	bool is_primary;          //!< True if this is the primary monitor
	void *hmonitor;           //!< HMONITOR on Windows, NULL elsewhere
};

/*!
 * List of all connected monitors with EDID data.
 */
struct os_display_edid_list
{
	uint32_t count;
	struct os_display_edid_monitor monitors[OS_DISPLAY_EDID_MAX_MONITORS];
};

/*!
 * Enumerate all connected monitors and read their EDID data.
 *
 * On Windows, uses SetupAPI to read EDID from the registry and
 * correlates with EnumDisplayMonitors for HMONITOR handles.
 * On other platforms, sets count to 0.
 *
 * @param[out] out_list  Receives the enumerated monitors.
 * @return true if at least one monitor was enumerated.
 */
bool
os_display_edid_enumerate(struct os_display_edid_list *out_list);

/*!
 * Find the first monitor matching any entry in a vendor's EDID table.
 *
 * Each table entry is a {manufacturer_id, product_id} pair.
 *
 * @param list       Previously enumerated monitor list.
 * @param table      Array of {manufacturer_id, product_id} pairs.
 * @param table_len  Number of entries in table.
 * @return Pointer to the first matching monitor, or NULL if none found.
 */
const struct os_display_edid_monitor *
os_display_edid_find_in_table(const struct os_display_edid_list *list,
                              const uint16_t table[][2],
                              uint32_t table_len);

#ifdef __cplusplus
}
#endif
