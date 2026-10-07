// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Window segments: one window canvas split across the screens it
 *         covers, each piece woven by the display processor of its own screen
 *         (multi-screen M2, ADR-047 D2).
 * @ingroup comp_util
 *
 * A **segment** is `canvas ∩ screen`: the part of a window's canvas that lands
 * on one screen. Each segment gets its own display processor (created from
 * that screen's registry factory), its own present origin (the window's origin
 * relative to THAT screen) and its own canvas rect (the segment, in window
 * pixels). The compositor hands each DP its segment and composites the results
 * into the one surface it presents. Design note: `docs/architecture/comp-segments.md`.
 *
 * This file is the backend-agnostic half — pure integer geometry and policy,
 * no graphics-API types, so it is unit-tested headless
 * (`tests/tests_comp_segments.cpp`):
 *
 *   - @ref comp_segments_compute — the segment table for one window placement.
 *   - @ref comp_segments_screen_1to1 / @ref comp_segments_decide — the per-
 *     segment refuse-rather-than-resample policy (weave, or a flat 2D blit).
 *   - @ref comp_segments_lifecycle — hysteresis for creating and destroying a
 *     segment's DP, so a window dragged across a seam never thrashes DPs.
 *
 * The rect math is @ref u_multi_display_compute_slices (half-open, so a seam
 * belongs to exactly one screen).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Max screens the segment math considers (= XRT_SCREEN_LIST_MAX).
#define COMP_SEGMENTS_MAX_SCREENS 16

//! Max segments one window is split into (= U_MD_MAX_SLICES).
#define COMP_SEGMENTS_MAX 8

//! Size of @ref comp_segments_screen::plugin_id, incl. the NUL.
#define COMP_SEGMENTS_ID_MAX 64

//! Default hysteresis: metric updates a segment must exist before its DP is created.
#define COMP_SEGMENTS_DEFAULT_CREATE_AFTER 2

//! Default hysteresis: metric updates a segment must be gone before its DP is destroyed
//! (~0.5 s at 60 Hz).
#define COMP_SEGMENTS_DEFAULT_DESTROY_AFTER 30

/*!
 * A rectangle: origin + size. Signed origin (screens sit at negative desktop
 * coordinates; a window can hang off the left of a screen).
 */
struct comp_seg_rect
{
	int32_t x;
	int32_t y;
	uint32_t w;
	uint32_t h;
};

/*!
 * One screen as the segment math needs it — a projection of `xrt_screen`.
 */
struct comp_segments_screen
{
	//! Screen (registry monitor) id; never 0.
	uint64_t id;

	//! Desktop rect, in the space the window position is read in (X11: the root).
	struct comp_seg_rect desktop;

	//! The connector's device mode; 0 = unknown.
	uint32_t native_w;
	uint32_t native_h;

	//! The screen the session's primary display processor weaves (the system default).
	bool is_primary;

	//! The screen's winning plug-in can make a VK display processor for it.
	bool has_dp_factory;

	//! Discovery id of the screen's plug-in ("" = none).
	char plugin_id[COMP_SEGMENTS_ID_MAX];
};

/*!
 * Whether pixels a window draws on a screen reach it unresampled.
 */
enum comp_seg_1to1
{
	//! Nothing to compare (device mode or desktop size unknown). Never degrades.
	COMP_SEG_1TO1_UNKNOWN = 0,
	//! The screen's desktop rect IS its device mode: window px == device px.
	COMP_SEG_1TO1_YES = 1,
	//! The display server resamples this screen's window pixels.
	COMP_SEG_1TO1_NO = 2,
};

/*!
 * What a segment is rendered with this frame.
 */
enum comp_seg_render
{
	//! Its screen's display processor weaves it.
	COMP_SEG_RENDER_WEAVE = 0,
	//! A flat 2D blit of one view (no DP, or a DP that needs 1:1 on a resampled screen).
	COMP_SEG_RENDER_FLAT_2D = 1,
};

/*!
 * One segment.
 */
struct comp_segment
{
	//! Index into the screen array handed to @ref comp_segments_compute.
	uint32_t screen_index;
	//! That screen's id.
	uint64_t screen_id;
	//! The segment is on the primary screen (woven by the session's primary DP).
	bool is_primary;
	//! The screen's plug-in can make a DP for it.
	bool has_dp_factory;

	//! The segment in WINDOW pixels — the canvas handed to process_atlas.
	struct comp_seg_rect window_rect;

	//! The segment in its SCREEN's pixels — where it lands on that screen.
	struct comp_seg_rect screen_rect;

	//! The window's origin relative to the screen (`set_present_origin`; may be
	//! negative). The DP adds the canvas offset: phase = present origin +
	//! canvas offset = @ref screen_rect origin.
	int32_t present_origin_x;
	int32_t present_origin_y;

	//! @ref comp_seg_1to1 of the screen.
	enum comp_seg_1to1 screen_1to1;
};

/*!
 * The segment table for one window placement. Ordered left to right by desktop
 * position (then top to bottom), which is the order M3's per-segment views use.
 */
struct comp_segment_table
{
	uint32_t count;
	struct comp_segment seg[COMP_SEGMENTS_MAX];
};

/*!
 * Is @p s 1:1 — its desktop rect equal to its device mode?
 */
enum comp_seg_1to1
comp_segments_screen_1to1(const struct comp_segments_screen *s);

/*!
 * Compute the segment table.
 *
 * @param window_desktop  The window's client area in desktop coordinates.
 * @param canvas          The canvas inside the window, window pixels; NULL or a
 *                        zero size = the whole window.
 * @param screens         The screens.
 * @param screen_count    Number of screens.
 * @param[out] out        The table (always written; count 0 when the window is
 *                        on no screen).
 * @return out->count.
 */
uint32_t
comp_segments_compute(const struct comp_seg_rect *window_desktop,
                      const struct comp_seg_rect *canvas,
                      const struct comp_segments_screen *screens,
                      uint32_t screen_count,
                      struct comp_segment_table *out);

/*!
 * True when @p t is anything other than "one segment, on the primary screen,
 * covering the whole canvas" — i.e. when the compositor must leave its
 * single-DP path. A window entirely on the primary screen (the common case)
 * returns false, which is what keeps that case byte-for-byte unchanged.
 *
 * @param canvas_w/h  The canvas size the table was computed for.
 */
bool
comp_segments_table_is_split(const struct comp_segment_table *t, uint32_t canvas_w, uint32_t canvas_h);

/*!
 * The refuse-rather-than-resample policy for one segment.
 *
 * - No DP for the segment → flat 2D.
 * - The screen is known NOT 1:1 and the DP does not tolerate a resample (a
 *   lenticular weave, sim_display INTERLACED) → flat 2D.
 * - Otherwise (1:1, unknown, or a DP whose output survives a resample such as
 *   anaglyph) → weave. Unknown never degrades.
 */
enum comp_seg_render
comp_segments_decide(bool have_dp, bool dp_tolerates_resample, enum comp_seg_1to1 screen_1to1);

//! Max rectangles @ref comp_segments_uncovered emits.
#define COMP_SEGMENTS_MAX_UNCOVERED 32

/*!
 * The parts of @p canvas (window px) that no segment covers — canvas on a
 * monitor no plug-in claimed, or off every screen. The compositor paints them
 * flat 2D, the same as a segment it cannot weave (#1654's off-panel rule).
 * Disjoint rectangles, row bands top to bottom, left to right within a band.
 *
 * @return the number written (excess dropped at @p cap).
 */
uint32_t
comp_segments_uncovered(const struct comp_segment_table *t,
                        const struct comp_seg_rect *canvas,
                        struct comp_seg_rect *out,
                        uint32_t cap);

/*!
 * Equal geometry and screens (used to log a table change once).
 */
bool
comp_segments_table_equal(const struct comp_segment_table *a, const struct comp_segment_table *b);

/*!
 * One-line description of @p t for the change log. Always NUL-terminates.
 */
void
comp_segments_table_format(const struct comp_segment_table *t, char *buf, size_t size);


/*
 *
 * DP lifecycle with hysteresis.
 *
 */

/*!
 * What the lifecycle wants done with one screen's segment DP.
 */
enum comp_seg_action
{
	COMP_SEG_ACTION_NONE = 0,
	//! Create the DP now (the segment has existed long enough).
	COMP_SEG_ACTION_CREATE = 1,
	//! Destroy the DP now (the segment has been gone long enough).
	COMP_SEG_ACTION_DESTROY = 2,
};

//! @private Per-screen lifecycle slot.
struct comp_segments_slot
{
	uint64_t screen_id;
	uint32_t present_streak;
	uint32_t absent_streak;
	//! A DP exists for the screen.
	bool live;
	//! Creation failed while the segment existed; not retried until it is gone.
	bool failed;
	bool used;
};

/*!
 * Hysteresis for secondary segment DPs. The primary screen's DP is owned by
 * the session and is not tracked here.
 *
 * Per metric update, a screen with a non-empty non-primary segment counts up a
 * present streak and one without counts up an absent streak. The DP is
 * created once the present streak reaches `create_after` and destroyed once
 * the absent streak reaches `destroy_after` — so a window dragged back and
 * forth across a seam keeps its DPs instead of rebuilding them every frame.
 */
struct comp_segments_lifecycle
{
	uint32_t create_after;
	uint32_t destroy_after;
	struct comp_segments_slot slots[COMP_SEGMENTS_MAX_SCREENS];
};

/*!
 * Actions out of one @ref comp_segments_lifecycle_update.
 */
struct comp_segments_actions
{
	uint32_t count;
	struct
	{
		uint64_t screen_id;
		enum comp_seg_action action;
	} items[COMP_SEGMENTS_MAX_SCREENS];
};

/*!
 * Initialise. 0 for either threshold selects the default.
 */
void
comp_segments_lifecycle_init(struct comp_segments_lifecycle *lc, uint32_t create_after, uint32_t destroy_after);

/*!
 * Advance one metric update with the current table; fill @p out with the
 * creates/destroys due now. The caller performs each and reports a create's
 * outcome with @ref comp_segments_lifecycle_set_created (a destroy is assumed
 * done).
 */
void
comp_segments_lifecycle_update(struct comp_segments_lifecycle *lc,
                               const struct comp_segment_table *t,
                               struct comp_segments_actions *out);

/*!
 * Report the outcome of a CREATE action.
 */
void
comp_segments_lifecycle_set_created(struct comp_segments_lifecycle *lc, uint64_t screen_id, bool ok);

/*!
 * Does a DP exist for @p screen_id?
 */
bool
comp_segments_lifecycle_is_live(const struct comp_segments_lifecycle *lc, uint64_t screen_id);

/*!
 * Every live slot as a DESTROY action, slots cleared (teardown).
 */
void
comp_segments_lifecycle_drain(struct comp_segments_lifecycle *lc, struct comp_segments_actions *out);



/*
 *
 * Deferred release.
 *
 */

//! Capacity of a @ref comp_segments_retire list.
#define COMP_SEGMENTS_RETIRE_MAX 64

/*!
 * Objects a segment no longer needs, held until no submitted work can still
 * reference them. The in-process Vulkan compositor's repaint ("fill") parks on
 * its fence with the compositor lock RELEASED (#1264 S1), so the app thread can
 * run a metric update — and want to destroy a segment DP or an input image —
 * while that fill's command buffer is still executing on the GPU. Releases go
 * here instead and are handed back only when the caller says it is safe.
 *
 * Items are opaque 64-bit handles with a caller-defined kind.
 */
struct comp_segments_retire
{
	uint32_t count;
	uint32_t kinds[COMP_SEGMENTS_RETIRE_MAX];
	uint64_t items[COMP_SEGMENTS_RETIRE_MAX];
};

/*!
 * Queue @p item. False when the list is full (the caller must then either
 * release now, if that is safe, or keep the object).
 */
bool
comp_segments_retire_push(struct comp_segments_retire *r, uint32_t kind, uint64_t item);

/*!
 * Hand back everything queued, oldest first, and empty the list — but only when
 * @p safe; otherwise nothing moves and 0 is returned. At most @p cap items are
 * returned per call (the rest stay queued).
 */
uint32_t
comp_segments_retire_take(struct comp_segments_retire *r,
                          bool safe,
                          uint32_t *out_kinds,
                          uint64_t *out_items,
                          uint32_t cap);

#ifdef __cplusplus
}
#endif
