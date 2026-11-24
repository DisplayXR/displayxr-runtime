// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Every claimed screen's own rendering-mode table, the effective table
 *         of a window, and the instance's worst case over every screen
 *         (ADR-047 Amendment 10, ADR-066 C1).
 * @ingroup aux_util
 *
 * ## Per-screen tables
 *
 * The head device carries the table of ITS screen (the system default,
 * `u_rendering_modes.h`). Every other claimed screen has its own table here: the
 * list its plug-in declares for that monitor
 * (`xrt_plugin_iface::get_rendering_modes_for_monitor`), else the head device's
 * list (today's behaviour). Each table keeps the full declared list in the
 * plug-in's own order and two SLOTS of it: slot 0 = entry 0 (flat 2D), slot 1
 * = the entry that screen's "3D mode" names — the per-screen choice
 * (`dp mode set --screen`, u_setting `mode_3d_per_screen`) when it names a
 * selectable entry, else the plug-in's default, else the first selectable 3D
 * entry. The app owns its slot (2D or 3D, ADR-066 principle 0); a screen's DP
 * draws that screen's entry for it.
 *
 * ## The effective table (what one window's app enumerates)
 *
 * @ref u_screen_modes_effective, from the screens the window covers (after the
 * hysteresis of @ref u_screen_modes_dwell):
 *  - wholly on one screen: that screen's two slots;
 *  - straddling: per slot, the layout (view count, tiles, name, lens state) of
 *    the covered entry with the most views the session can fill (the majority
 *    screen breaks ties), and the per-dimension MAXIMUM view scale over the
 *    covered entries, taken in window axes (a screen rotated by 90 / 270
 *    degrees has its scales swapped first); clamped so the window's atlas never
 *    exceeds the instance's worst case;
 *  - an entry needing more views than the session can fill counts as that
 *    screen's 2D entry (the screen shows the window flat, decision 5).
 *
 * ## The worst case
 *
 * @ref u_screen_modes_union_atlas: the per-dimension maximum, over every sized
 * screen and every entry of its envelope, of tiles x view scale x that screen's
 * pixels — the swapchain an app allocates once (ADR-010) so a move between
 * screens never overflows it. A screen can be left out (a user setting, the
 * per-screen sizing opt-out): a window on it is resampled from the union.
 *
 * The inline helpers are pure (tests_u_screen_modes); the registry (the .c) is
 * process-global, written by the target layer, read by compositors, the state
 * tracker and status.
 */

#pragma once

#include "xrt/xrt_device.h"
#include "xrt/xrt_display_metrics.h"
#include "xrt/xrt_display_status.h"
#include "xrt/xrt_limits.h"

#include "util/u_rendering_modes.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Max screens the registry holds (= XRT_SCREEN_LIST_MAX).
#define U_SCREEN_MODES_MAX_SCREENS 16

//! Size of the string fields, incl. the NUL.
#define U_SCREEN_MODES_NAME_MAX 64

//! Max screens one window's effective table is computed over (= COMP_SEGMENTS_MAX).
#define U_SCREEN_MODES_MAX_COVER 8

//! Default dwell before a new covered set / majority takes effect (Amendment 2's 0.5 s).
#define U_SCREEN_MODES_DEFAULT_HOLD_NS (500ull * 1000ull * 1000ull)

//! Default majority dead band, percent of the window's on-screen area (Amendment 2's 20 %).
#define U_SCREEN_MODES_DEFAULT_MARGIN_PCT 20u

//! Where a screen table's list came from.
enum u_screen_modes_list_source
{
	//! Nothing (an empty table).
	U_SCREEN_MODES_LIST_NONE = 0,
	//! The head device's own table: this is the system-default screen.
	U_SCREEN_MODES_LIST_HEAD = 1,
	//! The owning plug-in's `get_rendering_modes_for_monitor`.
	U_SCREEN_MODES_LIST_PLUGIN = 2,
	//! The plug-in did not answer: a copy of the head device's list.
	U_SCREEN_MODES_LIST_HEAD_COPY = 3,
};

/*!
 * One screen's rendering-mode table.
 */
struct u_screen_mode_table
{
	uint64_t screen_id;
	char key[U_SCREEN_MODES_NAME_MAX];       //!< Stable screen key (per-screen settings).
	char plugin_id[U_SCREEN_MODES_NAME_MAX]; //!< The plug-in that won the screen.

	//! The screen's pixel size in the PANEL's axes (the axes the scales are declared in).
	uint32_t pixel_width;
	uint32_t pixel_height;

	//! The current orientation is the panel's rotated by 90 / 270 degrees.
	bool rotated;

	//! `enum u_screen_modes_list_source`.
	uint32_t list_source;

	//! Counts toward the instance's worst case (false: the user opted it out).
	bool sized;

	//! The declared list, in the plug-in's own order, pixel fields for this screen.
	struct xrt_rendering_mode modes[XRT_MAX_RENDERING_MODES];
	uint32_t count;          //!< Declared entries.
	uint32_t envelope_count; //!< Without the trailing START_UP_ONLY listings.

	uint32_t default_3d;                   //!< The plug-in's own slot-1 entry (0 = none said).
	uint32_t slot1;                        //!< The entry slot 1 maps to (>= 1; 0 = the list has no 3D entry).
	uint32_t source;                       //!< What chose slot 1: `enum xrt_status_mode_3d_source`.
	char setting[U_SCREEN_MODES_NAME_MAX]; //!< The stored choice that was honoured ("" = none).
};

/*!
 * Fill @p t's list from @p modes (@p count entries, plug-in order) for a screen
 * of @p pixel_width x @p pixel_height (panel axes): copies, renumbers
 * `mode_index`, drops entries wider than XRT_MAX_VIEWS from selection (they
 * stay listed), computes every entry's pixel fields, the envelope, and slot 1
 * from @p default_3d. Leaves the identity fields to the caller. Pure.
 */
static inline void
u_screen_modes_table_set_list(struct u_screen_mode_table *t,
                              const struct xrt_rendering_mode *modes,
                              uint32_t count,
                              uint32_t default_3d,
                              uint32_t pixel_width,
                              uint32_t pixel_height)
{
	if (t == NULL) {
		return;
	}
	if (count > XRT_MAX_RENDERING_MODES) {
		count = XRT_MAX_RENDERING_MODES;
	}
	memset(t->modes, 0, sizeof(t->modes));
	t->count = 0;
	for (uint32_t i = 0; modes != NULL && i < count; i++) {
		t->modes[i] = modes[i];
		t->modes[i].mode_index = i;
		if (t->modes[i].tile_columns == 0) {
			t->modes[i].tile_columns = 1;
		}
		if (t->modes[i].tile_rows == 0) {
			t->modes[i].tile_rows = 1;
		}
		if (pixel_width > 0 && pixel_height > 0) {
			struct xrt_rendering_mode *m = &t->modes[i];
			uint32_t vw = (uint32_t)((float)pixel_width * m->view_scale_x);
			uint32_t vh = (uint32_t)((float)pixel_height * m->view_scale_y);
			m->view_width_pixels = vw > 0 ? vw : pixel_width;
			m->view_height_pixels = vh > 0 ? vh : pixel_height;
			m->atlas_width_pixels = m->tile_columns * m->view_width_pixels;
			m->atlas_height_pixels = m->tile_rows * m->view_height_pixels;
		}
		t->count++;
	}
	t->pixel_width = pixel_width;
	t->pixel_height = pixel_height;
	t->envelope_count = u_rendering_modes_envelope_of(t->modes, t->count);
	t->default_3d = default_3d;
	t->slot1 = u_rendering_modes_pick_3d_preferred(t->modes, t->envelope_count, 0, default_3d);
	t->source = XRT_STATUS_MODE_3D_SOURCE_RUNTIME_DEFAULT;
	if (t->slot1 != 0 && t->slot1 == default_3d) {
		t->source = XRT_STATUS_MODE_3D_SOURCE_DRIVER_DEFAULT;
	}
	t->setting[0] = '\0';
}

/*!
 * Point slot 1 at the entry the per-screen choice @p choice names (a choice
 * name or the plug-in's own name), when that entry is selectable; otherwise at
 * the plug-in's default (or the first selectable 3D entry). @p choice_source
 * is the `enum xrt_status_mode_3d_source` of the stored choice (USER /
 * MACHINE); NULL / "" means no choice. Pure.
 *
 * @return true when slot 1, its source or the honoured setting changed.
 */
static inline bool
u_screen_modes_table_apply_choice(struct u_screen_mode_table *t, const char *choice, uint32_t choice_source)
{
	if (t == NULL) {
		return false;
	}
	uint32_t slot1 = 0;
	uint32_t source = 0;
	const char *honoured = "";
	if (choice != NULL && choice[0] != '\0') {
		for (uint32_t i = 1; i < t->envelope_count; i++) {
			if (u_rendering_mode_is_selectable(&t->modes[i]) &&
			    u_rendering_mode_matches_choice(&t->modes[i], choice)) {
				slot1 = i;
				source = choice_source;
				honoured = choice;
				break;
			}
		}
	}
	if (slot1 == 0) {
		slot1 = u_rendering_modes_pick_3d_preferred(t->modes, t->envelope_count, 0, t->default_3d);
		source = (slot1 != 0 && slot1 == t->default_3d) ? (uint32_t)XRT_STATUS_MODE_3D_SOURCE_DRIVER_DEFAULT
		                                                : (uint32_t)XRT_STATUS_MODE_3D_SOURCE_RUNTIME_DEFAULT;
	}
	const bool changed = slot1 != t->slot1 || source != t->source || strcmp(honoured, t->setting) != 0;
	t->slot1 = slot1;
	t->source = source;
	snprintf(t->setting, sizeof(t->setting), "%s", honoured);
	return changed;
}

/*!
 * The entry run-time slot @p slot (0 = 2D, 1 = the 3D mode) of @p t maps to;
 * NULL for an empty table. A table with no 3D entry answers slot 1 with entry 0.
 */
static inline const struct xrt_rendering_mode *
u_screen_modes_table_slot(const struct u_screen_mode_table *t, uint32_t slot)
{
	if (t == NULL || t->count == 0) {
		return NULL;
	}
	if (slot == 0 || t->slot1 == 0 || t->slot1 >= t->count) {
		return &t->modes[0];
	}
	return &t->modes[t->slot1];
}

//! The plug-in index of slot @p slot's entry (0 for slot 0 or an empty table).
static inline uint32_t
u_screen_modes_table_slot_index(const struct u_screen_mode_table *t, uint32_t slot)
{
	const struct xrt_rendering_mode *m = u_screen_modes_table_slot(t, slot);
	return m != NULL ? m->mode_index : 0;
}

/*!
 * The instance's worst case over @p n tables (ADR-047 Amendment 10 decision 1):
 * the per-dimension maximum atlas and per-view rect over every SIZED table's
 * envelope, each entry against its own screen in its current orientation
 * (the rotated axes swap the pixel size before the scale; a CAN_ROTATE entry
 * counts in both). Unsized (opted-out) tables and empty ones are skipped.
 * Every out pointer may be NULL; the answers are max-merged INTO them, so a
 * caller seeds them with the head screen's worst case. Pure.
 */
static inline void
u_screen_modes_union_atlas(const struct u_screen_mode_table *tables,
                           uint32_t n,
                           uint32_t *inout_atlas_w,
                           uint32_t *inout_atlas_h,
                           uint32_t *inout_view_w,
                           uint32_t *inout_view_h)
{
	for (uint32_t s = 0; tables != NULL && s < n; s++) {
		const struct u_screen_mode_table *t = &tables[s];
		if (!t->sized || t->count == 0 || t->pixel_width == 0 || t->pixel_height == 0) {
			continue;
		}
		for (uint32_t i = 0; i < t->envelope_count; i++) {
			const struct xrt_rendering_mode *m = &t->modes[i];
			if (m->view_count > XRT_MAX_VIEWS) {
				continue;
			}
			// The current orientation first; a CAN_ROTATE entry also counts
			// in the other one (u_tiling_compute_system_atlas_oriented's rule).
			const uint32_t cur_w = t->rotated ? t->pixel_height : t->pixel_width;
			const uint32_t cur_h = t->rotated ? t->pixel_width : t->pixel_height;
			const float sx = t->rotated ? m->view_scale_y : m->view_scale_x;
			const float sy = t->rotated ? m->view_scale_x : m->view_scale_y;
			// The tile grid is the app's atlas layout: never rotated.
			const uint32_t tc = m->tile_columns;
			const uint32_t tr = m->tile_rows;
			const uint32_t orient_n = (m->mode_flags & XRT_RENDERING_MODE_FLAG_CAN_ROTATE) != 0 ? 2u : 1u;
			for (uint32_t o = 0; o < orient_n; o++) {
				const uint32_t dw = o == 0 ? cur_w : cur_h;
				const uint32_t dh = o == 0 ? cur_h : cur_w;
				uint32_t vw = (uint32_t)((float)dw * sx);
				uint32_t vh = (uint32_t)((float)dh * sy);
				if (vw == 0) {
					vw = dw;
				}
				if (vh == 0) {
					vh = dh;
				}
				const uint32_t aw = tc * vw;
				const uint32_t ah = tr * vh;
				if (inout_atlas_w != NULL && aw > *inout_atlas_w) {
					*inout_atlas_w = aw;
				}
				if (inout_atlas_h != NULL && ah > *inout_atlas_h) {
					*inout_atlas_h = ah;
				}
				if (inout_view_w != NULL && vw > *inout_view_w) {
					*inout_view_w = vw;
				}
				if (inout_view_h != NULL && vh > *inout_view_h) {
					*inout_view_h = vh;
				}
			}
		}
	}
}

/*!
 * Does every sized entry of @p t fit an instance whose worst case is
 * @p atlas_w x @p atlas_h (a screen claimed after the instance was sized,
 * ADR-066 C2)? Pure.
 */
static inline bool
u_screen_modes_table_fits(const struct u_screen_mode_table *t, uint32_t atlas_w, uint32_t atlas_h)
{
	if (t == NULL || atlas_w == 0 || atlas_h == 0) {
		return true; // nothing known to compare against
	}
	struct u_screen_mode_table one = *t;
	one.sized = true;
	uint32_t w = 0, h = 0;
	u_screen_modes_union_atlas(&one, 1, &w, &h, NULL, NULL);
	return w <= atlas_w && h <= atlas_h;
}


/*!
 * Is the screen @p key named by the list @p spec (`;` / `,` separated,
 * whitespace trimmed, ASCII case-insensitive)? The per-screen sizing opt-out
 * (`DXR_SIZE_EXCLUDE_SCREENS`) is spelled this way. Pure.
 */
static inline bool
u_screen_modes_key_listed(const char *spec, const char *key)
{
	if (spec == NULL || key == NULL || key[0] == '\0') {
		return false;
	}
	const size_t klen = strlen(key);
	const char *p = spec;
	while (*p != '\0') {
		while (*p == ' ' || *p == '\t' || *p == ';' || *p == ',') {
			p++;
		}
		const char *start = p;
		while (*p != '\0' && *p != ';' && *p != ',') {
			p++;
		}
		const char *end = p;
		while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
			end--;
		}
		if ((size_t)(end - start) == klen && klen > 0) {
			bool same = true;
			for (size_t i = 0; i < klen && same; i++) {
				char a = start[i], b = key[i];
				a = (a >= 'A' && a <= 'Z') ? (char)(a - 'A' + 'a') : a;
				b = (b >= 'A' && b <= 'Z') ? (char)(b - 'A' + 'a') : b;
				same = a == b;
			}
			if (same) {
				return true;
			}
		}
	}
	return false;
}

/*
 *
 * The effective table of one window.
 *
 */

//! One covered screen, as @ref u_screen_modes_effective reads it.
struct u_screen_modes_cover
{
	uint64_t screen_id;
	//! The screen's entry for slot 0 and slot 1 (NULL = the screen has no table: skipped).
	const struct xrt_rendering_mode *slot[2];
	//! The screen's pixel size, panel axes (for view_*_pixels).
	uint32_t pixel_width;
	uint32_t pixel_height;
	//! The screen is rotated by 90 / 270 degrees from its panel's native axes.
	bool rotated;
};

//! Copy @p m into effective slot @p out (scales in window axes for @p rotated).
static inline void
u_screen_modes_slot_from_mode(const struct xrt_rendering_mode *m, bool rotated, struct xrt_effective_mode_slot *out)
{
	memset(out, 0, sizeof(*out));
	out->view_count = m->view_count;
	out->tile_columns = m->tile_columns > 0 ? m->tile_columns : 1;
	out->tile_rows = m->tile_rows > 0 ? m->tile_rows : 1;
	out->view_scale_x = rotated ? m->view_scale_y : m->view_scale_x;
	out->view_scale_y = rotated ? m->view_scale_x : m->view_scale_y;
	out->hardware_display_3d = m->hardware_display_3d;
	out->mode_flags = m->mode_flags;
	snprintf(out->mode_name, sizeof(out->mode_name), "%s", m->mode_name);
}

/*!
 * Compute the effective table of a window covering @p n screens (ADR-047
 * Amendment 10 decision 2). Pure; see the file comment for the rule.
 *
 * @param covers          The covered screens (after the dwell), any order.
 * @param n               How many.
 * @param majority_index  Index into @p covers of the majority screen (ties go to it).
 * @param max_views       The most views one view set of this session can carry
 *                        (an entry needing more shows that screen in 2D); 0 = no limit.
 * @param window_w        The window's client size, window px (0 = no clamp).
 * @param window_h
 * @param atlas_cap_w     The instance's worst-case atlas (0 = no clamp).
 * @param atlas_cap_h
 * @param[out] out        The table; invalid when no cover has a table.
 */
static inline void
u_screen_modes_effective(const struct u_screen_modes_cover *covers,
                         uint32_t n,
                         uint32_t majority_index,
                         uint32_t max_views,
                         uint32_t window_w,
                         uint32_t window_h,
                         uint32_t atlas_cap_w,
                         uint32_t atlas_cap_h,
                         struct xrt_effective_mode_table *out)
{
	memset(out, 0, sizeof(*out));
	if (covers == NULL || n == 0) {
		return;
	}
	if (n > U_SCREEN_MODES_MAX_COVER) {
		n = U_SCREEN_MODES_MAX_COVER;
	}
	if (majority_index >= n) {
		majority_index = 0;
	}
	out->majority_screen_id = covers[majority_index].screen_id;
	for (uint32_t slot = 0; slot < 2; slot++) {
		int32_t layout = -1;
		uint32_t layout_views = 0;
		float sx = 0.0f, sy = 0.0f;
		for (uint32_t pass = 0; pass < 2; pass++) {
			// Pass 0 the majority, pass 1 the rest: a tie on view count stays
			// with the majority.
			for (uint32_t i = 0; i < n; i++) {
				if ((pass == 0) != (i == majority_index)) {
					continue;
				}
				const struct u_screen_modes_cover *c = &covers[i];
				const struct xrt_rendering_mode *m = c->slot[slot];
				if (m == NULL) {
					continue;
				}
				// Decision 5: an entry the session cannot fill shows its
				// screen in 2D — it counts as that screen's 2D entry.
				if (max_views > 0 && m->view_count > max_views && c->slot[0] != NULL) {
					m = c->slot[0];
				}
				const float mx = c->rotated ? m->view_scale_y : m->view_scale_x;
				const float my = c->rotated ? m->view_scale_x : m->view_scale_y;
				if (mx > sx) {
					sx = mx;
				}
				if (my > sy) {
					sy = my;
				}
				if (layout < 0 || m->view_count > layout_views) {
					layout = (int32_t)i;
					layout_views = m->view_count;
					u_screen_modes_slot_from_mode(m, c->rotated, &out->slot[slot]);
				}
			}
		}
		if (layout < 0) {
			memset(out, 0, sizeof(*out));
			return; // a covered screen without a table: nothing per screen
		}
		struct xrt_effective_mode_slot *e = &out->slot[slot];
		e->view_scale_x = sx;
		e->view_scale_y = sy;
		// The clamp: the window's atlas at this scale must fit the instance's
		// worst case (a straddle can combine one screen's tile grid with
		// another's scale, which no single declared entry has).
		if (window_w > 0 && atlas_cap_w > 0 && e->tile_columns > 0) {
			const float need = (float)window_w * e->view_scale_x * (float)e->tile_columns;
			if (need > (float)atlas_cap_w) {
				e->view_scale_x = (float)atlas_cap_w / ((float)window_w * (float)e->tile_columns);
				out->clamped[slot] = true;
			}
		}
		if (window_h > 0 && atlas_cap_h > 0 && e->tile_rows > 0) {
			const float need = (float)window_h * e->view_scale_y * (float)e->tile_rows;
			if (need > (float)atlas_cap_h) {
				e->view_scale_y = (float)atlas_cap_h / ((float)window_h * (float)e->tile_rows);
				out->clamped[slot] = true;
			}
		}
		const struct u_screen_modes_cover *lc = &covers[layout];
		const uint32_t pw = lc->rotated ? lc->pixel_height : lc->pixel_width;
		const uint32_t ph = lc->rotated ? lc->pixel_width : lc->pixel_height;
		e->view_width_pixels = (uint32_t)((float)pw * e->view_scale_x);
		e->view_height_pixels = (uint32_t)((float)ph * e->view_scale_y);
		out->layout_screen_id[slot] = lc->screen_id;
	}
	out->screen_count = n;
	out->valid = true;
}

//! Do two effective tables describe the same table to an app (validity, every slot field)?
static inline bool
u_screen_modes_effective_equal(const struct xrt_effective_mode_table *a, const struct xrt_effective_mode_table *b)
{
	if (a->valid != b->valid) {
		return false;
	}
	if (!a->valid) {
		return true;
	}
	for (uint32_t s = 0; s < 2; s++) {
		const struct xrt_effective_mode_slot *x = &a->slot[s];
		const struct xrt_effective_mode_slot *y = &b->slot[s];
		if (x->view_count != y->view_count || x->tile_columns != y->tile_columns ||
		    x->tile_rows != y->tile_rows || x->view_scale_x != y->view_scale_x ||
		    x->view_scale_y != y->view_scale_y || x->hardware_display_3d != y->hardware_display_3d ||
		    x->mode_flags != y->mode_flags || x->view_width_pixels != y->view_width_pixels ||
		    x->view_height_pixels != y->view_height_pixels || strcmp(x->mode_name, y->mode_name) != 0) {
			return false;
		}
	}
	return true;
}

/*!
 * Overlay effective slot @p e onto a copy of a device mode @p base (keeps the
 * fields an app does not see from the device: mode_index, reserved), so a
 * compositor can size its atlas from the table the app was told. Pure.
 */
static inline void
u_screen_modes_slot_apply(const struct xrt_effective_mode_slot *e, struct xrt_rendering_mode *inout)
{
	inout->view_count = e->view_count;
	inout->tile_columns = e->tile_columns;
	inout->tile_rows = e->tile_rows;
	inout->view_scale_x = e->view_scale_x;
	inout->view_scale_y = e->view_scale_y;
	inout->hardware_display_3d = e->hardware_display_3d;
	inout->mode_flags = e->mode_flags;
	inout->view_width_pixels = e->view_width_pixels;
	inout->view_height_pixels = e->view_height_pixels;
	inout->atlas_width_pixels = e->tile_columns * e->view_width_pixels;
	inout->atlas_height_pixels = e->tile_rows * e->view_height_pixels;
	snprintf(inout->mode_name, sizeof(inout->mode_name), "%s", e->mode_name);
}


/*
 *
 * The hysteresis (ADR-047 Amendment 2's dwell and dead band).
 *
 */

/*!
 * Which screens a window's effective table is computed over. A new covered
 * set (or a new majority) takes effect only after it has held continuously
 * for `hold_ns`; the majority additionally moves only when the challenger's
 * area beats the current majority's by `margin_pct` percent of the window's
 * on-screen area. Dragging back and forth across a seam faster than the hold
 * changes nothing. Pure state: the caller owns the clock.
 */
struct u_screen_modes_dwell
{
	uint64_t hold_ns;
	uint32_t margin_pct;

	bool have_stable;
	uint32_t stable_n;
	uint64_t stable_ids[U_SCREEN_MODES_MAX_COVER]; //!< Sorted ascending.
	uint64_t stable_majority;

	bool pending;
	uint32_t pend_n;
	uint64_t pend_ids[U_SCREEN_MODES_MAX_COVER];
	uint64_t pend_majority;
	uint64_t pend_since_ns;
};

//! 0 for either threshold selects the default.
static inline void
u_screen_modes_dwell_init(struct u_screen_modes_dwell *d, uint64_t hold_ns, uint32_t margin_pct)
{
	memset(d, 0, sizeof(*d));
	d->hold_ns = hold_ns != 0 ? hold_ns : U_SCREEN_MODES_DEFAULT_HOLD_NS;
	d->margin_pct = margin_pct != 0 ? margin_pct : U_SCREEN_MODES_DEFAULT_MARGIN_PCT;
}

/*!
 * Advance with the covered screens @p ids (each with its on-screen area
 * @p areas, window px) at @p now_ns. The first call adopts the set at once
 * (a window's first placement is not a move). An empty set keeps the stable
 * one (a window on no screen has nothing better to report).
 *
 * @return true when the stable set or majority changed on this call.
 */
static inline bool
u_screen_modes_dwell_update(
    struct u_screen_modes_dwell *d, const uint64_t *ids, const uint32_t *areas, uint32_t n, uint64_t now_ns)
{
	if (d == NULL || ids == NULL || areas == NULL || n == 0) {
		return false;
	}
	if (n > U_SCREEN_MODES_MAX_COVER) {
		n = U_SCREEN_MODES_MAX_COVER;
	}
	// The challenger majority: the largest area (first on a tie).
	uint32_t best = 0;
	uint64_t total = 0;
	uint32_t stable_area = 0;
	bool stable_covered = false;
	for (uint32_t i = 0; i < n; i++) {
		total += areas[i];
		if (areas[i] > areas[best]) {
			best = i;
		}
		if (d->have_stable && ids[i] == d->stable_majority) {
			stable_covered = true;
			stable_area = areas[i];
		}
	}
	uint64_t majority = ids[best];
	// The dead band: the current majority stays while it is covered and the
	// challenger does not beat it by the margin.
	if (stable_covered && majority != d->stable_majority &&
	    (uint64_t)(areas[best] - stable_area) * 100u < (uint64_t)d->margin_pct * total) {
		majority = d->stable_majority;
	}
	// The candidate set, sorted.
	uint64_t cand[U_SCREEN_MODES_MAX_COVER];
	for (uint32_t i = 0; i < n; i++) {
		cand[i] = ids[i];
	}
	for (uint32_t i = 1; i < n; i++) {
		for (uint32_t j = i; j > 0 && cand[j - 1] > cand[j]; j--) {
			const uint64_t t = cand[j - 1];
			cand[j - 1] = cand[j];
			cand[j] = t;
		}
	}
	if (!d->have_stable) {
		d->have_stable = true;
		d->stable_n = n;
		memcpy(d->stable_ids, cand, n * sizeof(uint64_t));
		d->stable_majority = majority;
		d->pending = false;
		return true;
	}
	const bool same_as_stable = n == d->stable_n && memcmp(cand, d->stable_ids, n * sizeof(uint64_t)) == 0 &&
	                            majority == d->stable_majority;
	if (same_as_stable) {
		d->pending = false;
		return false;
	}
	const bool same_as_pending = d->pending && n == d->pend_n &&
	                             memcmp(cand, d->pend_ids, n * sizeof(uint64_t)) == 0 &&
	                             majority == d->pend_majority;
	if (!same_as_pending) {
		d->pending = true;
		d->pend_n = n;
		memcpy(d->pend_ids, cand, n * sizeof(uint64_t));
		d->pend_majority = majority;
		d->pend_since_ns = now_ns;
		return false;
	}
	if (now_ns - d->pend_since_ns < d->hold_ns) {
		return false;
	}
	d->stable_n = d->pend_n;
	memcpy(d->stable_ids, d->pend_ids, d->pend_n * sizeof(uint64_t));
	d->stable_majority = d->pend_majority;
	d->pending = false;
	return true;
}

//! Is @p id in the dwell's stable set?
static inline bool
u_screen_modes_dwell_has(const struct u_screen_modes_dwell *d, uint64_t id)
{
	for (uint32_t i = 0; d != NULL && d->have_stable && i < d->stable_n; i++) {
		if (d->stable_ids[i] == id) {
			return true;
		}
	}
	return false;
}


/*
 *
 * The process-wide registry.
 *
 */

/*!
 * Replace the registry with @p n tables (the target layer, on every screen-table
 * publish). Choices already applied in @p tables are kept; bumps the generation
 * when anything an app or a DP could see changed.
 */
void
u_screen_modes_publish(const struct u_screen_mode_table *tables, uint32_t n);

//! Empty the registry (a system being destroyed).
void
u_screen_modes_clear(void);

//! Copy screen @p screen_id's table into @p out; false when it has none.
bool
u_screen_modes_get(uint64_t screen_id, struct u_screen_mode_table *out);

//! Copy every table (at most @p cap) into @p out; returns how many.
uint32_t
u_screen_modes_get_all(struct u_screen_mode_table *out, uint32_t cap);

//! Bumped whenever a table or a slot-1 mapping changes (a 3D-mode switch, a hotplug).
uint64_t
u_screen_modes_generation(void);

/*!
 * Resolves screen @p key's stored 3D-mode choice: fills @p out_name and
 * @p out_source (`enum xrt_status_mode_3d_source`, USER / MACHINE); false when
 * none is stored. Called without the registry's lock.
 */
typedef bool (*u_screen_modes_choice_fn)(const char *key, char *out_name, size_t cap, uint32_t *out_source);

//! Register (or clear) the process's choice resolver.
void
u_screen_modes_set_choice_resolver(u_screen_modes_choice_fn fn);

/*!
 * Re-read every table's per-screen choice and remap slot 1 where it moved.
 * Throttled to once a second unless @p force. Logs one WARN per remap.
 *
 * @return true when a slot-1 mapping changed (the generation was bumped).
 */
bool
u_screen_modes_refresh_choices(uint64_t now_ns, bool force);

/*!
 * The instance's worst case over the registry (@ref u_screen_modes_union_atlas
 * on every table), max-merged into the out values.
 */
void
u_screen_modes_registry_union(uint32_t *inout_atlas_w,
                              uint32_t *inout_atlas_h,
                              uint32_t *inout_view_w,
                              uint32_t *inout_view_h);

/*!
 * Record (0, 0 clears) the running instance's worst-case atlas, which a screen
 * claimed later must fit (target layer, at the system's first screen publish).
 */
void
u_screen_modes_set_instance_worst(uint32_t atlas_w, uint32_t atlas_h);

//! The recorded worst case; false (and 0, 0) when none.
bool
u_screen_modes_get_instance_worst(uint32_t *out_atlas_w, uint32_t *out_atlas_h);

/*!
 * Fill a status `mode_3d` (ADR-066 C1.3) from table @p t: the entry slot 1
 * maps to, what chose it, and every 3D entry of the list. Entries outside the
 * envelope report `restart_app`; with @p fits_instance false (the screen came
 * after the instance and does not fit its worst case) every entry reports
 * `restart_app` too.
 */
void
u_screen_modes_fill_status(const struct u_screen_mode_table *t, bool fits_instance, struct xrt_status_mode_3d *out);

#ifdef __cplusplus
}
#endif
