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
 *   - @ref comp_segments_owner — hysteresis for which screen's DP holds the
 *     window handle (it follows the majority segment, ADR-047 Amendment 2).
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
 * What the session's own display processor was made for (display hotplug,
 * ADR-047 Amendment 8): one monitor, by one plug-in. Recorded from the first
 * screen table a session sees (the system-default screen and the plug-in that
 * won it then).
 *
 * After a display hotplug the screen table can change under a live session:
 * the system default can move to another monitor, or the monitor the session
 * DP serves can be re-claimed by another plug-in (sim-display -> a vendor
 * whose panel just came up). The session DP is never swapped (ADR-045 D3);
 * instead it keeps weaving exactly that monitor while that plug-in still owns
 * it, and every other screen — the re-claimed one included — is woven through
 * a segment DP from its own claimant, even when it is the only segment.
 */
struct comp_segments_session_dp
{
	bool valid;
	uint64_t monitor_id;
	char plugin_id[COMP_SEGMENTS_ID_MAX];
	//! The winning claim's vendor serial then ("" = none).
	char serial[COMP_SEGMENTS_ID_MAX];
	//! The monitor's desktop rect then.
	struct comp_seg_rect desktop;
	//! The OS's handle for the output then (macOS: CGDirectDisplayID; 0 = none).
	uint64_t platform_id;
};

/*!
 * One registry screen as @ref comp_segments_session_dp_screen compares it.
 */
struct comp_segments_session_candidate
{
	uint64_t monitor_id;
	const char *plugin_id;
	const char *serial;
	struct comp_seg_rect desktop;
	uint64_t platform_id;
	//! The claim is "present, not ready" (no DP may weave it; flat 2D).
	bool not_ready;
};

//! Why the session DP does not own a screen (@ref comp_segments_session_dp_screen).
enum comp_segments_session_miss
{
	COMP_SEG_SESSION_OWNS = 0,     //!< It does.
	COMP_SEG_SESSION_NOT_RECORDED, //!< No identity recorded (not valid).
	COMP_SEG_SESSION_GONE,         //!< Its monitor is not listed (unplugged).
	COMP_SEG_SESSION_RECLAIMED,    //!< Listed, another plug-in wins it.
	COMP_SEG_SESSION_REBOUND,      //!< Listed, same plug-in, but another serial / place / OS handle.
	COMP_SEG_SESSION_NOT_READY,    //!< Listed, but its plug-in cannot drive it yet (flat 2D).
};

/*!
 * Which listed screen is the session DP's: the one with @p sd's monitor id,
 * still won by @p sd's plug-in, with the SAME vendor serial, the SAME desktop
 * rect and the SAME OS output handle as when the session DP was made for it.
 * A panel replugged unchanged is handed back to the session DP (its
 * window-bound weaver is kept); one that came back as another panel, at
 * another place or under a renumbered OS handle is not — its screen is woven
 * by a freshly created segment DP instead, the policy for a weaver whose
 * binding may be stale (ADR-047 Amendment 8).
 *
 * @param[out] out_miss  (may be NULL) why it does not own one.
 * @return the index into @p cands, or -1.
 */
int
comp_segments_session_dp_screen(const struct comp_segments_session_dp *sd,
                                const struct comp_segments_session_candidate *cands,
                                uint32_t n,
                                enum comp_segments_session_miss *out_miss);

//! A short word for @p miss ("owns", "gone", "re-claimed", "re-bound", "not ready", "not recorded").
const char *
comp_segments_session_miss_str(enum comp_segments_session_miss miss);

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
 * #2247: does the window cover screen @p screen_id — does @p t hold a
 * non-empty segment on it? A screen's lens wish follows this: a session in 3D
 * wishes 3D only on the screens its window covers, and 2D on a screen it has
 * wholly left (the screen shows the desktop there, not a woven window).
 *
 * An EMPTY table (the window on no screen at all, or no table yet) carries no
 * coverage information and answers true for every screen, so the wish is the
 * session's own — exactly what shipped before the rule. A window wholly on one
 * screen covers that screen alone.
 */
bool
comp_segments_screen_covered(const struct comp_segment_table *t, uint64_t screen_id);

/*!
 * #2247: the 2D/3D wish to send screen @p screen_id's DP — the session's
 * @p session_3d, but 2D on a screen the window does not cover
 * (@ref comp_segments_screen_covered).
 */
static inline bool
comp_segments_lens_wish(const struct comp_segment_table *t, uint64_t screen_id, bool session_3d)
{
	return session_3d && comp_segments_screen_covered(t, screen_id);
}

/*!
 * #2247: the dead band on a screen's lens wish. A lens switch is a physical
 * toggle, so a window dragged across a seam must not flap it: a screen counts
 * as uncovered (its wish goes 2D) only once the window has left it
 * continuously for @ref hold_ns — the window-handle hand-off's dwell,
 * @ref COMP_SEGMENTS_DEFAULT_HANDOFF_HOLD_NS (coverage is binary, so the
 * hand-off's majority margin does not apply) — and counts as covered again
 * the moment the window returns (a pending 2D is cancelled). Indexed like the
 * manager's screen list. Pure state: the caller owns the clock and the DPs.
 */
struct comp_segments_cover_dwell
{
	uint64_t hold_ns;
	bool pending[COMP_SEGMENTS_MAX_SCREENS];      //!< uncovered, dwell running
	uint64_t since_ns[COMP_SEGMENTS_MAX_SCREENS]; //!< when the dwell started
	bool uncovered[COMP_SEGMENTS_MAX_SCREENS];    //!< dwell elapsed: the wish is 2D
};

//! Every screen covered; @p hold_ns 0 = @ref COMP_SEGMENTS_DEFAULT_HANDOFF_HOLD_NS.
void
comp_segments_cover_dwell_init(struct comp_segments_cover_dwell *d, uint64_t hold_ns);

/*!
 * Advance one metric update with table @p t at @p now_ns, for @p screen_count
 * screens with ids @p screens[i].id (coverage from
 * @ref comp_segments_screen_covered). Returns a bit per screen index whose
 * @ref comp_segments_cover_dwell_covered answer changed on this update.
 */
uint32_t
comp_segments_cover_dwell_update(struct comp_segments_cover_dwell *d,
                                 const struct comp_segment_table *t,
                                 const struct comp_segments_screen *screens,
                                 uint32_t screen_count,
                                 uint64_t now_ns);

//! Screen @p i's lens counts as covered (true until its dwell has elapsed).
static inline bool
comp_segments_cover_dwell_covered(const struct comp_segments_cover_dwell *d, uint32_t i)
{
	return d == NULL || i >= COMP_SEGMENTS_MAX_SCREENS || !d->uncovered[i];
}

/*!
 * #2218 (#4): the part of a window-spanning zone mask that belongs to one
 * segment, in MASK pixels. A zones / Local2D frame's mask covers the whole
 * client window (@p win_w × @p win_h window px) at its own resolution
 * (@p mask_w × @p mask_h); the vendor's local-zone verdict is per PANEL (any
 * 3D pixel on that screen ⟹ that screen's lens goes 3D), so each segment DP
 * must see only the mask pixels that land on ITS screen. Scales
 * @p segment_window (window px, @ref comp_segment::window_rect) into mask
 * space, clamped to the mask and never empty when the segment is not.
 *
 * @return false when any dimension is zero (nothing to publish).
 */
bool
comp_segments_mask_crop_rect(const struct comp_seg_rect *segment_window,
                             uint32_t win_w,
                             uint32_t win_h,
                             uint32_t mask_w,
                             uint32_t mask_h,
                             struct comp_seg_rect *out_mask_rect);

/*!
 * #2218: would @p window_desktop take the split path at all — is it anything
 * other than wholly on the primary screen? Pure: computes a scratch table and
 * asks @ref comp_segments_table_is_split; no lifecycle, no DPs. The
 * compositors ask this BEFORE deciding zero-copy, so a stereo frame that
 * qualifies for zero-copy is still composed (and woven per screen) when its
 * window is not on the primary screen; before, zero-copy skipped segmentation
 * and the primary screen's DP wove the window wherever it was.
 */
bool
comp_segments_window_is_split(const struct comp_seg_rect *window_desktop,
                              const struct comp_seg_rect *canvas,
                              const struct comp_segments_screen *screens,
                              uint32_t screen_count);

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
 * A segment's rect in VIEW-TILE pixels: the atlas holds the canvas at view
 * resolution, so a segment (window px) maps into every tile at this rect.
 * Edges are rounded independently, so two segments sharing a seam share the
 * tile column too: no gap, no overlap. The ONE mapping both the per-segment
 * crop (M2) and the per-segment view routing (M3) use, so a segment's views
 * are always placed exactly where its crop reads them.
 *
 * Only the part of @p seg inside @p canvas is mapped, on every edge (#1575).
 * A canvas that is itself a cut of the window (an atlas clipped to the panel)
 * can start a pixel or two inside a segment the live window has moved since;
 * that segment still gets the tile rect of the part it shares with the canvas.
 *
 * @param seg     The segment, window px.
 * @param canvas  The canvas the atlas holds, window px.
 * @param tile_w/tile_h  One view tile, px.
 * @param[out] out The rect inside a tile.
 * @return false when no part of the segment is inside the canvas, or that
 *         part is thinner than a tile pixel.
 */
bool
comp_segments_tile_rect(const struct comp_seg_rect *seg,
                        const struct comp_seg_rect *canvas,
                        uint32_t tile_w,
                        uint32_t tile_h,
                        struct comp_seg_rect *out);

/*!
 * Intersect two rects (half-open). @return false (and @p out zeroed) when
 * they share no pixel.
 */
bool
comp_segments_rect_intersect(const struct comp_seg_rect *a, const struct comp_seg_rect *b, struct comp_seg_rect *out);

/*!
 * The inverse of @ref comp_segments_tile_rect: a rect in VIEW-TILE pixels
 * mapped back onto the canvas the atlas holds, in window px. Each edge is
 * rounded on its own, so the result is exactly where those tile pixels land
 * when the canvas is woven whole: a DP handed this rect as its canvas and the
 * tile rect as its tile puts every texel where it always was, at the same
 * scale (#1575).
 *
 * @return false when @p tile is empty or the inputs are degenerate.
 */
bool
comp_segments_canvas_from_tile(const struct comp_seg_rect *tile,
                               const struct comp_seg_rect *canvas,
                               uint32_t tile_w,
                               uint32_t tile_h,
                               struct comp_seg_rect *out);

/*!
 * #1575: what one frame of a window that overhangs the panel sends across the
 * output-device split's bridge.
 *
 * The bridge's rings are sized once, for the worst-case system atlas (the
 * panel's), but the renderer sizes its view tiles from the WINDOW. A window
 * larger than the panel therefore paints a content box the bridge cannot
 * carry, and every such frame was refused: a black panel, and before #2408 a
 * DEGRADED latch for the rest of the session.
 *
 * The pixels past the panel never reach it, so they need not be carried.
 * Every view tile is cut at its real offset, and the DP weaves that cut over
 * the same window pixels it always would. Nothing is scaled, so the weave
 * phase and the window-relative Kooima (both computed from the window) do not
 * change. This is the crop-before-the-DP rule the per-screen segments follow
 * (ADR-030).
 *
 * The cut is a FIXED size for a given tile size: the bridge's per-tile share,
 * `min(max_w / cols, view_w)` by `min(max_h / rows, view_h)`, placed over the
 * part of the canvas on the panel. A drag across the panel's edge moves the
 * cut and never resizes it, so the bridge box, the egress ring and the DP
 * input keep one size for the whole episode. The DP canvas is the cut mapped
 * back by @ref comp_segments_canvas_from_tile, so tile and canvas keep one
 * scale; its edges lie on the window's own edges or past the panel's, where
 * nothing reaches the panel.
 */
struct comp_segments_overhang_in
{
	//! The window's client area, desktop px.
	struct comp_seg_rect window;
	//! The part of the window the atlas maps onto, window px (usually 0,0 and the client size).
	struct comp_seg_rect canvas;
	//! The panel, desktop px.
	struct comp_seg_rect panel;
	//! The atlas's tile grid.
	uint32_t cols, rows;
	//! One tile as painted (its stride in the atlas).
	uint32_t view_w, view_h;
	//! The largest box the bridge carries.
	uint32_t max_w, max_h;
};

//! What @ref comp_segments_overhang_clip decided.
enum comp_segments_overhang_verdict
{
	//! A degenerate input. The caller keeps its own path.
	COMP_SEG_OVERHANG_NO_INPUT = 0,
	//! The whole content box fits the bridge: carried whole, no clip.
	COMP_SEG_OVERHANG_FITS,
	//! Too big, but the canvas lies wholly on the panel: the tiles are larger
	//! than the panel's own, which no clip to the panel can cure (not #1575).
	COMP_SEG_OVERHANG_ON_PANEL,
	//! Too big, and no part of the canvas is on the panel: nothing to carry.
	COMP_SEG_OVERHANG_OFF_PANEL,
	//! Carry @ref comp_segments_overhang::tile of every view and weave it
	//! over @ref comp_segments_overhang::canvas.
	COMP_SEG_OVERHANG_CLIPPED,
};

//! The clip @ref comp_segments_overhang_clip computed.
struct comp_segments_overhang
{
	//! How far the canvas reaches past each edge of the panel, window px.
	uint32_t over_left, over_right, over_top, over_bottom;
	//! The part of the canvas on the panel, window px: what must reach it.
	struct comp_seg_rect keep;
	//! @ref keep in tile px, rounded outward: what every tile must carry.
	struct comp_seg_rect need;
	//! What every tile carries, tile px: the bridge's per-tile share at its
	//! real offset, covering @ref need (a drag moves it, never resizes it).
	struct comp_seg_rect tile;
	//! Where the DP weaves @ref tile, window px (@ref comp_segments_canvas_from_tile).
	struct comp_seg_rect canvas;
	//! What crosses the bridge: cols x tile.w by rows x tile.h.
	uint32_t box_w, box_h;
};

/*!
 * Decide what one frame of @p in sends across the bridge (#1575).
 *
 * Only a content box larger than the bridge (@ref comp_segments_overhang_in::max_w
 * by max_h) is clipped; a box that fits is carried whole, exactly as before.
 * @ref comp_segments_overhang::canvas covers @ref comp_segments_overhang::keep
 * whenever the share is wide enough for it, which it is for tiles sized at the
 * panel's own scale up to one tile pixel of rounding on a window that covers
 * the whole panel. Tiles not at the panel's scale lose their far part.
 *
 * @param[out] out Filled for COMP_SEG_OVERHANG_CLIPPED, and its over_* fields
 *             for ON_PANEL and OFF_PANEL; zeroed otherwise.
 */
enum comp_segments_overhang_verdict
comp_segments_overhang_clip(const struct comp_segments_overhang_in *in, struct comp_segments_overhang *out);

/*!
 * #1575 — the proof an overhang episode's end line carries: that a DRAG
 * reallocated nothing.
 *
 * A drag keeps the window's size, so the cut keeps its size, and after its
 * first frame nothing may be rebuilt. A RESIZE changes the tiles, so the box,
 * and may rebuild the egress ring and the DP input. Both can happen in one
 * episode: on the G16 on 2026-10-28 Windows rescaled a window whose majority
 * crossed to the other display (5220x863 -> 2605x427 -> 5220x865 -> 5220x863),
 * and the person kept dragging it. One count could not tell the two apart, so
 * the drag read as a box change. Hence:
 *
 * - A change of the window's size or of its tile grid is a resize. It is
 *   counted, and the proof restarts there: the end line's counts cover the
 *   frames at the window's LAST size. The episode itself goes on (one WARN when
 *   it starts, one line when it ends): an interactive resize changes the size
 *   on nearly every frame.
 * - A box change at one window size and one tile grid is the drag bug (a cut
 *   that follows the window's position). It is still counted.
 * - The egress ring is sized by the frame's own submit, after its step, so its
 *   baseline is taken on the next frame (the first frame may size it once).
 * - The DP input is (re)created by the WEAVE, which takes a slot a frame or
 *   more after it was submitted. Its baseline waits until the DP input holds
 *   the box, or no output-device crop is needed (the egress ring is the box's
 *   own size), for at most @ref COMP_SEG_OVERHANG_PROOF_DP_WAIT frames. A
 *   baseline taken on the second submit counted the episode's own first box
 *   reaching the weave as a re-create whenever the ring was worst-case.
 */
#define COMP_SEG_OVERHANG_PROOF_DP_WAIT 8u

//! One clipped frame, as @ref comp_segments_overhang_proof_step reads it.
struct comp_segments_overhang_proof_in
{
	//! The client size the frame was cut for.
	uint32_t window_w, window_h;
	//! Its tile grid, as painted.
	uint32_t cols, rows, view_w, view_h;
	//! The box it carries across the bridge.
	uint32_t box_w, box_h;
	//! The bridge's egress rebuild count, read before this frame's submit.
	uint64_t egress_rebuilds;
	//! The DP input's create count, read together with @ref dp_settled.
	uint64_t dp_creates;
	//! The DP input already holds @ref box_w x @ref box_h, or the egress ring
	//! is that size (no output-device crop, so no DP input is made).
	bool dp_settled;
};

//! An episode's bookkeeping. Zero-init = no episode.
struct comp_segments_overhang_proof
{
	bool open;
	//! Frames in the episode, and in its stretch at the window's last size.
	uint64_t frames, frames_at_size;
	//! Resizes in the episode.
	uint64_t resizes;
	//! The size the current stretch is cut at, and the box it carries.
	uint32_t window_w, window_h, cols, rows, view_w, view_h;
	uint32_t box_w, box_h;
	//! Box changes at that size: a drag must make none.
	uint64_t box_changes;
	bool rebuild_base_set, dp_base_set;
	uint64_t rebuild_base, dp_base;
	//! Frames the DP baseline has waited for the DP input to hold the box.
	uint32_t dp_wait;
};

//! What @ref comp_segments_overhang_proof_step saw.
enum comp_segments_overhang_proof_event
{
	//! One more frame of the episode at its size.
	COMP_SEG_PROOF_FRAME = 0,
	//! The episode's first frame (the caller logs its WARN).
	COMP_SEG_PROOF_STARTED,
	//! The window or its tiles changed size: the proof restarts at this frame.
	COMP_SEG_PROOF_RESIZED,
};

/*!
 * Feed one clipped frame to the episode @p p, opening it if needed. Call it
 * before the frame's submit (the egress count in @p in must not include that
 * submit's rebuild yet).
 */
enum comp_segments_overhang_proof_event
comp_segments_overhang_proof_step(struct comp_segments_overhang_proof *p,
                                  const struct comp_segments_overhang_proof_in *in);

//! What an episode proved, for its end line.
struct comp_segments_overhang_proof_result
{
	uint64_t frames, frames_at_size, resizes;
	//! The box at the window's last size.
	uint32_t box_w, box_h;
	//! False when the last size held too few frames for a baseline; the counts are then 0.
	bool have_counts;
	uint64_t box_changes, egress_rebuilds, dp_creates;
};

/*!
 * Close the episode @p p (if one is open) against the counts now, and reset
 * it. @return false when no episode was open (@p out zeroed).
 */
bool
comp_segments_overhang_proof_end(struct comp_segments_overhang_proof *p,
                                 uint64_t egress_rebuilds,
                                 uint64_t dp_creates,
                                 struct comp_segments_overhang_proof_result *out);

/*
 * The partition the atlas content was painted with (#1883).
 */

//! Max segments a @ref comp_segments_content describes.
#define COMP_SEGMENTS_CONTENT_MAX 4

/*!
 * Where each segment's own views sit in an atlas, as the frame that PAINTED it
 * laid them out (multi-screen M3: a routed frame is a mosaic, segment k's
 * views at segment k's rect inside every tile).
 *
 * The routing comes from the table the frame was LOCATED against, which
 * during a drag is older than the table the weave cuts from the live window:
 * the seam column has moved since. Cropping the mosaic at the live seam hands
 * each DP a sliver of its neighbour's views (#1883). The content partition is
 * carried with the pixels instead (the renderer atlas, the #918 egress slot,
 * the service client's atlas) and the crop reads it.
 *
 * @ref count 0 = one view set fills every tile (an unrouted frame): the atlas
 * has no partition of its own and the live one is exact.
 */
struct comp_segments_content
{
	uint32_t count;
	//! The screen whose views sit at @ref rect.
	uint64_t screen_id[COMP_SEGMENTS_CONTENT_MAX];
	//! Each segment as the content was painted for it, window px.
	struct comp_seg_rect rect[COMP_SEGMENTS_CONTENT_MAX];
	//! The canvas @ref rect is relative to, window px (the painted window's).
	struct comp_seg_rect canvas;
};

/*!
 * The rect inside a view tile that holds segment @p seg's views — what the
 * per-segment crop (and the flat-2D fill) reads.
 *
 * With @p content routed (count > 0) and listing @p seg's screen, this is that
 * screen's rect as the content was PAINTED (@ref comp_segments_tile_rect of the
 * content rect against the content canvas), never the live one: each DP weaves
 * exactly its own views, and the DP stretches them over its live segment (its
 * canvas), which during a drag differs by the window's motion since the locate
 * — a sub-percent scale for a frame, where cropping at the live seam showed a
 * sliver of the neighbour's views. Unrouted content (NULL / count 0), or a
 * screen the content does not list (the frame a segment appears), maps the
 * live segment against the live canvas, as before.
 *
 * @param seg      The live segment (window px; only `screen_id` and `window_rect` are read).
 * @param canvas   The live canvas the atlas is mapped against when unrouted, window px.
 * @param tile_w/tile_h  One view tile of the atlas being woven, px.
 * @param content  The partition the atlas was painted with, or NULL.
 * @param[out] out The rect inside a tile.
 * @return false when the rect is outside the canvas or thinner than a tile pixel.
 */
bool
comp_segments_source_rect(const struct comp_segment *seg,
                          const struct comp_seg_rect *canvas,
                          uint32_t tile_w,
                          uint32_t tile_h,
                          const struct comp_segments_content *content,
                          struct comp_seg_rect *out);

//! Depth of a @ref comp_segments_content_ring (> any egress ring).
#define COMP_SEGMENTS_CONTENT_RING 8

/*!
 * The content partition of the last few submitted frames, keyed by the frame
 * sequence a transport stamps on its slots (#918: the weave consumes an egress
 * slot a frame or two behind the frame being committed, so the partition has
 * to travel with that slot, not with the current frame).
 */
struct comp_segments_content_ring
{
	uint64_t seq[COMP_SEGMENTS_CONTENT_RING]; //!< 0 = empty
	struct comp_segments_content content[COMP_SEGMENTS_CONTENT_RING];
};

/*!
 * Record frame @p seq's partition (@p content NULL = unrouted). @p seq 0 is
 * ignored.
 */
void
comp_segments_content_ring_put(struct comp_segments_content_ring *r,
                               uint64_t seq,
                               const struct comp_segments_content *content);

/*!
 * The partition frame @p seq was painted with. False (and @p out zeroed =
 * unrouted) when it is no longer, or never was, recorded.
 */
bool
comp_segments_content_ring_get(const struct comp_segments_content_ring *r,
                               uint64_t seq,
                               struct comp_segments_content *out);

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
 * Carry a live DP over into a fresh lifecycle (display hotplug, ADR-047
 * Amendment 8): a new screen table that still lists @p screen_id, bound the
 * same way, keeps that screen's DP instead of rebuilding it. The slot is live
 * with a full present streak, so no CREATE is issued for it; it is destroyed by
 * the usual absent-streak rule once its segment is gone.
 *
 * @return false when out of slots (the caller then releases the DP).
 */
bool
comp_segments_lifecycle_adopt(struct comp_segments_lifecycle *lc, uint64_t screen_id);

struct xrt_screen_binding;

/*!
 * What a live segment DP was made for, as far as "may a new screen table keep
 * it?" is concerned (display hotplug, ADR-047 Amendment 8).
 */
struct comp_segments_dp_key
{
	uint64_t screen_id;
	const void *iface;      //!< The owning plug-in iface (`xrt_plugin_iface *`).
	const void *inst;       //!< The owning plug-in instance.
	uint32_t confidence;    //!< The screen's winning claim level (`xrt_screen::confidence`).
	const struct xrt_screen_binding *binding; //!< The binding the DP was created with.
};

/*!
 * May a segment DP made for @p was be kept for @p now? Yes when it is the same
 * screen, claimed by the same plug-in instance at the same claim level, bound
 * at the same desktop rect / mode / scale / serial / output name. A different
 * claim level (PRESENT_NOT_READY -> claimed: the backend became able to drive
 * the panel) or different bounds (the same panel re-bound at a new place) is a
 * real change: the DP is rebuilt.
 *
 * The binding's EDID physical size is deliberately NOT part of the key. On
 * macOS it is refined ~0.5-1 s after a plug-in (CoreGraphics' 598x341 mm
 * until IOKit publishes the EDID blob, then the EDID's own 600x340 mm) for the
 * same panel; rebuilding a vendor weaver (and re-acquiring its eye tracker)
 * for that is wrong.
 */
bool
comp_segments_dp_key_keeps(const struct comp_segments_dp_key *was, const struct comp_segments_dp_key *now);

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
 * Window-handle ownership: the HWND follows the majority segment.
 *
 */

//! Default: how long a new majority must hold before the window handle moves (ns).
#define COMP_SEGMENTS_DEFAULT_HANDOFF_HOLD_NS (500ull * 1000ull * 1000ull)

//! Default: the margin a challenger's area must beat the owner's by, in percent
//! of the window's on-screen area (20 = a 60/40 split between two screens).
#define COMP_SEGMENTS_DEFAULT_HANDOFF_MARGIN_PCT 20u

/*!
 * The segment holding most of the window: the largest window-px area; an exact
 * tie goes to the primary screen, else to the earlier (leftmost) segment. The
 * same rule as `oxr_segment_views_majority`, so the screen a single view set
 * is framed from and the screen that owns the window handle agree.
 *
 * @return the index into @p t, or UINT32_MAX when @p t is empty.
 */
uint32_t
comp_segments_majority(const struct comp_segment_table *t);

/*!
 * #2245 (ADR-047 Amendment 9): the refresh rate a window is paced at — that of
 * the screen holding most of it (@ref comp_segments_majority over
 * @ref comp_segments_compute, so the same screen a single view set is framed
 * from and the same tie rule).
 *
 * @p primary_hz is returned whenever the answer would be the primary screen's,
 * or is unknown: the window is on no screen, its majority screen is the
 * primary, or that screen's rate is not known (<= 0). The caller passes the
 * period it uses today, so a window on the primary screen keeps exactly that
 * value rather than a re-read of the same monitor.
 *
 * @param window_desktop  The window's client area in desktop coordinates.
 * @param screens         The screens (desktop rect + primary flag are read).
 * @param screen_hz       Parallel to @p screens: each one's current refresh rate, 0 = unknown.
 * @param screen_count    Number of screens.
 * @param primary_hz      The primary screen's period-defining rate (today's value).
 * @param[out] out_screen_index  Optional: the majority screen's index into
 *                        @p screens, UINT32_MAX when the window is on none.
 */
float
comp_segments_window_refresh_hz(const struct comp_seg_rect *window_desktop,
                                const struct comp_segments_screen *screens,
                                const float *screen_hz,
                                uint32_t screen_count,
                                float primary_hz,
                                uint32_t *out_screen_index);

/*!
 * #2245 (ADR-047 Amendment 9, the phase-A adapter gate): may a window or a
 * segment be paced at its screen's own rate? Only when that screen is scanned
 * out by the SAME adapter the presenting chain (or the segment DP's device)
 * lives on. Until each screen has its own output adapter (phase B), a screen
 * scanned out by another adapter is reached through a cross-adapter copy and
 * a DWM composition, so pacing it faster than the primary is wasted work.
 *
 * @param screen_hz            The screen's current rate (0 = unknown).
 * @param screen_adapter_luid  The adapter scanning that screen out (packed LUID, 0 = unknown).
 * @param chain_adapter_luid   The adapter the chain / DP device lives on (packed LUID, 0 = unknown).
 * @param primary_hz           Today's (the primary's) rate.
 * @param[out] out_other_adapter  Optional: true when both adapters are known
 *                             and differ — the case the caller logs.
 * @return @p screen_hz when the adapters are known and equal and the rate is
 *         known; @p primary_hz otherwise (an unknown adapter is not proof of
 *         a local present).
 */
float
comp_segments_gated_refresh_hz(float screen_hz,
                               uint64_t screen_adapter_luid,
                               uint64_t chain_adapter_luid,
                               float primary_hz,
                               bool *out_other_adapter);

/*!
 * #2245: the refresh rate of the screen in @p screens whose desktop rect holds
 * the centre of @p screen_desktop — how a registry screen (a segment's) finds
 * its OS monitor's rate. 0 when none holds it or its rate is unknown.
 */
float
comp_segments_screen_refresh_hz(const struct comp_seg_rect *screen_desktop,
                                const struct comp_segments_screen *screens,
                                const float *screen_hz,
                                uint32_t screen_count);

/*!
 * Which screen's display processor holds the window's real handle (the
 * vendor's drag phase-snap and window-scoped behaviour follow it). Every other
 * segment's DP is windowless and is phased by `set_present_origin`.
 *
 * Starts on the primary screen. A hand-off to another screen is due only when
 *   - that screen holds the majority (@ref comp_segments_majority),
 *   - its area beats the current owner's by at least `margin_pct` percent of
 *     the window's on-screen area (an absent owner counts as 0), and
 *   - both have held continuously for `hold_ns`.
 * The margin is a dead band around the seam and the hold rides out a drag, so
 * a window dragged back and forth across a seam never flaps the handle (a
 * hand-off rebuilds two DPs). A target that could not take the handle is not
 * retried until the majority leaves it. A screen with no DP factory is never a
 * target (its segment is flat 2D).
 */
struct comp_segments_owner
{
	uint64_t hold_ns;
	uint32_t margin_pct;
	//! The screen whose DP holds the handle; 0 = none (a failed hand-off).
	uint64_t owner_id;
	//! The challenger being timed, 0 = none.
	uint64_t candidate_id;
	uint64_t candidate_since_ns;
	//! A target whose hand-off failed; skipped until it stops being the challenger.
	uint64_t failed_id;
};

/*!
 * Initialise with the primary screen as the owner. 0 for either threshold
 * selects the default.
 */
void
comp_segments_owner_init(struct comp_segments_owner *o, uint64_t primary_id, uint64_t hold_ns, uint32_t margin_pct);

/*!
 * Advance one metric update with the current table at @p now_ns.
 *
 * @return the screen id the handle should move to now, or 0 for no change. The
 *         caller performs the hand-off and reports it with
 *         @ref comp_segments_owner_set_result.
 */
uint64_t
comp_segments_owner_update(struct comp_segments_owner *o, const struct comp_segment_table *t, uint64_t now_ns);

/*!
 * Report a hand-off's outcome: @p owner_id is the screen whose DP holds the
 * handle afterwards (the target on success; the old owner after a rollback;
 * 0 when no DP holds it). @p ok false latches @p target_id as failed.
 */
void
comp_segments_owner_set_result(struct comp_segments_owner *o, uint64_t target_id, bool ok, uint64_t owner_id);

/*!
 * During a window-handle hand-off, does the OLD owner need a windowless
 * replacement DP before its DP (which holds the window) is destroyed? A weaver
 * is one-per-window and a live DP's window cannot be re-pointed, so the old
 * owner's DP must go; this says whether something must take its place:
 *   - the primary screen always keeps a DP (it is the session's own DP), and
 *   - another screen keeps one only while the window still covers it and its
 *     plug-in can make a per-screen DP (else its segment is flat 2D anyway).
 *
 * The rule the D3D11 manager's hwnd_handoff applies inline; the Vulkan manager
 * (comp_vk_native_segments) shares it from here.
 *
 * @param t                 The current segment table.
 * @param owner_id          The screen whose DP holds the window now (0 = none).
 * @param owner_is_primary  That screen is the primary (system-default) one.
 * @param owner_has_factory That screen's plug-in has the per-screen factory.
 */
bool
comp_segments_owner_needs_windowless_replacement(const struct comp_segment_table *t,
                                                 uint64_t owner_id,
                                                 bool owner_is_primary,
                                                 bool owner_has_factory);


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
