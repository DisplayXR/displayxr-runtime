// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Build the per-screen list (`xrt_screen_list`) from the per-monitor
 *         DP registry (multi-screen M1).
 *
 * Shared by the native instance (`xrt_instance::enumerate_displays`, which the
 * service also answers `system_enumerate_displays` with) and `displayxr-cli
 * info`, so the CLI prints exactly what `xrEnumerateDisplaysDXR` reports.
 *
 * @ingroup targets_common
 */

#pragma once

#include "xrt/xrt_screen.h"
#include "xrt/xrt_compositor.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * What the system says about its default screen — the inputs that become
 * the SYSTEM_DEFAULT screen, verbatim.
 */
struct target_screens_system
{
	//! The active plug-in described its panel (@ref info is meaningful).
	bool info_valid;
	struct xrt_screen_info info;

	//! The resolved panel desktop rect (#1301); zero = unknown.
	int32_t desktop_left;
	int32_t desktop_top;
	uint32_t desktop_width;
	uint32_t desktop_height;
	uint32_t native_width;
	uint32_t native_height;
	float desktop_scale;
	bool is_primary;
	char device_name[XRT_SCREEN_NAME_MAX];
};

/*!
 * Fill @p out from the system compositor info the runtime applied (the same
 * fields `xrGetSystemProperties` reads). `info_valid` follows the display
 * size being known.
 */
void
target_screens_system_from_info(const struct xrt_system_compositor_info *info, struct target_screens_system *out);

/*!
 * Which registry entry is the system-default screen: the one whose desktop
 * rect contains the origin the runtime resolved for the system panel (#1301,
 * `sys->desktop_left/top`), preferring an entry the active plug-in won; when
 * none contains it (or no rect was resolved), `xrt_dp_registry_primary_entry`.
 * @p out_matches_sys (may be NULL) is true only in the first case — the only
 * case in which the system's resolved rect / device name / primary flag may
 * be copied onto that entry. NULL on an empty registry.
 */
const struct xrt_dp_registry_entry *
target_screens_pick_default(const struct xrt_dp_factory_registry *reg,
                            const char *active_plugin_id,
                            const struct target_screens_system *sys,
                            bool *out_matches_sys);

/*!
 * Build the screen list: one @ref xrt_screen per registry entry, the
 * system-default one (@ref target_screens_pick_default) first and flagged
 * SYSTEM_DEFAULT, each screen's info resolved by
 * `u_screen_info_resolve` (system info / the owning plug-in's
 * `get_display_info_for_monitor` / EDID-derived). An empty registry yields
 * one synthesized screen (@ref XRT_SCREEN_ID_SYNTHETIC_DEFAULT) from @p sys,
 * or an empty list when @p sys knows nothing either.
 *
 * Reads the loader's monitor side table, so it must run after the registry
 * was built in this process.
 */
void
target_screens_build(const struct xrt_dp_factory_registry *reg,
                     const char *active_plugin_id,
                     const struct target_screens_system *sys,
                     struct xrt_screen_list *out);

#ifdef __cplusplus
}
#endif
