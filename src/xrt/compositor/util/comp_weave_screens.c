// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Per-screen display processors for a service-side XR_DXR_weave
 *         present-owner — see comp_weave_screens.h.
 * @ingroup comp_util
 */

#include "util/comp_weave_screens.h"

#include "xrt/xrt_compositor.h"

#include "util/u_weave_present_origin.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

const char *
comp_weave_frame_str(enum comp_weave_frame f)
{
	switch (f) {
	case COMP_WEAVE_FRAME_LOGICAL: return "logical";
	case COMP_WEAVE_FRAME_DEVICE: return "device";
	default: return "none";
	}
}

static bool
screen_placed(const struct xrt_screen *s)
{
	return s->desktop_width > 0 && s->desktop_height > 0;
}

static struct u_weave_panel_frame
screen_frame(const struct xrt_screen *s)
{
	struct u_weave_panel_frame p = {
	    .screen_left = s->desktop_left,
	    .screen_top = s->desktop_top,
	    .desktop_w = s->desktop_width,
	    .desktop_h = s->desktop_height,
	    .native_w = s->native_width,
	    .native_h = s->native_height,
	    .scale = (double)s->desktop_scale,
	};
	return p;
}

static uint32_t
round_u32(double v)
{
	return v <= 0.0 ? 0u : (uint32_t)(v + 0.5);
}

/*!
 * DEVICE frame: every placed screen's X11 rect is its device rect — rule 1
 * converts nothing and the device mode (when known) equals the rect.
 */
static bool
device_frame_holds(const struct xrt_screen_list *in)
{
	for (uint32_t i = 0; i < in->count && i < XRT_SCREEN_LIST_MAX; i++) {
		const struct xrt_screen *s = &in->screens[i];
		if (!screen_placed(s)) {
			continue;
		}
		const struct u_weave_panel_frame p = screen_frame(s);
		int32_t dx = 0, dy = 0;
		if (u_weave_panel_device_origin(&p, &dx, &dy)) {
			return false;
		}
		if (s->native_width != 0 &&
		    (s->native_width != s->desktop_width || s->native_height != s->desktop_height)) {
			return false;
		}
	}
	return true;
}

enum comp_weave_frame
comp_weave_screens_project(const struct xrt_screen_list *in,
                           const struct comp_weave_window *win,
                           struct xrt_screen_list *out,
                           struct comp_seg_rect *out_window)
{
	if (out == NULL || out_window == NULL) {
		return COMP_WEAVE_FRAME_NONE;
	}
	memset(out_window, 0, sizeof(*out_window));
	if (in == NULL) {
		memset(out, 0, sizeof(*out));
		return COMP_WEAVE_FRAME_NONE;
	}
	*out = *in;
	if (win == NULL || win->w == 0 || win->h == 0) {
		return COMP_WEAVE_FRAME_NONE;
	}
	*out_window = (struct comp_seg_rect){win->x, win->y, win->w, win->h};

	const bool logical = win->logical_valid && win->logical_scale > 0.0f && win->logical_scale <= 64.0f;
	if (!logical) {
		// Without the logical origin the weave geometry frame is only
		// unambiguous when it IS the X11 frame.
		return device_frame_holds(in) ? COMP_WEAVE_FRAME_DEVICE : COMP_WEAVE_FRAME_NONE;
	}

	const double F = (double)win->logical_scale; // the window's stage -> device factor
	for (uint32_t i = 0; i < in->count && i < XRT_SCREEN_LIST_MAX; i++) {
		const struct xrt_screen *s = &in->screens[i];
		struct xrt_screen *o = &out->screens[i];
		if (!screen_placed(s)) {
			continue; // keeps its zero rect: the manager's filter skips it alike
		}
		const struct u_weave_panel_frame p = screen_frame(s);

		// Rule 1 + rule 2 (u_weave_present_origin.h): the screen's device
		// origin, its stage factor, and so its stage origin — the exact
		// values the single-panel phase computes for the panel.
		int32_t pdx = 0, pdy = 0;
		(void)u_weave_panel_device_origin(&p, &pdx, &pdy);
		double f = u_weave_panel_stage_factor(&p, F);
		if (!(f > 0.0)) {
			f = F;
		}
		const int32_t psx = u_weave_round((double)pdx / f);
		const int32_t psy = u_weave_round((double)pdy / f);

		// Anchored on the window: window - screen == the rule-2 present
		// origin (stage offset x factor), exactly, for a 1:1 screen.
		o->desktop_left = win->x - u_wl_logical_to_px(win->logical_x - psx, F);
		o->desktop_top = win->y - u_wl_logical_to_px(win->logical_y - psy, F);

		// The screen's stage extent (device mode / its factor) in window px.
		const double dev_w = (double)(s->native_width != 0 ? s->native_width : s->desktop_width);
		const double dev_h = (double)(s->native_height != 0 ? s->native_height : s->desktop_height);
		o->desktop_width = round_u32(dev_w / f * F);
		o->desktop_height = round_u32(dev_h / f * F);

		// 1:1 exactly when the screen's factor is the window's: then its
		// device mode IS its rect in this frame. Otherwise the display
		// server resamples the window there: report the real device mode
		// (!= the rect). An unknown device mode stays unknown (never degrades).
		if (s->native_width == 0 || s->native_height == 0) {
			o->native_width = 0;
			o->native_height = 0;
		} else if (fabs(f - F) < 1e-3) {
			o->native_width = o->desktop_width;
			o->native_height = o->desktop_height;
		} else {
			o->native_width = s->native_width;
			o->native_height = s->native_height;
			if (o->native_width == o->desktop_width && o->native_height == o->desktop_height) {
				// A rounding coincidence must not read as 1:1.
				o->native_width = o->desktop_width + 1;
			}
		}
	}
	return COMP_WEAVE_FRAME_LOGICAL;
}

static bool
same_identity(const struct xrt_screen *a, const struct xrt_screen *b)
{
	return a->id == b->id && a->flags == b->flags && a->confidence == b->confidence &&
	       strncmp(a->plugin_id, b->plugin_id, sizeof(a->plugin_id)) == 0 &&
	       strncmp(a->device_name, b->device_name, sizeof(a->device_name)) == 0 &&
	       screen_placed(a) == screen_placed(b);
}

static bool
same_geometry(const struct xrt_screen *a, const struct xrt_screen *b)
{
	return a->desktop_left == b->desktop_left && a->desktop_top == b->desktop_top &&
	       a->desktop_width == b->desktop_width && a->desktop_height == b->desktop_height &&
	       a->native_width == b->native_width && a->native_height == b->native_height &&
	       a->desktop_scale == b->desktop_scale && a->physical_width_mm == b->physical_width_mm &&
	       a->physical_height_mm == b->physical_height_mm;
}

enum comp_weave_screens_change
comp_weave_screens_compare(const struct xrt_screen_list *a, const struct xrt_screen_list *b)
{
	const uint32_t na = a != NULL ? (a->count < XRT_SCREEN_LIST_MAX ? a->count : XRT_SCREEN_LIST_MAX) : 0;
	const uint32_t nb = b != NULL ? (b->count < XRT_SCREEN_LIST_MAX ? b->count : XRT_SCREEN_LIST_MAX) : 0;
	if (na != nb) {
		return COMP_WEAVE_SCREENS_MEMBERSHIP;
	}
	enum comp_weave_screens_change r = COMP_WEAVE_SCREENS_SAME;
	for (uint32_t i = 0; i < na; i++) {
		if (!same_identity(&a->screens[i], &b->screens[i])) {
			return COMP_WEAVE_SCREENS_MEMBERSHIP;
		}
		if (!same_geometry(&a->screens[i], &b->screens[i])) {
			r = COMP_WEAVE_SCREENS_GEOMETRY;
		}
	}
	return r;
}

static bool
entry_same(const struct xrt_dp_registry_entry *a, const struct xrt_dp_registry_entry *b)
{
	return a->monitor_id == b->monitor_id && a->confidence == b->confidence && a->screen_left == b->screen_left &&
	       a->screen_top == b->screen_top && a->pixel_width == b->pixel_width &&
	       a->pixel_height == b->pixel_height && strncmp(a->plugin_id, b->plugin_id, sizeof(a->plugin_id)) == 0 &&
	       strncmp(a->serial, b->serial, sizeof(a->serial)) == 0 && a->dp_factory_vk == b->dp_factory_vk &&
	       a->owning_iface == b->owning_iface && a->owning_instance == b->owning_instance &&
	       a->not_ready == b->not_ready && a->primary_override == b->primary_override &&
	       a->platform_id == b->platform_id;
}

bool
comp_weave_screens_registry_changed(const struct xrt_dp_factory_registry *a, const struct xrt_dp_factory_registry *b)
{
	const uint32_t na =
	    a != NULL ? (a->entry_count < XRT_DP_REGISTRY_MAX_ENTRIES ? a->entry_count : XRT_DP_REGISTRY_MAX_ENTRIES)
	              : 0;
	const uint32_t nb =
	    b != NULL ? (b->entry_count < XRT_DP_REGISTRY_MAX_ENTRIES ? b->entry_count : XRT_DP_REGISTRY_MAX_ENTRIES)
	              : 0;
	if (na != nb) {
		return true;
	}
	for (uint32_t i = 0; i < na; i++) {
		if (!entry_same(&a->entries[i], &b->entries[i])) {
			return true;
		}
	}
	return false;
}

uint32_t
comp_weave_screens_owners(const struct comp_segment_table *t,
                          const struct xrt_screen_list *list,
                          struct comp_weave_segment_owner *out,
                          uint32_t cap)
{
	if (t == NULL || out == NULL) {
		return 0;
	}
	uint32_t n = 0;
	for (uint32_t k = 0; k < t->count && n < cap; k++) {
		const struct comp_segment *g = &t->seg[k];
		struct comp_weave_segment_owner *o = &out[n++];
		memset(o, 0, sizeof(*o));
		o->screen_id = g->screen_id;
		o->session_dp = g->is_primary;
		o->present_origin_x = g->present_origin_x;
		o->present_origin_y = g->present_origin_y;
		o->window_rect = g->window_rect;
		o->one_to_one = g->screen_1to1 != COMP_SEG_1TO1_NO;
		const struct xrt_screen *s = xrt_screen_list_find(list, g->screen_id);
		if (s != NULL) {
			snprintf(o->plugin_id, sizeof(o->plugin_id), "%s", s->plugin_id);
		}
	}
	return n;
}

uint64_t
comp_weave_screens_eye_screen(const struct comp_segment_table *t, bool *out_is_primary)
{
	if (out_is_primary != NULL) {
		*out_is_primary = false;
	}
	const uint32_t k = comp_segments_majority(t);
	if (t == NULL || k >= t->count) {
		return 0;
	}
	if (out_is_primary != NULL) {
		*out_is_primary = t->seg[k].is_primary;
	}
	return t->seg[k].screen_id;
}
