// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Move-synchronised re-weave — the frame tag subsurface (#1748).
 * @ingroup comp_vk_native
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE // memfd_create
#endif

#include "comp_vk_native_wl_move_sync.h"

#include "util/u_logging.h"
#include "util/u_misc.h"

#include <wayland-client.h>

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

struct comp_vk_native_wl_move_sync
{
	struct wl_display *display;
	struct wl_event_queue *queue;
	struct wl_display *wrapper; //!< display proxy wrapper bound to @ref queue
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_subcompositor *subcompositor;
	struct wl_shm *shm;

	struct wl_surface *tag_surface;
	struct wl_subsurface *tag_subsurface;
	struct wl_buffer *tag_buffer;

	int32_t last_x, last_y;
	bool have_last;
	//! The tag surface has its pixel attached (see set_mapped).
	bool mapped;
};

static void
ms_registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *iface, uint32_t version)
{
	struct comp_vk_native_wl_move_sync *ms = data;
	if (strcmp(iface, wl_compositor_interface.name) == 0 && ms->compositor == NULL) {
		ms->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, version < 4 ? version : 4);
	} else if (strcmp(iface, wl_subcompositor_interface.name) == 0 && ms->subcompositor == NULL) {
		ms->subcompositor = wl_registry_bind(registry, name, &wl_subcompositor_interface, 1);
	} else if (strcmp(iface, wl_shm_interface.name) == 0 && ms->shm == NULL) {
		ms->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	}
}

static void
ms_registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	(void)data;
	(void)registry;
	(void)name;
}

static const struct wl_registry_listener ms_registry_listener = {
    .global = ms_registry_global,
    .global_remove = ms_registry_global_remove,
};

//! One fully transparent ARGB8888 pixel. Nothing of the tag is ever visible.
static struct wl_buffer *
ms_create_clear_pixel(struct comp_vk_native_wl_move_sync *ms)
{
	int fd = memfd_create("dxr-move-sync-tag", MFD_CLOEXEC);
	if (fd < 0) {
		return NULL;
	}
	if (ftruncate(fd, 4) != 0) {
		close(fd);
		return NULL;
	}
	// ftruncate zero-fills: the pixel is (0,0,0,0).
	struct wl_shm_pool *pool = wl_shm_create_pool(ms->shm, fd, 4);
	struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, 1, 1, 4, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buf;
}

struct comp_vk_native_wl_move_sync *
comp_vk_native_wl_move_sync_create(void *wl_display, void *wl_surface)
{
	if (wl_display == NULL || wl_surface == NULL) {
		return NULL;
	}
	struct comp_vk_native_wl_move_sync *ms = U_TYPED_CALLOC(struct comp_vk_native_wl_move_sync);
	if (ms == NULL) {
		return NULL;
	}
	ms->display = (struct wl_display *)wl_display;

	// A private queue: the app dispatches the default queue on its own thread,
	// and none of our objects' events may ever reach its listeners.
	ms->queue = wl_display_create_queue(ms->display);
	ms->wrapper = wl_proxy_create_wrapper(ms->display);
	if (ms->queue == NULL || ms->wrapper == NULL) {
		goto fail;
	}
	wl_proxy_set_queue((struct wl_proxy *)ms->wrapper, ms->queue);
	ms->registry = wl_display_get_registry(ms->wrapper);
	wl_registry_add_listener(ms->registry, &ms_registry_listener, ms);
	if (wl_display_roundtrip_queue(ms->display, ms->queue) < 0) {
		goto fail;
	}
	if (ms->compositor == NULL || ms->subcompositor == NULL || ms->shm == NULL) {
		U_LOG_W("wl_move_sync: compositor lacks wl_compositor/wl_subcompositor/wl_shm — move sync off");
		goto fail;
	}

	ms->tag_buffer = ms_create_clear_pixel(ms);
	if (ms->tag_buffer == NULL) {
		goto fail;
	}
	ms->tag_surface = wl_compositor_create_surface(ms->compositor);
	ms->tag_subsurface =
	    wl_subcompositor_get_subsurface(ms->subcompositor, ms->tag_surface, (struct wl_surface *)wl_surface);
	if (ms->tag_surface == NULL || ms->tag_subsurface == NULL) {
		goto fail;
	}
	// Below the content, so it is painted under it (and it is clear anyway),
	// and with an empty input region, so it never takes a pointer event.
	wl_subsurface_place_below(ms->tag_subsurface, (struct wl_surface *)wl_surface);
	wl_subsurface_set_sync(ms->tag_subsurface);
	wl_subsurface_set_position(ms->tag_subsurface, 0, 0);
	struct wl_region *empty = wl_compositor_create_region(ms->compositor);
	wl_surface_set_input_region(ms->tag_surface, empty);
	wl_region_destroy(empty);
	wl_surface_attach(ms->tag_surface, ms->tag_buffer, 0, 0);
	wl_surface_damage(ms->tag_surface, 0, 0, 1, 1);
	// Synchronised: cached until the bound surface's next commit.
	wl_surface_commit(ms->tag_surface);
	wl_display_flush(ms->display);

	ms->mapped = true;
	U_LOG_W("wl_move_sync: frame tag subsurface created (#1748)");
	return ms;

fail:
	comp_vk_native_wl_move_sync_destroy(&ms);
	return NULL;
}

void
comp_vk_native_wl_move_sync_tag(struct comp_vk_native_wl_move_sync *ms,
                                int32_t content_logical_x,
                                int32_t content_logical_y)
{
	if (ms == NULL || ms->tag_subsurface == NULL) {
		return;
	}
	// Drain our queue (buffer release, etc.) without blocking.
	wl_display_dispatch_queue_pending(ms->display, ms->queue);

	const int32_t tx = comp_vk_native_wl_move_sync_encode(content_logical_x);
	const int32_t ty = comp_vk_native_wl_move_sync_encode(content_logical_y);
	if (ms->have_last && tx == ms->last_x && ty == ms->last_y) {
		return; // unchanged: the cached position rides along with every commit
	}
	ms->last_x = tx;
	ms->last_y = ty;
	ms->have_last = true;
	// Parent state of a synchronised subsurface: it lands with the bound
	// surface's next commit, which is the present of the frame just woven.
	wl_subsurface_set_position(ms->tag_subsurface, tx, ty);
}

void
comp_vk_native_wl_move_sync_set_mapped(struct comp_vk_native_wl_move_sync *ms, bool mapped)
{
	if (ms == NULL || ms->tag_surface == NULL || mapped == ms->mapped) {
		return;
	}
	ms->mapped = mapped;
	// Synchronised: the attach is cached until the bound surface's next
	// commit, so the tag appears or disappears with a presented frame.
	wl_surface_attach(ms->tag_surface, mapped ? ms->tag_buffer : NULL, 0, 0);
	if (mapped) {
		wl_surface_damage(ms->tag_surface, 0, 0, 1, 1);
		ms->have_last = false; // re-send the position with the next tag
	}
	wl_surface_commit(ms->tag_surface);
	U_LOG_I("wl_move_sync: frame tag %s", mapped ? "mapped" : "unmapped (window covers its monitor)");
}

void
comp_vk_native_wl_move_sync_destroy(struct comp_vk_native_wl_move_sync **ms_ptr)
{
	if (ms_ptr == NULL || *ms_ptr == NULL) {
		return;
	}
	struct comp_vk_native_wl_move_sync *ms = *ms_ptr;
	if (ms->tag_subsurface != NULL) {
		wl_subsurface_destroy(ms->tag_subsurface);
	}
	if (ms->tag_surface != NULL) {
		wl_surface_destroy(ms->tag_surface);
	}
	if (ms->tag_buffer != NULL) {
		wl_buffer_destroy(ms->tag_buffer);
	}
	if (ms->shm != NULL) {
		wl_shm_destroy(ms->shm);
	}
	if (ms->subcompositor != NULL) {
		wl_subcompositor_destroy(ms->subcompositor);
	}
	if (ms->compositor != NULL) {
		wl_compositor_destroy(ms->compositor);
	}
	if (ms->registry != NULL) {
		wl_registry_destroy(ms->registry);
	}
	if (ms->wrapper != NULL) {
		wl_proxy_wrapper_destroy(ms->wrapper);
	}
	if (ms->display != NULL) {
		wl_display_flush(ms->display);
	}
	if (ms->queue != NULL) {
		wl_event_queue_destroy(ms->queue);
	}
	free(ms);
	*ms_ptr = NULL;
}
