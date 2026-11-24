// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The view size follows the window's screen (ADR-047 Amendment 10):
 *         a segment manager's half — which screens the window covers (after
 *         the dwell), each screen's own entry for the app's slot, and the
 *         effective table its app is told.
 * @ingroup comp_util
 *
 * Header-only and backend-agnostic, so every segment manager (vk_native,
 * D3D11, D3D12, GL, Metal) can hold one @ref comp_segments_view_size. Reads
 * each screen's table from the process registry (`util/u_screen_modes.h`),
 * re-copied only when the registry's generation moves.
 *
 * The session screen (the primary) is described by the HEAD DEVICE's two
 * slots, passed in by the caller — the registry's copy of that screen exists
 * for sizing and status only. A window wholly on the primary gets an INVALID
 * effective table: the head device's own table stands, exactly as before
 * this amendment.
 */

#pragma once

#include "util/comp_segments.h"

#include "util/u_screen_modes.h"

#include "xrt/xrt_display_metrics.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * One segment manager's view-size state.
 */
struct comp_segments_view_size
{
	struct u_screen_modes_dwell dwell;

	//! The registry generation @ref tables was copied at (0 = never).
	uint64_t registry_generation;
	//! Each screen's table, indexed like the manager's screen list.
	struct u_screen_mode_table tables[COMP_SEGMENTS_MAX_SCREENS];
	bool has_table[COMP_SEGMENTS_MAX_SCREENS];

	//! The effective table its app is told (after the dwell).
	struct xrt_effective_mode_table eff;
};

//! Start with no screens covered and an invalid table.
static inline void
comp_segments_view_size_init(struct comp_segments_view_size *vs)
{
	memset(vs, 0, sizeof(*vs));
	u_screen_modes_dwell_init(&vs->dwell, 0, 0);
}

//! Re-copy the screens' tables from the registry when it moved (cheap otherwise).
static inline void
comp_segments_view_size_sync_tables(struct comp_segments_view_size *vs,
                                    const struct comp_segments_screen *screens,
                                    uint32_t screen_count,
                                    bool force)
{
	const uint64_t gen = u_screen_modes_generation();
	if (!force && gen == vs->registry_generation && vs->registry_generation != 0) {
		return;
	}
	vs->registry_generation = gen != 0 ? gen : 1;
	for (uint32_t i = 0; i < COMP_SEGMENTS_MAX_SCREENS; i++) {
		vs->has_table[i] =
		    i < screen_count && screens != NULL && u_screen_modes_get(screens[i].id, &vs->tables[i]);
	}
}

/*!
 * Screen @p screen_index's own entry for run-time slot @p slot (0 = 2D, 1 =
 * the 3D mode), from its per-screen table; NULL when the screen has none (the
 * session's entry then applies). Never called for the primary screen, which
 * follows the head device.
 */
static inline const struct xrt_rendering_mode *
comp_segments_view_size_entry(const struct comp_segments_view_size *vs, uint32_t screen_index, uint32_t slot)
{
	if (vs == NULL || screen_index >= COMP_SEGMENTS_MAX_SCREENS || !vs->has_table[screen_index]) {
		return NULL;
	}
	return u_screen_modes_table_slot(&vs->tables[screen_index], slot);
}

//! The per-segment tile grid a segment DP is handed, see @ref comp_segments_view_size_segment_grid.
struct comp_segments_segment_grid
{
	uint32_t cols;
	uint32_t rows;
	uint32_t views;
	//! The entry the screen's DP draws, in its plug-in's per-monitor numbering (-1 = do not say).
	int32_t entry;
};

/*!
 * How a NON-primary segment's DP input is laid out (ADR-047 Amendment 10
 * decision 5), given its screen's entry @p e for the app's slot and the
 * session atlas's grid (@p session_cols x @p session_rows):
 *  - no entry: the session grid, as before;
 *  - a one-view entry (2D, or a passthrough 3D entry): one tile, the first view;
 *  - an entry needing more views than the session atlas holds: that screen
 *    shows the window in 2D — one tile, and its DP is told entry 0;
 *  - otherwise the entry's own grid, filled with the session's first views.
 * Pure.
 */
static inline struct comp_segments_segment_grid
comp_segments_view_size_segment_grid(const struct xrt_rendering_mode *e, uint32_t session_cols, uint32_t session_rows)
{
	struct comp_segments_segment_grid g;
	g.cols = session_cols > 0 ? session_cols : 1;
	g.rows = session_rows > 0 ? session_rows : 1;
	g.views = g.cols * g.rows;
	g.entry = -1;
	if (e == NULL) {
		return g;
	}
	const uint32_t session_views = g.views;
	if (e->view_count <= 1) {
		g.cols = g.rows = g.views = 1;
		g.entry = (int32_t)e->mode_index;
	} else if (e->view_count > session_views || e->tile_columns * e->tile_rows < e->view_count) {
		g.cols = g.rows = g.views = 1;
		g.entry = 0;
	} else {
		g.cols = e->tile_columns;
		g.rows = e->tile_rows;
		g.views = e->view_count;
		g.entry = (int32_t)e->mode_index;
	}
	return g;
}

/*!
 * Advance with the manager's current table @p t at @p now_ns and recompute
 * the effective table.
 *
 * @param head_modes    The head device's presented table (its two slots).
 * @param head_count    Its count.
 * @param max_views     The most views one view set of the session can carry
 *                      (0 = no limit).
 * @param cap_w, cap_h  The instance's worst-case atlas (0 = no clamp).
 * @param window_w, window_h  The window's client size (0 = no clamp).
 * @return true when the effective table changed.
 */
static inline bool
comp_segments_view_size_update(struct comp_segments_view_size *vs,
                               const struct comp_segment_table *t,
                               const struct comp_segments_screen *screens,
                               uint32_t screen_count,
                               const struct xrt_rendering_mode *head_modes,
                               uint32_t head_count,
                               uint32_t max_views,
                               uint32_t cap_w,
                               uint32_t cap_h,
                               uint32_t window_w,
                               uint32_t window_h,
                               uint64_t now_ns)
{
	if (vs == NULL || t == NULL || screens == NULL) {
		return false;
	}
	comp_segments_view_size_sync_tables(vs, screens, screen_count, false);

	uint64_t ids[U_SCREEN_MODES_MAX_COVER];
	uint32_t areas[U_SCREEN_MODES_MAX_COVER];
	uint32_t n = 0;
	for (uint32_t k = 0; k < t->count && n < U_SCREEN_MODES_MAX_COVER; k++) {
		const struct comp_segment *g = &t->seg[k];
		if (g->window_rect.w == 0 || g->window_rect.h == 0) {
			continue;
		}
		ids[n] = g->screen_id;
		areas[n] = g->window_rect.w * g->window_rect.h;
		n++;
	}
	(void)u_screen_modes_dwell_update(&vs->dwell, ids, areas, n, now_ns);

	struct xrt_effective_mode_table next;
	memset(&next, 0, sizeof(next));
	const struct u_screen_modes_dwell *d = &vs->dwell;
	bool primary_only = d->have_stable;
	struct u_screen_modes_cover covers[U_SCREEN_MODES_MAX_COVER];
	uint32_t cn = 0;
	uint32_t majority = 0;
	for (uint32_t s = 0; d->have_stable && s < d->stable_n && cn < U_SCREEN_MODES_MAX_COVER; s++) {
		int idx = -1;
		for (uint32_t i = 0; i < screen_count && i < COMP_SEGMENTS_MAX_SCREENS; i++) {
			if (screens[i].id == d->stable_ids[s]) {
				idx = (int)i;
				break;
			}
		}
		if (idx < 0) {
			continue; // a screen the list no longer has
		}
		struct u_screen_modes_cover *c = &covers[cn];
		memset(c, 0, sizeof(*c));
		c->screen_id = screens[idx].id;
		if (screens[idx].is_primary) {
			if (head_modes == NULL || head_count == 0) {
				continue;
			}
			c->slot[0] = &head_modes[0];
			c->slot[1] = head_count > 1 ? &head_modes[1] : &head_modes[0];
			c->pixel_width = vs->has_table[idx] ? vs->tables[idx].pixel_width : 0;
			c->pixel_height = vs->has_table[idx] ? vs->tables[idx].pixel_height : 0;
			c->rotated = vs->has_table[idx] && vs->tables[idx].rotated;
		} else {
			primary_only = false;
			if (!vs->has_table[idx]) {
				continue; // no table: this screen does not steer the size
			}
			c->slot[0] = u_screen_modes_table_slot(&vs->tables[idx], 0);
			c->slot[1] = u_screen_modes_table_slot(&vs->tables[idx], 1);
			c->pixel_width = vs->tables[idx].pixel_width;
			c->pixel_height = vs->tables[idx].pixel_height;
			c->rotated = vs->tables[idx].rotated;
		}
		if (c->screen_id == d->stable_majority) {
			majority = cn;
		}
		cn++;
	}
	if (!primary_only && cn > 0) {
		u_screen_modes_effective(covers, cn, majority, max_views, window_w, window_h, cap_w, cap_h, &next);
	}
	const bool changed =
	    !u_screen_modes_effective_equal(&vs->eff, &next) || vs->eff.majority_screen_id != next.majority_screen_id;
	vs->eff = next;
	return changed;
}

#ifdef __cplusplus
}
#endif
