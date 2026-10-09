// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Per-screen segment display processors for the in-process D3D11
 *         compositor (multi-screen M6, ADR-047 D2 on Windows).
 * @ingroup comp_d3d11
 *
 * Mirrors vk_native/comp_vk_native_segments.c: the screen-table join, the
 * lifecycle with hysteresis, the decide loop and the two-pass weave order are
 * the same by construction; only the GPU work (crop copies, the clear, the
 * flat-2D blit) is D3D11. There is no deferred-release list: the immediate
 * context executes in order and D3D11 objects are refcounted, so a retired DP
 * or texture can be released the moment the weave that used it has been
 * recorded.
 */

#include "comp_d3d11_segments.h"

#include "comp_d3d11_outcomp.h"
#include "d3d/d3d_dxgi_formats.h"
#include "os/os_threading.h"
#include "os/os_time.h"
#include "util/u_logging.h"
#include "util/u_misc.h"
#include "xrt/xrt_plugin.h"

#include <d3d11.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 *
 * Internal types.
 *
 */

//! One per-screen crop texture: the atlas tiles cut down to one segment.
struct seg_crop
{
	ID3D11Texture2D *texture;
	ID3D11ShaderResourceView *srv;
	uint32_t w, h;
	DXGI_FORMAT format;
};

//! Per-screen runtime state (indexed like comp_d3d11_segments::screens).
struct seg_screen_state
{
	struct xrt_display_processor_d3d11 *dp; //!< NULL until the lifecycle creates it.
	bool configured;                        //!< set_background_2d(NULL) sent once.
	int encoding_latched;                   //!< last atlas encoding declared, -1 = none.
	int transparent_latched;                //!< last transparency declared, -1 = none.
	int mode_sent;                          //!< last 2D/3D mode sent, -1 = none.
	bool tolerates_resample;
	bool has_hwnd; //!< this DP holds the session's window (the owner).
	struct seg_crop crop;
};

struct comp_d3d11_segments
{
	ID3D11Device *device;

	bool enabled;
	uint32_t screen_count;
	struct comp_segments_screen screens[COMP_SEGMENTS_MAX_SCREENS];
	struct xrt_screen_binding bindings[COMP_SEGMENTS_MAX_SCREENS];
	struct xrt_screen_info info[COMP_SEGMENTS_MAX_SCREENS]; //!< per-screen size + nominal viewer (M3)
	const struct xrt_plugin_iface *iface[COMP_SEGMENTS_MAX_SCREENS];
	struct xrt_plugin_instance *inst[COMP_SEGMENTS_MAX_SCREENS];
	struct seg_screen_state st[COMP_SEGMENTS_MAX_SCREENS];

	struct comp_segment_table table;
	struct comp_segments_lifecycle lc;

	/*
	 * Who holds the window handle (ADR-047 Amendment 2). Off the primary,
	 * the owner's DP lives in st[owner_index].dp with has_hwnd set, and the
	 * session's primary DP is a windowless per-screen DP the compositor
	 * swapped in through hooks.swap_primary.
	 */
	struct comp_d3d11_segments_hwnd_hooks hooks;
	struct comp_segments_owner owner;
	uint32_t primary_index;   //!< index of the primary screen, UINT32_MAX = none
	int owner_index;          //!< index of the screen holding the handle, -1 = none
	bool primary_for_screen;  //!< the primary's plug-in has create_dp_d3d11_for_screen
	bool primary_windowless;  //!< the session's primary DP is windowless right now
	ID3D11DeviceContext *ctx; //!< last update's context (the hand-back at destroy)

	//! The app thread reads st[i].dp for eyes; the weave creates/destroys it.
	struct os_mutex dp_mutex;

	bool mode_3d;
	uint32_t mode_index;

	// Change-only logging.
	bool have_logged_table;
	bool logged_split;
	struct comp_segment_table logged_table;
};


/*
 *
 * Helpers.
 *
 */

static int
screen_index_of(const struct comp_d3d11_segments *segs, uint64_t screen_id)
{
	for (uint32_t i = 0; i < segs->screen_count; i++) {
		if (segs->screens[i].id == screen_id) {
			return (int)i;
		}
	}
	return -1;
}

static void
crop_destroy(struct seg_crop *c)
{
	if (c->srv != nullptr) {
		c->srv->Release();
		c->srv = nullptr;
	}
	if (c->texture != nullptr) {
		c->texture->Release();
		c->texture = nullptr;
	}
	c->w = c->h = 0;
}

/*!
 * (Re)create the crop texture at @p w × @p h in the atlas's format. The SRV is
 * typed UNORM (the in-process DP samples raw bytes, ADR-021 Model A), which is
 * the same choice the compositor's own DP-input crop makes.
 */
static bool
crop_ensure(ID3D11Device *dev, struct seg_crop *c, uint32_t w, uint32_t h, DXGI_FORMAT format)
{
	if (c->texture != nullptr && c->w == w && c->h == h && c->format == format) {
		return true;
	}
	crop_destroy(c);
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = w;
	desc.Height = h;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	HRESULT hr = dev->CreateTexture2D(&desc, nullptr, &c->texture);
	if (FAILED(hr)) {
		U_LOG_E("segments: crop texture %ux%u failed: 0x%lx", w, h, hr);
		return false;
	}
	D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
	sd.Format = d3d_dxgi_format_to_unorm_sample(format);
	sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	sd.Texture2D.MostDetailedMip = 0;
	sd.Texture2D.MipLevels = 1;
	hr = dev->CreateShaderResourceView(c->texture, &sd, &c->srv);
	if (FAILED(hr)) {
		U_LOG_E("segments: crop SRV failed: 0x%lx", hr);
		crop_destroy(c);
		return false;
	}
	c->w = w;
	c->h = h;
	c->format = format;
	return true;
}

static void
dp_release(struct comp_d3d11_segments *segs, uint32_t i)
{
	struct seg_screen_state *st = &segs->st[i];
	if (st->dp != nullptr) {
		U_LOG_I("segments: retiring the segment DP for screen 0x%016llx ('%s')",
		        (unsigned long long)segs->screens[i].id, segs->bindings[i].device_name);
		os_mutex_lock(&segs->dp_mutex);
		struct xrt_display_processor_d3d11 *dp = st->dp;
		st->dp = nullptr;
		os_mutex_unlock(&segs->dp_mutex);
		xrt_display_processor_d3d11_destroy(&dp);
	}
	st->configured = false;
	st->encoding_latched = -1;
	st->transparent_latched = -1;
	st->mode_sent = -1;
	st->tolerates_resample = false;
	st->has_hwnd = false;
	crop_destroy(&st->crop);
}

/*!
 * Make (not install) a DP for screen @p i through the plug-in's per-screen
 * factory. @p window is NULL for a windowless segment DP (its phase comes from
 * set_present_origin) and the session's real HWND only for the screen that
 * owns it — the screen holding the majority of the window, whose DP keeps the
 * vendor's drag phase-snap (ADR-047 Amendment 2).
 *
 * No fallback to the plain `create_dp_d3d11`, for the reason given on
 * xrt_plugin_iface::create_dp_vk_for_screen: that DP describes the plug-in's
 * process-wide panel and is not bound by the canvas / not-first-writer
 * contract, so it could wipe the segment woven before it.
 */
static struct xrt_display_processor_d3d11 *
dp_make(struct comp_d3d11_segments *segs, uint32_t i, ID3D11DeviceContext *ctx, void *window)
{
	const struct xrt_plugin_iface *iface = segs->iface[i];
	const bool can = segs->screens[i].is_primary ? segs->primary_for_screen : segs->screens[i].has_dp_factory;
	xrt_result_t xret = XRT_ERROR_DEVICE_CREATION_FAILED;
	struct xrt_display_processor_d3d11 *dp = nullptr;
	if (can && xrt_plugin_iface_has_create_dp_d3d11_for_screen(iface)) {
		xret = iface->create_dp_d3d11_for_screen(segs->inst[i], segs->device, ctx, window, &segs->bindings[i],
		                                         &dp);
	}
	if (xret != XRT_SUCCESS || dp == nullptr || dp->process_atlas == nullptr) {
		if (dp != nullptr) {
			xrt_display_processor_d3d11_destroy(&dp);
		}
		U_LOG_W(
		    "segments: could not create a %s DP for screen 0x%016llx ('%s', plug-in '%s') via "
		    "create_dp_d3d11_for_screen (%d)",
		    window != nullptr ? "window-holding" : "windowless", (unsigned long long)segs->screens[i].id,
		    segs->bindings[i].device_name, segs->screens[i].plugin_id, (int)xret);
		return nullptr;
	}
	return dp;
}

/*!
 * Install @p dp (may be NULL) as screen @p i's segment DP, latches reset so
 * the next weave configures it. Returns the DP it replaced, for the caller to
 * destroy (once this returns, get_eyes can no longer reach it).
 */
static struct xrt_display_processor_d3d11 *
dp_install(struct comp_d3d11_segments *segs, uint32_t i, struct xrt_display_processor_d3d11 *dp, bool has_hwnd)
{
	struct seg_screen_state *st = &segs->st[i];
	os_mutex_lock(&segs->dp_mutex);
	struct xrt_display_processor_d3d11 *old = st->dp;
	st->dp = dp;
	os_mutex_unlock(&segs->dp_mutex);
	st->tolerates_resample = dp != nullptr && xrt_display_processor_d3d11_tolerates_resample(dp);
	st->has_hwnd = dp != nullptr && has_hwnd;
	st->configured = false;
	st->encoding_latched = -1;
	st->transparent_latched = -1;
	st->mode_sent = -1;
	if (dp == nullptr) {
		crop_destroy(&st->crop);
	}
	return old;
}

/*!
 * The lifecycle's CREATE: a windowless segment DP for screen @p i. A screen
 * that already has a DP (it holds the window handle) keeps it.
 */
static bool
dp_create(struct comp_d3d11_segments *segs, uint32_t i, ID3D11DeviceContext *ctx)
{
	if (segs->st[i].dp != nullptr) {
		return true;
	}
	struct xrt_display_processor_d3d11 *dp = dp_make(segs, i, ctx, nullptr);
	if (dp == nullptr) {
		U_LOG_W("segments: screen 0x%016llx's segment stays flat 2D", (unsigned long long)segs->screens[i].id);
		return false;
	}
	(void)dp_install(segs, i, dp, false);
	// Lifecycle event (hysteresis-gated), not per frame.
	U_LOG_W(
	    "segments: created a segment DP for screen 0x%016llx ('%s', plug-in '%s') via "
	    "create_dp_d3d11_for_screen — %s",
	    (unsigned long long)segs->screens[i].id, segs->bindings[i].device_name, segs->screens[i].plugin_id,
	    segs->st[i].tolerates_resample ? "tolerates a resample" : "needs 1:1 pixels");
	return true;
}

static bool
table_has_screen(const struct comp_segment_table *t, uint64_t screen_id)
{
	for (uint32_t k = 0; k < t->count; k++) {
		if (t->seg[k].screen_id == screen_id && t->seg[k].window_rect.w > 0 && t->seg[k].window_rect.h > 0) {
			return true;
		}
	}
	return false;
}

static const char *
screen_name(const struct comp_d3d11_segments *segs, int i)
{
	return i >= 0 ? segs->bindings[i].device_name : "(none)";
}

/*!
 * Put @p dp in screen @p i's slot — as the DP holding the window handle
 * (@p has_hwnd) or as a windowless one — and destroy what it replaces. The
 * primary's slot is the session's own DP, swapped by the compositor.
 */
static void
slot_replace(struct comp_d3d11_segments *segs, uint32_t i, struct xrt_display_processor_d3d11 *dp, bool has_hwnd)
{
	struct xrt_display_processor_d3d11 *old = nullptr;
	if (i == segs->primary_index) {
		old = segs->hooks.swap_primary(segs->hooks.userdata, dp);
		segs->primary_windowless = !has_hwnd;
	} else {
		old = dp_install(segs, i, dp, has_hwnd);
		if (dp != nullptr) {
			// The lifecycle tracks this screen's DP as live: no second create.
			comp_segments_lifecycle_set_created(&segs->lc, segs->screens[i].id, true);
		}
	}
	if (old != nullptr) {
		xrt_display_processor_d3d11_destroy(&old);
	}
}

/*!
 * Move the window handle from the current owner to screen @p t (ADR-047
 * Amendment 2). One weaver per window, and a live DP's window cannot be
 * re-pointed, so:
 *   1. make a windowless replacement for the old owner (the primary always
 *      needs one; another screen only while the window still covers it) —
 *      failing that, abort with nothing changed;
 *   2. swap it in and destroy the old owner's DP, which frees the window;
 *   3. make the target's DP WITH the window and swap it in — failing that,
 *      roll back (the old owner takes the window again).
 * All of it runs on the weave thread between two weaves (the caller holds the
 * compositor lock), so a frame weaves with the old pair or the new pair and
 * never with a gap.
 */
static void
hwnd_handoff(struct comp_d3d11_segments *segs, uint32_t t, ID3D11DeviceContext *ctx)
{
	const int o = segs->owner_index;
	if (o == (int)t) {
		return;
	}
	const uint64_t t_id = segs->screens[t].id;
	const uint64_t t0 = os_monotonic_get_ns();

	// 1. The old owner's windowless replacement.
	struct xrt_display_processor_d3d11 *w = nullptr;
	const bool o_needs_dp =
	    o >= 0 && ((uint32_t)o == segs->primary_index ||
	               (segs->screens[o].has_dp_factory && table_has_screen(&segs->table, segs->screens[o].id)));
	if (o_needs_dp) {
		w = dp_make(segs, (uint32_t)o, ctx, nullptr);
		if (w == nullptr) {
			U_LOG_W("segments: the window handle stays with '%s' — no windowless DP to replace it with",
			        screen_name(segs, o));
			comp_segments_owner_set_result(&segs->owner, t_id, false, segs->screens[o].id);
			return;
		}
	}

	// 2. Free the window.
	if (segs->hooks.bracket != nullptr) {
		segs->hooks.bracket(segs->hooks.userdata, true, nullptr);
	}
	if (o >= 0) {
		slot_replace(segs, (uint32_t)o, w, false);
	}

	// 3. The target takes the window.
	struct xrt_display_processor_d3d11 *h = dp_make(segs, t, ctx, segs->hooks.hwnd);
	int now_owner = -1;
	if (h != nullptr) {
		slot_replace(segs, t, h, true);
		now_owner = (int)t;
	} else if (o >= 0) {
		// Roll back: the old owner takes it again.
		struct xrt_display_processor_d3d11 *back = dp_make(segs, (uint32_t)o, ctx, segs->hooks.hwnd);
		if (back != nullptr) {
			slot_replace(segs, (uint32_t)o, back, true);
			now_owner = o;
		}
	}
	segs->owner_index = now_owner;
	if (segs->hooks.bracket != nullptr) {
		// The DP the drag snap asks now; NULL = the session's primary DP.
		struct xrt_display_processor_d3d11 *hdp = nullptr;
		if (now_owner >= 0 && (uint32_t)now_owner != segs->primary_index) {
			hdp = segs->st[now_owner].dp;
		}
		segs->hooks.bracket(segs->hooks.userdata, false, hdp);
	}
	comp_segments_owner_set_result(&segs->owner, t_id, h != nullptr,
	                               now_owner >= 0 ? segs->screens[now_owner].id : 0);

	// One line per flip (the flip itself is hysteresis-gated).
	const double ms = (double)(os_monotonic_get_ns() - t0) / 1.0e6;
	if (h != nullptr) {
		U_LOG_W(
		    "segments: window handle handed from '%s' to '%s' (screen 0x%016llx holds the majority) in "
		    "%.1f ms — drag snap follows it; '%s' weaves windowless",
		    screen_name(segs, o), screen_name(segs, (int)t), (unsigned long long)t_id, ms,
		    screen_name(segs, o));
	} else {
		U_LOG_W("segments: window handle hand-off to '%s' FAILED after %.1f ms — %s", screen_name(segs, (int)t),
		        ms, now_owner >= 0 ? "rolled back to the previous owner" : "no DP holds the window now");
	}
}

/*!
 * Hand the window back to the primary screen's DP (a screen-list rebuild
 * while another screen owns it). No-op when the primary owns it.
 */
static void
hwnd_return_to_primary(struct comp_d3d11_segments *segs)
{
	if (segs->hooks.swap_primary == nullptr || segs->primary_index == UINT32_MAX || segs->ctx == nullptr ||
	    (segs->owner_index == (int)segs->primary_index && !segs->primary_windowless)) {
		return;
	}
	hwnd_handoff(segs, segs->primary_index, segs->ctx);
}

static void
set_viewport_scissor(ID3D11DeviceContext *ctx, const struct comp_seg_rect *r)
{
	D3D11_VIEWPORT vp = {};
	vp.TopLeftX = (float)r->x;
	vp.TopLeftY = (float)r->y;
	vp.Width = (float)r->w;
	vp.Height = (float)r->h;
	vp.MaxDepth = 1.0f;
	ctx->RSSetViewports(1, &vp);
	D3D11_RECT sc = {r->x, r->y, r->x + (LONG)r->w, r->y + (LONG)r->h};
	ctx->RSSetScissorRects(1, &sc);
}


/*
 *
 * 'Exported' functions.
 *
 */

extern "C" struct comp_d3d11_segments *
comp_d3d11_segments_create(void *d3d11_device)
{
	if (d3d11_device == nullptr) {
		return nullptr;
	}
	struct comp_d3d11_segments *segs = U_TYPED_CALLOC(struct comp_d3d11_segments);
	if (segs == nullptr) {
		return nullptr;
	}
	segs->device = static_cast<ID3D11Device *>(d3d11_device);
	os_mutex_init(&segs->dp_mutex);
	comp_segments_lifecycle_init(&segs->lc, 0, 0);
	segs->mode_index = UINT32_MAX;
	segs->primary_index = UINT32_MAX;
	segs->owner_index = -1;
	for (uint32_t i = 0; i < COMP_SEGMENTS_MAX_SCREENS; i++) {
		segs->st[i].encoding_latched = -1;
		segs->st[i].transparent_latched = -1;
		segs->st[i].mode_sent = -1;
	}
	return segs;
}

extern "C" void
comp_d3d11_segments_destroy(struct comp_d3d11_segments **segs_ptr)
{
	if (segs_ptr == nullptr || *segs_ptr == nullptr) {
		return;
	}
	struct comp_d3d11_segments *segs = *segs_ptr;
	// Another screen holds the window: hand it back to the session's primary
	// DP first (no hooks = compositor teardown, where the primary DP goes too).
	// An empty table: the old owner needs no windowless replacement.
	segs->table.count = 0;
	hwnd_return_to_primary(segs);
	for (uint32_t i = 0; i < COMP_SEGMENTS_MAX_SCREENS; i++) {
		dp_release(segs, i);
	}
	os_mutex_destroy(&segs->dp_mutex);
	free(segs);
	*segs_ptr = nullptr;
}

extern "C" void
comp_d3d11_segments_set_screens(struct comp_d3d11_segments *segs,
                                const struct xrt_screen_list *list,
                                const struct xrt_system_compositor_info *info,
                                uint64_t pinned_display_id)
{
	if (segs == nullptr) {
		return;
	}
	segs->table.count = 0;
	hwnd_return_to_primary(segs);
	for (uint32_t i = 0; i < COMP_SEGMENTS_MAX_SCREENS; i++) {
		dp_release(segs, i);
	}
	comp_segments_lifecycle_init(&segs->lc, 0, 0);
	segs->primary_index = UINT32_MAX;
	segs->owner_index = -1;
	segs->primary_for_screen = false;
	segs->enabled = false;
	segs->screen_count = 0;
	segs->have_logged_table = false;
	segs->logged_split = false;
	memset(&segs->table, 0, sizeof(segs->table));

	const char *env = getenv("DXR_SEGMENTS");
	if (env != nullptr && env[0] == '0') {
		U_LOG_W("segments: DXR_SEGMENTS=0 — a window spanning screens keeps the single-DP path");
		return;
	}
	if (list == nullptr || info == nullptr || list->count < 2) {
		return;
	}
	if (pinned_display_id != 0) {
		U_LOG_I("segments: session pinned to display 0x%016llx (XrSessionDisplayBindingDXR) — no segmentation",
		        (unsigned long long)pinned_display_id);
		return;
	}

	// The primary: the system-default screen, woven by the session's own DP.
	const struct xrt_dp_registry_entry *primary_entry = nullptr;
	for (uint32_t i = 0; i < list->count && i < XRT_SCREEN_LIST_MAX; i++) {
		if ((list->screens[i].flags & XRT_SCREEN_FLAG_SYSTEM_DEFAULT) == 0) {
			continue;
		}
		for (uint32_t e = 0; e < info->dp_registry.entry_count; e++) {
			if (info->dp_registry.entries[e].monitor_id == list->screens[i].id) {
				primary_entry = &info->dp_registry.entries[e];
				break;
			}
		}
	}
	if (primary_entry == nullptr) {
		U_LOG_I("segments: the DP registry does not know the system-default screen — no segmentation");
		return;
	}

	uint32_t n = 0;
	bool mixed = false;
	char vendors[256] = {0};
	for (uint32_t i = 0; i < list->count && i < XRT_SCREEN_LIST_MAX && n < COMP_SEGMENTS_MAX_SCREENS; i++) {
		const struct xrt_screen *s = &list->screens[i];
		if (s->desktop_width == 0 || s->desktop_height == 0) {
			continue;
		}
		const struct xrt_dp_registry_entry *e = nullptr;
		for (uint32_t k = 0; k < info->dp_registry.entry_count; k++) {
			if (info->dp_registry.entries[k].monitor_id == s->id) {
				e = &info->dp_registry.entries[k];
				break;
			}
		}

		struct comp_segments_screen *cs = &segs->screens[n];
		memset(cs, 0, sizeof(*cs));
		cs->id = s->id;
		cs->desktop.x = s->desktop_left;
		cs->desktop.y = s->desktop_top;
		cs->desktop.w = s->desktop_width;
		cs->desktop.h = s->desktop_height;
		cs->native_w = s->native_width;
		cs->native_h = s->native_height;
		cs->is_primary = (e == primary_entry);
		snprintf(cs->plugin_id, sizeof(cs->plugin_id), "%s", s->plugin_id);

		segs->iface[n] = nullptr;
		segs->inst[n] = nullptr;
		if (cs->is_primary) {
			// The primary's own plug-in, for the window-handle hand-off: a
			// windowless primary DP while another screen holds the window.
			segs->iface[n] = (const struct xrt_plugin_iface *)e->owning_iface;
			segs->inst[n] = (struct xrt_plugin_instance *)e->owning_instance;
			segs->primary_for_screen = e->dp_factory_d3d11 != nullptr &&
			                           xrt_plugin_iface_has_create_dp_d3d11_for_screen(segs->iface[n]);
			segs->primary_index = n;
		} else if (e != nullptr) {
			// A segment's DP comes from ITS screen's registry entry,
			// whichever plug-in that is (mixed-vendor rigs, M4).
			if (strcmp(e->plugin_id, primary_entry->plugin_id) != 0) {
				mixed = true;
				if (strstr(vendors, e->plugin_id) == nullptr) {
					size_t used = strlen(vendors);
					snprintf(vendors + used, sizeof(vendors) - used, "%s%s", used > 0 ? ", " : "",
					         e->plugin_id);
				}
			}
			segs->iface[n] = (const struct xrt_plugin_iface *)e->owning_iface;
			segs->inst[n] = (struct xrt_plugin_instance *)e->owning_instance;
			// The per-screen slot, and only it (see dp_create). The registry's
			// D3D11 factory being set says the claim covers D3D11.
			cs->has_dp_factory = e->dp_factory_d3d11 != nullptr &&
			                     xrt_plugin_iface_has_create_dp_d3d11_for_screen(segs->iface[n]);
		}

		struct xrt_screen_binding *b = &segs->bindings[n];
		memset(b, 0, sizeof(*b));
		b->struct_size = (uint32_t)sizeof(*b);
		b->monitor_id = s->id;
		b->desktop_left = s->desktop_left;
		b->desktop_top = s->desktop_top;
		b->desktop_width = s->desktop_width;
		b->desktop_height = s->desktop_height;
		b->native_pixel_width = s->native_width;
		b->native_pixel_height = s->native_height;
		b->physical_width_mm = s->physical_width_mm;
		b->physical_height_mm = s->physical_height_mm;
		b->desktop_scale = s->desktop_scale;
		snprintf(b->device_name, sizeof(b->device_name), "%s", s->device_name);
		if (e != nullptr) {
			snprintf(b->serial, sizeof(b->serial), "%s", e->serial);
		}
		segs->info[n] = s->info;
		n++;
	}
	segs->screen_count = n;

	if (mixed) {
		U_LOG_I(
		    "segments: mixed-vendor screen table — primary '%s', other screens by '%s'; each segment DP "
		    "comes from its own screen's plug-in",
		    primary_entry->plugin_id, vendors);
	}
	segs->enabled = n >= 2;
	if (segs->primary_index == UINT32_MAX) {
		segs->primary_for_screen = false; // no primary segment: nothing to hand off from
	}
	if (segs->enabled) {
		if (segs->primary_index != UINT32_MAX) {
			segs->owner_index = (int)segs->primary_index;
			comp_segments_owner_init(&segs->owner, segs->screens[segs->primary_index].id, 0, 0);
		}
		const bool handoff = segs->hooks.swap_primary != nullptr && segs->primary_for_screen;
		U_LOG_W(
		    "segments: %u screens, plug-in '%s' — a window spanning monitors is woven per monitor "
		    "(multi-screen M6, D3D11); the window handle %s",
		    n, primary_entry->plugin_id,
		    handoff ? "follows the majority screen"
		            : (segs->hooks.swap_primary == nullptr
		                   ? "stays with the primary DP (no window to hand off)"
		                   : "stays with the primary DP (its plug-in has no create_dp_d3d11_for_screen)"));
	}
}

extern "C" void
comp_d3d11_segments_set_hwnd_hooks(struct comp_d3d11_segments *segs, const struct comp_d3d11_segments_hwnd_hooks *hooks)
{
	if (segs == nullptr) {
		return;
	}
	if (hooks == nullptr || hooks->hwnd == nullptr || hooks->swap_primary == nullptr) {
		memset(&segs->hooks, 0, sizeof(segs->hooks));
		return;
	}
	segs->hooks = *hooks;
}

extern "C" bool
comp_d3d11_segments_enabled(const struct comp_d3d11_segments *segs)
{
	return segs != nullptr && segs->enabled;
}

extern "C" bool
comp_d3d11_segments_update(struct comp_d3d11_segments *segs,
                           const struct comp_seg_rect *window_desktop,
                           const struct comp_seg_rect *canvas,
                           void *d3d11_context,
                           uint32_t mode_index)
{
	if (segs == nullptr || !segs->enabled || window_desktop == nullptr || d3d11_context == nullptr) {
		return false;
	}
	auto *ctx = static_cast<ID3D11DeviceContext *>(d3d11_context);

	comp_segments_compute(window_desktop, canvas, segs->screens, segs->screen_count, &segs->table);

	struct comp_segments_actions act;
	comp_segments_lifecycle_update(&segs->lc, &segs->table, &act);
	for (uint32_t a = 0; a < act.count; a++) {
		const int i = screen_index_of(segs, act.items[a].screen_id);
		if (i < 0) {
			continue;
		}
		if (act.items[a].action == COMP_SEG_ACTION_CREATE) {
			const bool ok = dp_create(segs, (uint32_t)i, ctx);
			comp_segments_lifecycle_set_created(&segs->lc, act.items[a].screen_id, ok);
		} else if (act.items[a].action == COMP_SEG_ACTION_DESTROY && !segs->st[i].has_hwnd) {
			// The DP holding the window outlives its segment: it moves
			// only by a hand-off (below), never by a retire.
			dp_release(segs, (uint32_t)i);
		}
	}

	/*
	 * ADR-047 Amendment 2: the window handle follows the majority screen,
	 * hysteresis in comp_segments_owner (a clear margin held for 0.5 s).
	 */
	segs->ctx = ctx;
	if (segs->hooks.swap_primary != nullptr && segs->primary_for_screen) {
		const uint64_t target = comp_segments_owner_update(&segs->owner, &segs->table, os_monotonic_get_ns());
		const int ti = target != 0 ? screen_index_of(segs, target) : -1;
		if (ti >= 0) {
			hwnd_handoff(segs, (uint32_t)ti, ctx);
		}
	}

	const uint32_t cw = canvas != nullptr && canvas->w > 0 ? canvas->w : window_desktop->w;
	const uint32_t ch = canvas != nullptr && canvas->h > 0 ? canvas->h : window_desktop->h;
	const bool split = comp_segments_table_is_split(&segs->table, cw, ch);

	// A DP's resample tolerance can follow the rendering mode (sim_display:
	// INTERLACED needs 1:1). Re-ask on a mode change or a table change —
	// never per frame.
	const bool table_changed =
	    split && (!segs->have_logged_table || !comp_segments_table_equal(&segs->table, &segs->logged_table));
	if (mode_index != segs->mode_index || table_changed) {
		segs->mode_index = mode_index;
		for (uint32_t i = 0; i < segs->screen_count; i++) {
			if (segs->st[i].dp != nullptr) {
				segs->st[i].tolerates_resample =
				    xrt_display_processor_d3d11_tolerates_resample(segs->st[i].dp);
			}
		}
	}

	// One INFO line per table change while split, and one on leaving it.
	if (table_changed) {
		char buf[768];
		comp_segments_table_format(&segs->table, buf, sizeof(buf));
		U_LOG_I("segments: window %d,%d %ux%u -> %s", window_desktop->x, window_desktop->y, window_desktop->w,
		        window_desktop->h, buf);
		segs->logged_table = segs->table;
		segs->have_logged_table = true;
	} else if (!split && segs->logged_split) {
		U_LOG_I("segments: window %d,%d %ux%u is on the primary screen only — single-DP path",
		        window_desktop->x, window_desktop->y, window_desktop->w, window_desktop->h);
		segs->have_logged_table = false;
	}
	segs->logged_split = split;
	// A windowless primary DP needs its present origin, which only the split
	// path sends: keep it while another screen (or nobody) holds the window,
	// even with the window back on the primary alone (until the hand-back).
	return split || segs->primary_windowless;
}

extern "C" bool
comp_d3d11_segments_record(struct comp_d3d11_segments *segs, const struct comp_d3d11_segments_frame *f)
{
	if (segs == nullptr || f == nullptr || f->context == nullptr || f->src_srv == nullptr ||
	    f->target_rtv == nullptr || f->tile_columns == 0 || f->tile_rows == 0 || f->view_width == 0 ||
	    f->view_height == 0) {
		return false;
	}
	auto *ctx = static_cast<ID3D11DeviceContext *>(f->context);
	auto *src_srv = static_cast<ID3D11ShaderResourceView *>(f->src_srv);
	auto *rtv = static_cast<ID3D11RenderTargetView *>(f->target_rtv);
	const struct comp_segment_table *t = &segs->table;

	// The atlas texture behind the SRV, for the crop copies.
	ID3D11Resource *src_res = nullptr;
	src_srv->GetResource(&src_res);
	if (src_res == nullptr) {
		return false;
	}
	auto *src_tex = static_cast<ID3D11Texture2D *>(src_res);
	D3D11_TEXTURE2D_DESC src_desc = {};
	src_tex->GetDesc(&src_desc);

	/*
	 * 1. Decide each segment: which DP (if any) and whether it may weave.
	 */
	struct xrt_display_processor_d3d11 *dp_of[COMP_SEGMENTS_MAX] = {};
	struct seg_screen_state *st_of[COMP_SEGMENTS_MAX] = {};
	bool weave[COMP_SEGMENTS_MAX] = {};
	struct comp_seg_rect tile[COMP_SEGMENTS_MAX] = {};
	bool have_tile[COMP_SEGMENTS_MAX] = {};

	for (uint32_t k = 0; k < t->count; k++) {
		const struct comp_segment *g = &t->seg[k];
		have_tile[k] =
		    comp_segments_tile_rect(&g->window_rect, &f->canvas, f->view_width, f->view_height, &tile[k]);
		const int i = (int)g->screen_index;
		st_of[k] = (i >= 0 && (uint32_t)i < segs->screen_count) ? &segs->st[i] : nullptr;
		if (g->is_primary) {
			dp_of[k] = f->primary_dp;
			weave[k] = f->primary_dp != nullptr;
		} else if (st_of[k] != nullptr) {
			dp_of[k] = st_of[k]->dp;
			weave[k] = comp_segments_decide(dp_of[k] != nullptr, st_of[k]->tolerates_resample,
			                                g->screen_1to1) == COMP_SEG_RENDER_WEAVE;
		}
		if (!have_tile[k] || st_of[k] == nullptr) {
			weave[k] = false;
		}
	}

	/*
	 * 2. Clear the target: a segment DP confines its draw to its canvas, so
	 * whatever no segment covers would otherwise be undefined.
	 */
	{
		const float clear[4] = {0.0f, 0.0f, 0.0f, f->transparent_background ? 0.0f : 1.0f};
		ctx->ClearRenderTargetView(rtv, clear);
	}

	/*
	 * 3. Crop each woven segment's views out of the atlas — crop before the
	 * DP is the law (ADR-030): a DP's atlas holds exactly its canvas.
	 */
	for (uint32_t k = 0; k < t->count; k++) {
		if (!weave[k]) {
			continue;
		}
		struct seg_crop *crop = &st_of[k]->crop;
		const uint32_t cw = tile[k].w * f->tile_columns;
		const uint32_t ch = tile[k].h * f->tile_rows;
		if (!crop_ensure(segs->device, crop, cw, ch, src_desc.Format)) {
			weave[k] = false; // no input image: flat 2D instead
			continue;
		}
		for (uint32_t row = 0; row < f->tile_rows; row++) {
			for (uint32_t col = 0; col < f->tile_columns; col++) {
				D3D11_BOX box = {};
				box.left = col * f->view_width + (UINT)tile[k].x;
				box.top = row * f->view_height + (UINT)tile[k].y;
				box.right = box.left + tile[k].w;
				box.bottom = box.top + tile[k].h;
				box.front = 0;
				box.back = 1;
				ctx->CopySubresourceRegion(crop->texture, 0, col * tile[k].w, row * tile[k].h, 0,
				                           src_tex, 0, &box);
			}
		}
	}

	/*
	 * 4. Weave: the primary first (a DP built before M6 may still clear the
	 * whole target, so it must not run after a sibling), then left to right.
	 * Each gets canvas = its segment, with viewport AND scissor already set to
	 * it (the vendor weaver reads the current viewport/scissor and writes only
	 * inside them); the back buffer is re-bound before every DP because a DP
	 * may leave another target bound.
	 */
	bool any_woven = false;
	for (uint32_t pass = 0; pass < 2; pass++) {
		for (uint32_t k = 0; k < t->count; k++) {
			const struct comp_segment *g = &t->seg[k];
			if (!weave[k] || g->is_primary != (pass == 0)) {
				continue;
			}
			struct xrt_display_processor_d3d11 *dp = dp_of[k];
			struct seg_screen_state *st = st_of[k];
			if (g->is_primary) {
				// The session feeds its own DP everything else; windowless
				// (another screen holds the window), it needs its phase here.
				if (segs->primary_windowless && g->screen_1to1 != COMP_SEG_1TO1_NO) {
					xrt_display_processor_d3d11_set_present_origin(dp, g->present_origin_x,
					                                               g->present_origin_y);
				}
			} else {
				if (!st->configured) {
					st->configured = true;
					xrt_display_processor_d3d11_set_background_2d(dp, nullptr, 0, 0);
				}
				if (f->atlas_encoding >= 0 && st->encoding_latched != f->atlas_encoding) {
					st->encoding_latched = f->atlas_encoding;
					xrt_display_processor_d3d11_set_atlas_encoding(
					    dp, (enum xrt_atlas_encoding)f->atlas_encoding);
				}
				if (st->transparent_latched != (int)f->transparent_background) {
					st->transparent_latched = (int)f->transparent_background;
					xrt_display_processor_d3d11_set_transparent_background(
					    dp, f->transparent_background, false);
				}
				if (st->mode_sent != (int)segs->mode_3d) {
					st->mode_sent = (int)segs->mode_3d;
					xrt_display_processor_d3d11_request_display_mode(dp, segs->mode_3d);
				}
				// Phase only where window px ARE device px; a resampled
				// screen's origin is in the wrong units (and only a
				// resample-tolerant DP weaves there, which has no phase).
				// The DP holding the window phases from it, like the
				// primary always has.
				if (!st->has_hwnd && g->screen_1to1 != COMP_SEG_1TO1_NO) {
					xrt_display_processor_d3d11_set_present_origin(dp, g->present_origin_x,
					                                               g->present_origin_y);
				}
			}
			ctx->OMSetRenderTargets(1, &rtv, nullptr);
			set_viewport_scissor(ctx, &g->window_rect);
			xrt_display_processor_d3d11_process_atlas(dp, ctx, st->crop.srv, tile[k].w, tile[k].h,
			                                          f->tile_columns, f->tile_rows, f->src_format,
			                                          f->target_width, f->target_height, g->window_rect.x,
			                                          g->window_rect.y, g->window_rect.w, g->window_rect.h);
			any_woven = true;
		}
	}

	/*
	 * 5. Flat 2D for every segment that could not be woven, and for the
	 * canvas no segment covers (a monitor no plug-in claimed, or off every
	 * screen): one view (the middle one — the left eye of a stereo pair),
	 * linearly scaled from the view tile to the window rect (#1654 look).
	 */
	struct comp_seg_rect flat_dst[COMP_SEGMENTS_MAX + COMP_SEGMENTS_MAX_UNCOVERED];
	uint32_t flat_n = 0;
	for (uint32_t k = 0; k < t->count; k++) {
		if (!weave[k] && have_tile[k]) {
			flat_dst[flat_n++] = t->seg[k].window_rect;
		}
	}
	flat_n += comp_segments_uncovered(t, &f->canvas, &flat_dst[flat_n], COMP_SEGMENTS_MAX_UNCOVERED);
	if (flat_n > 0 && f->outcomp != nullptr) {
		const uint32_t views = f->tile_columns * f->tile_rows;
		const uint32_t vi = views > 0 ? (views - 1) / 2 : 0;
		const int32_t base_x = (int32_t)((vi % f->tile_columns) * f->view_width);
		const int32_t base_y = (int32_t)((vi / f->tile_columns) * f->view_height);
		for (uint32_t k = 0; k < flat_n; k++) {
			const struct comp_seg_rect *d = &flat_dst[k];
			struct comp_seg_rect s;
			if (!comp_segments_tile_rect(d, &f->canvas, f->view_width, f->view_height, &s)) {
				continue; // thinner than one source pixel
			}
			comp_d3d11_outcomp_blit_rect(f->outcomp, src_srv, base_x + s.x, base_y + s.y, s.w, s.h, rtv,
			                             d->x, d->y, d->w, d->h);
		}
	}

	// Leave the back buffer bound, viewport = whole target, like the
	// single-DP path does after process_atlas.
	ctx->OMSetRenderTargets(1, &rtv, nullptr);
	{
		D3D11_VIEWPORT vp = {};
		vp.Width = (float)f->target_width;
		vp.Height = (float)f->target_height;
		vp.MaxDepth = 1.0f;
		ctx->RSSetViewports(1, &vp);
		D3D11_RECT sc = {0, 0, (LONG)f->target_width, (LONG)f->target_height};
		ctx->RSSetScissorRects(1, &sc);
	}
	src_res->Release();
	return any_woven;
}

extern "C" void
comp_d3d11_segments_set_display_mode(struct comp_d3d11_segments *segs, bool enable_3d)
{
	if (segs == nullptr) {
		return;
	}
	segs->mode_3d = enable_3d;
	for (uint32_t i = 0; i < segs->screen_count; i++) {
		struct seg_screen_state *st = &segs->st[i];
		if (st->dp != nullptr && st->mode_sent != (int)enable_3d) {
			st->mode_sent = (int)enable_3d;
			xrt_display_processor_d3d11_request_display_mode(st->dp, enable_3d);
		}
	}
}

extern "C" bool
comp_d3d11_segments_get_metrics(const struct comp_d3d11_segments *segs,
                                const struct comp_seg_rect *window_desktop,
                                const struct comp_seg_rect *canvas,
                                bool primary_has_dp,
                                struct xrt_segment_metrics *out)
{
	memset(out, 0, sizeof(*out));
	if (segs == nullptr || !segs->enabled || !segs->logged_split || window_desktop == nullptr ||
	    canvas == nullptr) {
		return false;
	}
	const struct comp_segment_table *t = &segs->table;
	if (t->count == 0 || t->count > XRT_MAX_SEGMENTS) {
		// More screens than view sets: the window keeps one view set (M2).
		return false;
	}
	for (uint32_t k = 0; k < t->count; k++) {
		const struct comp_segment *g = &t->seg[k];
		if (g->screen_index >= segs->screen_count) {
			return false;
		}
		const uint32_t i = g->screen_index;
		const struct seg_screen_state *st = &segs->st[i];
		struct xrt_segment_metric *m = &out->seg[k];
		m->screen_id = g->screen_id;
		m->window_rect.offset.w = g->window_rect.x;
		m->window_rect.offset.h = g->window_rect.y;
		m->window_rect.extent.w = (int)g->window_rect.w;
		m->window_rect.extent.h = (int)g->window_rect.h;
		m->screen_rect.offset.w = g->screen_rect.x;
		m->screen_rect.offset.h = g->screen_rect.y;
		m->screen_rect.extent.w = (int)g->screen_rect.w;
		m->screen_rect.extent.h = (int)g->screen_rect.h;
		m->screen_desktop_left = segs->screens[i].desktop.x;
		m->screen_desktop_top = segs->screens[i].desktop.y;
		m->screen_desktop_width = segs->screens[i].desktop.w;
		m->screen_desktop_height = segs->screens[i].desktop.h;
		m->screen_width_m = segs->info[i].width_m;
		m->screen_height_m = segs->info[i].height_m;
		m->nominal_viewer_x_m = segs->info[i].nominal_viewer_x_m;
		m->nominal_viewer_y_m = segs->info[i].nominal_viewer_y_m;
		m->nominal_viewer_z_m = segs->info[i].nominal_viewer_z_m;
		m->is_primary = g->is_primary;
		if (g->is_primary) {
			m->has_dp = primary_has_dp;
			m->tolerates_resample = false; // the primary keeps the session-level gates
			m->woven = primary_has_dp;
		} else {
			m->has_dp = st->dp != nullptr;
			m->tolerates_resample = st->tolerates_resample;
			m->woven = comp_segments_decide(m->has_dp, st->tolerates_resample, g->screen_1to1) ==
			           COMP_SEG_RENDER_WEAVE;
		}
	}
	out->count = t->count;
	out->canvas.offset.w = canvas->x;
	out->canvas.offset.h = canvas->y;
	out->canvas.extent.w = (int)canvas->w;
	out->canvas.extent.h = (int)canvas->h;
	out->window_screen_left = window_desktop->x;
	out->window_screen_top = window_desktop->y;
	out->window_pixel_width = window_desktop->w;
	out->window_pixel_height = window_desktop->h;
	return true;
}

extern "C" uint64_t
comp_d3d11_segments_get_owner(const struct comp_d3d11_segments *segs)
{
	if (segs == nullptr || !segs->enabled || segs->owner_index < 0 ||
	    (uint32_t)segs->owner_index >= segs->screen_count) {
		return 0;
	}
	return segs->screens[segs->owner_index].id;
}

extern "C" bool
comp_d3d11_segments_get_eyes(struct comp_d3d11_segments *segs, uint64_t screen_id, struct xrt_eye_positions *out)
{
	if (segs == nullptr || out == nullptr) {
		return false;
	}
	bool ok = false;
	os_mutex_lock(&segs->dp_mutex);
	const int i = screen_index_of(segs, screen_id);
	if (i >= 0 && segs->st[i].dp != nullptr) {
		ok = xrt_display_processor_d3d11_get_predicted_eye_positions(segs->st[i].dp, out) && out->valid;
	}
	os_mutex_unlock(&segs->dp_mutex);
	return ok;
}
