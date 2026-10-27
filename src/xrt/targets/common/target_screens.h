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
	//! The compositor's preferred mode (see xrt_screen::preferred_width); 0 = unknown.
	uint32_t preferred_width;
	uint32_t preferred_height;
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

struct os_display_panel_claim;
struct os_display_panel_hint;
struct os_display_desktop_info;
struct os_display_edid_monitor;

/*!
 * The active plug-in's REAL claims in the per-monitor registry, as input to
 * the claim rule (`os_display_desktop_select_claim`, displayxr-leia-plugin#430).
 *
 * Only entries the active plug-in WON through its own `probe_displays`, at
 * `XRT_DISPLAY_CLAIM_EDID` or above, on a monitor with a known desktop
 * position. Excluded, so this path can never select an unclaimed or virtual
 * monitor: FALLBACK claims (sim-display's "anything unclaimed"),
 * PRESENT_NOT_READY ones, the claim the loader SYNTHESIZES for a plug-in
 * without `probe_displays` (it is always the primary — evidence of nothing),
 * other plug-ins' entries, and DRM-only records whose origin is unknown.
 *
 * @p out_mons (parallel to @p out_claims, may be NULL) receives each claim's
 * enumeration record — the HMONITOR / output name the pick is resolved by.
 * Reads the loader's monitor side table, so it must run after the registry
 * was built in this process.
 *
 * @return the number of claims written (<= @p max).
 */
uint32_t
target_screens_collect_panel_claims(const struct xrt_dp_factory_registry *reg,
                                    const char *active_plugin_id,
                                    struct os_display_panel_claim *out_claims,
                                    struct os_display_edid_monitor *out_mons,
                                    uint32_t max);

/*!
 * The claim rule, end to end: collect (@ref target_screens_collect_panel_claims),
 * select (`os_display_desktop_select_claim`) and resolve the pick by OS
 * identity (`os_display_desktop_info_for_device`). Shared by the runtime's
 * panel rect (`fill_display_desktop_info`) and `displayxr-cli info`, so both
 * report the same monitor.
 *
 * @param[out] out_overrides_origin  Optional; set when the pick contradicts
 *             an origin the plug-in did report (a VERIFIED claim won).
 * @return true when the claim rule fired and the monitor resolved; false
 *         leaves the caller on the origin / size / primary rules.
 */
bool
target_screens_panel_from_claims(const struct xrt_dp_factory_registry *reg,
                                 const char *active_plugin_id,
                                 const struct os_display_panel_hint *hint,
                                 struct os_display_desktop_info *out_desktop,
                                 bool *out_overrides_origin);

/*!
 * @ref target_screens_panel_from_claims with display-hotplug stickiness
 * (#2305): @p incumbent_monitor_id (0 = none) is the registry monitor the
 * panel rect was resolved to before this re-resolve; while the active plug-in
 * still claims it, it wins over every other claim (a second panel plugged in
 * never moves the rect off the one a live session weaves). @p out_monitor_id
 * (may be NULL) receives the registry monitor id of the pick.
 */
bool
target_screens_panel_from_claims_ex(const struct xrt_dp_factory_registry *reg,
                                    const char *active_plugin_id,
                                    const struct os_display_panel_hint *hint,
                                    uint64_t incumbent_monitor_id,
                                    struct os_display_desktop_info *out_desktop,
                                    bool *out_overrides_origin,
                                    uint64_t *out_monitor_id);

/*!
 * #2305: the claimed panel is back where the plug-in itself says it is. True
 * when @p prev_claim_monitor_id (the monitor the last resolve took by the
 * claim rule) is no longer among the active plug-in's claims, and the
 * plug-in's reported origin lies on a monitor it does claim now — the case in
 * which the claim rule declines (the two sources agree) although a claim
 * exists, so the origin rule lands on that claimed monitor.
 *
 * Seen on Windows (runtime #2311, Acer DS1 over HDMI): mid-replug, Windows
 * reports the panel ALONE at (0, 0) for about a second, the claim rule takes
 * it there (a new, position-derived monitor id), and when the layout returns
 * the panel is claimed again under its own id at the plug-in's origin. Without
 * this the re-resolve read "the claim is gone" and kept the transient (0, 0)
 * rect for the rest of the session.
 */
bool
target_screens_panel_claimed_at_origin(const struct xrt_dp_factory_registry *reg,
                                       const char *active_plugin_id,
                                       const struct os_display_panel_hint *hint,
                                       uint64_t prev_claim_monitor_id);

//! What a panel-rect re-resolve does (@ref target_screens_panel_reresolve_decide).
enum target_panel_reresolve
{
	TARGET_PANEL_TAKE_CLAIM = 0,    //!< The claim rule resolved: publish it.
	TARGET_PANEL_KEEP_PREVIOUS = 1, //!< The claimed panel vanished: keep the last rect.
	TARGET_PANEL_TAKE_FALLBACK = 2, //!< No claim now or before, or the panel is back at the plug-in's
	                                //!< origin: origin / size / primary, as always.
};

/*!
 * Pure (#2305): on a re-resolve (hotplug sweep, world event — never the first
 * resolve), the panel rect never falls from a claimed monitor to the origin /
 * size / primary rules — those can land on an unclaimed or virtual monitor —
 * unless the plug-in's own origin is on a monitor it claims again.
 *
 * @param prev_claim_monitor_id  The registry monitor the last resolve took by
 *                               the claim rule; 0 when it took another rule.
 * @param same_active_plugin     The active plug-in is the one that claimed it.
 * @param claim_resolved         The claim rule resolved now.
 * @param claimed_at_origin      @ref target_screens_panel_claimed_at_origin:
 *                               the claim rule declined only because the
 *                               plug-in's origin is on a monitor it claims.
 */
enum target_panel_reresolve
target_screens_panel_reresolve_decide(uint64_t prev_claim_monitor_id,
                                      bool same_active_plugin,
                                      bool claim_resolved,
                                      bool claimed_at_origin);

#ifdef __cplusplus
}
#endif
