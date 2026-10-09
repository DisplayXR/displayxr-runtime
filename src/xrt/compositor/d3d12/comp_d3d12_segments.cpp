// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Per-screen segment display processors for the in-process D3D12
 *         compositor (multi-screen M6, ADR-047 D2 on Windows).
 * @ingroup comp_d3d12
 *
 * Mirrors d3d11/comp_d3d11_segments.cpp: the screen-table join, the lifecycle
 * with hysteresis, the decide loop and the two-pass weave order are the same by
 * construction; only the GPU work is D3D12. What D3D12 changes:
 *
 *  - Everything is RECORDED onto the compositor's one weave command list, so a
 *    descriptor cannot be rewritten between two DPs that read it: each screen
 *    owns its own slot in this module's shader-visible SRV heap (plus one slot
 *    for the flat-2D source).
 *  - A command list carries no viewport/scissor a DP could inherit — every DP
 *    sets both from the canvas it is handed (the screen-bound contract on
 *    `create_dp_d3d12_for_screen`). The module still sets them per segment so a
 *    DP that only sets one of the two stays confined.
 *  - Resource states are explicit: the crops and the source go through
 *    COMMON → COPY_* → COMMON, the DP reads its crop in COMMON exactly like the
 *    primary path hands it the atlas (#747: never leave a promoted state behind
 *    a barrier the caller declares from COMMON).
 *  - No deferred-release list: every weave ends in a full queue wait, so the
 *    next weave's update may release whatever the last one used.
 */

#include "comp_d3d12_segments.h"

#include "comp_d3d12_renderer.h"
#include "d3d/d3d_dxgi_formats.h"
#include "os/os_threading.h"
#include "util/u_logging.h"
#include "util/u_misc.h"
#include "xrt/xrt_plugin.h"

#include <d3d12.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 *
 * Internal types.
 *
 */

//! Descriptor slots: one per screen's crop, then one for the flat-2D source.
#define SEG_SRV_SLOT_FLAT COMP_SEGMENTS_MAX_SCREENS
#define SEG_SRV_SLOTS (COMP_SEGMENTS_MAX_SCREENS + 1)

//! One per-screen crop texture: the atlas tiles cut down to one segment.
struct seg_crop
{
	ID3D12Resource *texture; //!< plain 2D, COMMON between frames
	uint32_t w, h;
	DXGI_FORMAT format;
};

//! Per-screen runtime state (indexed like comp_d3d12_segments::screens).
struct seg_screen_state
{
	struct xrt_display_processor_d3d12 *dp; //!< NULL until the lifecycle creates it.
	bool configured;                        //!< set_background_2d(NULL) sent once.
	int transparent_latched;                //!< last transparency declared, -1 = none.
	int mode_sent;                          //!< last 2D/3D mode sent, -1 = none.
	bool tolerates_resample;
	struct seg_crop crop;
};

struct comp_d3d12_segments
{
	ID3D12Device *device;
	ID3D12CommandQueue *queue;

	//! Shader-visible SRV heap, @ref SEG_SRV_SLOTS slots.
	ID3D12DescriptorHeap *srv_heap;
	UINT srv_inc;

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
screen_index_of(const struct comp_d3d12_segments *segs, uint64_t screen_id)
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
	if (c->texture != nullptr) {
		c->texture->Release();
		c->texture = nullptr;
	}
	c->w = c->h = 0;
}

//! (Re)create the crop texture at @p w × @p h in the atlas's format, in COMMON.
static bool
crop_ensure(ID3D12Device *dev, struct seg_crop *c, uint32_t w, uint32_t h, DXGI_FORMAT format)
{
	if (c->texture != nullptr && c->w == w && c->h == h && c->format == format) {
		return true;
	}
	crop_destroy(c);
	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = w;
	desc.Height = h;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.Flags = D3D12_RESOURCE_FLAG_NONE;
	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	HRESULT hr = dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
	                                          nullptr, IID_PPV_ARGS(&c->texture));
	if (FAILED(hr)) {
		U_LOG_E("segments: D3D12 crop texture %ux%u failed: 0x%lx", w, h, hr);
		c->texture = nullptr;
		return false;
	}
	c->texture->SetName(L"DXR.segment_crop"); // #747: debug-layer attribution
	c->w = w;
	c->h = h;
	c->format = format;
	return true;
}

//! Write an SRV for @p res into heap slot @p slot; returns its GPU handle.
static uint64_t
write_srv(struct comp_d3d12_segments *segs, uint32_t slot, ID3D12Resource *res)
{
	const D3D12_RESOURCE_DESC rd = res->GetDesc();
	D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
	// The in-process DP samples raw bytes (ADR-021 Model A), the same choice
	// the compositor's own DP-input SRV makes; a view cannot be typeless.
	sd.Format = d3d_dxgi_format_to_unorm_sample(rd.Format);
	sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	sd.Texture2D.MipLevels = 1;
	sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	D3D12_CPU_DESCRIPTOR_HANDLE cpu = segs->srv_heap->GetCPUDescriptorHandleForHeapStart();
	cpu.ptr += (SIZE_T)segs->srv_inc * slot;
	segs->device->CreateShaderResourceView(res, &sd, cpu);
	D3D12_GPU_DESCRIPTOR_HANDLE gpu = segs->srv_heap->GetGPUDescriptorHandleForHeapStart();
	gpu.ptr += (UINT64)segs->srv_inc * slot;
	return gpu.ptr;
}

static void
transition(ID3D12GraphicsCommandList *cl, ID3D12Resource *res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
	D3D12_RESOURCE_BARRIER b = {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = res;
	b.Transition.StateBefore = from;
	b.Transition.StateAfter = to;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	cl->ResourceBarrier(1, &b);
}

static void
dp_release(struct comp_d3d12_segments *segs, uint32_t i)
{
	struct seg_screen_state *st = &segs->st[i];
	if (st->dp != nullptr) {
		U_LOG_I("segments: retiring the D3D12 segment DP for screen 0x%016llx ('%s')",
		        (unsigned long long)segs->screens[i].id, segs->bindings[i].device_name);
		os_mutex_lock(&segs->dp_mutex);
		struct xrt_display_processor_d3d12 *dp = st->dp;
		st->dp = nullptr;
		os_mutex_unlock(&segs->dp_mutex);
		xrt_display_processor_d3d12_destroy(&dp);
	}
	st->configured = false;
	st->transparent_latched = -1;
	st->mode_sent = -1;
	st->tolerates_resample = false;
	crop_destroy(&st->crop);
}

/*!
 * Create screen @p i's segment DP through the plug-in's per-screen factory.
 * Windowless (NULL window) on purpose: a segment DP's phase comes from
 * set_present_origin; the session's real HWND stays with the primary DP (the
 * vendor's drag phase-snap follows it). No fallback to the plain
 * `create_dp_d3d12`, for the reason given on
 * xrt_plugin_iface::create_dp_vk_for_screen.
 */
static bool
dp_create(struct comp_d3d12_segments *segs, uint32_t i)
{
	struct seg_screen_state *st = &segs->st[i];
	const struct xrt_plugin_iface *iface = segs->iface[i];
	xrt_result_t xret = XRT_ERROR_DEVICE_CREATION_FAILED;
	struct xrt_display_processor_d3d12 *dp = nullptr;
	if (segs->screens[i].has_dp_factory && xrt_plugin_iface_has_create_dp_d3d12_for_screen(iface)) {
		xret = iface->create_dp_d3d12_for_screen(segs->inst[i], segs->device, segs->queue, nullptr,
		                                         &segs->bindings[i], &dp);
	}
	if (xret != XRT_SUCCESS || dp == nullptr || dp->process_atlas == nullptr) {
		if (dp != nullptr) {
			xrt_display_processor_d3d12_destroy(&dp);
		}
		U_LOG_W(
		    "segments: could not create a DP for screen 0x%016llx ('%s', plug-in '%s') via "
		    "create_dp_d3d12_for_screen (%d) — that segment stays flat 2D",
		    (unsigned long long)segs->screens[i].id, segs->bindings[i].device_name, segs->screens[i].plugin_id,
		    (int)xret);
		return false;
	}
	// Same post-create handshake the compositor gives its primary DP
	// (d3d12_make_dp): the output format (the DXGI back buffer's) and
	// "the runtime presents the full target".
	xrt_display_processor_d3d12_set_output_format(dp, (uint32_t)DXGI_FORMAT_R8G8B8A8_UNORM);
	xrt_display_processor_d3d12_set_shared_texture_present(dp, false);

	os_mutex_lock(&segs->dp_mutex);
	st->dp = dp;
	os_mutex_unlock(&segs->dp_mutex);
	st->tolerates_resample = xrt_display_processor_d3d12_tolerates_resample(dp);
	st->configured = false;
	st->transparent_latched = -1;
	st->mode_sent = -1;
	// Lifecycle event (hysteresis-gated), not per frame.
	U_LOG_W(
	    "segments: created a segment DP for screen 0x%016llx ('%s', plug-in '%s') via "
	    "create_dp_d3d12_for_screen — %s",
	    (unsigned long long)segs->screens[i].id, segs->bindings[i].device_name, segs->screens[i].plugin_id,
	    st->tolerates_resample ? "tolerates a resample" : "needs 1:1 pixels");
	return true;
}

static void
set_viewport_scissor(ID3D12GraphicsCommandList *cl, const struct comp_seg_rect *r)
{
	D3D12_VIEWPORT vp = {};
	vp.TopLeftX = (float)r->x;
	vp.TopLeftY = (float)r->y;
	vp.Width = (float)r->w;
	vp.Height = (float)r->h;
	vp.MaxDepth = 1.0f;
	cl->RSSetViewports(1, &vp);
	D3D12_RECT sc = {r->x, r->y, r->x + (LONG)r->w, r->y + (LONG)r->h};
	cl->RSSetScissorRects(1, &sc);
}


/*
 *
 * 'Exported' functions.
 *
 */

extern "C" struct comp_d3d12_segments *
comp_d3d12_segments_create(void *d3d12_device, void *d3d12_queue)
{
	if (d3d12_device == nullptr || d3d12_queue == nullptr) {
		return nullptr;
	}
	struct comp_d3d12_segments *segs = U_TYPED_CALLOC(struct comp_d3d12_segments);
	if (segs == nullptr) {
		return nullptr;
	}
	segs->device = static_cast<ID3D12Device *>(d3d12_device);
	segs->queue = static_cast<ID3D12CommandQueue *>(d3d12_queue);

	D3D12_DESCRIPTOR_HEAP_DESC hd = {};
	hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	hd.NumDescriptors = SEG_SRV_SLOTS;
	hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	HRESULT hr = segs->device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&segs->srv_heap));
	if (FAILED(hr)) {
		U_LOG_E("segments: D3D12 SRV heap failed: 0x%lx", hr);
		free(segs);
		return nullptr;
	}
	segs->srv_inc = segs->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	os_mutex_init(&segs->dp_mutex);
	comp_segments_lifecycle_init(&segs->lc, 0, 0);
	segs->mode_index = UINT32_MAX;
	for (uint32_t i = 0; i < COMP_SEGMENTS_MAX_SCREENS; i++) {
		segs->st[i].transparent_latched = -1;
		segs->st[i].mode_sent = -1;
	}
	return segs;
}

extern "C" void
comp_d3d12_segments_destroy(struct comp_d3d12_segments **segs_ptr)
{
	if (segs_ptr == nullptr || *segs_ptr == nullptr) {
		return;
	}
	struct comp_d3d12_segments *segs = *segs_ptr;
	for (uint32_t i = 0; i < COMP_SEGMENTS_MAX_SCREENS; i++) {
		dp_release(segs, i);
	}
	if (segs->srv_heap != nullptr) {
		segs->srv_heap->Release();
		segs->srv_heap = nullptr;
	}
	os_mutex_destroy(&segs->dp_mutex);
	free(segs);
	*segs_ptr = nullptr;
}

extern "C" void
comp_d3d12_segments_set_screens(struct comp_d3d12_segments *segs,
                                const struct xrt_screen_list *list,
                                const struct xrt_system_compositor_info *info,
                                uint64_t pinned_display_id)
{
	if (segs == nullptr) {
		return;
	}
	for (uint32_t i = 0; i < COMP_SEGMENTS_MAX_SCREENS; i++) {
		dp_release(segs, i);
	}
	comp_segments_lifecycle_init(&segs->lc, 0, 0);
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
		if (e != nullptr && !cs->is_primary) {
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
			// D3D12 factory being set says the claim covers D3D12.
			cs->has_dp_factory = e->dp_factory_d3d12 != nullptr &&
			                     xrt_plugin_iface_has_create_dp_d3d12_for_screen(segs->iface[n]);
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
	if (segs->enabled) {
		U_LOG_W(
		    "segments: %u screens, plug-in '%s' — a window spanning monitors is woven per monitor "
		    "(multi-screen M6, D3D12)",
		    n, primary_entry->plugin_id);
	}
}

extern "C" bool
comp_d3d12_segments_enabled(const struct comp_d3d12_segments *segs)
{
	return segs != nullptr && segs->enabled;
}

extern "C" bool
comp_d3d12_segments_update(struct comp_d3d12_segments *segs,
                           const struct comp_seg_rect *window_desktop,
                           const struct comp_seg_rect *canvas,
                           uint32_t mode_index)
{
	if (segs == nullptr || !segs->enabled || window_desktop == nullptr) {
		return false;
	}

	comp_segments_compute(window_desktop, canvas, segs->screens, segs->screen_count, &segs->table);

	struct comp_segments_actions act;
	comp_segments_lifecycle_update(&segs->lc, &segs->table, &act);
	for (uint32_t a = 0; a < act.count; a++) {
		const int i = screen_index_of(segs, act.items[a].screen_id);
		if (i < 0) {
			continue;
		}
		if (act.items[a].action == COMP_SEG_ACTION_CREATE) {
			const bool ok = dp_create(segs, (uint32_t)i);
			comp_segments_lifecycle_set_created(&segs->lc, act.items[a].screen_id, ok);
		} else if (act.items[a].action == COMP_SEG_ACTION_DESTROY) {
			dp_release(segs, (uint32_t)i);
		}
	}

	const uint32_t cw = canvas != nullptr && canvas->w > 0 ? canvas->w : window_desktop->w;
	const uint32_t ch = canvas != nullptr && canvas->h > 0 ? canvas->h : window_desktop->h;
	const bool split = comp_segments_table_is_split(&segs->table, cw, ch);

	// A DP's resample tolerance can follow the rendering mode. Re-ask on a
	// mode change or a table change — never per frame.
	const bool table_changed =
	    split && (!segs->have_logged_table || !comp_segments_table_equal(&segs->table, &segs->logged_table));
	if (mode_index != segs->mode_index || table_changed) {
		segs->mode_index = mode_index;
		for (uint32_t i = 0; i < segs->screen_count; i++) {
			if (segs->st[i].dp != nullptr) {
				segs->st[i].tolerates_resample =
				    xrt_display_processor_d3d12_tolerates_resample(segs->st[i].dp);
			}
		}
	}

	// One INFO line per table change while split, and one on leaving it.
	if (table_changed) {
		char buf[768];
		comp_segments_table_format(&segs->table, buf, sizeof(buf));
		U_LOG_I("segments: D3D12 window %d,%d %ux%u -> %s", window_desktop->x, window_desktop->y,
		        window_desktop->w, window_desktop->h, buf);
		segs->logged_table = segs->table;
		segs->have_logged_table = true;
	} else if (!split && segs->logged_split) {
		U_LOG_I("segments: D3D12 window %d,%d %ux%u is on the primary screen only — single-DP path",
		        window_desktop->x, window_desktop->y, window_desktop->w, window_desktop->h);
		segs->have_logged_table = false;
	}
	segs->logged_split = split;
	return split;
}

extern "C" bool
comp_d3d12_segments_record(struct comp_d3d12_segments *segs, const struct comp_d3d12_segments_frame *f)
{
	if (segs == nullptr || f == nullptr || f->cmd_list == nullptr || f->src_resource == nullptr ||
	    f->target_rtv == 0 || f->tile_columns == 0 || f->tile_rows == 0 || f->view_width == 0 ||
	    f->view_height == 0 || segs->srv_heap == nullptr) {
		return false;
	}
	auto *cl = static_cast<ID3D12GraphicsCommandList *>(f->cmd_list);
	auto *src = static_cast<ID3D12Resource *>(f->src_resource);
	D3D12_CPU_DESCRIPTOR_HANDLE rtv;
	rtv.ptr = (SIZE_T)f->target_rtv;
	const struct comp_segment_table *t = &segs->table;
	const D3D12_RESOURCE_DESC src_desc = src->GetDesc();

	/*
	 * 1. Decide each segment: which DP (if any) and whether it may weave.
	 */
	struct xrt_display_processor_d3d12 *dp_of[COMP_SEGMENTS_MAX] = {};
	struct seg_screen_state *st_of[COMP_SEGMENTS_MAX] = {};
	int screen_of[COMP_SEGMENTS_MAX] = {};
	bool weave[COMP_SEGMENTS_MAX] = {};
	struct comp_seg_rect tile[COMP_SEGMENTS_MAX] = {};
	bool have_tile[COMP_SEGMENTS_MAX] = {};

	for (uint32_t k = 0; k < t->count; k++) {
		const struct comp_segment *g = &t->seg[k];
		have_tile[k] =
		    comp_segments_tile_rect(&g->window_rect, &f->canvas, f->view_width, f->view_height, &tile[k]);
		const int i = (int)g->screen_index;
		screen_of[k] = i;
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
		cl->ClearRenderTargetView(rtv, clear, 0, nullptr);
	}

	/*
	 * 3. Crop each woven segment's views out of the atlas — crop before the
	 * DP is the law (ADR-030): a DP's atlas holds exactly its canvas. The
	 * source goes COMMON → COPY_SOURCE → COMMON around all of them.
	 */
	bool any_crop = false;
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
		if (!any_crop) {
			transition(cl, src, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
			any_crop = true;
		}
		transition(cl, crop->texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
		D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
		dst_loc.pResource = crop->texture;
		dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		D3D12_TEXTURE_COPY_LOCATION src_loc = {};
		src_loc.pResource = src;
		src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		for (uint32_t row = 0; row < f->tile_rows; row++) {
			for (uint32_t col = 0; col < f->tile_columns; col++) {
				D3D12_BOX box = {};
				box.left = col * f->view_width + (UINT)tile[k].x;
				box.top = row * f->view_height + (UINT)tile[k].y;
				box.right = box.left + tile[k].w;
				box.bottom = box.top + tile[k].h;
				box.front = 0;
				box.back = 1;
				cl->CopyTextureRegion(&dst_loc, col * tile[k].w, row * tile[k].h, 0, &src_loc, &box);
			}
		}
		transition(cl, crop->texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
	}
	if (any_crop) {
		transition(cl, src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
	}

	/*
	 * 4. Weave: the primary first (a DP built before M6 may still clear the
	 * whole target, so it must not run after a sibling), then left to right.
	 * Each gets canvas = its segment, its own crop + descriptor, the back
	 * buffer re-bound and viewport + scissor = the segment.
	 */
	ID3D12DescriptorHeap *heaps[] = {segs->srv_heap};
	bool any_woven = false;
	for (uint32_t pass = 0; pass < 2; pass++) {
		for (uint32_t k = 0; k < t->count; k++) {
			const struct comp_segment *g = &t->seg[k];
			if (!weave[k] || g->is_primary != (pass == 0)) {
				continue;
			}
			struct xrt_display_processor_d3d12 *dp = dp_of[k];
			struct seg_screen_state *st = st_of[k];
			if (!g->is_primary) {
				if (!st->configured) {
					st->configured = true;
					xrt_display_processor_d3d12_set_background_2d(dp, nullptr, 0, 0);
				}
				if (st->transparent_latched != (int)f->transparent_background) {
					st->transparent_latched = (int)f->transparent_background;
					xrt_display_processor_d3d12_set_transparent_background(
					    dp, f->transparent_background, false);
				}
				if (st->mode_sent != (int)segs->mode_3d) {
					st->mode_sent = (int)segs->mode_3d;
					xrt_display_processor_d3d12_request_display_mode(dp, segs->mode_3d);
				}
				// Phase only where window px ARE device px; a resampled
				// screen's origin is in the wrong units (and only a
				// resample-tolerant DP weaves there, which has no phase).
				if (g->screen_1to1 != COMP_SEG_1TO1_NO) {
					xrt_display_processor_d3d12_set_present_origin(dp, g->present_origin_x,
					                                               g->present_origin_y);
				}
			}
			const uint64_t srv = write_srv(segs, (uint32_t)screen_of[k], st->crop.texture);
			cl->SetDescriptorHeaps(1, heaps);
			cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
			set_viewport_scissor(cl, &g->window_rect);
			xrt_display_processor_d3d12_process_atlas(
			    dp, cl, st->crop.texture, srv, f->target_rtv, f->target_resource, tile[k].w, tile[k].h,
			    f->tile_columns, f->tile_rows, f->src_format, f->target_width, f->target_height,
			    g->window_rect.x, g->window_rect.y, g->window_rect.w, g->window_rect.h);
			any_woven = true;
		}
	}

	/*
	 * 5. Flat 2D for every segment that could not be woven, and for the
	 * canvas no segment covers: one view (the middle one — the left eye of a
	 * stereo pair), linearly scaled from the view tile to the window rect.
	 */
	struct comp_seg_rect flat_dst[COMP_SEGMENTS_MAX + COMP_SEGMENTS_MAX_UNCOVERED];
	uint32_t flat_n = 0;
	for (uint32_t k = 0; k < t->count; k++) {
		if (!weave[k] && have_tile[k]) {
			flat_dst[flat_n++] = t->seg[k].window_rect;
		}
	}
	flat_n += comp_segments_uncovered(t, &f->canvas, &flat_dst[flat_n], COMP_SEGMENTS_MAX_UNCOVERED);
	if (flat_n > 0 && f->renderer != nullptr) {
		const uint32_t views = f->tile_columns * f->tile_rows;
		const uint32_t vi = views > 0 ? (views - 1) / 2 : 0;
		const int32_t base_x = (int32_t)((vi % f->tile_columns) * f->view_width);
		const int32_t base_y = (int32_t)((vi / f->tile_columns) * f->view_height);
		const uint64_t srv = write_srv(segs, SEG_SRV_SLOT_FLAT, src);
		transition(cl, src, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		for (uint32_t k = 0; k < flat_n; k++) {
			const struct comp_seg_rect *d = &flat_dst[k];
			struct comp_seg_rect s;
			if (!comp_segments_tile_rect(d, &f->canvas, f->view_width, f->view_height, &s)) {
				continue; // thinner than one source pixel
			}
			comp_d3d12_renderer_blit_rect(f->renderer, cl, segs->srv_heap, srv, (uint32_t)src_desc.Width,
			                              src_desc.Height, base_x + s.x, base_y + s.y, s.w, s.h,
			                              f->target_rtv, d->x, d->y, d->w, d->h);
		}
		transition(cl, src, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
	}

	// Leave the back buffer bound, viewport + scissor = the whole target, the
	// state the single-DP path's caller set before process_atlas.
	cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
	{
		const struct comp_seg_rect whole = {0, 0, f->target_width, f->target_height};
		set_viewport_scissor(cl, &whole);
	}
	return any_woven;
}

extern "C" void
comp_d3d12_segments_set_display_mode(struct comp_d3d12_segments *segs, bool enable_3d)
{
	if (segs == nullptr) {
		return;
	}
	segs->mode_3d = enable_3d;
	for (uint32_t i = 0; i < segs->screen_count; i++) {
		struct seg_screen_state *st = &segs->st[i];
		if (st->dp != nullptr && st->mode_sent != (int)enable_3d) {
			st->mode_sent = (int)enable_3d;
			xrt_display_processor_d3d12_request_display_mode(st->dp, enable_3d);
		}
	}
}

extern "C" bool
comp_d3d12_segments_get_metrics(const struct comp_d3d12_segments *segs,
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

extern "C" bool
comp_d3d12_segments_get_eyes(struct comp_d3d12_segments *segs, uint64_t screen_id, struct xrt_eye_positions *out)
{
	if (segs == nullptr || out == nullptr) {
		return false;
	}
	bool ok = false;
	os_mutex_lock(&segs->dp_mutex);
	const int i = screen_index_of(segs, screen_id);
	if (i >= 0 && segs->st[i].dp != nullptr) {
		ok = xrt_display_processor_d3d12_get_predicted_eye_positions(segs->st[i].dp, out) && out->valid;
	}
	os_mutex_unlock(&segs->dp_mutex);
	return ok;
}
