// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The workspace on N screens (ADR-052): which screens the workspace
 *         presents on, how a window placed in one screen's display space maps
 *         into another's, and which presenters a window overlaps.
 * @ingroup comp_util
 *
 * The workspace is ONE layout in desktop space (ADR-052 D2). A controller
 * places a window in the display space of an **anchor screen** (pose origin =
 * that screen's centre, +x right, +y up, +z toward the viewer, metres); the
 * window's **footprint** is the desktop-pixel rect that placement covers, read
 * through the anchor's own pixel pitch. Every presenter the footprint overlaps
 * draws the window, at the pose the same footprint has in ITS display space
 * (its own pixel pitch). Depth (z) and orientation carry over unchanged. The
 * mapping goes through desktop pixels, so it composes exactly: anchor -> A ->
 * B equals anchor -> B, and a window straddling a seam is pixel-continuous
 * across it, the way the OS desktop is.
 *
 * Backend-agnostic, pure geometry, no graphics-API types — unit-tested headless
 * (`tests/tests_comp_workspace_screens.cpp`). The D3D11 service compose path
 * is the consumer.
 */

#pragma once

#include "xrt/xrt_defines.h"
#include "xrt/xrt_display_metrics.h"
#include "xrt/xrt_screen.h"

#include "util/comp_segments.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Max screens the workspace considers (= COMP_SEGMENTS_MAX: a window splits into at most that many parts).
#define COMP_WS_MAX_SCREENS 8

/*!
 * Bits for @ref comp_ws_screen::flags. Values match `XrWorkspaceScreenFlagsDXR`.
 * @{
 */
//! The workspace's primary screen: the system-default screen, presented by the service window.
#define COMP_WS_SCREEN_PRIMARY (1u << 0)
//! Claimed by a 3D plug-in that can make a display processor for it.
#define COMP_WS_SCREEN_3D (1u << 1)
//! The workspace presents on it (one presenter per such screen, ADR-052 D1).
#define COMP_WS_SCREEN_PRESENTER (1u << 2)
//! Its plug-in reports live eye tracking.
#define COMP_WS_SCREEN_TRACKED (1u << 3)
/*! @} */

/*!
 * Which screens become presenters (ADR-052 D1/D4).
 */
enum comp_ws_policy
{
	//! Only the primary presents (`DXR_SEGMENTS=0`, or the multi-screen workspace is off).
	COMP_WS_POLICY_SINGLE = 0,
	//! The primary plus every other screen a 3D plug-in claims above the fallback confidence.
	COMP_WS_POLICY_CLAIMED = 1,
	//! As CLAIMED, but a fallback claim counts too (development: two monitors, no 3D hardware).
	COMP_WS_POLICY_CLAIMED_INCL_FALLBACK = 2,
};

/*!
 * One screen as the workspace sees it.
 */
struct comp_ws_screen
{
	//! Registry monitor id (= `XR_DXR_display_info` displayId); never 0.
	uint64_t id;
	//! Desktop rect, in the space the OS places windows in (physical px on Windows).
	struct comp_seg_rect desktop;
	//! Physical size of the visible area, metres. Resolved: never 0 once the set is built
	//! (falls back to the primary's pixel pitch when the screen does not report one).
	float width_m;
	float height_m;
	//! COMP_WS_SCREEN_* bits.
	uint32_t flags;
	//! Rendering-mode index of the screen's presenter; meaningful for presenters only.
	uint32_t mode_index;
	//! View count of that mode (0 = unknown).
	uint32_t view_count;
	//! Whether the presenter's panel is in hardware 3D.
	bool hardware_3d;
	//! Nominal viewer relative to the screen centre, metres.
	float nominal_viewer_m[3];
	//! Discovery id of the plug-in that won the screen ("" = none).
	char plugin_id[COMP_SEGMENTS_ID_MAX];
};

/*!
 * The workspace's screens. Order is the registry's (system default first).
 */
struct comp_ws_screen_set
{
	uint32_t count;
	//! Index of the primary in @ref screens (UINT32_MAX when @ref count is 0).
	uint32_t primary_index;
	struct comp_ws_screen screens[COMP_WS_MAX_SCREENS];
};

/*!
 * Build the workspace screen set from the service's screen registry.
 *
 * Every monitor with a desktop rect is listed (a 2D monitor is listed without
 * @ref COMP_WS_SCREEN_3D and never presents, ADR-052 D4). The primary is the
 * `XRT_SCREEN_FLAG_SYSTEM_DEFAULT` screen (else the first one); it always
 * presents. Another screen presents iff @p policy allows more than one, it is
 * claimed (`plugin_id` set) above the fallback confidence (or at it, under
 * @ref COMP_WS_POLICY_CLAIMED_INCL_FALLBACK), and bit `i` of @p dp_capable_mask
 * says the claim can make a display processor for it (list index `i`).
 *
 * @return out->count.
 */
uint32_t
comp_ws_screens_build(const struct xrt_screen_list *list,
                      uint32_t dp_capable_mask,
                      enum comp_ws_policy policy,
                      struct comp_ws_screen_set *out);

//! The screen with @p id; id 0 = the primary. NULL when absent.
const struct comp_ws_screen *
comp_ws_screens_find(const struct comp_ws_screen_set *set, uint64_t id);

//! Number of presenter screens in @p set.
uint32_t
comp_ws_screens_presenter_count(const struct comp_ws_screen_set *set);

/*!
 * True when the two sets describe the same topology for a controller: same
 * screens, rects, sizes, flags and plug-ins. Mode fields are ignored (a mode
 * flip is not a screen change).
 */
bool
comp_ws_screens_equal(const struct comp_ws_screen_set *a, const struct comp_ws_screen_set *b);

/*!
 * Map a placement from @p from's display space into @p to's.
 *
 * Centre and size go through desktop pixels with each screen's own pixel
 * pitch; z and orientation are unchanged. `from == to` is an exact copy.
 *
 * @return false when either screen has no usable pixel pitch (out untouched).
 */
bool
comp_ws_map_pose(const struct comp_ws_screen *from,
                 const struct comp_ws_screen *to,
                 const struct xrt_pose *in,
                 float width_m,
                 float height_m,
                 struct xrt_pose *out,
                 float *out_width_m,
                 float *out_height_m);

/*!
 * The desktop-pixel footprint of a window placed in @p s's display space:
 * the axis-aligned bounds of its four corners, rotated by the pose and
 * projected straight onto the screen plane (perspective ignored — the
 * footprint decides which presenters draw the window, not how).
 *
 * @return false when @p s has no usable pixel pitch or the footprint is empty.
 */
bool
comp_ws_footprint(const struct comp_ws_screen *s,
                  const struct xrt_pose *pose,
                  float width_m,
                  float height_m,
                  struct comp_seg_rect *out);

/*!
 * One part of a window footprint: the piece one presenter draws.
 */
struct comp_ws_part
{
	uint32_t screen_index;            //!< Index into the set.
	uint64_t screen_id;               //!< That screen's id.
	bool is_primary;                  //!< It is the primary presenter.
	struct comp_seg_rect window_rect; //!< The part, in footprint pixels (origin = footprint top-left).
	struct comp_seg_rect screen_rect; //!< The part, in that screen's pixels.
};

/*!
 * Split a footprint across the PRESENTER screens of @p set (the window ->
 * presenter split, ADR-052 D2). Reuses @ref comp_segments_compute, so the seam
 * rule (half-open: a seam column belongs to exactly one screen) is the one
 * every other segment path uses. Ordered left to right.
 *
 * @return the number of parts written to @p out (at most COMP_SEGMENTS_MAX).
 */
uint32_t
comp_ws_split(const struct comp_ws_screen_set *set,
              const struct comp_seg_rect *footprint,
              struct comp_ws_part out[COMP_SEGMENTS_MAX]);

/*!
 * How a secondary presenter's compose pass draws the primary's display space
 * (ADR-052 D1/D2): window geometry stays in the PRIMARY's metres, and a point
 * (x, y) of that space lands at presenter pixel
 * (cx_px + x * ppm_x, cy_px - y * ppm_y) — the primary's centre expressed in
 * the presenter's pixels, at the primary's pixel pitch. Through desktop pixels
 * that is exactly @ref comp_ws_map_pose followed by the presenter's own
 * metres -> pixels, so the compose pass needs no per-window remapping.
 */
struct comp_ws_frame
{
	float cx_px;
	float cy_px;
	float ppm_x;
	float ppm_y;
};

/*!
 * The frame of @p presenter's pass. @return false when the primary has no
 * usable pixel pitch.
 */
bool
comp_ws_presenter_frame(const struct comp_ws_screen *primary,
                        const struct comp_ws_screen *presenter,
                        struct comp_ws_frame *out);

/*!
 * Which screen's eyes and geometry a workspace window's views are framed from
 * (ADR-052 D2, per-screen views). A client renders its window from the viewer
 * of the screen it is on, not always the primary's:
 *
 * - **On one presenter** (one part): that presenter frames the window — its
 *   eyes, its display space (the pose mapped through desktop pixels, z and
 *   orientation unchanged). The primary when that is where it is, which is the
 *   single-screen path byte for byte.
 * - **Straddling presenters** (two or more parts): a client that renders one
 *   view set per screen (`PRIMARY_MULTIVIEW_DXR` with set capacity > 1, the
 *   M3 contract) is handed a per-segment table (@ref comp_ws_segment_table) —
 *   each part framed from its own screen's eyes; any other client keeps ONE
 *   view set framed from the **majority** part's screen (most pixels; a tie
 *   goes to the primary, then to the leftmost — `oxr_segment_views_majority`'s
 *   rule).
 *
 * The per-segment table is cut from the footprint, so it is exact only for a
 * window parallel to the screens: a tilted window's footprint is the bound of
 * its rotated corners, which the client's view tiles do not map onto, so a
 * tilted straddler keeps the majority's one view set (@ref per_segment false).
 */
struct comp_ws_view_framing
{
	//! The window's footprint, desktop px.
	struct comp_seg_rect footprint;
	//! The window -> presenter split (@ref comp_ws_split), left to right.
	uint32_t part_count;
	struct comp_ws_part parts[COMP_SEGMENTS_MAX];
	//! Index into @ref parts of the majority part (0 when there are none).
	uint32_t majority;
	//! Index into the set of the screen that frames ONE view set for the
	//! window: the majority part's screen, the primary when there is no part.
	uint32_t frame_screen_index;
	//! True when the framing screen is not the primary (the views change).
	bool off_primary;
	//! A per-segment table may be handed out: 2+ parts, at most the caller's
	//! segment cap, an untilted window.
	bool per_segment;
};

/*!
 * Frame a window placed at @p pose (the PRIMARY's display space — where every
 * slot's pose lives) with size @p width_m x @p height_m. See
 * @ref comp_ws_view_framing.
 *
 * @param max_segments  The most parts a per-segment table may carry (XRT_MAX_SEGMENTS).
 * @return false when the set has no primary or the window no footprint
 *         (@p out then frames from the primary, no parts).
 */
bool
comp_ws_frame_window(const struct comp_ws_screen_set *set,
                     const struct xrt_pose *pose,
                     float width_m,
                     float height_m,
                     uint32_t max_segments,
                     struct comp_ws_view_framing *out);

/*!
 * The window metrics of a window placed at @p pose (the primary's display
 * space) as seen from screen @p s: @p s is the display, the window's centre,
 * size and depth are mapped into @p s's display space (@ref comp_ws_map_pose)
 * and its orientation is unchanged — the canvas the Kooima frustum of a view
 * set framed from @p s looks through. On the primary this equals the metrics
 * the single-screen workspace path has always built.
 *
 * @return false when either screen has no usable pixel pitch.
 */
bool
comp_ws_window_metrics(const struct comp_ws_screen_set *set,
                       const struct comp_ws_screen *s,
                       const struct xrt_pose *pose,
                       float width_m,
                       float height_m,
                       struct xrt_window_metrics *out);

/*!
 * The per-segment table of a straddling window (@p f with
 * @ref comp_ws_view_framing::per_segment): one segment per part, in the
 * `xrt_segment_metrics` shape the state tracker's per-segment views already
 * read (`oxr_segment_views_*`) — window px = footprint px (desktop px, origin
 * the footprint's top-left), canvas = the whole footprint, every screen's
 * desktop rect, size and nominal viewer, @p depth_m = the window's z.
 * `has_dp` / `woven` say whether the screen is 3D; eyes are left for the
 * caller (`have_eyes` false), which owns the DPs.
 *
 * @return the segment count (0 when @p f is not per-segment).
 */
uint32_t
comp_ws_segment_table(const struct comp_ws_screen_set *set,
                      const struct comp_ws_view_framing *f,
                      float depth_m,
                      struct xrt_segment_metrics *out);

/*!
 * Workspace pixels — the coordinate space of every cursor position the
 * workspace reports — are desktop pixels relative to the primary screen's
 * desktop origin. With one presenter they are the service window's client
 * pixels, exactly as before; with several, a cursor on another screen has
 * coordinates outside the primary's rect (negative to its left / above).
 */
void
comp_ws_desktop_to_workspace(
    const struct comp_ws_screen_set *set, int32_t desktop_x, int32_t desktop_y, int32_t *out_ws_x, int32_t *out_ws_y);

/*!
 * The screen under workspace pixel (@p ws_x, @p ws_y), and the point in that
 * screen's own pixels. Every listed screen is considered (a cursor on a 2D
 * monitor still resolves).
 *
 * @return the screen index, or -1 when the point is on no screen.
 */
int32_t
comp_ws_screen_at(
    const struct comp_ws_screen_set *set, int32_t ws_x, int32_t ws_y, int32_t *out_local_x, int32_t *out_local_y);

/*!
 * The policy the environment asks for: `DXR_SEGMENTS=0` -> single;
 * `DXR_WORKSPACE_SCREENS=fallback` -> claimed incl. fallback claims;
 * `DXR_WORKSPACE_SCREENS=single` -> single; otherwise claimed.
 */
enum comp_ws_policy
comp_ws_policy_from_env(const char *dxr_segments, const char *dxr_workspace_screens);

/*!
 * Which presenter window owns the pointer for a controller's pointer capture
 * (title-bar drag, edge resize, RMB rotation).
 *
 * Every presenter window runs its own window thread, and Win32 capture
 * (`SetCapture`) only takes on the thread whose window the button went down
 * on. So the controller's "capture the pointer" has to be applied to the
 * presenter that received the press, not to the primary: a press on a
 * secondary panel that grabs the primary's window grabs nothing, and the
 * secondary never reports motion (ADR-052).
 *
 * The owner is the source of the newest pointer DOWN drained from the
 * presenters (wrap-safe `GetTickCount` order; on a tie the later-noted DOWN
 * wins, so callers note in drain order). Screen id 0 means the primary
 * presenter (the service window), which is also the answer before any DOWN.
 */
struct comp_ws_pointer_owner
{
	uint64_t screen_id;    //!< 0 = the primary presenter
	uint32_t timestamp_ms; //!< the owning DOWN's timestamp
	bool valid;            //!< false until the first DOWN
};

/*!
 * Note a pointer DOWN from presenter @p screen_id (0 = primary) at
 * @p timestamp_ms. It becomes the owner unless an already-noted DOWN is newer.
 */
void
comp_ws_pointer_owner_note_down(struct comp_ws_pointer_owner *o, uint64_t screen_id, uint32_t timestamp_ms);

/*!
 * The presenter that owns the pointer: its screen id, 0 for the primary
 * (also before any DOWN was noted).
 */
uint64_t
comp_ws_pointer_owner_screen(const struct comp_ws_pointer_owner *o);

#ifdef __cplusplus
}
#endif
