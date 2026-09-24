// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Bulk window-drag snap over a grid (#1723). See u_snap_grid.h.
 * @ingroup aux_util
 */

#include "util/u_snap_grid.h"

#include <stddef.h>
#include <string.h>

static bool
in_coord_range(int64_t v)
{
	return v >= -(int64_t)U_SNAP_GRID_MAX_COORD && v <= (int64_t)U_SNAP_GRID_MAX_COORD;
}

bool
u_snap_grid_validate(const struct u_snap_grid *g, const char **out_reason)
{
	const char *why = NULL;
	if (g == NULL) {
		why = "no grid";
	} else if (g->count_x == 0 || g->count_y == 0) {
		why = "empty grid (count_x or count_y is 0)";
	} else if (g->count_x > U_SNAP_GRID_MAX_AXIS || g->count_y > U_SNAP_GRID_MAX_AXIS) {
		why = "more points on one axis than XR_WEAVE_SNAP_GRID_MAX_AXIS_DXR";
	} else if ((uint64_t)g->count_x * (uint64_t)g->count_y > U_SNAP_GRID_MAX_POINTS) {
		why = "more points than XR_WEAVE_SNAP_GRID_MAX_POINTS_DXR";
	} else if (g->step_x < 1 || g->step_y < 1 || g->step_x > U_SNAP_GRID_MAX_COORD ||
	           g->step_y > U_SNAP_GRID_MAX_COORD) {
		why = "step out of range (must be >= 1)";
	} else if (!in_coord_range(g->origin_x) || !in_coord_range(g->origin_y) || !in_coord_range(g->first_x) ||
	           !in_coord_range(g->first_y) ||
	           !in_coord_range((int64_t)g->first_x + (int64_t)g->step_x * (int64_t)(g->count_x - 1)) ||
	           !in_coord_range((int64_t)g->first_y + (int64_t)g->step_y * (int64_t)(g->count_y - 1))) {
		why = "a coordinate is out of range";
	}
	if (out_reason != NULL) {
		*out_reason = why;
	}
	return why == NULL;
}

void
u_snap_grid_eval(const struct u_snap_grid *g,
                 u_snap_grid_point_fn fn,
                 void *userdata,
                 int8_t *out_dxdy,
                 bool *out_declined,
                 uint32_t *out_calls,
                 uint32_t *out_no_delta)
{
	const uint32_t n = u_snap_grid_point_count(g);
	uint32_t calls = 0, no_delta = 0;
	bool declined = fn == NULL;

	for (uint32_t j = 0; j < g->count_y && !declined; j++) {
		const int32_t ty = g->first_y + g->step_y * (int32_t)j;
		for (uint32_t i = 0; i < g->count_x; i++) {
			const int32_t tx = g->first_x + g->step_x * (int32_t)i;
			int32_t sx = tx, sy = ty;
			calls++;
			if (!fn(userdata, g->origin_x, g->origin_y, tx, ty, &sx, &sy)) {
				declined = true;
				break;
			}
			const int64_t dx = (int64_t)sx - tx, dy = (int64_t)sy - ty;
			int8_t *p = out_dxdy + 2 * ((size_t)j * g->count_x + i);
			if (dx < -127 || dx > 127 || dy < -127 || dy > 127) {
				p[0] = U_SNAP_GRID_NO_DELTA;
				p[1] = U_SNAP_GRID_NO_DELTA;
				no_delta++;
			} else {
				p[0] = (int8_t)dx;
				p[1] = (int8_t)dy;
			}
		}
	}

	if (declined) {
		// Identity everywhere: what the per-point call hands back on a decline.
		memset(out_dxdy, 0, 2 * (size_t)n);
		no_delta = 0;
	}
	*out_declined = declined;
	if (out_calls != NULL) {
		*out_calls = calls;
	}
	if (out_no_delta != NULL) {
		*out_no_delta = no_delta;
	}
}
