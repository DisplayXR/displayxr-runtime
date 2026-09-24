// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Bulk window-drag snap: evaluate a display processor's per-point
 *         `snap_window_rect` over a rectangular grid of targets (#1723).
 *
 * The drag lattice (docs/specs/runtime/wayland-window-geometry.md §8) is built
 * by asking the display processor where the window may land for thousands of
 * nearby targets. The DP answers ONE point per call and must keep answering
 * one point per call: its lens lattice never leaves the plug-in (ADR-019), and
 * a new "grid" slot would be a vendor change for what is only a loop. So the
 * runtime runs the loop itself, next to the DP, and hands the caller the whole
 * answer set at once — in-process that saves nothing, over IPC it turns
 * 16,641 service round trips (~2.4 s measured) into one.
 *
 * This file is the one loop every route shares (the in-process vk_native
 * session, the comp_multi weave engine, the D3D11 service), so the grid
 * semantics — order, encoding, declined, bounds — cannot drift between them.
 *
 * ## Encoding
 *
 * Row-major (y outer, x inner): point k = j * count_x + i targets
 * (first_x + i * step_x, first_y + j * step_y). Each point is TWO int8 values,
 * the snapped displacement relative to its target: `out[2k] = snapped_x -
 * target_x`, `out[2k + 1] = snapped_y - target_y`. A correct snap never
 * travels far (the vendor search radius is ~2 px), so int8 is generous; a
 * delta outside [-127, 127] on either axis is reported as
 * @ref U_SNAP_GRID_NO_DELTA in BOTH bytes rather than being clamped into a
 * wrong but plausible position.
 *
 * ## Declined
 *
 * The per-point slot returns false when the DP produced no snap — no slot, no
 * viewing distance yet, the DP not up. The first such answer stops the loop:
 * the whole grid is reported declined and every point is zero (identity), so a
 * caller that ignores the flag degrades exactly like the per-point call, which
 * hands the target back on a decline.
 *
 * @ingroup aux_util
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Upper bound on grid points per evaluation (= XR_WEAVE_SNAP_GRID_MAX_POINTS_DXR).
#define U_SNAP_GRID_MAX_POINTS (1024u * 1024u)
//! Upper bound on points along one axis (= XR_WEAVE_SNAP_GRID_MAX_AXIS_DXR).
#define U_SNAP_GRID_MAX_AXIS 1024u
//! Largest |step| and |coordinate| accepted: keeps every target in int32 range.
#define U_SNAP_GRID_MAX_COORD (1 << 28)
//! Sentinel written in both bytes of a point whose delta does not fit int8.
#define U_SNAP_GRID_NO_DELTA ((int8_t)-128)

/*!
 * A grid of targets around one drag origin. All coordinates in the same frame
 * as the per-point snap (device px; only target - origin matters).
 */
struct u_snap_grid
{
	int32_t origin_x, origin_y; //!< drag-start window top-left
	int32_t first_x, first_y;   //!< target of grid point (0, 0)
	int32_t step_x, step_y;     //!< grid pitch, >= 1
	uint32_t count_x, count_y;  //!< points per axis, 1..U_SNAP_GRID_MAX_AXIS
};

/*!
 * The per-point snap being looped: the shape of every `snap_window_rect` in
 * the runtime. Must write out_x/out_y on true; false = no snap produced.
 */
typedef bool (*u_snap_grid_point_fn)(void *userdata,
                                     int32_t origin_x,
                                     int32_t origin_y,
                                     int32_t target_x,
                                     int32_t target_y,
                                     int32_t *out_x,
                                     int32_t *out_y);

/*!
 * Is @p g a grid this runtime will evaluate? Checks the counts (each axis
 * 1..U_SNAP_GRID_MAX_AXIS, product <= U_SNAP_GRID_MAX_POINTS), the steps
 * (1..U_SNAP_GRID_MAX_COORD) and that the origin and every target stay within
 * ±U_SNAP_GRID_MAX_COORD. Everything that crosses the IPC wire goes through
 * this before anything is allocated.
 *
 * @param[out] out_reason optional; a static string naming the failed check.
 */
bool
u_snap_grid_validate(const struct u_snap_grid *g, const char **out_reason);

//! count_x * count_y. Only meaningful for a grid that validated.
static inline uint32_t
u_snap_grid_point_count(const struct u_snap_grid *g)
{
	return g->count_x * g->count_y;
}

/*!
 * Evaluate @p fn over every point of @p g (which must validate) into
 * @p out_dxdy, 2 * u_snap_grid_point_count(g) bytes, encoded as the file
 * comment describes.
 *
 * @param[out] out_declined  true when @p fn declined; @p out_dxdy is then all zero.
 * @param[out] out_calls     optional; how many times @p fn was called.
 * @param[out] out_no_delta  optional; how many points were written as NO_DELTA.
 */
void
u_snap_grid_eval(const struct u_snap_grid *g,
                 u_snap_grid_point_fn fn,
                 void *userdata,
                 int8_t *out_dxdy,
                 bool *out_declined,
                 uint32_t *out_calls,
                 uint32_t *out_no_delta);


#ifdef __cplusplus
}
#endif
