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
 * `set_property` / `get_property` / `destroy` are wrapped so that
 * `XRT_DEVICE_PROPERTY_OUTPUT_MODE` crosses in the driver's numbering
 * (run-time 1 -> the driver's original index, and back). A driver that checks
 * that index against its own, now trimmed, `rendering_mode_count` will still
 * refuse it; the runtime's own active index stays right either way.
 *
 * Idempotent: a second call on an already two-mode table does nothing but
 * renumber (which it will not need to). Not thread-safe against concurrent
 * calls for the same device; it runs at system creation.
 *
 * Does NOT recompute the per-mode tiling or the worst-case atlas: that is the
 * display-info apply's job, which runs on the trimmed table afterwards.
 *
 * @return The trim result (UNCHANGED for a NULL / non-HMD device).
 */
enum u_rendering_modes_trim_result
u_device_enforce_two_rendering_modes(struct xrt_device *xdev, const char *driver_name);

#ifdef __cplusplus
}
#endif
