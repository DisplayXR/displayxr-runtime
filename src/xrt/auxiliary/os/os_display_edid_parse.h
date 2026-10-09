// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  EDID base-block parser shared by the desktop-Linux (DRM sysfs) and
 *         macOS (IOKit) monitor enumerators. Pure: no I/O, no logging.
 * @ingroup aux_os
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * What the runtime reads out of one EDID base block.
 */
struct os_display_edid_parsed
{
	uint16_t manufacturer_id; //!< Bytes 8-9, raw (little-endian load), as on Windows.
	uint16_t product_id;      //!< Bytes 10-11, little-endian.
	uint32_t serial_number;   //!< Bytes 12-15, little-endian; 0 = none.
	uint32_t cm_width_mm;     //!< Byte 21 x 10; 0 when absent or an aspect-ratio encoding.
	uint32_t cm_height_mm;    //!< Byte 22 x 10; as above.
	uint32_t dtd_width_mm;    //!< First detailed timing's image size; 0 = none.
	uint32_t dtd_height_mm;
	uint32_t dtd_width_px; //!< First detailed timing's active pixels; 0 = none.
	uint32_t dtd_height_px;
	uint32_t dtd_refresh_mhz; //!< First detailed timing's refresh, milli-Hz; 0 = none.

	//! Monitor name from the 0xFC display descriptor, trailing LF/spaces
	//! stripped, NUL-terminated; "" when the block carries none.
	char monitor_name[14];
};

/*!
 * Parse an EDID base block. Checks the 8-byte header and the length (>= 128);
 * the checksum is not enforced (sysfs and IOKit hand over what the sink sent,
 * and a bad checksum still carries a usable vendor id).
 *
 * @return false on a short or headerless blob; @p out is zeroed either way.
 */
bool
os_display_edid_parse(const uint8_t *edid, size_t len, struct os_display_edid_parsed *out);

/*!
 * Best physical size from a parsed EDID: the detailed timing's mm when it is
 * present, else the base block's cm. 0 when neither is.
 */
void
os_display_edid_parsed_mm(const struct os_display_edid_parsed *p, uint32_t *out_w_mm, uint32_t *out_h_mm);

#ifdef __cplusplus
}
#endif
