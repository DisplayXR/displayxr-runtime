// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The two-mode contract: a display driver's run-time rendering-mode
 *         table is `[0] = 2D, [1] = 3D` (ADR-065).
 * @ingroup aux_util
 *
 * ## The rule
 *
 * A driver exposes exactly two rendering modes at run time. Which 3D *kind*
 * mode 1 is (anaglyph, side-by-side, a 4-view quilt, a weaver variant) is a
 * per-driver setting applied when the driver creates its device, never a third
 * mode that apps or number keys cycle through. The runtime enforces the rule
 * once, where it takes the head device's table (the instance's create-system,
 * before any compositor exists, and again when a plug-in's display info is
 * applied): see @ref u_device_enforce_two_rendering_modes.
 *
 * A table of one or two modes is left as it is. A longer table is trimmed by
 * @ref u_rendering_modes_trim:
 *
 *  - mode 0 is kept, and must be the flat mode (one view, hardware 2D) —
 *    otherwise the table is REFUSED: left untouched, and the caller WARNs;
 *  - exactly one 3D mode is kept: the one the device marks active at creation
 *    when that is a 3D mode, else the first mode (from index 1) that is 3D;
 *    a table with no 3D mode at all is REFUSED too;
 *  - the kept 3D mode moves to index 1, the count becomes 2 and every kept
 *    `mode_index` is renumbered to its array position (also done on an
 *    untrimmed or refused table, so `mode_index == array position` always
 *    holds);
 *  - the active index is translated: the kept 3D mode becomes 1, mode 0 stays
 *    0, and an active mode that was dropped becomes 1 if it was 3D (it cannot
 *    be: the active 3D mode is the one kept) or 0 otherwise.
 *
 * "3D" here means `hardware_display_3d || view_count >= 2`; "flat" means
 * `view_count == 1 && !hardware_display_3d`.
 *
 * Entries past the new count are NOT cleared. Nothing in the runtime reads past
 * `rendering_mode_count`, and a driver that indexes its own table with its own
 * numbering keeps finding sane data there.
 *
 * `XRT_MAX_RENDERING_MODES` and every struct layout stay as they are: this is a
 * runtime rule, not a plug-in ABI change.
 */

#pragma once

#include "xrt/xrt_device.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! The run-time index of the flat mode under the two-mode contract.
#define U_RENDERING_MODE_2D 0u
//! The run-time index of the driver's 3D mode under the two-mode contract.
#define U_RENDERING_MODE_3D 1u
//! How many modes a driver exposes at run time.
#define U_RENDERING_MODES_RUNTIME_COUNT 2u

//! Is @p m the flat mode: one view, panel in hardware 2D?
static inline bool
u_rendering_mode_is_flat(const struct xrt_rendering_mode *m)
{
	return m != NULL && m->view_count == 1 && !m->hardware_display_3d;
}

//! Is @p m a 3D mode: the panel in hardware 3D, or more than one view?
static inline bool
u_rendering_mode_is_3d(const struct xrt_rendering_mode *m)
{
	return m != NULL && (m->hardware_display_3d || m->view_count >= 2);
}

//! The widest mode in @p modes — what the device's max view count must be.
static inline uint32_t
u_rendering_modes_max_view_count(const struct xrt_rendering_mode *modes, uint32_t count)
{
	uint32_t max = 0;
	for (uint32_t i = 0; modes != NULL && i < count && i < XRT_MAX_RENDERING_MODES; i++) {
		if (modes[i].view_count > max) {
			max = modes[i].view_count;
		}
	}
	return max;
}

/*!
 * The 3D mode a trim keeps: @p active when that is a 3D mode at index >= 1,
 * else the first 3D mode from index 1; 0 when the table has none.
 */
static inline uint32_t
u_rendering_modes_pick_3d(const struct xrt_rendering_mode *modes, uint32_t count, uint32_t active)
{
	if (modes == NULL) {
		return 0;
	}
	if (count > XRT_MAX_RENDERING_MODES) {
		count = XRT_MAX_RENDERING_MODES;
	}
	if (active >= 1 && active < count && u_rendering_mode_is_3d(&modes[active])) {
		return active;
	}
	for (uint32_t i = 1; i < count; i++) {
		if (u_rendering_mode_is_3d(&modes[i])) {
			return i;
		}
	}
	return 0;
}

/*!
 * The mode the V key toggles to (ADR-065): 2D from the 3D mode, the 3D mode
 * (index 1) from anything else. Decided from the mode the device is in NOW, so
 * the toggle is right whatever moved the mode last (an app request, the
 * session-begin floor, a 0/1 key, a driver that starts in 2D).
 *
 * A table of fewer than two modes has nothing to toggle between: @p active is
 * returned unchanged.
 */
static inline uint32_t
u_rendering_modes_toggle_target(const struct xrt_rendering_mode *modes, uint32_t count, uint32_t active)
{
	if (modes == NULL || count < U_RENDERING_MODES_RUNTIME_COUNT) {
		return active;
	}
	return (active < count && u_rendering_mode_is_3d(&modes[active])) ? U_RENDERING_MODE_2D : U_RENDERING_MODE_3D;
}

/*!
 * May a runtime key (V, '1') move the display to @p target for a session that
 * can submit at most @p fill_views views? The key-path twin of the
 * session-begin floor and of xrRequestDisplayRenderingModeDXR's
 * VIEW_CONFIG_CANNOT_FILL denial (#1499): with {2D, a 4-view kind}, V would
 * otherwise put a stereo or legacy session straight back into the mode the
 * floor took it out of.
 *
 * @p fill_views == 0 means "no limit": the state tracker has not set one, or
 * the session is exempt (an orchestrator, a pinned device, service mode, the
 * floor switched off). An out-of-range target is never allowed; with no table
 * at all there is no opinion (true).
 */
static inline bool
u_rendering_modes_key_target_allowed(const struct xrt_rendering_mode *modes,
                                     uint32_t count,
                                     uint32_t target,
                                     uint32_t fill_views)
{
	if (modes == NULL || count == 0) {
		return true;
	}
	if (target >= count || target >= XRT_MAX_RENDERING_MODES) {
		return false;
	}
	return fill_views == 0 || modes[target].view_count <= fill_views;
}

//! What a runtime mode key (ADR-065: V toggles, '0' / '1' select) does.
enum u_rendering_mode_key_action
{
	//! Nothing: no key, or a selected index the table does not have.
	U_RENDERING_MODE_KEY_NOTHING = 0,
	//! Move the device to `target` (write the active index, pass it to the
	//! driver as OUTPUT_MODE) and request the hardware state `want_3d`.
	U_RENDERING_MODE_KEY_APPLY = 1,
	//! The session cannot fill `target` (#1499): log it, change nothing.
	U_RENDERING_MODE_KEY_REFUSED = 2,
	//! No two-mode table to move within (no head, or fewer than two modes):
	//! only request the hardware state `want_3d`.
	U_RENDERING_MODE_KEY_HARDWARE_ONLY = 3,
};

//! The decision behind one mode key, see @ref u_rendering_modes_decide_key.
struct u_rendering_mode_key_decision
{
	enum u_rendering_mode_key_action action;
	//! The mode to land (APPLY) or the one refused (REFUSED); always < count.
	uint32_t target;
	//! The hardware state to request (APPLY, HARDWARE_ONLY).
	bool want_3d;
};

/*!
 * THE decision behind the runtime's mode keys, V and '0' / '1' alike
 * (ADR-065: "V toggles, 0 and 1 select, on every platform and every driver").
 *
 * The two key kinds differ only in how they pick the target: V from the mode
 * the device is in now (@ref u_rendering_modes_toggle_target), a digit by
 * naming it. Everything after that is one path, so a digit does exactly what V
 * does for the same target: the #1499 fill check, the active-index write the
 * state tracker turns into XrEventDataRenderingModeChangedDXR, the OUTPUT_MODE
 * property for a driver that implements it, and the hardware 2D/3D request
 * that a driver which does NOT implement OUTPUT_MODE needs to switch at all.
 * (Before this, a digit only set OUTPUT_MODE, so on such a driver it did
 * nothing.)
 *
 * @param modes, count    the head device's table (NULL / 0 when there is none).
 * @param active          the device's active index.
 * @param toggle          true for V; false for a digit.
 * @param selected        the digit's mode (0 or 1); ignored for V; < 0 = none.
 * @param qwerty_force_2d qwerty's own V flag, used only when there is no table.
 * @param fill_views      the session's fill limit, 0 = none (#1499).
 */
static inline struct u_rendering_mode_key_decision
u_rendering_modes_decide_key(const struct xrt_rendering_mode *modes,
                             uint32_t count,
                             uint32_t active,
                             bool toggle,
                             int32_t selected,
                             bool qwerty_force_2d,
                             uint32_t fill_views)
{
	struct u_rendering_mode_key_decision d = {U_RENDERING_MODE_KEY_NOTHING, 0, false};
	if (!toggle && (selected < 0 || (uint32_t)selected > U_RENDERING_MODE_3D)) {
		return d;
	}
	if (modes == NULL || count < U_RENDERING_MODES_RUNTIME_COUNT) {
		d.action = U_RENDERING_MODE_KEY_HARDWARE_ONLY;
		d.want_3d = toggle ? !qwerty_force_2d : ((uint32_t)selected == U_RENDERING_MODE_3D);
		return d;
	}
	const uint32_t target = toggle ? u_rendering_modes_toggle_target(modes, count, active) : (uint32_t)selected;
	if (target >= count || target >= XRT_MAX_RENDERING_MODES) {
		return d;
	}
	d.target = target;
	if (!u_rendering_modes_key_target_allowed(modes, count, target, fill_views)) {
		d.action = U_RENDERING_MODE_KEY_REFUSED;
		return d;
	}
	d.action = U_RENDERING_MODE_KEY_APPLY;
	d.want_3d = (target != U_RENDERING_MODE_2D);
	return d;
}

//! What @ref u_rendering_modes_trim did.
enum u_rendering_modes_trim_result
{
	//! One or two modes: nothing dropped (mode_index may still be renumbered).
	U_RENDERING_MODES_TRIM_UNCHANGED = 0,
	//! More than two modes: trimmed to the flat mode + one 3D mode.
	U_RENDERING_MODES_TRIM_TRIMMED = 1,
	//! More than two modes, but mode 0 is not flat or no mode is 3D: the table
	//! is left exactly as it was. The caller WARNs; the contract is not met.
	U_RENDERING_MODES_TRIM_REFUSED = 2,
};

//! The details of a trim, for the caller's WARN and its index translation.
struct u_rendering_modes_trim_info
{
	enum u_rendering_modes_trim_result result;

	//! The table's count before the trim.
	uint32_t original_count;

	/*!
	 * The driver's own (original array) index of each run-time index:
	 * `to_driver[0] = 0`, `to_driver[1] =` the kept 3D mode's original index.
	 * Identity when nothing was trimmed.
	 */
	uint32_t to_driver[U_RENDERING_MODES_RUNTIME_COUNT];

	/*!
	 * The run-time index of each original index (0 or 1); for a dropped mode,
	 * 1 if it was a 3D mode and 0 otherwise. Identity below the new count when
	 * nothing was trimmed.
	 */
	uint32_t from_driver[XRT_MAX_RENDERING_MODES];

	//! Bit i set: the original mode i was dropped.
	uint32_t dropped_mask;

	//! At least one kept mode's mode_index was rewritten to its position.
	bool renumbered;
};

/*!
 * Apply the two-mode contract to a mode table, in place. Pure: no logging, no
 * device. See the file comment for the rule.
 *
 * @param modes       The table (in/out). At most XRT_MAX_RENDERING_MODES entries.
 * @param count       Its count (in/out).
 * @param active      The active index (in/out): translated to the new table.
 * @param out_info    Optional: what happened, for the caller's WARN and its
 *                    run-time <-> driver index translation.
 * @return The result, also in @p out_info.
 */
static inline enum u_rendering_modes_trim_result
u_rendering_modes_trim(struct xrt_rendering_mode *modes,
                       uint32_t *count,
                       uint32_t *active,
                       struct u_rendering_modes_trim_info *out_info)
{
	struct u_rendering_modes_trim_info info;
	info.result = U_RENDERING_MODES_TRIM_UNCHANGED;
	info.original_count = (count != NULL) ? *count : 0;
	info.dropped_mask = 0;
	info.renumbered = false;
	info.to_driver[0] = 0;
	info.to_driver[1] = 1;
	for (uint32_t i = 0; i < XRT_MAX_RENDERING_MODES; i++) {
		info.from_driver[i] = i < U_RENDERING_MODES_RUNTIME_COUNT ? i : 0;
	}

	if (modes == NULL || count == NULL) {
		if (out_info != NULL) {
			*out_info = info;
		}
		return info.result;
	}

	uint32_t n = *count;
	if (n > XRT_MAX_RENDERING_MODES) {
		n = XRT_MAX_RENDERING_MODES;
	}
	info.original_count = n;

	// Which 3D mode survives a trim (0 = none: refused).
	const uint32_t keep = (n > U_RENDERING_MODES_RUNTIME_COUNT)
	                          ? u_rendering_modes_pick_3d(modes, n, (active != NULL) ? *active : 0)
	                          : 0;

	if (n > U_RENDERING_MODES_RUNTIME_COUNT && (!u_rendering_mode_is_flat(&modes[0]) || keep == 0)) {
		// Refused: the modes, the count and the active index stay exactly as
		// the driver declared them (only mode_index is renumbered, below).
		// Identity maps over the untouched table.
		info.result = U_RENDERING_MODES_TRIM_REFUSED;
		for (uint32_t i = 0; i < XRT_MAX_RENDERING_MODES; i++) {
			info.from_driver[i] = i;
		}
	} else if (n > U_RENDERING_MODES_RUNTIME_COUNT) {
		for (uint32_t i = 0; i < n; i++) {
			if (i == 0) {
				info.from_driver[i] = U_RENDERING_MODE_2D;
			} else if (i == keep) {
				info.from_driver[i] = U_RENDERING_MODE_3D;
			} else {
				info.dropped_mask |= (1u << i);
				info.from_driver[i] =
				    u_rendering_mode_is_3d(&modes[i]) ? U_RENDERING_MODE_3D : U_RENDERING_MODE_2D;
			}
		}
		info.to_driver[1] = keep;

		if (keep != 1) {
			modes[1] = modes[keep];
		}
		*count = U_RENDERING_MODES_RUNTIME_COUNT;
		n = U_RENDERING_MODES_RUNTIME_COUNT;

		if (active != NULL) {
			*active = (*active < info.original_count) ? info.from_driver[*active] : U_RENDERING_MODE_2D;
		}
		info.result = U_RENDERING_MODES_TRIM_TRIMMED;
	}

	// mode_index == array position, on every table the runtime serves.
	for (uint32_t i = 0; i < n; i++) {
		if (modes[i].mode_index != i) {
			modes[i].mode_index = i;
			info.renumbered = true;
		}
	}

	if (out_info != NULL) {
		*out_info = info;
	}
	return info.result;
}

/*!
 * Enforce the two-mode contract on a head device: trim its table with
 * @ref u_rendering_modes_trim, translate its active index, recompute
 * `hmd->view_count` from the trimmed table, and log one WARN naming
 * @p driver_name and what was dropped (or why the table was refused).
 *
 * When the kept 3D mode is not the driver's own index 1, the device's
 * `set_property` / `get_property` / `destroy` are wrapped (once) so that
 * `XRT_DEVICE_PROPERTY_OUTPUT_MODE` crosses in the driver's numbering
 * (run-time 1 -> the driver's original index, and back). During each
 * forwarded call, under a lock the wrapper owns, the device shows the driver
 * its own `rendering_mode_count` (its entries past index 1 are never cleared),
 * so a pre-ADR-065 driver that range-checks the index and reads
 * `rendering_modes[index]` keeps working; the two-mode count is restored
 * before the call returns. u_rendering_modes.c states the residual race.
 *
 * The FIRST enforce of a device, at system creation. Single-threaded there.
 * Does NOT recompute the per-mode tiling or the worst-case atlas: that is the
 * display-info apply's job, which runs on the trimmed table afterwards.
 *
 * @return The trim result (UNCHANGED for a NULL / non-HMD device).
 */
enum u_rendering_modes_trim_result
u_device_enforce_two_rendering_modes(struct xrt_device *xdev, const char *driver_name);

/*!
 * Re-enforce the contract after the first enforce (the display-info apply).
 * A no-op on a table that is still two modes. If the driver has rewritten its
 * full table since, it is trimmed again keeping the SAME driver index as
 * before (the kind does not change under the session) when that is still a 3D
 * mode, else the first 3D mode; the active index keeps its run-time meaning;
 * the wrappers are never installed twice - only their index maps are updated,
 * under the wrapper's lock.
 *
 * Threading: the caller serialises calls for one device (target_instance.c
 * holds its display-info mutex); concurrent readers of the table are not
 * locked out, exactly as they are not from the driver's own rewrite.
 */
enum u_rendering_modes_trim_result
u_device_reenforce_two_rendering_modes(struct xrt_device *xdev, const char *driver_name);

//! One throttled INFO line (never per press) when a mode key is refused by
//! @ref u_rendering_modes_key_target_allowed.
void
u_rendering_modes_log_key_refused(const struct xrt_rendering_mode *mode, uint32_t fill_views);

//! The driver index that is run-time mode 1 for a TRIMMED device; 0 when it was never trimmed.
uint32_t
u_device_rendering_modes_kept_driver_index(struct xrt_device *xdev);

#ifdef __cplusplus
}
#endif
