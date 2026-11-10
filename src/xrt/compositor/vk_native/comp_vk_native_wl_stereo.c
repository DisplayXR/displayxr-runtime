// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Full side-by-side declaration of the presented Wayland surface.
 * @ingroup comp_vk_native
 */

#include "comp_vk_native_wl_stereo.h"

#include "util/u_logging.h"
#include "util/u_misc.h"

#include "kde-stereo-content-v1-client-protocol.h"

#include <wayland-client.h>

#include <stdlib.h>
#include <string.h>

/*!
 * The version this client speaks. Version 2 is the first in which a declared
 * surface without a viewport destination is one view wide (half its buffer's
 * width), which is what keeps an app-owned window at its own size while the
 * runtime's buffer holds both views. Version 3 adds only desktop depth.
 */
#define WL_STEREO_VERSION 2u

struct comp_vk_native_wl_stereo
{
	struct wl_display *display;
	struct wl_event_queue *queue;
	struct wl_display *wrapper; //!< display proxy wrapper bound to @ref queue
	struct wl_registry *registry;
	struct kde_stereo_content_manager_v1 *manager;
	uint32_t offered_version;
	uint32_t version;

	struct wl_surface *surface;
	//! The registry was asked for the compositor's globals (once, on the first set_full_sbs(true)).
	bool bound;
	//! Why binding failed, a static sentence; NULL while it has not or succeeded.
	const char *missing;
	//! The surface's declaration object, made on the first set_full_sbs.
	struct kde_stereo_content_v1 *declaration;
	bool full_sbs;
};

static void
wl_stereo_registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *iface, uint32_t version)
{
	struct comp_vk_native_wl_stereo *s = data;
	if (strcmp(iface, kde_stereo_content_manager_v1_interface.name) == 0 && s->manager == NULL) {
		s->offered_version = version;
		if (version < WL_STEREO_VERSION) {
			return;
		}
		s->version = WL_STEREO_VERSION;
		s->manager = wl_registry_bind(registry, name, &kde_stereo_content_manager_v1_interface, s->version);
	}
}

static void
wl_stereo_registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	(void)data;
	(void)registry;
	(void)name;
}

static const struct wl_registry_listener wl_stereo_registry_listener = {
    .global = wl_stereo_registry_global,
    .global_remove = wl_stereo_registry_global_remove,
};

struct comp_vk_native_wl_stereo *
comp_vk_native_wl_stereo_create(void *wl_display, void *wl_surface)
{
	if (wl_display == NULL || wl_surface == NULL) {
		return NULL;
	}
	struct comp_vk_native_wl_stereo *s = U_TYPED_CALLOC(struct comp_vk_native_wl_stereo);
	if (s == NULL) {
		return NULL;
	}
	s->display = (struct wl_display *)wl_display;
	s->surface = (struct wl_surface *)wl_surface;
	return s;
}

//! Ask the compositor for its stereo declaration global, once; sets s->missing when it has none.
static bool
wl_stereo_bind(struct comp_vk_native_wl_stereo *s)
{
	if (s->bound) {
		return s->manager != NULL;
	}
	s->bound = true;

	// A private queue: the app dispatches the default queue on its own thread,
	// and none of our objects' events may ever reach its listeners.
	s->missing = "could not make a private Wayland event queue";
	s->queue = wl_display_create_queue(s->display);
	s->wrapper = wl_proxy_create_wrapper(s->display);
	if (s->queue == NULL || s->wrapper == NULL) {
		return false;
	}
	wl_proxy_set_queue((struct wl_proxy *)s->wrapper, s->queue);
	s->registry = wl_display_get_registry(s->wrapper);
	wl_registry_add_listener(s->registry, &wl_stereo_registry_listener, s);
	s->missing = "the Wayland roundtrip for the compositor's globals failed";
	if (wl_display_roundtrip_queue(s->display, s->queue) < 0) {
		return false;
	}
	if (s->manager == NULL) {
		s->missing = s->offered_version == 0
		                 ? "the compositor offers no kde_stereo_content_manager_v1 (no stereo declaration: "
		                   "not Stereo KWin)"
		                 : "the compositor offers kde_stereo_content_manager_v1 version 1 only, where a "
		                   "declared surface keeps its buffer's width (version 2 or later is needed)";
		return false;
	}
	s->missing = NULL;
	return true;
}

bool
comp_vk_native_wl_stereo_set_full_sbs(struct comp_vk_native_wl_stereo *s, bool full_sbs, const char **out_reason)
{
	if (s == NULL) {
		if (out_reason != NULL) {
			*out_reason = "no Wayland surface to declare";
		}
		return false;
	}
	if (s->declaration == NULL) {
		if (!full_sbs) {
			return true; // never declared: already none
		}
		// The first declaration is the first time the registry is touched: a
		// session that never presents full side by side adds no traffic.
		if (!wl_stereo_bind(s)) {
			if (out_reason != NULL) {
				*out_reason = s->missing;
			}
			return false;
		}
		s->declaration = kde_stereo_content_manager_v1_create(s->manager, s->surface);
		if (s->declaration == NULL) {
			if (out_reason != NULL) {
				*out_reason = "the compositor could not make the declaration object";
			}
			return false;
		}
	}
	kde_stereo_content_v1_set_content(s->declaration, full_sbs ? KDE_STEREO_CONTENT_V1_CONTENT_SIDE_BY_SIDE_FULL
	                                                           : KDE_STEREO_CONTENT_V1_CONTENT_NONE);
	s->full_sbs = full_sbs;
	wl_display_flush(s->display);
	return true;
}

uint32_t
comp_vk_native_wl_stereo_version(const struct comp_vk_native_wl_stereo *s)
{
	return s != NULL ? s->version : 0;
}

void
comp_vk_native_wl_stereo_destroy(struct comp_vk_native_wl_stereo **s_ptr)
{
	if (s_ptr == NULL || *s_ptr == NULL) {
		return;
	}
	struct comp_vk_native_wl_stereo *s = *s_ptr;
	if (s->declaration != NULL) {
		// Resets the content to none with the surface's next commit.
		kde_stereo_content_v1_destroy(s->declaration);
	}
	if (s->manager != NULL) {
		kde_stereo_content_manager_v1_destroy(s->manager);
	}
	if (s->registry != NULL) {
		wl_registry_destroy(s->registry);
	}
	if (s->wrapper != NULL) {
		wl_proxy_wrapper_destroy(s->wrapper);
	}
	if (s->display != NULL) {
		wl_display_flush(s->display);
	}
	if (s->queue != NULL) {
		wl_event_queue_destroy(s->queue);
	}
	free(s);
	*s_ptr = NULL;
}
