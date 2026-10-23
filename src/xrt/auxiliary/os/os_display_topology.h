// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  A cheap, poll-able read of the OS display topology (display
 *         hotplug, ADR-047 Amendment 8 / #2305).
 * @ingroup aux_os
 *
 * The in-process display hotplug sweep (`target_display_hotplug`) polls a
 * topology signature and re-runs the (expensive) EDID enumeration + claim
 * re-resolve only when it moves. On macOS and X11 the desktop monitor list
 * (@ref os_display_desktop_enumerate) is that signature, but Windows has no
 * desktop enumeration and pure Wayland has no RandR. This is the platform part
 * that closes the gap, and it is deliberately much cheaper than
 * @ref os_display_edid_enumerate (no SetupAPI, no registry, no EDID blob):
 *
 * - **Windows:** `EnumDisplayDevicesW` over every adapter output and the
 *   monitor attached to it (the monitor's device-interface name carries its
 *   EDID hardware id — `\\?\DISPLAY#<PNP><PROD>#…` — so a different panel on
 *   the same output changes the key), with the output's current mode from
 *   `EnumDisplaySettingsExW` (position + size). Read under a pinned
 *   per-monitor-v2 thread DPI context (docs/reference/dpi-awareness.md), so a
 *   DPI-unaware host reads the same physical numbers as everyone else.
 * - **Desktop Linux:** the DRM connectors in `/sys/class/drm/card*-*`: name,
 *   `status` and `enabled` (what the kernel's hotplug uevent changes).
 * - **Elsewhere:** nothing (0 entries): the desktop enumeration is the signature.
 *
 * Any thread; never blocks on another process. Microseconds to a millisecond.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Max entries @ref os_display_topology_read returns.
#define OS_DISPLAY_TOPOLOGY_MAX 32

//! One output (Windows: adapter output + its monitor; Linux: DRM connector).
struct os_display_topology_entry
{
	//! Stable identity: Windows `\\.\DISPLAYn|<monitor interface name>`, Linux `card1-HDMI-A-1`.
	char key[192];
	//! Platform state bits (Windows `StateFlags` of output | monitor<<16; Linux connected/enabled bits).
	uint32_t state;
	//! The output's current mode / position; 0 when the platform has none (Linux).
	int32_t left;
	int32_t top;
	uint32_t width;
	uint32_t height;
};

/*!
 * Read the current topology.
 *
 * @return the number of entries written; 0 on a platform with no read (the
 *         caller relies on @ref os_display_desktop_enumerate) or on failure.
 */
uint32_t
os_display_topology_read(struct os_display_topology_entry *out, uint32_t max);

/*!
 * Pure: FNV-1a over @p count entries, in the order given (every platform's
 * read is in a stable order). Two reads hash equal iff they are equal.
 */
uint64_t
os_display_topology_hash(const struct os_display_topology_entry *entries, uint32_t count);

#ifdef __cplusplus
}
#endif
