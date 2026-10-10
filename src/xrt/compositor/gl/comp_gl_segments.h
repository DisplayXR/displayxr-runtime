// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Per-screen segment display processors for the in-process OpenGL
 *         compositor (multi-screen M6, ADR-047 D2 on Windows).
 * @ingroup comp_gl
 *
 * The OpenGL twin of d3d11/comp_d3d11_segments.h (and of the D3D12 port). A
 * window whose canvas covers several monitors is woven per segment: each
 * segment (canvas ∩ screen, see util/comp_segments.h) gets the display
 * processor of ITS screen, a pre-cropped GL_TEXTURE_2D holding exactly that
 * segment's views, `canvas = segment rect`, and its own present origin; the
 * results land in the one window framebuffer, each DP drawing with viewport
 * AND scissor = its segment. A segment with no DP for its screen (a plug-in
 * without `create_dp_gl_for_screen`), or whose DP needs 1:1 pixels on a
 * resampled screen, gets a flat 2D blit of one view instead.
 *
 * The session's primary DP (`comp_gl_compositor::display_processor`) keeps
 * weaving the primary screen's segment and keeps owning everything
 * view-related. The real HWND follows the screen holding the majority of the
 * window (ADR-047 Amendment 2): when another screen clearly holds it for 0.5 s,
 * that screen's segment DP is recreated WITH the window and the primary DP is
 * swapped for a windowless one (through @ref comp_gl_segments_hwnd_hooks), and
 * back. A window entirely on the primary screen never enters this module's
 * record path while the primary holds the window: that case stays
 * byte-for-byte the single-DP path.
 *
 * GL specifics: everything runs on the compositor's ONE GL context, current
 * on the weave thread (the app thread's layer_commit or the #868 repaint
 * thread, both under the compositor mutex) — DP creation and destruction
 * included, so every segment DP shares that context. GL executes in order on
 * one context, so retired DPs and crop textures need no deferred-release list.
 * All coordinates the GL calls take are converted from the top-left segment
 * math by util/comp_segments_gl.h.
 *
 * Design note: docs/architecture/comp-segments.md (§ Windows / OpenGL).
 */
#pragma once

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_compositor.h"
#include "xrt/xrt_display_metrics.h"
#include "xrt/xrt_display_processor_gl.h"
#include "xrt/xrt_screen.h"
#include "util/comp_segments.h"

#ifdef __cplusplus
extern "C" {
#endif

struct comp_gl_segments;

/*!
 * Create the segment manager. Nothing is allocated on the GPU until a segment
 * needs it (GL objects are made lazily, on the weave thread, with the
 * compositor's context current).
 */
struct comp_gl_segments *
comp_gl_segments_create(void);

/*!
 * Tear down every segment DP and GL object. The compositor's GL context MUST
 * be current and no weave in flight (the repaint thread is stopped). With hooks
 * set and another screen holding the window, the window is handed back to the
 * primary DP first; clear the hooks before a teardown that destroys the
 * primary DP too.
 */
void
comp_gl_segments_destroy(struct comp_gl_segments **segs_ptr);

/*!
 * How the manager moves the session's window between DPs (ADR-047
 * Amendment 2). Called on the weave thread with the compositor's lock held and
 * its GL context current.
 */
struct comp_gl_segments_hwnd_hooks
{
	//! The session's real window (HWND). NULL disables the hand-off.
	void *hwnd;
	void *userdata;
	/*!
	 * Install @p dp as the session's primary DP and return the previous one,
	 * which the manager destroys. The compositor re-sends its session-level
	 * state (transparency, shared-texture present, 2D/3D mode, eye-tracking
	 * mode) to @p dp and guards the exchange against its other threads.
	 */
	struct xrt_display_processor_gl *(*swap_primary)(void *userdata, struct xrt_display_processor_gl *dp);
};

/*!
 * Enable the window-handle hand-off. Call before
 * @ref comp_gl_segments_set_screens. NULL (or a NULL window) disables it: the
 * window stays with the primary DP.
 */
void
comp_gl_segments_set_hwnd_hooks(struct comp_gl_segments *segs, const struct comp_gl_segments_hwnd_hooks *hooks);

/*!
 * Hand over the system's screens and DP registry. Segmentation is enabled only
 * when at least two screens are listed, the registry knows the system-default
 * screen, @p pinned_display_id is 0 (`XrSessionDisplayBindingDXR` pins a
 * session to one display, which turns segmentation off) — and, GL-specific,
 * the primary screen's plug-in implements `create_dp_gl_for_screen`. A GL DP
 * from a plug-in that predates the slot re-states a whole-target viewport
 * inside process_atlas (the pre-M6 GL contract), so it would weave the
 * primary segment's cropped atlas across the whole window; such a session
 * keeps the single-DP path. Call with the GL context current (it may hand the
 * window back to the primary DP).
 */
void
comp_gl_segments_set_screens(struct comp_gl_segments *segs,
                             const struct xrt_screen_list *list,
                             const struct xrt_system_compositor_info *info,
                             uint64_t pinned_display_id);

//! Is segmentation possible at all for this session?
bool
comp_gl_segments_enabled(const struct comp_gl_segments *segs);

/*!
 * One metric update: recompute the segment table for the window, run the DP
 * lifecycle (create/destroy secondary DPs with hysteresis), run the
 * window-handle hand-off when one is due, log a table change once at INFO.
 * Call once per weave, before deciding which path draws, with the GL context
 * current.
 *
 * @param window_desktop  The window's client area in desktop device px.
 * @param canvas          The canvas in window px (NULL / empty = whole window).
 * @param mode_index      The head's active rendering-mode index; a change
 *                        re-reads each segment DP's resample tolerance.
 * @return true when this frame must take the split path
 *         (@ref comp_gl_segments_record) — the window spans screens, or the
 *         primary DP is windowless (another screen holds the window) and needs
 *         its present origin; false = the single-DP path.
 */
bool
comp_gl_segments_update(struct comp_gl_segments *segs,
                        const struct comp_seg_rect *window_desktop,
                        const struct comp_seg_rect *canvas,
                        uint32_t mode_index);

//! Everything the split path needs about this frame.
struct comp_gl_segments_frame
{
	//! The atlas (GLuint, GL_TEXTURE_2D): the tile grid stored from its
	//! bottom-left origin in GL row order (u_tiling_view_origin_gl).
	uint32_t src_texture;
	uint32_t view_width;
	uint32_t view_height;
	uint32_t tile_columns;
	uint32_t tile_rows;
	//! The format declared to the DPs (GLenum of the atlas, GL_RGBA8).
	uint32_t src_format;
	//! The framebuffer to weave into (GLuint; 0 = the window) and its size.
	uint32_t target_fbo;
	uint32_t target_width;
	uint32_t target_height;
	//! The canvas the atlas holds, window px (whole-canvas view dims above).
	struct comp_seg_rect canvas;
	//! Clear alpha for the target outside every segment; also re-declared to
	//! segment DPs when it changes.
	bool transparent_background;
	//! The session's primary DP (weaves the primary screen's segment). The
	//! caller has already fed it this frame's background.
	struct xrt_display_processor_gl *primary_dp;
};

/*!
 * Draw the split frame into @ref comp_gl_segments_frame::target_fbo: clear
 * it, crop each segment's views out of the atlas, run each segment's DP over
 * its canvas (primary first, then left to right; viewport + scissor =
 * segment, the target re-bound before every DP), and blit flat 2D into the
 * segments that cannot be woven. Leaves the target bound as the draw
 * framebuffer, viewport = the whole target, scissor test OFF.
 *
 * @return true if at least one DP wove.
 */
bool
comp_gl_segments_record(struct comp_gl_segments *segs, const struct comp_gl_segments_frame *f);

/*!
 * The session-wide hardware 2D/3D mode. Every segment DP follows it: sent to
 * each live DP on a change and to every newly created one (the primary DP
 * gets it from the compositor's own request path). Weave thread.
 */
void
comp_gl_segments_set_display_mode(struct comp_gl_segments *segs, bool enable_3d);

/*!
 * The predicted eyes of screen @p screen_id's segment DP, in that screen's
 * display space. Thread-safe against the weave creating or destroying segment
 * DPs. False when the screen has no segment DP (the primary's eyes come from
 * the session's own DP).
 */
bool
comp_gl_segments_get_eyes(struct comp_gl_segments *segs, uint64_t screen_id, struct xrt_eye_positions *out);

/*!
 * The screen whose DP holds the session's window handle right now (ADR-047
 * Amendment 2), 0 when none does or segmentation is off.
 */
uint64_t
comp_gl_segments_get_owner(const struct comp_gl_segments *segs);

/*!
 * Multi-screen M3: the last update's segment table as per-segment view
 * metrics (geometry, each screen's physical size + nominal viewer, whether it
 * is woven). Eyes are NOT filled — they are predicted per query
 * (@ref comp_gl_segments_get_eyes).
 *
 * @return false when the last update did not split the window, or split it
 *         into more than XRT_MAX_SEGMENTS segments (one view set then).
 */
bool
comp_gl_segments_get_metrics(const struct comp_gl_segments *segs,
                             const struct comp_seg_rect *window_desktop,
                             const struct comp_seg_rect *canvas,
                             bool primary_has_dp,
                             struct xrt_segment_metrics *out);

#ifdef __cplusplus
}
#endif
