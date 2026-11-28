// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The weave-rect cursor of the POSIX XR_DXR_weave engines - the
 *         graphics-API-neutral half (see comp_multi_weave_cursor.h).
 * @author David Fattal
 * @ingroup comp_multi
 *
 * Function for function the D3D11 service's svc_weave_cursor_draw_impl
 * (comp_d3d11_service.cpp): the same order of checks, the same refusal
 * reasons, the same shared policy calls. What differs is only what a POSIX
 * service can know - the pointer source - and that the drawing is handed back
 * to the engine as boxes.
 */

#include "comp_multi_weave_cursor.h"

#include "util/u_debug.h"
#include "util/u_logging.h"
#include "util/u_misc.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

DEBUG_GET_ONCE_OPTION(weave_cursor_pointer, "DXR_WEAVE_CURSOR_POINTER", "auto")

enum weave_cursor_reason
{
	WC_NONE = 0,
	WC_DRAWN,
	WC_DRAWN_HANDING_BACK,
	WC_NO_CURSOR_POS,
	WC_NOT_OVER_WINDOW,
	WC_NO_CALLER_POINTER,
	WC_NOT_OVER_A_REQUESTED_RECT,
	WC_LIFTED_RECT,
	WC_NO_DEPTH,
	WC_FLAT_2D,
	WC_NO_DISPLAY_INFO,
	WC_NO_EYES,
	WC_SPRITE_NONE,
	WC_NO_SPRITE,
	WC_NO_PIPELINE,
	WC_FORCED_REFUSE,
	WC_PLACEMENT_FAILED,
	WC_ON_PLANE,
	WC_NO_TILES,
	WC_LEGACY_LAYOUT,
};

static const char *
weave_cursor_reason_str(int r)
{
	switch (r) {
	case WC_DRAWN: return "DRAWN";
	case WC_DRAWN_HANDING_BACK: return "drawn, reported NOT drawn (handing back to the OS cursor)";
	case WC_NO_CURSOR_POS: return "not drawn: the OS pointer could not be read";
	case WC_NOT_OVER_WINDOW: return "not drawn: the pointer is not over the bound window";
	case WC_NO_CALLER_POINTER:
		return "not drawn: the caller is the pointer source here and this submit carries no entry with a "
		       "sampleUV inside its rect";
	case WC_NOT_OVER_A_REQUESTED_RECT: return "not drawn: the pointer is over no rect with a depth";
	case WC_LIFTED_RECT: return "not drawn here: the pointer is over a lifted rect (Amendment 1's cursor)";
	case WC_NO_DEPTH: return "not drawn: no fresh depth for the hovered rect";
	case WC_FLAT_2D: return "not drawn: the panel is in 2D";
	case WC_NO_DISPLAY_INFO: return "not drawn: no window metrics / pixel pitch";
	case WC_NO_EYES: return "not drawn: no valid eye pair from the display processor";
	case WC_SPRITE_NONE: return "not drawn: the page hides its cursor (sprite NONE)";
	case WC_NO_SPRITE: return "not drawn: no sprite could be built";
	case WC_NO_PIPELINE: return "not drawn: the engine's cursor pass is unavailable";
	case WC_FORCED_REFUSE: return "not drawn: DXR_CURSOR_DRAW_REFUSE";
	case WC_PLACEMENT_FAILED: return "not drawn: degenerate placement";
	case WC_ON_PLANE: return "not drawn: the cursor is on the display plane under it (the OS cursor)";
	case WC_NO_TILES: return "not drawn: the sprite falls outside the rect in every view";
	case WC_LEGACY_LAYOUT: return "not drawn: the legacy single-rect layout has no cursor";
	default: return "?";
	}
}

static void
weave_cursor_log(struct comp_multi_weave_cursor *c, const char *client, int why, const char *detail)
{
	if (why == c->reason) {
		return;
	}
	c->reason = why;
	// The outcome flips at mouse rate (the pointer crossing a rect, content
	// depth crossing the plane): WARN the first time each outcome appears,
	// INFO for every later transition back to it.
	const uint32_t bit = (why >= 0 && why < 32) ? (1u << (uint32_t)why) : 0u;
	const bool first = bit != 0 && (c->reasons_warned & bit) == 0;
	c->reasons_warned |= bit;
	U_LOG_IFL((first ? U_LOGGING_WARN : U_LOGGING_INFO), u_log_get_global_level(),
	          "[weave cursor] '%s': %s%s%s (sprite generation %llu, ADR-046 Amendment 6)",
	          client != NULL && client[0] != '\0' ? client : "?", weave_cursor_reason_str(why),
	          detail != NULL ? " - " : "", detail != NULL ? detail : "", (unsigned long long)c->sprite_gen);
}

static void
weave_cursor_release_sprite(struct comp_multi_weave_cursor *c)
{
	free(c->tex_rgba);
	c->tex_rgba = NULL;
	c->tex_gpu = false;
	c->tex_held = false;
	c->tex_w = 0;
	c->tex_h = 0;
	c->sprite_gen = 0;
	c->sprite_kind = U_CURSOR_WEAVE_SPRITE_KEEP;
	c->sprite_size_m = 0.0f;
}

static void
weave_cursor_release_staged(struct comp_multi_weave_cursor *c)
{
	free(c->staged_rgba);
	c->staged_rgba = NULL;
	c->staged = false;
}

void
comp_multi_weave_cursor_set_request(struct comp_multi_weave_cursor *c, const struct xrt_weave_cursor_request *req)
{
	if (c == NULL || req == NULL || req->rect_count > XRT_WEAVE_CURSOR_MAX_RECTS) {
		return;
	}
	c->req_next = *req;
	c->req_pending = true;
}

bool
comp_multi_weave_cursor_stage_texture(struct comp_multi_weave_cursor *c,
                                      const struct xrt_weave_cursor_sprite_texture *info,
                                      const uint8_t *rgba)
{
	if (c == NULL || info == NULL || info->width == 0 || info->height == 0 || info->width > 256 ||
	    info->height > 256) {
		return false;
	}
	uint8_t *copy = NULL;
	if (rgba != NULL) {
		const size_t size = (size_t)info->width * info->height * 4;
		copy = (uint8_t *)malloc(size);
		if (copy == NULL) {
			return false;
		}
		memcpy(copy, rgba, size);
	}
	weave_cursor_release_staged(c); // latest wins
	c->staged_rgba = copy;
	c->staged_info = *info;
	c->staged = true;
	c->ever_requested = true; // something is held: the idle release applies
	return true;
}

void
comp_multi_weave_cursor_drop_staged(struct comp_multi_weave_cursor *c)
{
	if (c != NULL) {
		weave_cursor_release_staged(c);
	}
}

void
comp_multi_weave_cursor_get_state(const struct comp_multi_weave_cursor *c, struct xrt_weave_cursor_state *out)
{
	if (out == NULL) {
		return;
	}
	if (c == NULL) {
		U_ZERO(out);
		return;
	}
	*out = c->out;
}

bool
comp_multi_weave_cursor_begin(struct comp_multi_weave_cursor *c,
                              uint64_t now_ns,
                              struct xrt_weave_cursor_request *out_req,
                              bool *out_released)
{
	*out_released = false;
	if (!c->req_pending) {
		if (!c->ever_requested) {
			return false; // the whole cost of a client that never asks
		}
		U_ZERO(&c->out);
		if (u_cursor_weave_sprite_idle(c->last_req_ns, now_ns)) {
			weave_cursor_release_sprite(c);
			weave_cursor_release_staged(c);
			comp_cursor_look_fini(&c->look);
			U_ZERO(&c->look);
			c->look_serial_gen = 0;
			U_ZERO(&c->state);
			U_ZERO(&c->memo);
			c->ever_requested = false;
			c->reason = WC_NONE;
			*out_released = true;
			U_LOG_W("[weave cursor] no request for 1 s - sprite released");
		}
		return false;
	}
	*out_req = c->req_next;
	c->req_pending = false;
	c->ever_requested = true;
	c->last_req_ns = now_ns;
	U_ZERO(&c->out);
	return true;
}

bool
comp_multi_weave_cursor_pointer_is_caller(bool os_available)
{
	const char *opt = debug_get_option_weave_cursor_pointer();
	if (opt != NULL && strcmp(opt, "caller") == 0) {
		return true;
	}
	if (opt != NULL && strcmp(opt, "os") == 0) {
		return false;
	}
	return !os_available;
}

//! The sprite one draw uses.
struct weave_cursor_sprite
{
	const uint8_t *rgba; //!< NULL = GPU-held
	uint32_t w, h;
	float hot_u, hot_v;
	float height_px; //!< drawn height in window px at the glass
	uint64_t serial;
	bool nothing; //!< sprite NONE
};

/*!
 * Resolve the request's sprite against the held one (the POSIX twin of
 * svc_weave_cursor_sprite_get). Updates the held generation. False if no
 * sprite at all could be produced.
 */
static bool
weave_cursor_sprite_get(struct comp_multi_weave_cursor *c,
                        const struct xrt_weave_cursor_request *req,
                        float backing_scale,
                        float m_per_px,
                        struct weave_cursor_sprite *out)
{
	U_ZERO(out);
	enum u_cursor_weave_sprite_action act =
	    u_cursor_weave_sprite_resolve(req->sprite_kind, req->sprite_generation, c->sprite_gen);
	if (act == U_CURSOR_WEAVE_SPRITE_LOAD_OS_CURSOR) {
		// An OS cursor handle is a Windows carrier: no POSIX platform can
		// rasterise another process's cursor object. Keep the last look,
		// never a wrong one.
		act = c->tex_held ? U_CURSOR_WEAVE_SPRITE_USE_HELD : U_CURSOR_WEAVE_SPRITE_USE_DEFAULT;
	} else if (act == U_CURSOR_WEAVE_SPRITE_LOAD_TEXTURE) {
		if (c->staged && c->staged_info.generation == req->sprite_generation) {
			free(c->tex_rgba);
			c->tex_rgba = c->staged_rgba; // may be NULL: the engine holds it
			c->tex_gpu = c->staged_rgba == NULL;
			c->staged_rgba = NULL;
			c->staged = false;
			c->tex_held = true;
			c->tex_w = c->staged_info.width;
			c->tex_h = c->staged_info.height;
			c->tex_hot_x = c->staged_info.hot_x;
			c->tex_hot_y = c->staged_info.hot_y;
			c->tex_serial = ++c->serial_counter;
			c->sprite_gen = req->sprite_generation;
			c->sprite_kind = U_CURSOR_WEAVE_SPRITE_TEXTURE;
			c->sprite_size_m = req->size_m > 0.0f ? req->size_m : c->staged_info.size_m;
			U_LOG_W("[weave cursor] sprite: caller texture %ux%u (generation %llu, hotspot %.1f,%.1f, %s)",
			        c->tex_w, c->tex_h, (unsigned long long)c->sprite_gen, (double)c->tex_hot_x,
			        (double)c->tex_hot_y, c->tex_gpu ? "held on the GPU" : "CPU pixels");
			act = U_CURSOR_WEAVE_SPRITE_USE_HELD;
		} else {
			act = c->tex_held ? U_CURSOR_WEAVE_SPRITE_USE_HELD : U_CURSOR_WEAVE_SPRITE_USE_DEFAULT;
		}
	} else if (act == U_CURSOR_WEAVE_SPRITE_DRAW_NOTHING) {
		c->sprite_gen = req->sprite_generation;
		c->sprite_kind = U_CURSOR_WEAVE_SPRITE_NONE;
	} else if (act == U_CURSOR_WEAVE_SPRITE_USE_DEFAULT && req->sprite_kind == U_CURSOR_WEAVE_SPRITE_DEFAULT) {
		c->sprite_gen = req->sprite_generation;
		c->sprite_kind = U_CURSOR_WEAVE_SPRITE_DEFAULT;
		c->sprite_size_m = req->size_m;
	}

	if (act == U_CURSOR_WEAVE_SPRITE_USE_HELD) {
		switch (c->sprite_kind) {
		case U_CURSOR_WEAVE_SPRITE_NONE: act = U_CURSOR_WEAVE_SPRITE_DRAW_NOTHING; break;
		case U_CURSOR_WEAVE_SPRITE_DEFAULT: act = U_CURSOR_WEAVE_SPRITE_USE_DEFAULT; break;
		default: break;
		}
	}

	if (act == U_CURSOR_WEAVE_SPRITE_DRAW_NOTHING) {
		out->nothing = true;
		return true;
	}
	if (act == U_CURSOR_WEAVE_SPRITE_USE_DEFAULT || !c->tex_held || c->tex_w == 0 || c->tex_h == 0) {
		if (!comp_cursor_look_update(&c->look, backing_scale) || c->look.rgba == NULL || c->look.w == 0 ||
		    c->look.h == 0) {
			return false;
		}
		if (c->look_serial_gen != c->look.generation || c->look_serial == 0) {
			c->look_serial_gen = c->look.generation;
			c->look_serial = ++c->serial_counter;
		}
		out->rgba = c->look.rgba;
		out->w = c->look.w;
		out->h = c->look.h;
		out->hot_u = c->look.hot_u;
		out->hot_v = c->look.hot_v;
		out->serial = c->look_serial;
		const float size_m = c->sprite_kind == U_CURSOR_WEAVE_SPRITE_DEFAULT ? c->sprite_size_m : 0.0f;
		out->height_px =
		    size_m > 0.0f && m_per_px > 0.0f ? size_m / m_per_px : c->look.height_pt * backing_scale;
		return out->height_px > 0.0f;
	}
	out->rgba = c->tex_rgba;
	out->w = c->tex_w;
	out->h = c->tex_h;
	out->hot_u = c->tex_hot_x / (float)c->tex_w;
	out->hot_v = c->tex_hot_y / (float)c->tex_h;
	out->serial = c->tex_serial;
	// One image px is one panel px at the glass; sizeMeters overrides.
	out->height_px = c->sprite_size_m > 0.0f && m_per_px > 0.0f ? c->sprite_size_m / m_per_px : (float)c->tex_h;
	return out->height_px > 0.0f;
}

static int
weave_cursor_plan_impl(struct comp_multi_weave_cursor *c,
                       const struct xrt_weave_cursor_request *req,
                       const struct comp_multi_weave_cursor_frame *f,
                       struct comp_multi_weave_cursor_draw *out)
{
	const uint64_t now_ns = f->now_ns;
	uint32_t rect_count = f->rects != NULL ? f->rect_count : 0;
	if (rect_count > XRT_WEAVE_CURSOR_MAX_RECTS) {
		rect_count = XRT_WEAVE_CURSOR_MAX_RECTS;
	}

	// This submit's depths -> the 500 ms memo; every rect with a fresh depth is eligible.
	struct u_cursor_weave_rect wr[XRT_WEAVE_CURSOR_MAX_RECTS];
	for (uint32_t i = 0; i < rect_count; i++) {
		// xrt_offset names its fields w/h; they hold x/y here.
		wr[i] = (struct u_cursor_weave_rect){f->rects[i].offset.w, f->rects[i].offset.h, f->rects[i].extent.w,
		                                     f->rects[i].extent.h};
	}
	uint32_t entry_index[XRT_WEAVE_CURSOR_MAX_RECTS];
	float entry_uv[2 * XRT_WEAVE_CURSOR_MAX_RECTS];
	uint32_t entry_count = 0;
	for (uint32_t i = 0; i < req->rect_count && i < XRT_WEAVE_CURSOR_MAX_RECTS; i++) {
		const struct xrt_weave_cursor_rect *e = &req->rects[i];
		if (e->rect_index < rect_count) {
			u_cursor_weave_memo_put(&c->memo[e->rect_index], e->rect_index, &wr[e->rect_index],
			                        e->has_content != 0, e->disparity_px, e->flags, now_ns);
			entry_index[entry_count] = e->rect_index;
			entry_uv[2 * entry_count + 0] = e->sample_u;
			entry_uv[2 * entry_count + 1] = e->sample_v;
			entry_count++;
		}
	}
	uint32_t caller_mask = 0;
	for (uint32_t i = 0; i < rect_count; i++) {
		bool has = false;
		float dpx = 0.0f;
		if (u_cursor_weave_memo_get(&c->memo[i], i, &wr[i], now_ns, &has, &dpx)) {
			caller_mask |= 1u << i;
		}
	}

	// The sprite is resolved on every request, so the reported generation is
	// right even on a frame that draws nothing.
	const float backing_scale = f->backing_scale > 0.0f ? f->backing_scale : comp_cursor_look_backing_scale();
	struct weave_cursor_sprite spr;
	const bool have_sprite = weave_cursor_sprite_get(c, req, backing_scale, f->m_per_px, &spr);
	c->out.sprite_generation = c->sprite_gen;

	if (rect_count == 0 || f->win_w == 0 || f->win_h == 0) {
		return WC_LEGACY_LAYOUT;
	}

	// The pointer, in the bound window's pixels.
	float px = 0.0f, py = 0.0f;
	if (f->pointer == COMP_MULTI_WEAVE_CURSOR_POINTER_CALLER) {
		if (!u_cursor_weave_caller_pointer(wr, rect_count, entry_index, entry_uv, entry_count, &px, &py)) {
			return WC_NO_CALLER_POINTER;
		}
	} else {
		if (!f->os_pointer_read) {
			return WC_NO_CURSOR_POS;
		}
		if (!f->os_pointer_over_window) {
			return WC_NOT_OVER_WINDOW;
		}
		px = f->os_x;
		py = f->os_y;
	}
	const int32_t pick = u_cursor_weave_pick_rect(wr, rect_count, caller_mask | f->lift_rect_mask, px, py);
	const enum u_cursor_weave_source src = u_cursor_weave_source_for(pick, caller_mask, f->lift_rect_mask);
	if (src == U_CURSOR_WEAVE_SOURCE_LIFT) {
		return WC_LIFTED_RECT;
	}
	if (src != U_CURSOR_WEAVE_SOURCE_CALLER) {
		return WC_NOT_OVER_A_REQUESTED_RECT;
	}
	bool has_content = false;
	float disparity_px = 0.0f;
	if (!u_cursor_weave_memo_get(&c->memo[pick], (uint32_t)pick, &wr[pick], now_ns, &has_content, &disparity_px)) {
		return WC_NO_DEPTH;
	}
	if (f->flat_2d) {
		return WC_FLAT_2D; // the views are shown flat: the OS cursor is right
	}
	if (!f->frame_valid || !(f->width_m > 0.0f) || !(f->height_m > 0.0f) || !(f->m_per_px > 0.0f)) {
		return WC_NO_DISPLAY_INFO;
	}
	const struct xrt_eye_positions *eyes = f->eyes;
	if (eyes == NULL || !eyes->valid || eyes->count < 2) {
		return WC_NO_EYES;
	}
	const uint32_t view_count = f->view_count;
	if (view_count < 2 || view_count > XRT_MAX_VIEWS) {
		return WC_NO_TILES;
	}
	float eye_xyz[3 * 8];
	const uint32_t ec = eyes->count > 8 ? 8 : eyes->count;
	for (uint32_t i = 0; i < ec; i++) {
		eye_xyz[3 * i + 0] = eyes->eyes[i].x;
		eye_xyz[3 * i + 1] = eyes->eyes[i].y;
		eye_xyz[3 * i + 2] = eyes->eyes[i].z;
	}
	float vps[3 * XRT_MAX_VIEWS];
	if (!u_cursor_weave_viewpoints(eye_xyz, ec, view_count, f->center, vps)) {
		return WC_NO_EYES;
	}
	float content_d = 0.0f;
	if (has_content && !u_cursor_weave_px_to_baselines(disparity_px, f->m_per_px,
	                                                   u_cursor_weave_baseline(vps, view_count), &content_d)) {
		return WC_NO_EYES;
	}

	struct u_cursor_depth_tuning tuning;
	u_cursor_depth_tuning_defaults(&tuning);
	// Spec v9 (ADR-046 Amendment 5): the rect's own range, from the flags of
	// the submit that last carried its depth. No bit = FRONT_ONLY.
	tuning.range = u_cursor_weave_range_from_flags(c->memo[pick].flags);
	struct u_cursor_weave_input in;
	U_ZERO(&in);
	in.viewpoints = vps;
	in.view_count = view_count;
	in.u = px / (float)f->win_w;
	in.v = py / (float)f->win_h;
	in.sx = (in.u - 0.5f) * f->width_m;
	in.sy = (0.5f - in.v) * f->height_m;
	in.rect_index = (uint32_t)pick;
	in.rect_w = wr[pick].w;
	in.rect_h = wr[pick].h;
	in.has_content = has_content;
	in.content_d = content_d;
	in.now_ns = now_ns;
	struct u_cursor_weave_output step;
	if (!u_cursor_weave_step(&c->state, &tuning, &in, &step)) {
		return WC_PLACEMENT_FAILED;
	}
	if (!step.draw) {
		return WC_ON_PLANE;
	}
	if (!have_sprite) {
		return WC_NO_SPRITE;
	}
	if (spr.nothing) {
		return WC_SPRITE_NONE;
	}
	struct u_cursor_draw_request probe; // the verification hook only reads the env
	U_ZERO(&probe);
	if (comp_cursor_draw_debug_refuse(&probe)) {
		return WC_FORCED_REFUSE;
	}

	// Per view: the hotspot on the plane -> window px -> the view's tile.
	const float spr_h = spr.height_px;
	const float spr_w = spr_h * (float)spr.w / (float)spr.h;
	const float hot_x = spr.hot_u * spr_w, hot_y = spr.hot_v * spr_h;
	int32_t cx0 = wr[pick].x < 0 ? 0 : wr[pick].x, cy0 = wr[pick].y < 0 ? 0 : wr[pick].y;
	int32_t cx1 = wr[pick].x + wr[pick].w, cy1 = wr[pick].y + wr[pick].h;
	cx1 = cx1 > (int32_t)f->win_w ? (int32_t)f->win_w : cx1;
	cy1 = cy1 > (int32_t)f->win_h ? (int32_t)f->win_h : cy1;
	if (cx1 <= cx0 || cy1 <= cy0) {
		return WC_NO_TILES;
	}
	const struct u_cursor_weave_rect clip = {cx0, cy0, cx1 - cx0, cy1 - cy0};
	float hot_px[2 * XRT_MAX_VIEWS];
	for (uint32_t v = 0; v < view_count; v++) {
		hot_px[2 * v + 0] = (step.xy[2 * v + 0] / f->width_m + 0.5f) * (float)f->win_w;
		hot_px[2 * v + 1] = (0.5f - step.xy[2 * v + 1] / f->height_m) * (float)f->win_h;
	}
	struct u_cursor_weave_rect bounds;
	if (!u_cursor_weave_sprite_bounds(hot_px, view_count, spr_w, spr_h, hot_x, hot_y, &clip, &bounds)) {
		return WC_NO_TILES;
	}

	// B.7: the ghost is the largest neighbouring-view hotspot distance (window =
	// panel px); the blur cap follows the drawn sprite.
	float ghost_px = 0.0f;
	for (uint32_t k = 0; k + 1 < view_count; k++) {
		const float gx = hot_px[2 * (k + 1)] - hot_px[2 * k], gy = hot_px[2 * (k + 1) + 1] - hot_px[2 * k + 1];
		const float g = sqrtf(gx * gx + gy * gy);
		if (isfinite(g) && g > ghost_px) {
			ghost_px = g;
		}
	}
	const struct u_cursor_soften soften =
	    u_cursor_draw_soften_sized(step.disparity, ghost_px, spr_h, comp_cursor_soften_params());

	U_ZERO(out);
	out->alpha = soften.alpha;
	const float blur_px = soften.blur_px > 0.0f ? soften.blur_px : 0.0f;
	if (blur_px > 0.0f) {
		out->radius_uv[0] = blur_px / spr_w;
		out->radius_uv[1] = blur_px / spr_h;
	}
	for (uint32_t v = 0; v < view_count; v++) {
		struct u_cursor_weave_quad q;
		if (!u_cursor_weave_view_quad(&f->tiles, v, hot_px[2 * v + 0], hot_px[2 * v + 1], spr_w, spr_h, hot_x,
		                              hot_y, &clip, &q)) {
			continue;
		}
		struct comp_multi_weave_cursor_quad *d = &out->quads[out->quad_count++];
		u_cursor_weave_quad_pad(&q, blur_px * f->tiles.scale_x, blur_px * f->tiles.scale_y, d->rect, d->uv);
		d->clip[0] = q.clip_x;
		d->clip[1] = q.clip_y;
		d->clip[2] = q.clip_w;
		d->clip[3] = q.clip_h;
	}
	if (out->quad_count == 0) {
		return WC_NO_TILES;
	}
	out->rgba = spr.rgba;
	out->image_w = spr.w;
	out->image_h = spr.h;
	out->image_serial = spr.serial;

	if (!step.report_drawn) {
		return WC_DRAWN_HANDING_BACK;
	}
	c->out.drawn_rect_mask = 1u << (uint32_t)pick;
	c->out.bounds_x = bounds.x;
	c->out.bounds_y = bounds.y;
	c->out.bounds_w = bounds.w;
	c->out.bounds_h = bounds.h;
	return WC_DRAWN;
}

bool
comp_multi_weave_cursor_plan(struct comp_multi_weave_cursor *c,
                             const struct xrt_weave_cursor_request *req,
                             const struct comp_multi_weave_cursor_frame *f,
                             struct comp_multi_weave_cursor_draw *out)
{
	U_ZERO(out);
	const int why = weave_cursor_plan_impl(c, req, f, out);
	weave_cursor_log(c, f->client, why, NULL);
	if (why != WC_DRAWN && why != WC_DRAWN_HANDING_BACK) {
		out->quad_count = 0;
		return false;
	}
	return true;
}

void
comp_multi_weave_cursor_not_drawn(struct comp_multi_weave_cursor *c, const char *client, const char *why)
{
	const uint64_t gen = c->out.sprite_generation;
	U_ZERO(&c->out);
	c->out.sprite_generation = gen;
	weave_cursor_log(c, client, WC_NO_PIPELINE, why);
}

void
comp_multi_weave_cursor_fini(struct comp_multi_weave_cursor *c)
{
	if (c == NULL) {
		return;
	}
	weave_cursor_release_sprite(c);
	weave_cursor_release_staged(c);
	comp_cursor_look_fini(&c->look);
	U_ZERO(c);
}
