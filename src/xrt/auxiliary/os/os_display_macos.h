// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS, aux_os-internal: the CoreGraphics display walk shared by the
 *         desktop resolver (os_display_desktop_macos.c) and the EDID
 *         enumerator (os_display_edid_macos.c), plus the pure EDID <-> display
 *         match, exposed so a host test can pin it without a display.
 * @ingroup aux_os
 */

#pragma once

#include "os_display_desktop.h"
#include "os_display_edid_parse.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * The active CoreGraphics displays (`CGDirectDisplayID`s), main display
 * first, with displays that mirror another dropped (a mirror set is one
 * placement target).
 *
 * @return the number of ids written.
 */
uint32_t
os_display_macos_list_displays(uint32_t *out_ids, uint32_t max_ids);

/*!
 * One display's desktop record: rect in top-down points, backing-pixel mode in
 * `native_width`/`width_in_caller_dpi`, `scale` = pixels / points, EDID mm
 * (via `CGDisplayScreenSize`), and the display UUID as `device_name`.
 *
 * @return false when the display has no bounds (gone mid-call).
 */
bool
os_display_macos_fill_desktop_info(uint32_t display_id, struct os_display_desktop_info *out_info);

/*!
 * CoreGraphics' vendor number (`CGDisplayVendorNumber`) is the EDID PNP id as
 * a big-endian value ("SAM" = 0x4C2D); @ref os_display_edid_monitor stores
 * EDID bytes 8-9 loaded little-endian, as Windows and Linux do. Swap.
 */
static inline uint16_t
os_display_macos_cg_vendor_to_edid_raw(uint32_t cg_vendor)
{
	return (uint16_t)(((cg_vendor & 0xFFu) << 8) | ((cg_vendor >> 8) & 0xFFu));
}

/*!
 * Pick the EDID belonging to a display, from CoreGraphics' identity numbers.
 *
 * A candidate matches on manufacturer (after @ref
 * os_display_macos_cg_vendor_to_edid_raw) and product (`CGDisplayModelNumber`
 * is EDID bytes 10-11). When the display has a serial, the candidate must carry
 * the same one; when it has none (0), or the EDID has none, manufacturer +
 * product decide. Candidates already marked in @p used are skipped, so two
 * identical serial-less monitors take one blob each instead of both taking the
 * first.
 *
 * Pure: no I/O.
 *
 * @return the candidate index, or -1 when none matches.
 */
int32_t
os_display_macos_match_edid(uint32_t cg_vendor,
                            uint32_t cg_model,
                            uint32_t cg_serial,
                            const struct os_display_edid_parsed *cands,
                            const bool *used,
                            uint32_t cand_count);

#ifdef __cplusplus
}
#endif
