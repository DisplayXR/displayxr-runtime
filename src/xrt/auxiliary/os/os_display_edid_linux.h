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
 * 0. **randr-edid** — the X server publishes the output's own `EDID` property
 *    (native X does; XWayland does not). That IS the monitor's identity; the
 *    DRM connector is then the one unused, enabled connector carrying the same
 *    EDID (vendor, product, serial), when exactly one does — or, among several
 *    identical ones, the one whose name agrees.
 * 1. **name** — the RandR output name equals the DRM connector name once the
 *    card prefix and the kernel's subtype letter are dropped
 *    ("card1-HDMI-A-1" -> "HDMI-1"), AND the connector agrees on something
 *    physical: its mode list holds the monitor's device mode, or its EDID size
 *    is within tolerance of RandR's mm. A bare name is not enough: the NVIDIA
 *    X driver numbers outputs from 0 (DP-0, DP-1) while nvidia-drm numbers
 *    connectors from 1, and two GPUs can each have an HDMI-A-1.
 * 2. **mm** — exactly one unused, enabled connector whose EDID physical size
 *    is within @ref OS_DISPLAY_EDID_JOIN_MM_TOLERANCE of RandR's mm.
 * 3. **mode** — exactly one unused, enabled connector that has the monitor's
 *    device mode among its modes.
 *
 * "Device mode" is the compositor's current mode when Mutter reported it,
 * else the RandR rect — never the DRM-derived mode, which itself came from a
 * name match and would make rule 1 circular.
 *
 * A RandR monitor nothing joins is still listed, with no EDID identity. With
 * no RandR monitors at all (no X server: pure Wayland, or headless) every
 * connected, enabled connector becomes a DRM-only record whose desktop origin
 * is unknown, in connector-name order so the order is stable across boots.
 */

#pragma once

#include "os_display_desktop.h"
#include "os_display_edid.h"
#include "os_display_edid_parse.h"

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
 * One DRM connector from sysfs.
 */
struct os_display_drm_connector
{
	char name[32]; //!< Kernel name WITH the card prefix, e.g. "card1-HDMI-A-1".
	bool enabled;  //!< `enabled` reads "enabled" (or the file is absent).
	bool has_edid; //!< `edid` parsed.
	struct os_display_edid_parsed edid;
	uint32_t mode_w[OS_DISPLAY_DRM_MAX_MODES]; //!< `modes`, in kernel order
	uint32_t mode_h[OS_DISPLAY_DRM_MAX_MODES]; //!< (the first is the preferred one).
	uint32_t mode_count;
};

/*!
 * Read every CONNECTED connector under @p root (normally `/sys/class/drm`;
 * a directory of `card<N>-<name>/{status,enabled,edid,modes}` in tests),
 * sorted by name so the order does not depend on readdir.
 *
 * @return the number written to @p out.
 */
uint32_t
os_display_drm_read_connectors(const char *root, struct os_display_drm_connector *out, uint32_t max);

/*!
 * What the X server itself says a RandR monitor is: the parsed `EDID` output
 * property, when it publishes one.
 */
struct os_display_randr_identity
{
	bool valid;
	struct os_display_edid_parsed edid;
};

/*!
 * The join described in the file comment. Pure: no I/O, no logging.
 *
 * @param randr      RandR monitors (may be NULL when @p randr_count is 0).
 * @param randr_id   Per-RandR-monitor EDID from the X server, aligned with
 *                   @p randr; NULL when none was read.
 * @param drm        Connected DRM connectors.
 * @param[out] out   One record per RandR monitor, or per enabled connector
 *                   when there is no RandR monitor.
 * @return the number of records written.
 */
uint32_t
os_display_edid_linux_join(const struct os_display_desktop_info *randr,
                           const struct os_display_randr_identity *randr_id,
                           uint32_t randr_count,
                           const struct os_display_drm_connector *drm,
                           uint32_t drm_count,
                           struct os_display_edid_monitor *out,
                           uint32_t max);

//! Largest EDID blob kept from the X server (base block + one extension).
#define OS_DISPLAY_RANDR_EDID_MAX 256

/*!
 * One monitor's `EDID` output property as the X server publishes it.
 */
struct os_display_randr_edid
{
	char name[64]; //!< RandR monitor name, as in os_display_desktop_info::device_name.
	uint8_t edid[OS_DISPLAY_RANDR_EDID_MAX];
	uint32_t len; //!< 0 = the server publishes no EDID for this monitor.
};

/*!
 * Read the `EDID` property of each RandR monitor's first output
 * (implemented in os_display_desktop_x11.c, Xrandr dlopen'd). Native X
 * servers publish it; XWayland does not, and then every entry has len 0.
 *
 * @return the number of monitors written, 0 without an X server.
 */
uint32_t
os_display_x11_read_monitor_edids(struct os_display_randr_edid *out, uint32_t max);

#ifdef __cplusplus
}
#endif
