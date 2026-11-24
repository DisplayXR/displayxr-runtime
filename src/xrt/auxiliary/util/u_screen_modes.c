// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The process-wide registry of per-screen rendering-mode tables
 *         (ADR-047 Amendment 10); see u_screen_modes.h.
 * @ingroup aux_util
 */

#include "util/u_screen_modes.h"
#include "util/u_logging.h"

#include "xrt/xrt_display_status.h"

#include "os/os_threading.h"

#include <stdio.h>
#include <string.h>

//! The once-a-second rule of the choice re-read (ADR-066 C1.9, like the head's).
#define U_SCREEN_MODES_REFRESH_NS (1000ull * 1000ull * 1000ull)

static struct
{
	bool lock_ready;
	struct os_mutex lock;
	uint32_t count;
	struct u_screen_mode_table tables[U_SCREEN_MODES_MAX_SCREENS];
	uint64_t generation;
	uint64_t last_refresh_ns;
	u_screen_modes_choice_fn resolver;
	uint32_t worst_w, worst_h;
} g_sm;

static void
sm_lock(void)
{
	// The first touch is the target layer's publish at system creation,
	// single-threaded; every later caller finds the lock ready.
	if (!g_sm.lock_ready) {
		g_sm.lock_ready = os_mutex_init(&g_sm.lock) == 0;
	}
	if (g_sm.lock_ready) {
		os_mutex_lock(&g_sm.lock);
	}
}

static void
sm_unlock(void)
{
	if (g_sm.lock_ready) {
		os_mutex_unlock(&g_sm.lock);
	}
}

//! Do two tables differ in anything an app or a DP could see?
static bool
table_differs(const struct u_screen_mode_table *a, const struct u_screen_mode_table *b)
{
	return a->screen_id != b->screen_id || a->count != b->count || a->envelope_count != b->envelope_count ||
	       a->slot1 != b->slot1 || a->pixel_width != b->pixel_width || a->pixel_height != b->pixel_height ||
	       a->rotated != b->rotated || a->sized != b->sized ||
	       memcmp(a->modes, b->modes, sizeof(a->modes[0]) * a->count) != 0;
}

void
u_screen_modes_publish(const struct u_screen_mode_table *tables, uint32_t n)
{
	if (n > U_SCREEN_MODES_MAX_SCREENS) {
		n = U_SCREEN_MODES_MAX_SCREENS;
	}
	sm_lock();
	bool changed = n != g_sm.count;
	for (uint32_t i = 0; i < n && !changed; i++) {
		changed = table_differs(&tables[i], &g_sm.tables[i]);
	}
	for (uint32_t i = 0; i < n; i++) {
		g_sm.tables[i] = tables[i];
	}
	g_sm.count = n;
	if (changed) {
		g_sm.generation++;
	}
	sm_unlock();
}

void
u_screen_modes_clear(void)
{
	sm_lock();
	if (g_sm.count != 0) {
		g_sm.generation++;
	}
	g_sm.count = 0;
	sm_unlock();
}

bool
u_screen_modes_get(uint64_t screen_id, struct u_screen_mode_table *out)
{
	bool found = false;
	sm_lock();
	for (uint32_t i = 0; i < g_sm.count; i++) {
		if (g_sm.tables[i].screen_id == screen_id) {
			if (out != NULL) {
				*out = g_sm.tables[i];
			}
			found = true;
			break;
		}
	}
	sm_unlock();
	return found;
}

uint32_t
u_screen_modes_get_all(struct u_screen_mode_table *out, uint32_t cap)
{
	sm_lock();
	uint32_t n = g_sm.count < cap ? g_sm.count : cap;
	for (uint32_t i = 0; out != NULL && i < n; i++) {
		out[i] = g_sm.tables[i];
	}
	sm_unlock();
	return n;
}

uint64_t
u_screen_modes_generation(void)
{
	sm_lock();
	const uint64_t g = g_sm.generation;
	sm_unlock();
	return g;
}

void
u_screen_modes_set_choice_resolver(u_screen_modes_choice_fn fn)
{
	sm_lock();
	g_sm.resolver = fn;
	sm_unlock();
}

bool
u_screen_modes_refresh_choices(uint64_t now_ns, bool force)
{
	// Snapshot the keys under the lock, resolve without it (the resolver
	// reads the settings store), then apply under it again.
	char keys[U_SCREEN_MODES_MAX_SCREENS][U_SCREEN_MODES_NAME_MAX];
	uint64_t ids[U_SCREEN_MODES_MAX_SCREENS];
	uint32_t n = 0;
	u_screen_modes_choice_fn resolver = NULL;
	sm_lock();
	if (!force && g_sm.last_refresh_ns != 0 && now_ns - g_sm.last_refresh_ns < U_SCREEN_MODES_REFRESH_NS) {
		sm_unlock();
		return false;
	}
	g_sm.last_refresh_ns = now_ns != 0 ? now_ns : 1;
	resolver = g_sm.resolver;
	for (uint32_t i = 0; i < g_sm.count; i++) {
		// The head's screen follows the head device (u_rendering_modes).
		if (g_sm.tables[i].list_source == U_SCREEN_MODES_LIST_HEAD) {
			continue;
		}
		ids[n] = g_sm.tables[i].screen_id;
		snprintf(keys[n], sizeof(keys[n]), "%s", g_sm.tables[i].key);
		n++;
	}
	sm_unlock();
	if (resolver == NULL || n == 0) {
		return false;
	}

	char names[U_SCREEN_MODES_MAX_SCREENS][U_SCREEN_MODES_NAME_MAX];
	uint32_t sources[U_SCREEN_MODES_MAX_SCREENS];
	for (uint32_t i = 0; i < n; i++) {
		names[i][0] = '\0';
		sources[i] = XRT_STATUS_MODE_3D_SOURCE_USER;
		if (keys[i][0] == '\0' || !resolver(keys[i], names[i], sizeof(names[i]), &sources[i])) {
			names[i][0] = '\0';
		}
	}

	bool any = false;
	sm_lock();
	for (uint32_t i = 0; i < n; i++) {
		for (uint32_t k = 0; k < g_sm.count; k++) {
			struct u_screen_mode_table *t = &g_sm.tables[k];
			if (t->screen_id != ids[i]) {
				continue;
			}
			const uint32_t was = t->slot1;
			if (u_screen_modes_table_apply_choice(t, names[i], sources[i])) {
				if (was != t->slot1) {
					any = true;
					U_LOG_W(
					    "screen 0x%016llx ('%s'): 3D mode is now '%s' (%u views, %ux%u tiles, "
					    "scale "
					    "%.2f x %.2f) - %s (ADR-047 Amendment 10)",
					    (unsigned long long)t->screen_id, t->key,
					    t->slot1 < t->count ? t->modes[t->slot1].mode_name : "-",
					    t->slot1 < t->count ? t->modes[t->slot1].view_count : 0u,
					    t->slot1 < t->count ? t->modes[t->slot1].tile_columns : 0u,
					    t->slot1 < t->count ? t->modes[t->slot1].tile_rows : 0u,
					    t->slot1 < t->count ? (double)t->modes[t->slot1].view_scale_x : 0.0,
					    t->slot1 < t->count ? (double)t->modes[t->slot1].view_scale_y : 0.0,
					    names[i][0] != '\0' ? "the stored per-screen choice"
					                        : "the driver's default");
				}
			}
		}
	}
	if (any) {
		g_sm.generation++;
	}
	sm_unlock();
	return any;
}

void
u_screen_modes_set_instance_worst(uint32_t atlas_w, uint32_t atlas_h)
{
	sm_lock();
	g_sm.worst_w = atlas_w;
	g_sm.worst_h = atlas_h;
	sm_unlock();
}

bool
u_screen_modes_get_instance_worst(uint32_t *out_atlas_w, uint32_t *out_atlas_h)
{
	sm_lock();
	const uint32_t w = g_sm.worst_w, h = g_sm.worst_h;
	sm_unlock();
	if (out_atlas_w != NULL) {
		*out_atlas_w = w;
	}
	if (out_atlas_h != NULL) {
		*out_atlas_h = h;
	}
	return w > 0 && h > 0;
}

void
u_screen_modes_registry_union(uint32_t *inout_atlas_w,
                              uint32_t *inout_atlas_h,
                              uint32_t *inout_view_w,
                              uint32_t *inout_view_h)
{
	sm_lock();
	u_screen_modes_union_atlas(g_sm.tables, g_sm.count, inout_atlas_w, inout_atlas_h, inout_view_w, inout_view_h);
	sm_unlock();
}

void
u_screen_modes_fill_status(const struct u_screen_mode_table *t, bool fits_instance, struct xrt_status_mode_3d *out)
{
	if (out == NULL) {
		return;
	}
	memset(out, 0, sizeof(*out));
	if (t == NULL || t->count < 2) {
		return;
	}
	out->valid = true;
	const struct xrt_rendering_mode *s1 = u_screen_modes_table_slot(t, 1);
	u_rendering_mode_slug(s1 != NULL ? s1->mode_name : "", out->active, sizeof(out->active));
	out->source = t->source;
	snprintf(out->setting, sizeof(out->setting), "%s", t->setting);
	for (uint32_t j = 1; j < t->count && out->choice_count < XRT_STATUS_MAX_MODE_3D_CHOICES; j++) {
		const struct xrt_rendering_mode *m = &t->modes[j];
		if (!u_rendering_mode_is_3d(m) && !(t->count == 2 && j == 1)) {
			continue;
		}
		struct xrt_status_mode_3d_choice *c = &out->choices[out->choice_count++];
		u_rendering_mode_slug(m->mode_name, c->name, sizeof(c->name));
		snprintf(c->label, sizeof(c->label), "%s", m->mode_name);
		c->views = m->view_count;
		c->tracked = (m->mode_flags & XRT_RENDERING_MODE_FLAG_HAS_TRACKING) != 0;
		c->passthrough = u_rendering_mode_is_passthrough_3d(m);
		if (m->view_count > XRT_MAX_VIEWS) {
			c->applies = XRT_STATUS_MODE_3D_APPLIES_NOT_SUPPORTED;
			snprintf(c->reason, sizeof(c->reason), "needs %u views; at most %u can be presented",
			         m->view_count, (unsigned)XRT_MAX_VIEWS);
		} else if (j >= t->envelope_count || (m->mode_flags & XRT_RENDERING_MODE_FLAG_START_UP_ONLY) != 0) {
			c->applies = XRT_STATUS_MODE_3D_APPLIES_RESTART_APP;
			snprintf(c->reason, sizeof(c->reason), "declared only at start-up: restart the app to apply");
		} else if (!fits_instance) {
			// ADR-066 C2 is a later stage: a screen claimed after the instance
			// was sized, larger than it, is clamped and resampled until restart.
			c->applies = XRT_STATUS_MODE_3D_APPLIES_RESTART_APP;
			snprintf(c->reason, sizeof(c->reason),
			         "this screen came after running apps were sized: they show it resampled until "
			         "they restart");
		} else if (m->view_count > 2) {
			c->applies = XRT_STATUS_MODE_3D_APPLIES_NOW;
			snprintf(c->reason, sizeof(c->reason), "%u views: stereo apps show 2D", m->view_count);
		} else {
			c->applies = XRT_STATUS_MODE_3D_APPLIES_NOW;
		}
	}
}
