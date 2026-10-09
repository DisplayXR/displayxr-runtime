// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The service's display status snapshot + generation counters
 *         (ADR-051 D3, display dashboard phase 2).
 *
 * The service owns ONE @ref xrt_status_snapshot and two counters:
 *
 *  - `topology` — screens and plug-ins. Bumped by the world-event re-probe
 *    (`WM_DISPLAYCHANGE` / `WM_DEVICECHANGE` through the re-probe worker), and
 *    whenever the screen list or the plug-ins' load / platform state / vendor
 *    change counters read differently from the last status read.
 *  - `status` — everything else. Bumped on client connect / disconnect, and
 *    whenever the cheap live facts (client list + session flags, presenter,
 *    lease, window, segment-table generation, window-handle owner, each bound
 *    DP's backend state and `is_tracking`, the mode) read differently; plus a
 *    2 s floor that rebuilds the snapshot and bumps only if its content moved
 *    (integrity counters, plug-in hints, anything without its own event).
 *
 * Everything but the connect / disconnect / re-probe bumps is evaluated when a
 * DIAG consumer reads (`system_get_status_*`), on that consumer's IPC thread:
 * no consumer, no work (ADR-051 D5.2 "the service rebuilds nothing for
 * nobody"), and nothing on the 20 Hz main loop or the render thread. The
 * snapshot itself is rebuilt lazily — on the first piece fetched after a
 * counter moved — never per bump, never per frame. Nothing here logs.
 *
 * The rows only `targets/common` can fill (runtime, plug-ins, screens) come
 * from the status provider the service target registers
 * (`target_status_snapshot_build_service`); this file gathers the live facts
 * it merges.
 *
 * Locks: `lock` (this module; held across a read) -> `global_state.lock`
 * (briefly, for the slot table) and -> the service compositor's render mutex
 * (per client, inside `comp_d3d11_service_get_client_status`). The counters
 * have their own leaf `counter_lock` so the connect / disconnect / re-probe
 * paths can bump them while holding anything.
 *
 * @ingroup ipc_server
 */

#include "xrt/xrt_device.h"
#include "xrt/xrt_display_status.h"
#include "xrt/xrt_instance.h"
#include "xrt/xrt_system.h"

#include "os/os_threading.h"
#include "os/os_time.h"
#include "util/u_misc.h"
#include "util/u_status_snapshot.h"
#include "util/u_time.h"

#include "shared/ipc_protocol.h"
#include "server/ipc_server.h"

#if defined(XRT_HAVE_D3D11_SERVICE_COMPOSITOR)
#include "d3d11_service/comp_d3d11_service.h"
#endif

#include <stdlib.h>
#include <string.h>


//! The 2 s floor: a missed event costs at most this (ADR-051 D5.3).
#define STATUS_FLOOR_NS ((uint64_t)2 * U_TIME_1S_IN_NS)
//! Plug-in state / vendor change counters are sampled at most this often.
#define STATUS_CHANGE_KEY_PERIOD_NS ((uint64_t)U_TIME_1S_IN_NS)

//! One bound DP's not-tracking clock (TRACKER_DOWN needs "for more than 5 s").
struct status_nt_entry
{
	bool used;
	bool seen;
	uint32_t client_id;
	uint32_t kind;
	uint64_t screen_id;
	uint64_t since_ns;
};

struct ipc_server_status
{
	//! Held across a whole status read (gather, compare, build).
	struct os_mutex lock;

	//! Leaf lock for the two counters only.
	struct os_mutex counter_lock;
	uint64_t topology;
	uint64_t status;

	// Last read's keys (under @ref lock).
	bool have_keys;
	uint64_t live_key;
	uint64_t topology_key;
	uint64_t change_key;
	uint64_t change_key_ns;
	uint64_t floor_ns;

	// The cached snapshot and what it was built at.
	struct xrt_status_snapshot *snap;
	struct xrt_status_snapshot *scratch;
	bool built_valid;
	struct xrt_status_generation built;
	//! Raw segment tables, same order as `snap->clients`.
	struct xrt_segment_metrics metrics[XRT_STATUS_MAX_CLIENTS];

	// Scratch for one read (heap: tens of KB).
	struct xrt_status_live *live;
	struct xrt_segment_metrics live_metrics[XRT_STATUS_MAX_CLIENTS];
	struct ipc_service_health *health;
	struct xrt_screen_list *screens;
	struct xrt_dp_factory_registry *reg;

	struct status_nt_entry nt[XRT_STATUS_MAX_LIVE_DPS];
};

static struct ipc_server_status_provider g_provider;


/*
 *
 * Provider + lifecycle.
 *
 */

void
ipc_server_set_status_provider(const struct ipc_server_status_provider *provider)
{
	if (provider == NULL) {
		memset(&g_provider, 0, sizeof(g_provider));
	} else {
		g_provider = *provider;
	}
}

void
ipc_server_status_init(struct ipc_server *s)
{
	struct ipc_server_status *st = U_TYPED_CALLOC(struct ipc_server_status);
	if (st == NULL) {
		return;
	}
	st->snap = U_TYPED_CALLOC(struct xrt_status_snapshot);
	st->scratch = U_TYPED_CALLOC(struct xrt_status_snapshot);
	st->live = U_TYPED_CALLOC(struct xrt_status_live);
	st->health = U_TYPED_CALLOC(struct ipc_service_health);
	st->screens = U_TYPED_CALLOC(struct xrt_screen_list);
	st->reg = U_TYPED_CALLOC(struct xrt_dp_factory_registry);
	if (st->snap == NULL || st->scratch == NULL || st->live == NULL || st->health == NULL || st->screens == NULL ||
	    st->reg == NULL || os_mutex_init(&st->lock) != 0) {
		goto fail;
	}
	if (os_mutex_init(&st->counter_lock) != 0) {
		os_mutex_destroy(&st->lock);
		goto fail;
	}
	// Start at 1: a consumer may treat 0/0 as "no service counters" (headless).
	st->topology = 1;
	st->status = 1;
	s->status = st;
	return;

fail:
	free(st->snap);
	free(st->scratch);
	free(st->live);
	free(st->health);
	free(st->screens);
	free(st->reg);
	free(st);
}

void
ipc_server_status_fini(struct ipc_server *s)
{
	struct ipc_server_status *st = s->status;
	if (st == NULL) {
		return;
	}
	s->status = NULL;
	os_mutex_destroy(&st->counter_lock);
	os_mutex_destroy(&st->lock);
	free(st->snap);
	free(st->scratch);
	free(st->live);
	free(st->health);
	free(st->screens);
	free(st->reg);
	free(st);
}

void
ipc_server_status_bump(struct ipc_server *s, bool topology)
{
	struct ipc_server_status *st = s != NULL ? s->status : NULL;
	if (st == NULL) {
		return;
	}
	os_mutex_lock(&st->counter_lock);
	if (topology) {
		st->topology++;
	} else {
		st->status++;
	}
	os_mutex_unlock(&st->counter_lock);
}

static struct xrt_status_generation
counters(struct ipc_server_status *st)
{
	struct xrt_status_generation g;
	os_mutex_lock(&st->counter_lock);
	g.topology = st->topology;
	g.status = st->status;
	os_mutex_unlock(&st->counter_lock);
	return g;
}


/*
 *
 * Keys (FNV-1a over the fields that matter, never over padding).
 *
 */

#define FNV_INIT 1469598103934665603ull

static void
mix(uint64_t *h, const void *data, size_t len)
{
	const unsigned char *b = (const unsigned char *)data;
	for (size_t i = 0; i < len; i++) {
		*h = (*h ^ b[i]) * 1099511628211ull;
	}
}

static void
mix_u64(uint64_t *h, uint64_t v)
{
	mix(h, &v, sizeof(v));
}

static void
mix_str(uint64_t *h, const char *s)
{
	mix(h, s, strlen(s));
	mix_u64(h, 0);
}

//! The live facts the `status` counter follows event-by-event.
static uint64_t
live_key(const struct xrt_status_live *live)
{
	uint64_t h = FNV_INIT;
	mix_u64(&h, live->client_count);
	for (uint32_t i = 0; i < live->client_count; i++) {
		const struct xrt_status_client *c = &live->clients[i];
		mix_u64(&h, c->id);
		mix_u64(&h, c->client_class);
		mix_u64(&h, c->class_verified);
		mix_str(&h, c->name);
		mix_u64(&h, (uint64_t)c->flags.active | ((uint64_t)c->flags.visible << 1) |
		                ((uint64_t)c->flags.focused << 2) | ((uint64_t)c->flags.overlay << 3));
		mix_u64(&h, (uint64_t)c->presenter);
		mix_u64(&h, (uint64_t)c->lease);
		mix_u64(&h, c->window.valid);
		mix_u64(&h, (uint64_t)(int64_t)c->window.left);
		mix_u64(&h, (uint64_t)(int64_t)c->window.top);
		mix_u64(&h, c->window.width);
		mix_u64(&h, c->window.height);
		mix_u64(&h, c->owner_screen);
		mix_u64(&h, c->segments.generation);
		mix_u64(&h, c->segments.count);
		for (uint32_t k = 0; k < c->segments.count && k < XRT_STATUS_MAX_SEGMENTS; k++) {
			mix_u64(&h, c->segments.items[k].screen);
			mix_u64(&h, c->segments.items[k].has_dp);
			mix_u64(&h, c->segments.items[k].woven);
			mix_u64(&h, (uint64_t)c->segments.items[k].eye_source);
		}
		mix_u64(&h, c->views.active);
	}
	mix_u64(&h, live->dp_count);
	for (uint32_t d = 0; d < live->dp_count; d++) {
		const struct xrt_status_live_dp *ld = &live->dps[d];
		mix_u64(&h, ld->screen_id);
		mix_u64(&h, ld->dp.client_id);
		mix_u64(&h, (uint64_t)ld->dp.kind);
		mix_u64(&h, (uint64_t)ld->dp.backend);
		mix_u64(&h, ld->answered);
		mix_u64(&h, ld->is_tracking); // the is_tracking edge
	}
	mix_u64(&h, live->mode.valid);
	mix_u64(&h, live->mode.index);
	mix_u64(&h, live->mode.is_3d);
	mix_u64(&h, live->workspace.enabled);
	mix_str(&h, live->workspace.controller);
	return h;
}

//! The screen list the service's apps see, by value.
static uint64_t
screens_key(const struct xrt_screen_list *list)
{
	uint64_t h = FNV_INIT;
	mix_u64(&h, list->count);
	for (uint32_t i = 0; i < list->count && i < XRT_SCREEN_LIST_MAX; i++) {
		const struct xrt_screen *sc = &list->screens[i];
		mix_u64(&h, sc->id);
		mix_u64(&h, sc->flags);
		mix_u64(&h, sc->confidence);
		mix_u64(&h, (uint64_t)(int64_t)sc->desktop_left);
		mix_u64(&h, (uint64_t)(int64_t)sc->desktop_top);
		mix_u64(&h, sc->desktop_width);
		mix_u64(&h, sc->desktop_height);
		mix_u64(&h, sc->native_width);
		mix_u64(&h, sc->native_height);
		mix(&h, &sc->desktop_scale, sizeof(sc->desktop_scale));
		mix_u64(&h, sc->physical_width_mm);
		mix_u64(&h, sc->physical_height_mm);
		mix_str(&h, sc->plugin_id);
		mix_str(&h, sc->device_name);
		mix(&h, &sc->info.width_m, sizeof(sc->info.width_m));
		mix(&h, &sc->info.height_m, sizeof(sc->info.height_m));
		mix_u64(&h, sc->info.supported_eye_tracking_modes);
		mix_u64(&h, sc->info.source);
	}
	return h;
}


/*
 *
 * Gather the live facts.
 *
 */

//! Not-tracking clock of one bound DP; 0 while tracking / not answering.
static uint32_t
nt_update(struct ipc_server_status *st, const struct xrt_status_live_dp *ld, uint64_t now_ns)
{
	struct status_nt_entry *free_e = NULL;
	struct status_nt_entry *e = NULL;
	for (uint32_t i = 0; i < XRT_STATUS_MAX_LIVE_DPS; i++) {
		struct status_nt_entry *x = &st->nt[i];
		if (!x->used) {
			if (free_e == NULL) {
				free_e = x;
			}
			continue;
		}
		if (x->client_id == ld->dp.client_id && x->kind == (uint32_t)ld->dp.kind &&
		    x->screen_id == ld->screen_id) {
			e = x;
			break;
		}
	}
	const bool not_tracking = ld->answered && !ld->is_tracking;
	if (!not_tracking) {
		if (e != NULL) {
			e->used = false;
		}
		return 0;
	}
	if (e == NULL) {
		if (free_e == NULL) {
			return 0;
		}
		e = free_e;
		e->used = true;
		e->client_id = ld->dp.client_id;
		e->kind = (uint32_t)ld->dp.kind;
		e->screen_id = ld->screen_id;
		e->since_ns = now_ns;
	}
	e->seen = true;
	const uint64_t ms = (now_ns - e->since_ns) / U_TIME_1MS_IN_NS;
	return ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}

static void
push_dp(struct xrt_status_live *live,
        uint64_t screen_id,
        uint32_t client_id,
        enum xrt_status_dp_kind kind,
        uint32_t backend,
        bool answered,
        bool tracking)
{
	if (live->dp_count >= XRT_STATUS_MAX_LIVE_DPS) {
		return;
	}
	struct xrt_status_live_dp *ld = &live->dps[live->dp_count++];
	memset(ld, 0, sizeof(*ld));
	ld->screen_id = screen_id;
	ld->dp.client_id = client_id;
	ld->dp.api = XRT_STATUS_DP_API_D3D11;
	ld->dp.kind = kind;
	ld->dp.backend = backend <= (uint32_t)XRT_STATUS_DP_BACKEND_STALE ? (enum xrt_status_dp_backend)backend
	                                                                  : XRT_STATUS_DP_BACKEND_OK;
	ld->answered = answered;
	ld->is_tracking = answered && tracking;
}

/*!
 * Everything only the service knows, into `st->live` (+ the raw segment tables
 * into `st->live_metrics`). Caller holds `st->lock`.
 */
static void
gather_live(struct ipc_server *s, struct ipc_server_status *st, uint64_t now_ns)
{
	struct xrt_status_live *live = st->live;
	memset(live, 0, sizeof(*live));
	memset(st->live_metrics, 0, sizeof(st->live_metrics));

	struct ipc_service_health *h = st->health;
	ipc_server_get_health(s, h);

	// The compositor + overlay flag of each health slot, under the same lock
	// the slot table is written under. The pointers are only handed to
	// comp_d3d11_service_get_client_status, which re-validates them.
	struct xrt_compositor *xcs[IPC_MAX_CLIENTS] = {0};
	bool overlay[IPC_MAX_CLIENTS] = {false};
	os_mutex_lock(&s->global_state.lock);
	for (uint32_t k = 0; k < h->slot_count && k < IPC_MAX_CLIENTS; k++) {
		volatile struct ipc_client_state *ics = &s->threads[h->slots[k].slot].ics;
		if (ics->client_state.id == h->slots[k].id) {
			xcs[k] = (struct xrt_compositor *)ics->xc;
			overlay[k] = ics->client_state.session_overlay;
		}
	}
	os_mutex_unlock(&s->global_state.lock);

	// The head's rendering mode (one per service).
	uint32_t view_capacity = 0;
	struct xrt_device *head = s->xsysd != NULL ? s->xsysd->static_roles.head : NULL;
	if (head != NULL && head->hmd != NULL) {
		const uint32_t n = head->rendering_mode_count < XRT_MAX_RENDERING_MODES ? head->rendering_mode_count
		                                                                        : XRT_MAX_RENDERING_MODES;
		const uint32_t idx = head->hmd->active_rendering_mode_index;
		for (uint32_t i = 0; i < n; i++) {
			if (head->rendering_modes[i].view_count > view_capacity) {
				view_capacity = head->rendering_modes[i].view_count;
			}
		}
		if (idx < n) {
			const struct xrt_rendering_mode *rm = &head->rendering_modes[idx];
			live->mode.valid = true;
			live->mode.index = idx;
			(void)snprintf(live->mode.name, sizeof(live->mode.name), "%s", rm->mode_name);
			live->mode.views = rm->view_count;
			live->mode.is_3d = rm->hardware_display_3d;
		}
	}

	live->workspace.enabled = s->workspace_mode;

#if defined(XRT_HAVE_D3D11_SERVICE_COMPOSITOR)
	struct comp_d3d11_render_diag rd;
	memset(&rd, 0, sizeof(rd));
	if (s->xsysc != NULL && comp_d3d11_service_is_d3d11_service(s->xsysc)) {
		comp_d3d11_service_get_render_diag(s->xsysc, &rd);
	}
	const uint32_t backend = h->dp_state_known ? h->dp_backend_state : 0u;
	bool any_primary = false;
#endif

	for (uint32_t k = 0; k < h->slot_count && live->client_count < XRT_STATUS_MAX_CLIENTS; k++) {
		const struct ipc_service_health_slot *sl = &h->slots[k];
		const uint32_t n = live->client_count++;
		struct xrt_status_client *c = &live->clients[n];
		memset(c, 0, sizeof(*c));
		c->id = sl->id;
		c->pid = sl->pid;
		c->client_class = sl->client_class;
		c->class_verified = sl->class_verified;
		(void)snprintf(c->name, sizeof(c->name), "%s", sl->name);
		c->flags.active = sl->session_active;
		c->flags.visible = sl->session_visible;
		c->flags.focused = sl->session_focused;
		c->flags.overlay = overlay[k];
		c->presenter = XRT_STATUS_PRESENTER_NONE;
		if (h->lease == IPC_SERVICE_LEASE_CONTROLLER) {
			c->lease = XRT_STATUS_LEASE_CONTROLLER;
		} else if (h->lease == IPC_SERVICE_LEASE_SLOT && h->lease_slot == (int32_t)sl->slot) {
			c->lease = XRT_STATUS_LEASE_SLOT;
		} else {
			c->lease = XRT_STATUS_LEASE_NONE;
		}
		if (sl->client_class == XRT_CLIENT_CLASS_CONTROLLER && sl->class_verified &&
		    live->workspace.controller[0] == '\0') {
			(void)snprintf(live->workspace.controller, sizeof(live->workspace.controller), "%s", sl->name);
		}

#if defined(XRT_HAVE_D3D11_SERVICE_COMPOSITOR)
		struct comp_d3d11_client_status cs;
		if (xcs[k] == NULL || s->xsysc == NULL ||
		    !comp_d3d11_service_get_client_status(s->xsysc, xcs[k], &cs)) {
			continue;
		}
		c->presenter = (enum xrt_status_presenter)cs.presenter;
		if (cs.window.valid) {
			c->window.valid = true;
			c->window.left = cs.window.window_screen_left;
			c->window.top = cs.window.window_screen_top;
			c->window.width = cs.window.window_pixel_width;
			c->window.height = cs.window.window_pixel_height;
		}
		c->owner_screen = cs.owner_screen;
		st->live_metrics[n] = cs.segments;
		const struct xrt_segment_metrics *m = &cs.segments;
		const uint32_t nseg = m->count < XRT_STATUS_MAX_SEGMENTS ? m->count : XRT_STATUS_MAX_SEGMENTS;
		c->segments.generation = m->generation;
		c->segments.split = nseg > 0;
		c->segments.count = nseg;
		uint64_t primary_screen = 0; // 0 = the runtime-default screen
		for (uint32_t i = 0; i < nseg; i++) {
			const struct xrt_segment_metric *sm = &m->seg[i];
			struct xrt_status_segment *it = &c->segments.items[i];
			it->screen = sm->screen_id;
			it->canvas.x = sm->window_rect.offset.w;
			it->canvas.y = sm->window_rect.offset.h;
			it->canvas.w = (uint32_t)sm->window_rect.extent.w;
			it->canvas.h = (uint32_t)sm->window_rect.extent.h;
			it->has_dp = sm->has_dp;
			it->woven = sm->woven;
			it->eye_source = sm->have_eyes ? XRT_STATUS_EYE_SOURCE_DP : XRT_STATUS_EYE_SOURCE_NONE;
			if (sm->is_primary) {
				primary_screen = sm->screen_id;
			} else if (sm->has_dp) {
				push_dp(live, sm->screen_id, c->id, XRT_STATUS_DP_KIND_SEGMENT,
				        XRT_STATUS_DP_BACKEND_OK, sm->have_eyes,
				        sm->have_eyes && sm->eyes.valid && sm->eyes.is_tracking);
			}
		}
		if (cs.eyes_answered) {
			push_dp(live, primary_screen, c->id, XRT_STATUS_DP_KIND_PRIMARY, backend, true, cs.is_tracking);
			any_primary = true;
		}

		// Views: what the head's modes size for, what the current mode
		// uses, and what the app is told (one view set per segment).
		c->views.capacity = view_capacity;
		c->views.active = live->mode.valid ? live->mode.views : 0u;
		c->views.reported = c->views.active * (nseg > 0 ? nseg : 1u);

		// Integrity (#1248): the last completed [RENDER] window belongs to
		// the active presenter — the client holding the lease slot.
		if (rd.valid && rd.pipe_valid && c->lease == XRT_STATUS_LEASE_SLOT) {
			c->integrity.paint = rd.capture_renders;
			c->integrity.present = rd.pipe_active_present;
			c->integrity.skip = rd.pipe_active_skip;
			(void)snprintf(c->integrity.weave_placement, sizeof(c->integrity.weave_placement), "%s",
			               rd.split_active ? "scanout" : "render");
		}
#else
		(void)xcs;
		(void)view_capacity;
#endif
	}

#if defined(XRT_HAVE_D3D11_SERVICE_COMPOSITOR)
	// No client has a primary DP bound: report the service's own panel DP,
	// when one answers (idle service, workspace controller only, ...).
	if (!any_primary && s->xsysc != NULL && comp_d3d11_service_is_d3d11_service(s->xsysc)) {
		struct xrt_eye_positions eyes;
		memset(&eyes, 0, sizeof(eyes));
		if (comp_d3d11_service_get_predicted_eye_positions_full(s->xsysc, &eyes)) {
			push_dp(live, 0, 0, XRT_STATUS_DP_KIND_PRIMARY, backend, true, eyes.valid && eyes.is_tracking);
		}
	}
#endif

	// Not-tracking clocks; forget DPs that went away.
	for (uint32_t i = 0; i < XRT_STATUS_MAX_LIVE_DPS; i++) {
		st->nt[i].seen = false;
	}
	for (uint32_t d = 0; d < live->dp_count; d++) {
		live->dps[d].not_tracking_ms = nt_update(st, &live->dps[d], now_ns);
	}
	for (uint32_t i = 0; i < XRT_STATUS_MAX_LIVE_DPS; i++) {
		if (st->nt[i].used && !st->nt[i].seen) {
			st->nt[i].used = false;
		}
	}
}


/*
 *
 * Build + compare.
 *
 */

static void
build_into(struct ipc_server *s, struct ipc_server_status *st, struct xrt_status_snapshot *out)
{
	st->live->generation = counters(st);
	if (g_provider.build != NULL) {
		g_provider.build(s->xinst, s->xsysc != NULL ? st->reg : NULL, st->live, out);
	} else {
		memset(out, 0, sizeof(*out));
		out->schema = XRT_STATUS_SCHEMA;
		out->source = XRT_STATUS_SOURCE_SERVICE;
		out->generation = st->live->generation;
	}
}

//! @p a and @p b say the same thing (generation and the running not-tracking clocks aside).
static bool
same_content(struct xrt_status_snapshot *a, struct xrt_status_snapshot *b)
{
	const struct xrt_status_generation ga = a->generation;
	const struct xrt_status_generation gb = b->generation;
	uint32_t nta[XRT_STATUS_MAX_SCREENS], ntb[XRT_STATUS_MAX_SCREENS];
	for (uint32_t i = 0; i < XRT_STATUS_MAX_SCREENS; i++) {
		nta[i] = a->screens[i].eye_tracking.not_tracking_ms;
		ntb[i] = b->screens[i].eye_tracking.not_tracking_ms;
		a->screens[i].eye_tracking.not_tracking_ms = 0;
		b->screens[i].eye_tracking.not_tracking_ms = 0;
	}
	memset(&a->generation, 0, sizeof(a->generation));
	memset(&b->generation, 0, sizeof(b->generation));
	const bool same = memcmp(a, b, sizeof(*a)) == 0;
	a->generation = ga;
	b->generation = gb;
	for (uint32_t i = 0; i < XRT_STATUS_MAX_SCREENS; i++) {
		a->screens[i].eye_tracking.not_tracking_ms = nta[i];
		b->screens[i].eye_tracking.not_tracking_ms = ntb[i];
	}
	return same;
}

static void
ensure_built(struct ipc_server *s, struct ipc_server_status *st)
{
	const struct xrt_status_generation now = counters(st);
	if (st->built_valid && u_status_generation_equal(&st->built, &now)) {
		return;
	}
	build_into(s, st, st->snap);
	memcpy(st->metrics, st->live_metrics, sizeof(st->metrics));
	st->built = st->snap->generation;
	st->built_valid = true;
}

/*!
 * One status read's bookkeeping: gather the live facts, bump the counters
 * whose inputs moved, run the 2 s floor. Caller holds `st->lock`.
 */
static void
tick(struct ipc_server *s, struct ipc_server_status *st)
{
	const uint64_t now_ns = os_monotonic_get_ns();

	gather_live(s, st, now_ns);
	const uint64_t lk = live_key(st->live);

	// Topology inputs: the screen list (as apps see it) + the plug-ins' state.
	memset(st->screens, 0, sizeof(*st->screens));
	if (s->xinst == NULL || xrt_instance_enumerate_displays(s->xinst, st->screens) != XRT_SUCCESS) {
		memset(st->screens, 0, sizeof(*st->screens));
	}
	if (s->xsysc != NULL) {
		// An unlocked copy, as the segments-enable path takes it: claim
		// serials and per-API factory bits are display-only here.
		*st->reg = s->xsysc->info.dp_registry;
	} else {
		memset(st->reg, 0, sizeof(*st->reg));
	}
	if (g_provider.change_key != NULL &&
	    (st->change_key_ns == 0 || now_ns - st->change_key_ns >= STATUS_CHANGE_KEY_PERIOD_NS)) {
		st->change_key = g_provider.change_key(s->xsysc != NULL ? st->reg : NULL);
		st->change_key_ns = now_ns;
	}
	uint64_t tk = screens_key(st->screens);
	mix_u64(&tk, st->change_key);

	if (!st->have_keys) {
		st->have_keys = true;
		st->live_key = lk;
		st->topology_key = tk;
		st->floor_ns = now_ns;
	} else {
		if (tk != st->topology_key) {
			st->topology_key = tk;
			ipc_server_status_bump(s, true);
		}
		if (lk != st->live_key) {
			st->live_key = lk;
			ipc_server_status_bump(s, false);
		}
	}

	// The 2 s floor: rebuild, and bump only if the content moved.
	if (now_ns - st->floor_ns >= STATUS_FLOOR_NS) {
		st->floor_ns = now_ns;
		ensure_built(s, st);
		build_into(s, st, st->scratch);
		if (!same_content(st->snap, st->scratch)) {
			ipc_server_status_bump(s, false);
			struct xrt_status_snapshot *t = st->snap;
			st->snap = st->scratch;
			st->scratch = t;
			st->snap->generation = counters(st);
			memcpy(st->metrics, st->live_metrics, sizeof(st->metrics));
			st->built = st->snap->generation;
			st->built_valid = true;
		}
	}
}


/*
 *
 * The three RPC bodies.
 *
 */

xrt_result_t
ipc_server_status_get_generation(struct ipc_server *s, struct xrt_status_generation *out)
{
	memset(out, 0, sizeof(*out));
	struct ipc_server_status *st = s->status;
	if (st == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	os_mutex_lock(&st->lock);
	tick(s, st);
	*out = counters(st);
	os_mutex_unlock(&st->lock);
	return XRT_SUCCESS;
}

xrt_result_t
ipc_server_status_get_snapshot_piece(struct ipc_server *s,
                                     uint32_t screen_index,
                                     struct xrt_status_head *out_head,
                                     struct xrt_status_screen *out_screen)
{
	memset(out_head, 0, sizeof(*out_head));
	memset(out_screen, 0, sizeof(*out_screen));
	struct ipc_server_status *st = s->status;
	if (st == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	os_mutex_lock(&st->lock);
	tick(s, st);
	ensure_built(s, st);
	u_status_snapshot_get_head(st->snap, out_head);
	if (screen_index < st->snap->screen_count && screen_index < XRT_STATUS_MAX_SCREENS) {
		*out_screen = st->snap->screens[screen_index];
	}
	os_mutex_unlock(&st->lock);
	return XRT_SUCCESS;
}

xrt_result_t
ipc_server_status_get_client(struct ipc_server *s,
                             uint32_t client_id,
                             struct xrt_status_client *out_client,
                             struct xrt_segment_metrics *out_metrics,
                             struct xrt_status_generation *out_generation)
{
	memset(out_client, 0, sizeof(*out_client));
	memset(out_metrics, 0, sizeof(*out_metrics));
	memset(out_generation, 0, sizeof(*out_generation));
	struct ipc_server_status *st = s->status;
	if (st == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	xrt_result_t xret = XRT_ERROR_IPC_FAILURE; // no such client in the snapshot
	os_mutex_lock(&st->lock);
	tick(s, st);
	ensure_built(s, st);
	*out_generation = st->snap->generation;
	for (uint32_t i = 0; i < st->snap->client_count && i < XRT_STATUS_MAX_CLIENTS; i++) {
		if (st->snap->clients[i].id == client_id) {
			*out_client = st->snap->clients[i];
			*out_metrics = st->metrics[i];
			xret = XRT_SUCCESS;
			break;
		}
	}
	os_mutex_unlock(&st->lock);
	return xret;
}
