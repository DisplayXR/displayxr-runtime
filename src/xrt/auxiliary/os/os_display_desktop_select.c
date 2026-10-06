// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Platform-independent monitor selection for the 3D panel.
 * @ingroup aux_os
 *
 * The platform files answer two narrow questions — "which monitor is at this
 * point" and "what monitors are there" — and this file holds the one policy
 * built on top of them: given whatever a display-processor plug-in could tell
 * us about the panel, which monitor is it?
 *
 * ## Why the point lookup alone is not enough
 *
 * `os_display_desktop_info_at()` needs a position, and a plug-in that reports
 * none lands on the primary monitor. On Windows that is fine, because the
 * Leia plug-in reads the panel's origin out of EDID and reports it. Elsewhere
 * there is nothing to report: LeiaSR's `srDisplayGetLocation()` identifies the
 * panel by EDID PHYSICAL size and returns a (0, 0) origin under a Wayland
 * compositor, where every output's CRTC framebuffer rect starts at the origin.
 * RandR gives no plug-in-visible identity either — there is no connector name
 * anywhere in @ref xrt_plugin_display_info, and adding one would be an ABI
 * change (ADR-020) that the vendor SDK could not fill in anyway.
 *
 * So on a two-monitor rig — which is EVERY real deployment, a 3D panel next to
 * a laptop screen — the primary fallback picks the wrong monitor, the app opens
 * on the laptop, and the weave is refused for a panel that was sitting there
 * matching 1:1 the whole time.
 *
 * ## What is left: match on what the plug-in does report
 *
 * It reports the panel's native pixel size and its physical size in metres, and
 * both are per-monitor facts the OS also knows (RandR reports each monitor's
 * rect and its EDID mm). The rule order is in @ref
 * os_display_desktop_info_for_panel: a reported position beats everything, then
 * the monitor whose CONNECTOR runs the panel's native mode, then a unique
 * PIXEL-size match in the placement space, then the primary fallback.
 *
 * ## Why the connector's mode comes before the placement-space size (#1831)
 *
 * The placement space is not the hardware on a scaled X11 desktop. Under
 * XWayland, Mutter scales the whole X screen by one integer, the ceiling of the
 * largest monitor scale, so a 3840x2160 panel at 150 % is a 5120x2880 RandR
 * rect: the size rule misses it and the primary fallback silently weaves for
 * the laptop. Worse, a different monitor's scaled rect can read exactly the
 * panel's size and win. The connector's device mode (Mutter DisplayConfig,
 * else DRM sysfs, joined by connector name) is the same number the plug-in
 * reports, at any scale.
 *
 * Physical size is a tie-breaker only, never a rule of its own. Matching on
 * millimetres alone would place windows on a monitor whose pixel size differs
 * from the declared panel — never 1:1, so never panel-confirmed and never
 * phase-correct — on evidence that is weak to begin with, since every 15.6"
 * laptop panel is about 344x194 mm. It was tried, and on a two-monitor dev box
 * it moved the default sim_display run (1920x1080 declared at 0.344x0.194 m)
 * onto a 3840x2160 3D panel measuring 340x190 mm, four millimetres away.
 *
 * Nothing here logs: aux_os deliberately does not depend on `u_logging` (see
 * the no-DSO rule at the top of its CMakeLists). The rule that fired and the
 * ambiguity counts come back in @ref os_display_panel_match so the caller —
 * which does have a logger, and knows whether this is a runtime init or a CLI
 * dump — says it out loud.
 */

#include "os_display_desktop.h"

#include <string.h>

//! Absolute mm difference between a monitor's EDID size and the declared panel.
static double
mm_error(const struct os_display_desktop_info *m, uint32_t panel_w_mm, uint32_t panel_h_mm)
{
	double dw = (double)m->physical_width_mm - (double)panel_w_mm;
	double dh = (double)m->physical_height_mm - (double)panel_h_mm;

	return (dw < 0 ? -dw : dw) + (dh < 0 ? -dh : dh);
}

/*!
 * Pick between several monitors that all matched on pixels.
 *
 * Physical size first when both sides know it — among monitors already proven
 * to be the right resolution, the closest millimetres is a real discriminator.
 * Otherwise the non-primary one, because a 3D panel is an added monitor: the
 * machine's own screen is what the user logs into.
 */
static uint32_t
disambiguate(const struct os_display_desktop_info *mons,
             const uint32_t *cand,
             uint32_t cand_count,
             uint32_t panel_w_mm,
             uint32_t panel_h_mm)
{
	if (panel_w_mm > 0 && panel_h_mm > 0) {
		uint32_t best = UINT32_MAX;
		double best_err = 0.0;
		for (uint32_t i = 0; i < cand_count; i++) {
			const struct os_display_desktop_info *m = &mons[cand[i]];
			if (m->physical_width_mm == 0 || m->physical_height_mm == 0) {
				continue;
			}
			double err = mm_error(m, panel_w_mm, panel_h_mm);
			if (best == UINT32_MAX || err < best_err) {
				best = cand[i];
				best_err = err;
			}
		}
		if (best != UINT32_MAX) {
			return best;
		}
	}

	for (uint32_t i = 0; i < cand_count; i++) {
		if (!mons[cand[i]].is_primary) {
			return cand[i];
		}
	}

	return cand[0];
}

const char *
os_display_desktop_rule_str(enum os_display_desktop_rule rule)
{
	switch (rule) {
	case OS_DISPLAY_DESKTOP_RULE_ORIGIN: return "origin";
	case OS_DISPLAY_DESKTOP_RULE_CONNECTOR_MODE: return "connector match";
	case OS_DISPLAY_DESKTOP_RULE_PIXEL_MATCH: return "size match";
	case OS_DISPLAY_DESKTOP_RULE_PRIMARY_FALLBACK: return "primary fallback";
	case OS_DISPLAY_DESKTOP_RULE_UNRESOLVED:
	default: return "unresolved";
	}
}

int32_t
os_display_desktop_select_by_size(const struct os_display_desktop_info *mons,
                                  uint32_t count,
                                  const struct os_display_panel_hint *hint,
                                  struct os_display_panel_match *out_match)
{
	struct os_display_panel_match match = {0};
	match.rule = OS_DISPLAY_DESKTOP_RULE_UNRESOLVED;
	match.monitor_count = count;
	if (out_match != NULL) {
		*out_match = match;
	}

	if (mons == NULL || count == 0 || hint == NULL || hint->pixel_width == 0 || hint->pixel_height == 0) {
		return -1;
	}
	if (count > OS_DISPLAY_DESKTOP_MAX_MONITORS) {
		count = OS_DISPLAY_DESKTOP_MAX_MONITORS;
	}

	// Metres -> mm, rounded. 0 stays 0 ("unknown").
	const uint32_t panel_w_mm = hint->width_m > 0.0f ? (uint32_t)(hint->width_m * 1000.0f + 0.5f) : 0;
	const uint32_t panel_h_mm = hint->height_m > 0.0f ? (uint32_t)(hint->height_m * 1000.0f + 0.5f) : 0;

	uint32_t cand[OS_DISPLAY_DESKTOP_MAX_MONITORS];
	uint32_t cand_count = 0;

	// Rule 2 (#1831): the connector is RUNNING the panel's native mode. The
	// device mode is the one number that means the same thing on both sides
	// at every desktop scale — the plug-in reports hardware pixels, and so
	// does the connector — where the X11 rect of a fractionally-scaled output
	// is device x ceil(scale) / scale and matches nothing.
	for (uint32_t i = 0; i < count; i++) {
		if (mons[i].native_width == hint->pixel_width && mons[i].native_height == hint->pixel_height) {
			cand[cand_count++] = i;
		}
	}
	enum os_display_desktop_rule rule = OS_DISPLAY_DESKTOP_RULE_CONNECTOR_MODE;

	// Rule 3: the monitor's rect in the placement space IS the panel's
	// native resolution — the only evidence on platforms with no device
	// mode. Compared in the caller's DPI space, the space a plug-in reports
	// in (see the field docs on os_display_desktop_info). A monitor whose
	// device mode IS known and is something else is not the panel, however
	// its rect happens to read: under XWayland a 2880x1800 laptop at 125 %
	// with the X screen at 2x reads 4608x2880, and a different laptop could
	// read exactly a panel's 3840x2160.
	if (cand_count == 0) {
		rule = OS_DISPLAY_DESKTOP_RULE_PIXEL_MATCH;
		for (uint32_t i = 0; i < count; i++) {
			const bool native_known = mons[i].native_width > 0 && mons[i].native_height > 0;
			if (native_known) {
				continue;
			}
			if (mons[i].width_in_caller_dpi == hint->pixel_width &&
			    mons[i].height_in_caller_dpi == hint->pixel_height) {
				cand[cand_count++] = i;
			}
		}
	}

	if (cand_count == 0) {
		return -1;
	}

	const uint32_t pick = cand_count == 1 ? cand[0] : disambiguate(mons, cand, cand_count, panel_w_mm, panel_h_mm);
	match.rule = rule;
	match.candidate_count = cand_count;
	if (out_match != NULL) {
		*out_match = match;
	}
	return (int32_t)pick;
}

bool
os_display_desktop_info_for_panel(const struct os_display_panel_hint *hint,
                                  struct os_display_desktop_info *out_info,
                                  struct os_display_panel_match *out_match)
{
	struct os_display_panel_match match = {0};
	match.rule = OS_DISPLAY_DESKTOP_RULE_UNRESOLVED;

	if (out_info == NULL) {
		if (out_match != NULL) {
			*out_match = match;
		}
		return false;
	}
	memset(out_info, 0, sizeof(*out_info));

	struct os_display_panel_hint h = {0};
	if (hint != NULL) {
		h = *hint;
	}

	// Rule 1: a position the plug-in actually reported. (0, 0) is defined as
	// "no preference", so it is NOT a position — that is the whole reason the
	// size rules below exist.
	if (h.screen_left != 0 || h.screen_top != 0) {
		match.rule = OS_DISPLAY_DESKTOP_RULE_ORIGIN;
		bool ok = os_display_desktop_info_at(h.screen_left, h.screen_top, out_info);
		if (!ok) {
			match.rule = OS_DISPLAY_DESKTOP_RULE_UNRESOLVED;
		}
		if (out_match != NULL) {
			*out_match = match;
		}
		return ok;
	}

	struct os_display_desktop_info mons[OS_DISPLAY_DESKTOP_MAX_MONITORS];
	uint32_t count = os_display_desktop_enumerate(mons, OS_DISPLAY_DESKTOP_MAX_MONITORS);

	// Rules 2 and 3: the panel's hardware mode, then its size in the
	// placement space.
	int32_t pick = os_display_desktop_select_by_size(mons, count, &h, &match);
	if (pick >= 0) {
		*out_info = mons[pick];
		if (out_match != NULL) {
			*out_match = match;
		}
		return true;
	}

	// No rule between here and the fallback. A monitor matching only in
	// millimetres is NOT taken: its pixel size differs from the declared
	// panel, so the rect could never be panel-confirmed or weave in phase,
	// and physical size alone does not identify a panel well enough to move a
	// window on (see the file comment for the measurement that settled this).

	// Rule 4: the monitor at the desktop origin — i.e. exactly what this
	// resolver did before any of the above existed, which is what keeps the
	// Windows path (no enumeration, so it arrives straight here whenever the
	// plug-in reported no position) bit-for-bit unchanged.
	match.rule = OS_DISPLAY_DESKTOP_RULE_PRIMARY_FALLBACK;
	bool ok = os_display_desktop_info_at(0, 0, out_info);
	if (!ok) {
		match.rule = OS_DISPLAY_DESKTOP_RULE_UNRESOLVED;
	}
	if (out_match != NULL) {
		*out_match = match;
	}
	return ok;
}
