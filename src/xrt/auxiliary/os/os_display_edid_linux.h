// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Desktop Linux, aux_os-internal: the pieces behind
 *         @ref os_display_edid_enumerate — an EDID parser, the DRM sysfs
 *         reader and the RandR <-> DRM join. Exposed so host tests can pin
 *         them without a display.
 * @ingroup aux_os
 *
 * ## Why a join at all
 *
 * Neither source alone is a monitor record. RandR (through
 * `os_display_desktop_enumerate`) knows where a monitor is on the desktop and
 * which one is primary, but under XWayland it publishes no EDID property. DRM
 * sysfs has the EDID blob and the connector name, but no desktop position. So
 * each RandR monitor is tied to one DRM connector, in this order:
 *
 * 1. **name** — the RandR output name equals the DRM connector name once the
 *    kernel's subtype letter is dropped ("HDMI-A-1" -> "HDMI-1"). This is the
 *    normal case on X11 and on Mutter's XWayland.
 * 2. **mm** — exactly one unused connector whose EDID physical size is within
 *    @ref OS_DISPLAY_EDID_JOIN_MM_TOLERANCE of RandR's mm.
 * 3. **mode** — exactly one unused connector that has the monitor's pixel size
 *    (device mode when known, else the RandR rect) among its modes.
 *
 * A RandR monitor nothing joins is still listed, with no EDID identity. With
 * no RandR monitors at all (no X server: pure Wayland, or headless) every
 * connected, enabled connector becomes a DRM-only record whose desktop origin
 * is unknown.
 */

#pragma once

#include "os_display_desktop.h"
#include "os_display_edid.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Physical-size slack for the mm join. EDID stores cm in the base block and
//! mm in the detailed timing, and RandR may report either: 344 vs 340 is the
//! same DS1.
#define OS_DISPLAY_EDID_JOIN_MM_TOLERANCE 10

//! Modes kept per DRM connector.
#define OS_DISPLAY_DRM_MAX_MODES 16

//! Connectors kept from sysfs.
#define OS_DISPLAY_DRM_MAX_CONNECTORS 32

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
};

/*!
 * Parse an EDID base block. Checks the 8-byte header and the length (>= 128);
 * the checksum is not enforced (sysfs hands over what the sink sent, and a bad
 * checksum still carries a usable vendor id).
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

/*!
 * One DRM connector from sysfs.
 */
struct os_display_drm_connector
{
	char name[32]; //!< Kernel name without the card prefix, e.g. "HDMI-A-1".
	bool enabled;  //!< `enabled` reads "enabled" (or the file is absent).
	bool has_edid; //!< `edid` parsed.
	struct os_display_edid_parsed edid;
	uint32_t mode_w[OS_DISPLAY_DRM_MAX_MODES]; //!< `modes`, in kernel order
	uint32_t mode_h[OS_DISPLAY_DRM_MAX_MODES]; //!< (the first is the preferred one).
	uint32_t mode_count;
};

/*!
 * Read every CONNECTED connector under @p root (normally `/sys/class/drm`;
 * a directory of `card<N>-<name>/{status,enabled,edid,modes}` in tests).
 *
 * @return the number written to @p out.
 */
uint32_t
os_display_drm_read_connectors(const char *root, struct os_display_drm_connector *out, uint32_t max);

/*!
 * The join described in the file comment. Pure: no I/O, no logging.
 *
 * @param randr      RandR monitors (may be NULL when @p randr_count is 0).
 * @param drm        Connected DRM connectors.
 * @param[out] out   One record per RandR monitor, or per enabled connector
 *                   when there is no RandR monitor.
 * @return the number of records written.
 */
uint32_t
os_display_edid_linux_join(const struct os_display_desktop_info *randr,
                           uint32_t randr_count,
                           const struct os_display_drm_connector *drm,
                           uint32_t drm_count,
                           struct os_display_edid_monitor *out,
                           uint32_t max);

#ifdef __cplusplus
}
#endif
