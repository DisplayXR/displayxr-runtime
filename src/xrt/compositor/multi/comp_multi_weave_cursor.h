// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The weave-rect cursor of the POSIX XR_DXR_weave engines (desktop
 *         Linux, macOS) - the graphics-API-neutral half.
 * @author David Fattal
 * @ingroup comp_multi
 *
 * XR_DXR_cursor_depth spec v10 (ADR-046 Amendment 6): the POSIX twin of the
 * D3D11 service's weave-rect cursor (ADR-046 Amendment 4, spec v8). A weave
 * caller (the DisplayXR Browser) chains XrCursorDepthWeaveRequestDXR on a
 * submit: the depth under the pointer per rect and the cursor it would have
 * shown. The engine draws that cursor into every view of the hovered rect, in
 * the atlas its display processor is about to weave, and reports what it drew.
 *
 * This module owns everything that is not a GPU call, so the three engines
 * (Linux Vulkan, macOS Vulkan, macOS Metal) cannot drift apart and none of
 * them can drift from Windows: the request staged for the next submit, the
 * 500 ms depth memo, the sprite (generations, KEEP, the texture carrier's
 * pixels, the default look), the pick, the ONE policy and filter and HYBRID
 * anchor (u_cursor_weave_step, including the spec v9 range flag), the
 * crosstalk softening, the per-view boxes in the atlas, the state the caller
 * reads back and the change-only log. An engine supplies the frame (rects,
 * layout, window metres, eyes, the OS pointer when it can read one) and draws
 * the boxes this module returns (comp_multi_weave_cursor_vk.h, or the Metal
 * backend's own pass).
 *
 * ZERO COST UNLESS REQUESTED (ADR-046 section 0): a client that never chains a
 * request never reaches anything here but one boolean test per submit
 * (comp_multi_weave_cursor_begin), holds no sprite and allocates nothing.
 *
 * Not thread safe: every call is made under the engine's per-client lock.
 */

#pragma once

#include "xrt/xrt_defines.h"
#include "xrt/xrt_limits.h"
#include "xrt/xrt_display_metrics.h"

#include "util/u_cursor_depth.h"
#include "util/comp_cursor_look.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Where this submit's pointer comes from.
enum comp_multi_weave_cursor_pointer
{
	//! The engine read the OS pointer inside the submit (macOS, X11).
	COMP_MULTI_WEAVE_CURSOR_POINTER_OS = 0,
	//! The caller's sampleUV of this submit (Wayland: a service cannot read
	//! the pointer; and DXR_WEAVE_CURSOR_POINTER=caller, the probes' hook).
	COMP_MULTI_WEAVE_CURSOR_POINTER_CALLER = 1,
};

/*!
 * Per-client state. Zero-initialise; release with
 * comp_multi_weave_cursor_fini().
 */
struct comp_multi_weave_cursor
{
	//! @name The request staged for the NEXT submit (one connection = one thread)
	//! @{
	bool req_pending;
	struct xrt_weave_cursor_request req_next;
	//! @}

	//! What the last submit drew (XrCursorDepthWeaveStateDXR).
	struct xrt_weave_cursor_state out;

	//! One filter + anchor per weave client (one pointer per window).
	struct u_cursor_weave_state state;
	//! The caller's last depth per rect index (500 ms).
	struct u_cursor_weave_memo memo[XRT_WEAVE_CURSOR_MAX_RECTS];

	bool ever_requested;  //!< anything below may be held
	uint64_t last_req_ns; //!< the sprite is released 1 s after the last request

	//! @name The held sprite
	//! @{
	uint64_t sprite_gen;  //!< the generation held (0 = none)
	uint32_t sprite_kind; //!< u_cursor_weave_sprite_kind of that generation
	float sprite_size_m;
	//! A held TEXTURE sprite: tightly packed premultiplied sRGB RGBA8, top row
	//! first - or NULL with @ref tex_gpu when the pixels live in the engine's
	//! own GPU import (desktop Linux).
	uint8_t *tex_rgba;
	bool tex_gpu;
	bool tex_held;
	uint32_t tex_w, tex_h;
	float tex_hot_x, tex_hot_y;
	uint64_t tex_serial;
	//! @}

	//! @name The texture carrier staged by the IPC layer (latest wins)
	//! @{
	bool staged;
	uint8_t *staged_rgba; //!< NULL = the engine holds the carrier on its GPU
	struct xrt_weave_cursor_sprite_texture staged_info;
	//! @}

	//! The runtime's default look (zero until first needed).
	struct comp_cursor_look look;
	uint64_t look_serial;     //!< the image serial of look.generation == look_serial_gen
	uint64_t look_serial_gen; //!< 0 = no serial yet
	uint64_t serial_counter;

	int reason; //!< last logged outcome (change-only WARN)
};

//! One view's draw: the (blur-padded) box in atlas px, its sprite uv, its scissor.
struct comp_multi_weave_cursor_quad
{
	float rect[4];   //!< x, y, w, h (atlas px, y down)
	float uv[4];     //!< sprite uv of the box's top-left corner, and the uv it spans
	int32_t clip[4]; //!< x, y, w, h (atlas px)
};

//! What an engine draws this submit (valid until the next call into the module).
struct comp_multi_weave_cursor_draw
{
	uint32_t quad_count;
	struct comp_multi_weave_cursor_quad quads[XRT_MAX_VIEWS];
	//! B.7 softening: the 13-tap radius in sprite uv (0 = crisp) and the opacity.
	float radius_uv[2];
	float alpha;
	/*!
	 * The sprite: premultiplied, sRGB-encoded RGBA8, top row first. NULL when
	 * the engine holds it on the GPU (the caller's texture carrier it
	 * imported, desktop Linux). @ref image_serial moves whenever the pixels
	 * to draw change: upload / copy again when it differs from the last draw.
	 */
	const uint8_t *rgba;
	uint32_t image_w, image_h;
	uint64_t image_serial;
};

//! One submit's frame, supplied by the engine.
struct comp_multi_weave_cursor_frame
{
	const char *client; //!< for the log line
	//! The submit's rects (bound-window px); NULL / 0 = the legacy single-rect layout.
	const struct xrt_rect *rects;
	uint32_t rect_count;
	//! The atlas the display processor consumes (u_cursor_weave_tiles_batch / _nview).
	struct u_cursor_weave_tiles tiles;
	uint32_t view_count;
	//! The bound window in pixels (the rects' space).
	uint32_t win_w, win_h;
	//! Rect indices whose lifted rect draws its own (spec v6) cursor.
	uint32_t lift_rect_mask;

	enum comp_multi_weave_cursor_pointer pointer;
	//! POINTER_OS: was the pointer read, is it over the bound window, and where (window px).
	bool os_pointer_read;
	bool os_pointer_over_window;
	float os_x, os_y;

	bool flat_2d; //!< the panel is in 2D: the views are shown flat

	//! The weave frame: the window's centre in display space (metres), its size, the panel pitch.
	bool frame_valid;
	float center[3];
	float width_m, height_m;
	float m_per_px;

	const struct xrt_eye_positions *eyes; //!< NULL / invalid = none
	//! Window pixels per OS point (the default look's raster scale); <= 0 = the platform's.
	float backing_scale;
	uint64_t now_ns;
};

/*!
 * Stage the request of the next submit (IPC weave_cursor_request).
 */
void
comp_multi_weave_cursor_set_request(struct comp_multi_weave_cursor *c, const struct xrt_weave_cursor_request *req);

/*!
 * Stage a texture carrier (IPC weave_cursor_sprite_texture / _dmabuf). With
 * @p rgba (tightly packed premultiplied sRGB RGBA8, info->width x
 * info->height; copied) the module holds the pixels; with NULL the engine
 * holds the carrier on its GPU and draws it itself when a draw's rgba is NULL.
 * Latest wins. False (nothing staged) on bad dimensions.
 */
bool
comp_multi_weave_cursor_stage_texture(struct comp_multi_weave_cursor *c,
                                      const struct xrt_weave_cursor_sprite_texture *info,
                                      const uint8_t *rgba);

//! The engine could not import the GPU carrier it staged: forget it.
void
comp_multi_weave_cursor_drop_staged(struct comp_multi_weave_cursor *c);

//! What the last submit drew (IPC weave_cursor_state).
void
comp_multi_weave_cursor_get_state(const struct comp_multi_weave_cursor *c, struct xrt_weave_cursor_state *out);

/*!
 * Top of every weave submit: consume the staged request into @p out_req and
 * clear the reported state. Without a request: releases the held sprite once
 * none has arrived for 1 s (@p out_released, so the engine drops its GPU copy
 * too) and returns false - the submit then does no cursor work at all.
 */
bool
comp_multi_weave_cursor_begin(struct comp_multi_weave_cursor *c,
                              uint64_t now_ns,
                              struct xrt_weave_cursor_request *out_req,
                              bool *out_released);

/*!
 * Is the caller the pointer source on this window system?
 * DXR_WEAVE_CURSOR_POINTER=auto|os|caller (read once); auto = @p os_available.
 */
bool
comp_multi_weave_cursor_pointer_is_caller(bool os_available);

/*!
 * Decide and place this submit's cursor. True = draw @p out (then the state
 * already reports it; call comp_multi_weave_cursor_not_drawn() if the GPU
 * pass could not run). False = nothing to draw; the reason is logged when it
 * changes, never per frame.
 */
bool
comp_multi_weave_cursor_plan(struct comp_multi_weave_cursor *c,
                             const struct xrt_weave_cursor_request *req,
                             const struct comp_multi_weave_cursor_frame *f,
                             struct comp_multi_weave_cursor_draw *out);

//! The engine's pass could not draw what the plan returned.
void
comp_multi_weave_cursor_not_drawn(struct comp_multi_weave_cursor *c, const char *client, const char *why);

void
comp_multi_weave_cursor_fini(struct comp_multi_weave_cursor *c);

#ifdef __cplusplus
}
#endif
