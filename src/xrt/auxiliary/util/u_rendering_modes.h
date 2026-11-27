// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The two-slot contract: apps see a display driver's rendering modes as
 *         `[0] = 2D, [1] = 3D` (ADR-065), while the runtime keeps the driver's
 *         FULL declared list as the sizing envelope and maps slot 1 to the
 *         entry the driver's configuration names (ADR-066 C1).
 * @ingroup aux_util
 *
 * ## The rule
 *
 * A driver declares every rendering mode it can show, up to
 * `XRT_MAX_RENDERING_MODES` (8). That declared list is the ENVELOPE: the
 * runtime keeps all of it and sizes everything fixed for an instance over it
 * (`PRIMARY_MULTIVIEW_DXR`'s located view count = `hmd->view_count`, the
 * worst-case atlas, `maxImageRect*`). Apps see exactly two SLOTS: slot 0 = the
 * flat entry 0, slot 1 = the entry the driver's configuration maps to it. A
 * config switch only REMAPS slot 1 (@ref u_device_rendering_modes_refresh_config)
 * and bumps a generation the state tracker turns into an equal-index
 * XrEventDataRenderingModeChangedDXR. Nothing is resized or recreated.
 *
 * ## How the device carries it
 *
 * `xdev->rendering_mode_count` is the presented count (2); every reader that
 * iterates `[0, rendering_mode_count)` sees the two slots, as under ADR-065.
 * The full list stays IN the device's array: `[0, envelope_count)` holds every
 * declared entry, permuted by one transposition so the slot-1 entry sits at
 * index 1 (the driver's own entry 1 then sits where the slot-1 entry was).
 * Anything the runtime writes into the entries in place (per-mode tiling, the
 * composed HAS_TRACKING flag) is written over the envelope, so a remap never
 * brings in a stale entry. Sizing reads
 * `[0, u_device_rendering_modes_envelope_count())`.
 *
 * A table of one or two modes is left as it is (envelope == count). A longer
 * table is selected by @ref u_rendering_modes_trim:
 *
 *  - mode 0 is kept at 0, and must be the flat mode (one view, hardware 2D,
 *    not a passthrough 3D entry) - otherwise the table is REFUSED: left
 *    untouched, and the caller WARNs;
 *  - slot 1 is the driver's CONFIGURED entry (@p preferred, from
 *    `XRT_DEVICE_PROPERTY_CONFIGURED_3D_MODE`) when that is a 3D entry, else
 *    the one the device marks active at creation when that is 3D, else the
 *    first 3D entry from index 1; a table with no 3D entry is REFUSED too;
 *  - the slot-1 entry is swapped into index 1, the presented count becomes 2,
 *    the envelope count is the declared count, and every `mode_index` in the
 *    envelope is renumbered to its array position;
 *  - the active index is translated: the slot-1 entry becomes 1, mode 0 stays
 *    0, and any other active entry becomes 1 if it is 3D or 0 otherwise.
 *
 * ## 3D by position
 *
 * "3D" in a DECLARED list means `hardware_display_3d || view_count >= 2 ||
 * XRT_RENDERING_MODE_FLAG_3D_PASSTHROUGH`; "flat" means one view, no lens and
 * no passthrough flag. In a PRESENTED two-slot table, slot 1 is the 3D mode by
 * position, whatever its content (@ref u_rendering_modes_index_is_3d): a
 * passthrough slot 1 is one full-resolution view with the lens in 2D, and the
 * V key still toggles between the two slots.
 *
 * `XRT_MAX_RENDERING_MODES` and every struct layout stay as they are: this is a
 * runtime rule, not a plug-in ABI change.
 */

#pragma once

#include "xrt/xrt_device.h"
#include "xrt/xrt_limits.h"

#include <string.h>

#include <stdbool.h>
#include <stddef.h>
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

//! Is @p m a passthrough 3D entry (one view meant to serve as a 3D slot, ADR-066 C1.2)?
static inline bool
u_rendering_mode_is_passthrough_3d(const struct xrt_rendering_mode *m)
{
	return m != NULL && (m->mode_flags & XRT_RENDERING_MODE_FLAG_3D_PASSTHROUGH) != 0;
}

//! Is @p m the flat mode: one view, panel in hardware 2D, not a passthrough 3D entry?
static inline bool
u_rendering_mode_is_flat(const struct xrt_rendering_mode *m)
{
	return m != NULL && m->view_count == 1 && !m->hardware_display_3d && !u_rendering_mode_is_passthrough_3d(m);
}

//! Is @p m a 3D entry: the panel in hardware 3D, more than one view, or a passthrough 3D entry?
static inline bool
u_rendering_mode_is_3d(const struct xrt_rendering_mode *m)
{
	return m != NULL && (m->hardware_display_3d || m->view_count >= 2 || u_rendering_mode_is_passthrough_3d(m));
}

/*!
 * Can @p m be put in slot 1 on a running system (ADR-066 C1)? A 3D entry that
 * fits XRT_MAX_VIEWS and is not a start-up-only listing.
 */
static inline bool
u_rendering_mode_is_selectable(const struct xrt_rendering_mode *m)
{
	return u_rendering_mode_is_3d(m) && m->view_count <= XRT_MAX_VIEWS &&
	       (m->mode_flags & XRT_RENDERING_MODE_FLAG_START_UP_ONLY) == 0;
}

/*!
 * The sizing envelope of a declared list of @p count entries: the count
 * without the trailing start-up-only listings (XRT_RENDERING_MODE_FLAG_START_UP_ONLY).
 */
static inline uint32_t
u_rendering_modes_envelope_of(const struct xrt_rendering_mode *modes, uint32_t count)
{
	if (count > XRT_MAX_RENDERING_MODES) {
		count = XRT_MAX_RENDERING_MODES;
	}
	while (modes != NULL && count > 1 &&
	       (modes[count - 1].mode_flags & XRT_RENDERING_MODE_FLAG_START_UP_ONLY) != 0) {
		count--;
	}
	return count;
}

/*!
 * Is run-time mode @p index the 3D mode? In a PRESENTED two-slot table this is
 * positional (ADR-066 C1.2): slot 1 is 3D whatever its content, slot 0 is not.
 * Any other table (one mode, or a refused longer one) is judged by content.
 */
static inline bool
u_rendering_modes_index_is_3d(const struct xrt_rendering_mode *modes, uint32_t count, uint32_t index)
{
	if (modes == NULL || index >= count || index >= XRT_MAX_RENDERING_MODES) {
		return false;
	}
	if (count == 2) {
		return index == 1;
	}
	return u_rendering_mode_is_3d(&modes[index]);
}

/*!
 * The hardware (lens) state run-time mode @p index asks for: 3D for the 3D
 * slot, unless that slot is a passthrough 3D entry, which keeps the lens in 2D
 * (ADR-066 C1.2: the bit does not set the lens).
 */
static inline bool
u_rendering_modes_index_wants_hw_3d(const struct xrt_rendering_mode *modes, uint32_t count, uint32_t index)
{
	return u_rendering_modes_index_is_3d(modes, count, index) && !u_rendering_mode_is_passthrough_3d(&modes[index]);
}

/*!
 * The stable CHOICE NAME of a declared entry (ADR-066 C1): its driver-given
 * `mode_name` lower-cased, every run of other characters folded to one '_'
 * ("Same as 2D" -> "same_as_2d", "Cropped SBS" -> "cropped_sbs"). What the
 * per-screen 3D-mode setting stores and `dp mode set` takes; the label shown
 * to people stays the driver's own name.
 */
static inline void
u_rendering_mode_slug(const char *name, char *out, size_t cap)
{
	if (out == NULL || cap == 0) {
		return;
	}
	size_t n = 0;
	bool sep = false;
	for (const char *p = name; p != NULL && *p != '\0' && n + 1 < cap; p++) {
		char c = *p;
		if (c >= 'A' && c <= 'Z') {
			c = (char)(c - 'A' + 'a');
		}
		const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
		if (alnum) {
			if (sep && n > 0 && n + 2 < cap) {
				out[n++] = '_';
			}
			sep = false;
			out[n++] = c;
		} else {
			sep = true;
		}
	}
	out[n] = '\0';
}

//! Does the choice @p choice (a slug or the driver's own name, any case) name entry @p m?
static inline bool
u_rendering_mode_matches_choice(const struct xrt_rendering_mode *m, const char *choice)
{
	if (m == NULL || choice == NULL || choice[0] == '\0') {
		return false;
	}
	char a[XRT_DEVICE_NAME_LEN];
	char b[XRT_DEVICE_NAME_LEN];
	u_rendering_mode_slug(m->mode_name, a, sizeof(a));
	u_rendering_mode_slug(choice, b, sizeof(b));
	return a[0] != '\0' && strcmp(a, b) == 0;
}

//! The widest mode in @p modes — what the device's max view count must be.
static inline uint32_t
u_rendering_modes_max_view_count(const struct xrt_rendering_mode *modes, uint32_t count)
{
	uint32_t max = 0;
	for (uint32_t i = 0; modes != NULL && i < count && i < XRT_MAX_RENDERING_MODES; i++) {
		// ADR-066 C1.1: an entry wider than XRT_MAX_VIEWS cannot be presented
		// and never sizes the located view count.
		if (modes[i].view_count > XRT_MAX_VIEWS) {
			continue;
		}
		if (modes[i].view_count > max) {
			max = modes[i].view_count;
		}
	}
	return max;
}

//! No configured slot-1 entry (the driver does not answer CONFIGURED_3D_MODE).
#define U_RENDERING_MODES_NO_PREFERENCE 0xFFFFFFFFu

/*!
 * The 3D entry slot 1 maps to: @p preferred when that is a 3D entry at
 * index >= 1, else @p active when that is, else the first 3D entry from
 * index 1; 0 when the table has none.
 */
static inline uint32_t
u_rendering_modes_pick_3d_preferred(const struct xrt_rendering_mode *modes,
                                    uint32_t count,
                                    uint32_t active,
                                    uint32_t preferred)
{
	if (modes == NULL) {
		return 0;
	}
	if (count > XRT_MAX_RENDERING_MODES) {
		count = XRT_MAX_RENDERING_MODES;
	}
	if (preferred >= 1 && preferred < count && u_rendering_mode_is_selectable(&modes[preferred])) {
		return preferred;
	}
	if (active >= 1 && active < count && u_rendering_mode_is_selectable(&modes[active])) {
		return active;
	}
	for (uint32_t i = 1; i < count; i++) {
		if (u_rendering_mode_is_selectable(&modes[i])) {
			return i;
		}
	}
	return 0;
}

//! @ref u_rendering_modes_pick_3d_preferred with no configured entry.
static inline uint32_t
u_rendering_modes_pick_3d(const struct xrt_rendering_mode *modes, uint32_t count, uint32_t active)
{
	return u_rendering_modes_pick_3d_preferred(modes, count, active, U_RENDERING_MODES_NO_PREFERENCE);
}

/*!
 * The mode the V key toggles to (ADR-065): 2D from the 3D mode, the 3D mode
 * (index 1) from anything else. Decided from the mode the device is in NOW, so
 * the toggle is right whatever moved the mode last (an app request, the
 * session-begin floor, a 0/1 key, a driver that starts in 2D). "The 3D mode"
 * is positional in a two-slot table (ADR-066 C1.2,
 * @ref u_rendering_modes_index_is_3d), so V leaves a one-view passthrough
 * slot 1 for slot 0 instead of staying put.
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
	return u_rendering_modes_index_is_3d(modes, count, active) ? U_RENDERING_MODE_2D : U_RENDERING_MODE_3D;
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
	//! Nothing: no key, a digit for a mode the table does not have, or a
	//! digit for the mode already in effect.
	U_RENDERING_MODE_KEY_NOTHING = 0,
	//! Move the device to `target` (write the active index, pass it to the
	//! driver as OUTPUT_MODE) and request the hardware state `want_3d`.
	U_RENDERING_MODE_KEY_APPLY = 1,
	//! The session cannot fill `target` (#1499): log it, change nothing.
	U_RENDERING_MODE_KEY_REFUSED = 2,
	//! V only, with no two-mode table to move within (no head, or fewer than
	//! two modes): only request the hardware state `want_3d`.
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
 * A digit acts only on a mode that exists and is not already the one in
 * effect: with no table, a one-entry table, or the digit for @p active, it is
 * NOTHING on every path (never a hardware-only request, never a queued
 * transition to where the device already is). Only V keeps the hardware-only
 * fallback for a device without a two-mode table.
 *
 * @param modes, count    the head device's table (NULL / 0 when there is none).
 * @param active          the mode in effect: the device's active index, or the
 *                        target of a transition already queued for it.
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
		if (!toggle) {
			return d; // a digit names a mode this device does not have
		}
		d.action = U_RENDERING_MODE_KEY_HARDWARE_ONLY;
		d.want_3d = !qwerty_force_2d;
		return d;
	}
	const uint32_t target = toggle ? u_rendering_modes_toggle_target(modes, count, active) : (uint32_t)selected;
	if (target >= count || target >= XRT_MAX_RENDERING_MODES) {
		return d;
	}
	if (!toggle && target == active) {
		return d; // already there: no index write, no request, no transition
	}
	d.target = target;
	if (!u_rendering_modes_key_target_allowed(modes, count, target, fill_views)) {
		d.action = U_RENDERING_MODE_KEY_REFUSED;
		return d;
	}
	d.action = U_RENDERING_MODE_KEY_APPLY;
	// The 3D slot asks for the lens in 3D unless it is a passthrough entry
	// (ADR-066 C1.2): the app is in its 3D mode, the panel stays sharp 2D.
	d.want_3d = (target != U_RENDERING_MODE_2D) && !u_rendering_mode_is_passthrough_3d(&modes[target]);
	return d;
}

//! What @ref u_rendering_modes_trim did.
enum u_rendering_modes_trim_result
{
	//! One or two modes: nothing to select (mode_index may still be renumbered).
	U_RENDERING_MODES_TRIM_UNCHANGED = 0,
	//! More than two modes: two slots selected, the full list kept as the
	//! envelope (ADR-066 C1; nothing is dropped any more).
	U_RENDERING_MODES_TRIM_TRIMMED = 1,
	//! More than two modes, but mode 0 is not flat or no mode is 3D: the table
	//! is left exactly as it was. The caller WARNs; the contract is not met.
	U_RENDERING_MODES_TRIM_REFUSED = 2,
};

//! The details of a selection, for the caller's WARN and its index translation.
struct u_rendering_modes_trim_info
{
	enum u_rendering_modes_trim_result result;

	//! The table's count before the selection.
	uint32_t original_count;

	/*!
	 * How many entries the device's array holds as the envelope: the declared
	 * count when selected, else the (unchanged) count.
	 */
	uint32_t envelope_count;

	/*!
	 * The driver's own index of each run-time slot: `to_driver[0] = 0`,
	 * `to_driver[1] =` the slot-1 entry's driver index. Identity when nothing
	 * was selected.
	 */
	uint32_t to_driver[U_RENDERING_MODES_RUNTIME_COUNT];

	/*!
	 * The run-time slot of each driver index (0 or 1); for an entry not in a
	 * slot, 1 if it is a 3D entry and 0 otherwise. Identity below the count
	 * when nothing was selected.
	 */
	uint32_t from_driver[XRT_MAX_RENDERING_MODES];

	//! At least one envelope entry's mode_index was rewritten to its position.
	bool renumbered;
};

//! Swap entries @p a and @p b of @p modes (whole structs).
static inline void
u_rendering_modes_swap(struct xrt_rendering_mode *modes, uint32_t a, uint32_t b)
{
	if (a == b) {
		return;
	}
	struct xrt_rendering_mode t = modes[a];
	modes[a] = modes[b];
	modes[b] = t;
}

//! Fill @p info's maps for a selection whose slot 1 is driver entry @p keep (@p driver_order unpermuted).
static inline void
u_rendering_modes_fill_maps(const struct xrt_rendering_mode *driver_order,
                            uint32_t n,
                            uint32_t keep,
                            struct u_rendering_modes_trim_info *info)
{
	info->to_driver[0] = 0;
	info->to_driver[1] = keep;
	for (uint32_t i = 0; i < XRT_MAX_RENDERING_MODES; i++) {
		if (i == 0) {
			info->from_driver[i] = U_RENDERING_MODE_2D;
		} else if (i == keep) {
			info->from_driver[i] = U_RENDERING_MODE_3D;
		} else if (i < n) {
			info->from_driver[i] =
			    u_rendering_mode_is_3d(&driver_order[i]) ? U_RENDERING_MODE_3D : U_RENDERING_MODE_2D;
		} else {
			info->from_driver[i] = U_RENDERING_MODE_2D;
		}
	}
}

/*!
 * Apply the two-slot contract to a mode table, in place. Pure: no logging, no
 * device. See the file comment for the rule.
 *
 * @param modes       The table (in/out). At most XRT_MAX_RENDERING_MODES entries.
 * @param count       Its count (in/out): the presented count afterwards.
 * @param active      The active index (in/out): translated to the slots.
 * @param preferred   The driver's configured slot-1 entry (its own index), or
 *                    @ref U_RENDERING_MODES_NO_PREFERENCE.
 * @param out_info    Optional: what happened, for the caller's WARN and its
 *                    run-time <-> driver index translation.
 * @return The result, also in @p out_info.
 */
static inline enum u_rendering_modes_trim_result
u_rendering_modes_trim(struct xrt_rendering_mode *modes,
                       uint32_t *count,
                       uint32_t *active,
                       uint32_t preferred,
                       struct u_rendering_modes_trim_info *out_info)
{
	struct u_rendering_modes_trim_info info;
	info.result = U_RENDERING_MODES_TRIM_UNCHANGED;
	info.original_count = (count != NULL) ? *count : 0;
	info.envelope_count = info.original_count;
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
	// The sizing envelope leaves out trailing start-up-only listings.
	const uint32_t env = (n > U_RENDERING_MODES_RUNTIME_COUNT) ? u_rendering_modes_envelope_of(modes, n) : n;
	info.envelope_count = env;

	// Which 3D entry slot 1 maps to (0 = none: refused), from the envelope.
	const uint32_t keep =
	    (n > U_RENDERING_MODES_RUNTIME_COUNT)
	        ? u_rendering_modes_pick_3d_preferred(modes, env, (active != NULL) ? *active : 0, preferred)
	        : 0;

	if (n > U_RENDERING_MODES_RUNTIME_COUNT && (!u_rendering_mode_is_flat(&modes[0]) || keep == 0)) {
		// Refused: the modes, the count and the active index stay exactly as
		// the driver declared them (only mode_index is renumbered, below).
		info.result = U_RENDERING_MODES_TRIM_REFUSED;
		for (uint32_t i = 0; i < XRT_MAX_RENDERING_MODES; i++) {
			info.from_driver[i] = i;
		}
	} else if (n > U_RENDERING_MODES_RUNTIME_COUNT) {
		u_rendering_modes_fill_maps(modes, n, keep, &info);
		u_rendering_modes_swap(modes, 1, keep);
		*count = U_RENDERING_MODES_RUNTIME_COUNT;

		if (active != NULL) {
			*active = (*active < info.original_count) ? info.from_driver[*active] : U_RENDERING_MODE_2D;
		}
		info.result = U_RENDERING_MODES_TRIM_TRIMMED;
	}

	// mode_index == array position, on every entry of the envelope.
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
 * Point slot 1 of a SELECTED table at another declared entry (ADR-066 C1.5),
 * in place. Pure.
 *
 * The array holds the declared list permuted by the transposition (1, @p cur);
 * this undoes it and applies (1, @p next). Refused (false, nothing changed)
 * when @p next is outside the envelope, is 0, is not a 3D entry, or equals
 * @p cur. The presented count and the active index are untouched: the slot
 * keeps its index, only its entry changes.
 *
 * @param modes          The device's array.
 * @param envelope_count The declared count (> 2).
 * @param cur            The driver index slot 1 maps to now.
 * @param next           The driver index it should map to.
 * @param out_info       Optional: the new maps (result TRIMMED).
 */
static inline bool
u_rendering_modes_remap(struct xrt_rendering_mode *modes,
                        uint32_t envelope_count,
                        uint32_t cur,
                        uint32_t next,
                        struct u_rendering_modes_trim_info *out_info)
{
	if (modes == NULL || envelope_count <= U_RENDERING_MODES_RUNTIME_COUNT ||
	    envelope_count > XRT_MAX_RENDERING_MODES || cur < 1 || cur >= envelope_count || next < 1 ||
	    next >= envelope_count || next == cur) {
		return false;
	}
	// Driver entry `next` sits at `next` unless it is the driver's entry 1,
	// which the current transposition parked at `cur`.
	const uint32_t at = (next == 1) ? cur : next;
	if (!u_rendering_mode_is_selectable(&modes[at])) {
		return false;
	}
	u_rendering_modes_swap(modes, 1, cur); // back to the driver's order
	if (out_info != NULL) {
		// original_count: the caller's (the record keeps the listed count).
		out_info->result = U_RENDERING_MODES_TRIM_TRIMMED;
		out_info->original_count = envelope_count;
		out_info->envelope_count = envelope_count;
		out_info->renumbered = false;
		u_rendering_modes_fill_maps(modes, envelope_count, next, out_info);
	}
	u_rendering_modes_swap(modes, 1, next);
	for (uint32_t i = 0; i < envelope_count; i++) {
		modes[i].mode_index = i;
	}
	return true;
}

/*!
 * Enforce the two-slot contract on a head device: select its two slots with
 * @ref u_rendering_modes_trim (slot 1 = the entry the driver's
 * `XRT_DEVICE_PROPERTY_CONFIGURED_3D_MODE` names, when it answers), translate
 * its active index, set `hmd->view_count` to the maximum over the FULL
 * declared list (the envelope, ADR-066 C1.4), and log one WARN naming
 * @p driver_name and the selection (or why the table was refused).
 *
 * A device that declares more than two entries gets its `set_property` /
 * `get_property` / `destroy` wrapped (once), so that
 * `XRT_DEVICE_PROPERTY_OUTPUT_MODE` crosses in the driver's numbering
 * (run-time 1 -> the slot-1 entry's driver index, and back) and a later
 * remap (@ref u_device_rendering_modes_refresh_config) needs no new wrapper.
 * During each forwarded call, under a lock the wrapper owns, the device shows
 * the driver its own table: its declared count and its own order (the slot-1
 * transposition undone), so a pre-ADR-065 driver that range-checks the index
 * and reads `rendering_modes[index]` keeps working; the presented view is
 * restored before the call returns. u_rendering_modes.c states the residual
 * race.
 *
 * The FIRST enforce of a device, at system creation. Single-threaded there.
 * Does NOT recompute the per-mode tiling or the worst-case atlas: that is the
 * display-info apply's job, which runs over the envelope afterwards.
 *
 * @return The selection result (UNCHANGED for a NULL / non-HMD device).
 */
enum u_rendering_modes_trim_result
u_device_enforce_two_rendering_modes(struct xrt_device *xdev, const char *driver_name);

/*!
 * Re-enforce the contract after the first enforce (the display-info apply).
 * A no-op on a table that is still presented as two slots. If the driver has
 * rewritten its full table since, it is selected again keeping the SAME
 * driver index as before when that is still a 3D entry, else the first 3D
 * entry; the active index keeps its run-time meaning; the wrappers are never
 * installed twice - only their index maps are updated, under the wrapper's
 * lock.
 *
 * Threading: the caller serialises calls for one device (target_instance.c
 * holds its display-info mutex); concurrent readers of the table are not
 * locked out, exactly as they are not from the driver's own rewrite.
 */
enum u_rendering_modes_trim_result
u_device_reenforce_two_rendering_modes(struct xrt_device *xdev, const char *driver_name);

/*!
 * ADR-065 Amendment 1: the RUNTIME, not the display driver, decides the mode an
 * app starts in when the app does not ask for one: slot 1 (the 3D slot), for
 * every driver. Whatever start index the driver wrote is overridden (one WARN
 * when it differs) and the driver is told through
 * `set_property(OUTPUT_MODE, 1)`, so it and its display processor draw the slot.
 * The app overrides it with xrRequestDisplayRenderingModeDXR, as always.
 *
 * Left alone: a device pinned by its developer override
 * (`XRT_DEVICE_PROPERTY_OUTPUT_MODE_PINNED`, e.g. sim-display FORCE_MODE), a
 * table with no 3D slot (fewer than two modes), and a table the contract
 * refused (mode 0 not the flat mode).
 *
 * Call ONCE, at system creation, right after
 * @ref u_device_enforce_two_rendering_modes; never on the display-info refresh
 * path, which runs while apps are live.
 *
 * @return true when the active index was moved to slot 1.
 */
bool
u_device_rendering_modes_apply_start_slot(struct xrt_device *xdev, const char *driver_name);

/*!
 * How many entries of `xdev->rendering_modes` are the device's envelope (its
 * full declared list, ADR-066 C1.4): what every worst-case size (the system
 * atlas, `maxImageRect*`, the renderers' atlases, the located view count) is
 * computed over. Equal to `rendering_mode_count` for a device the runtime did
 * not select slots for. An IPC client's head proxy reports the count it
 * published with @ref u_device_rendering_modes_publish.
 */
uint32_t
u_device_rendering_modes_envelope_count(struct xrt_device *xdev);

/*!
 * The device's slot-table generation: bumped every time a config switch
 * remaps slot 1 (ADR-066 C1.5). 0 for a device whose table never changed. The
 * state tracker compares it per session and, on a change, queues
 * XrEventDataRenderingModeChangedDXR with the index unchanged.
 */
uint32_t
u_device_rendering_modes_generation(struct xrt_device *xdev);

/*!
 * Re-read the driver's configuration and remap slot 1 to the entry it names
 * now (ADR-066 C1.5). Asks the driver `XRT_DEVICE_PROPERTY_CONFIGURED_3D_MODE`
 * (the driver re-reads its own setting); if the answer is a 3D entry of the
 * declared list other than the current one, slot 1 is remapped in place (the
 * active index does not move), the generation is bumped, one WARN is logged
 * and, when the device is in slot 1, `OUTPUT_MODE = 1` is re-issued so the
 * driver and its display processor draw the new entry. An answer outside the
 * declared list is refused with one WARN per value.
 *
 * Threading: called from one thread per process (the state tracker's poll
 * in-process; the service's component-watch thread). The table swap is not
 * atomic against concurrent readers - the same residual race as a re-trim.
 *
 * @return true when slot 1 was remapped.
 */
bool
u_device_rendering_modes_refresh_config(struct xrt_device *xdev, const char *driver_name);

/*!
 * The per-process sync the state tracker calls each poll (ADR-066 open
 * question 8): returns the device's current generation after making it
 * current. For an IPC client's head proxy that runs the hook it registered
 * (@ref u_device_rendering_modes_set_sync_hook: re-copy the table when the
 * service's generation moved). For an in-process device whose slots the
 * runtime selected it runs @ref u_device_rendering_modes_refresh_config at
 * most once per second (@p now_ns: os_monotonic_get_ns()). Cheap otherwise:
 * a lookup in a four-entry table.
 */
uint32_t
u_device_rendering_modes_sync(struct xrt_device *xdev, uint64_t now_ns);

//! The hook an IPC head proxy registers: make the table current, return the generation.
typedef uint32_t (*u_rendering_modes_sync_fn)(struct xrt_device *xdev);

/*!
 * Record, for a device the runtime did not select slots for (an IPC client's
 * head proxy that copied the service's table), its envelope count and its
 * current generation. Creates the record when needed.
 */
void
u_device_rendering_modes_publish(struct xrt_device *xdev, uint32_t envelope_count, uint32_t generation);

//! Register (or clear, with NULL) @p xdev's sync hook; see @ref u_device_rendering_modes_sync.
void
u_device_rendering_modes_set_sync_hook(struct xrt_device *xdev, u_rendering_modes_sync_fn fn);

/*!
 * The per-screen 3D-mode choice for @p xdev's screen (ADR-066 C1): the target
 * layer resolves the screen key and reads the store (u_setting
 * `mode_3d_per_screen`). Fills @p out_name with a choice name and
 * @p out_source with an `enum xrt_status_mode_3d_source` (USER / MACHINE);
 * false when nothing is chosen. Called without any of this module's locks.
 */
typedef bool (*u_rendering_modes_choice_fn)(struct xrt_device *xdev, char *out_name, size_t cap, uint32_t *out_source);

//! Register (or clear) the process's choice resolver; see @ref u_rendering_modes_choice_fn.
void
u_rendering_modes_set_choice_resolver(u_rendering_modes_choice_fn fn);

struct xrt_status_mode_3d;

/*!
 * Fill a status `mode_3d` (ADR-066 C1) from @p xdev: the entry slot 1 maps
 * to, what chose it, and every 3D entry of the envelope in the driver's order
 * (all `applies = now`: they are sized for). `setting` is the stored choice
 * the last refresh honoured; a renderer may overlay a fresher read.
 */
void
u_device_rendering_modes_fill_status(struct xrt_device *xdev, struct xrt_status_mode_3d *out);

/*!
 * At system creation, honour a stored per-screen 3D-mode choice that names a
 * START-UP-ONLY entry (sim_display's Quad): clear that entry's
 * XRT_RENDERING_MODE_FLAG_START_UP_ONLY so it joins the envelope for this
 * instance (sizing, the located view count) and becomes selectable, as if the
 * driver had declared it - the same as OUTPUT=quad at start. Call before any
 * session exists, then re-tile. One WARN.
 *
 * @return true when an entry was promoted (the caller re-tiles and re-sizes).
 */
bool
u_device_rendering_modes_promote_choice(struct xrt_device *xdev, const char *choice);

//! Drop @p xdev's record (an unwrapped device being destroyed: an IPC proxy).
void
u_device_rendering_modes_forget(struct xrt_device *xdev);

//! One throttled INFO line (never per press) when a mode key is refused by
//! @ref u_rendering_modes_key_target_allowed.
void
u_rendering_modes_log_key_refused(const struct xrt_rendering_mode *mode, uint32_t fill_views);

//! The driver index that is run-time mode 1 for a selected device; 0 when it never was.
uint32_t
u_device_rendering_modes_kept_driver_index(struct xrt_device *xdev);

#ifdef __cplusplus
}
#endif
