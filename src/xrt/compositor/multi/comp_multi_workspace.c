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
