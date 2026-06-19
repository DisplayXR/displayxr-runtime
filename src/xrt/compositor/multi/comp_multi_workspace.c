// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Generic workspace-controller chrome registry (macOS shell Tier 1, #48).
 * @ingroup comp_multi
 */

#include "comp_multi_workspace.h"

#include "xrt/xrt_compositor.h" // xrt_swapchain_reference

#include "os/os_threading.h"

#include <string.h>

// Chrome decorates managed content windows; a handful is plenty for Tier 1.
#define CHROME_MAX_ENTRIES 16

struct chrome_entry
{
	struct xrt_compositor *target_xc; //!< Key: per-client compositor (== ics->xc == multi_compositor). NULL = free.
	struct xrt_swapchain *xsc;        //!< Strong ref to the controller's chrome swapchain.
	uint32_t swapchain_id;            //!< Controller-side IPC swapchain id (unregister key).
	struct comp_multi_chrome_layout layout;
	bool layout_valid;
};

// One process-global table; the service is a single process. The mutex is
// initialized on first use (no global ctor in C) — guarded by g_lock_once.
static struct os_mutex g_lock;
static bool g_lock_ready = false;
static struct chrome_entry g_entries[CHROME_MAX_ENTRIES];

static void
ensure_lock(void)
{
	// Benign race on first call is avoided in practice: the first chrome RPC
	// arrives long after the single-threaded service start. os_mutex_init is
	// idempotent enough for our single init; keep it simple.
	if (!g_lock_ready) {
		os_mutex_init(&g_lock);
		g_lock_ready = true;
	}
}

// Caller must hold g_lock.
static struct chrome_entry *
find_locked(struct xrt_compositor *target_xc)
{
	if (target_xc == NULL) {
		return NULL;
	}
	for (int i = 0; i < CHROME_MAX_ENTRIES; i++) {
		if (g_entries[i].target_xc == target_xc) {
			return &g_entries[i];
		}
	}
	return NULL;
}

// Caller must hold g_lock.
static struct chrome_entry *
find_or_add_locked(struct xrt_compositor *target_xc)
{
	struct chrome_entry *e = find_locked(target_xc);
	if (e != NULL) {
		return e;
	}
	for (int i = 0; i < CHROME_MAX_ENTRIES; i++) {
		if (g_entries[i].target_xc == NULL) {
			g_entries[i].target_xc = target_xc;
			return &g_entries[i];
		}
	}
	return NULL; // Table full.
}

// Caller must hold g_lock. Releases the strong ref and zeroes the slot.
static void
free_entry_locked(struct chrome_entry *e)
{
	if (e->xsc != NULL) {
		xrt_swapchain_reference(&e->xsc, NULL);
	}
	memset(e, 0, sizeof(*e));
}

void
comp_multi_workspace_chrome_register(struct xrt_compositor *target_xc,
                                     uint32_t swapchain_id,
                                     struct xrt_swapchain *xsc)
{
	if (target_xc == NULL || xsc == NULL) {
		return;
	}
	ensure_lock();
	os_mutex_lock(&g_lock);
	struct chrome_entry *e = find_or_add_locked(target_xc);
	if (e != NULL) {
		// Swap the strong ref (drops a prior chrome swapchain if re-registering).
		xrt_swapchain_reference(&e->xsc, xsc);
		e->swapchain_id = swapchain_id;
	}
	os_mutex_unlock(&g_lock);
}

void
comp_multi_workspace_chrome_unregister_by_id(uint32_t swapchain_id)
{
	ensure_lock();
	os_mutex_lock(&g_lock);
	for (int i = 0; i < CHROME_MAX_ENTRIES; i++) {
		if (g_entries[i].target_xc != NULL && g_entries[i].xsc != NULL &&
		    g_entries[i].swapchain_id == swapchain_id) {
			free_entry_locked(&g_entries[i]);
		}
	}
	os_mutex_unlock(&g_lock);
}

void
comp_multi_workspace_chrome_set_layout(struct xrt_compositor *target_xc,
                                       const struct comp_multi_chrome_layout *layout)
{
	if (target_xc == NULL || layout == NULL) {
		return;
	}
	ensure_lock();
	os_mutex_lock(&g_lock);
	struct chrome_entry *e = find_or_add_locked(target_xc);
	if (e != NULL) {
		e->layout = *layout;
		e->layout_valid = true;
	}
	os_mutex_unlock(&g_lock);
}

void
comp_multi_workspace_chrome_update_pose(struct xrt_compositor *target_xc, const struct xrt_pose *pose_in_client)
{
	if (target_xc == NULL || pose_in_client == NULL) {
		return;
	}
	ensure_lock();
	os_mutex_lock(&g_lock);
	struct chrome_entry *e = find_locked(target_xc);
	if (e != NULL && e->layout_valid) {
		e->layout.pose_in_client = *pose_in_client;
	}
	os_mutex_unlock(&g_lock);
}

bool
comp_multi_workspace_chrome_get(struct xrt_compositor *target_xc,
                                struct xrt_swapchain **out_xsc,
                                struct comp_multi_chrome_layout *out_layout)
{
	bool found = false;
	ensure_lock();
	os_mutex_lock(&g_lock);
	struct chrome_entry *e = find_locked(target_xc);
	if (e != NULL && e->layout_valid && e->xsc != NULL) {
		if (out_xsc != NULL) {
			*out_xsc = e->xsc;
		}
		if (out_layout != NULL) {
			*out_layout = e->layout;
		}
		found = true;
	}
	os_mutex_unlock(&g_lock);
	return found;
}

void
comp_multi_workspace_chrome_clear(struct xrt_compositor *target_xc)
{
	if (target_xc == NULL) {
		return;
	}
	ensure_lock();
	os_mutex_lock(&g_lock);
	struct chrome_entry *e = find_locked(target_xc);
	if (e != NULL) {
		free_entry_locked(e);
	}
	os_mutex_unlock(&g_lock);
}


/*
 *
 * Session-global cursor + overlays (#48 Phase 2).
 *
 */

// All guarded by g_lock (shared with the chrome table; the registry is one
// process-global object and contention is trivial).
static struct
{
	struct xrt_swapchain *xsc; // strong ref
	struct comp_multi_cursor_state state;
} g_cursor;

static int32_t g_pointer_x;
static int32_t g_pointer_y;

struct overlay_entry
{
	bool used;
	struct xrt_swapchain *xsc; // strong ref
	struct comp_multi_overlay_state state;
};
static struct overlay_entry g_overlays[COMP_MULTI_WORKSPACE_MAX_OVERLAYS];

void
comp_multi_workspace_set_cursor(struct xrt_swapchain *xsc, float hot_x, float hot_y, float size_meters, bool visible)
{
	ensure_lock();
	os_mutex_lock(&g_lock);
	xrt_swapchain_reference(&g_cursor.xsc, xsc); // NULL clears
	g_cursor.state.hot_x = hot_x;
	g_cursor.state.hot_y = hot_y;
	g_cursor.state.size_meters = size_meters;
	g_cursor.state.visible = visible;
	os_mutex_unlock(&g_lock);
}

void
comp_multi_workspace_set_cursor_depth(float hit_z_m, bool over_window)
{
	ensure_lock();
	os_mutex_lock(&g_lock);
	g_cursor.state.hit_z_m = hit_z_m;
	g_cursor.state.over_window = over_window;
	os_mutex_unlock(&g_lock);
}

bool
comp_multi_workspace_get_cursor(struct xrt_swapchain **out_xsc, struct comp_multi_cursor_state *out)
{
	bool found = false;
	ensure_lock();
	os_mutex_lock(&g_lock);
	if (g_cursor.xsc != NULL && g_cursor.state.visible) {
		if (out_xsc != NULL) {
			*out_xsc = g_cursor.xsc;
		}
		if (out != NULL) {
			*out = g_cursor.state;
		}
		found = true;
	}
	os_mutex_unlock(&g_lock);
	return found;
}

void
comp_multi_workspace_set_pointer_px(int32_t x, int32_t y)
{
	ensure_lock();
	os_mutex_lock(&g_lock);
	g_pointer_x = x;
	g_pointer_y = y;
	os_mutex_unlock(&g_lock);
}

void
comp_multi_workspace_get_pointer_px(int32_t *out_x, int32_t *out_y)
{
	ensure_lock();
	os_mutex_lock(&g_lock);
	if (out_x != NULL) {
		*out_x = g_pointer_x;
	}
	if (out_y != NULL) {
		*out_y = g_pointer_y;
	}
	os_mutex_unlock(&g_lock);
}

void
comp_multi_workspace_set_overlay(uint32_t overlay_id,
                                 struct xrt_swapchain *xsc,
                                 float anchor_x,
                                 float anchor_y,
                                 float pivot_x,
                                 float pivot_y,
                                 float size_w_m,
                                 float size_h_m,
                                 bool visible,
                                 bool stereo_sbs)
{
	ensure_lock();
	os_mutex_lock(&g_lock);

	// Find existing slot for this id, or a free slot.
	struct overlay_entry *slot = NULL;
	struct overlay_entry *free_slot = NULL;
	for (int i = 0; i < COMP_MULTI_WORKSPACE_MAX_OVERLAYS; i++) {
		if (g_overlays[i].used && g_overlays[i].state.overlay_id == overlay_id) {
			slot = &g_overlays[i];
			break;
		}
		if (!g_overlays[i].used && free_slot == NULL) {
			free_slot = &g_overlays[i];
		}
	}

	if (!visible || xsc == NULL) {
		// Remove.
		if (slot != NULL) {
			xrt_swapchain_reference(&slot->xsc, NULL);
			slot->used = false;
			memset(&slot->state, 0, sizeof(slot->state));
		}
		os_mutex_unlock(&g_lock);
		return;
	}

	if (slot == NULL) {
		slot = free_slot;
	}
	if (slot != NULL) {
		xrt_swapchain_reference(&slot->xsc, xsc);
		slot->used = true;
		slot->state.overlay_id = overlay_id;
		slot->state.anchor_x = anchor_x;
		slot->state.anchor_y = anchor_y;
		slot->state.pivot_x = pivot_x;
		slot->state.pivot_y = pivot_y;
		slot->state.size_w_m = size_w_m;
		slot->state.size_h_m = size_h_m;
		slot->state.stereo_sbs = stereo_sbs;
	}
	os_mutex_unlock(&g_lock);
}

uint32_t
comp_multi_workspace_copy_overlays(struct comp_multi_overlay_state *out_states,
                                   struct xrt_swapchain **out_xscs,
                                   uint32_t max)
{
	uint32_t count = 0;
	ensure_lock();
	os_mutex_lock(&g_lock);
	// Emit in ascending overlay_id so low ids composite behind high ids.
	// Simple selection over a tiny fixed table — order doesn't matter for the
	// flat z=0 overlays in v1, but keep it deterministic.
	for (uint32_t pass = 0; pass < (uint32_t)COMP_MULTI_WORKSPACE_MAX_OVERLAYS && count < max; pass++) {
		// Find the (pass)-th smallest used id by linear scan.
		struct overlay_entry *best = NULL;
		for (int i = 0; i < COMP_MULTI_WORKSPACE_MAX_OVERLAYS; i++) {
			if (!g_overlays[i].used || g_overlays[i].xsc == NULL) {
				continue;
			}
			// Skip ids already emitted.
			bool emitted = false;
			for (uint32_t k = 0; k < count; k++) {
				if (out_states[k].overlay_id == g_overlays[i].state.overlay_id) {
					emitted = true;
					break;
				}
			}
			if (emitted) {
				continue;
			}
			if (best == NULL || g_overlays[i].state.overlay_id < best->state.overlay_id) {
				best = &g_overlays[i];
			}
		}
		if (best == NULL) {
			break;
		}
		out_states[count] = best->state;
		out_xscs[count] = best->xsc;
		count++;
	}
	os_mutex_unlock(&g_lock);
	return count;
}
