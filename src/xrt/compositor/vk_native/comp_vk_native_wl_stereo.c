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
comp_vk_native_wl_stereo_create(void *wl_display, void *wl_surface, const char **out_reason)
{
	const char *reason = "no Wayland surface to declare";
	struct comp_vk_native_wl_stereo *s = NULL;
	if (wl_display == NULL || wl_surface == NULL) {
		goto fail;
	}
	reason = "out of memory";
	s = U_TYPED_CALLOC(struct comp_vk_native_wl_stereo);
	if (s == NULL) {
		goto fail;
	}
	s->display = (struct wl_display *)wl_display;
	s->surface = (struct wl_surface *)wl_surface;

	// A private queue: the app dispatches the default queue on its own thread,
	// and none of our objects' events may ever reach its listeners.
	reason = "could not make a private Wayland event queue";
	s->queue = wl_display_create_queue(s->display);
	s->wrapper = wl_proxy_create_wrapper(s->display);
	if (s->queue == NULL || s->wrapper == NULL) {
		goto fail;
	}
	wl_proxy_set_queue((struct wl_proxy *)s->wrapper, s->queue);
	s->registry = wl_display_get_registry(s->wrapper);
	wl_registry_add_listener(s->registry, &wl_stereo_registry_listener, s);
	reason = "the Wayland roundtrip for the compositor's globals failed";
	if (wl_display_roundtrip_queue(s->display, s->queue) < 0) {
		goto fail;
	}
	if (s->manager == NULL) {
		reason = s->offered_version == 0
		             ? "the compositor offers no kde_stereo_content_manager_v1 (no stereo declaration: "
		               "not Stereo KWin)"
		             : "the compositor offers kde_stereo_content_manager_v1 version 1 only, where a declared "
		               "surface keeps its buffer's width (version 2 or later is needed)";
		goto fail;
	}
	return s;

fail:
	if (out_reason != NULL) {
		*out_reason = reason;
	}
	comp_vk_native_wl_stereo_destroy(&s);
	return NULL;
}

void
comp_vk_native_wl_stereo_set_full_sbs(struct comp_vk_native_wl_stereo *s, bool full_sbs)
{
	if (s == NULL || s->manager == NULL) {
		return;
	}
	if (s->declaration == NULL) {
		if (!full_sbs) {
			return; // never declared: already none
		}
		s->declaration = kde_stereo_content_manager_v1_create(s->manager, s->surface);
		if (s->declaration == NULL) {
			return;
		}
	}
	kde_stereo_content_v1_set_content(s->declaration, full_sbs ? KDE_STEREO_CONTENT_V1_CONTENT_SIDE_BY_SIDE_FULL
	                                                           : KDE_STEREO_CONTENT_V1_CONTENT_NONE);
	s->full_sbs = full_sbs;
	wl_display_flush(s->display);
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
