// Copyright 2023, Collabora, Ltd.
// Copyright 2024-2025, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Helper to implement @ref xrt_system.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup aux_util
 */

#include "xrt/xrt_compositor.h"

#include "os/os_threading.h"

#include "util/u_system.h"
#include "util/u_session.h"
#include "util/u_misc.h"
#include "util/u_logging.h"

#include <stdio.h>

/*
 *
 * Helpers.
 *
 */

static inline struct u_system *
u_system(struct xrt_system *xsys)
{
	return (struct u_system *)xsys;
}


/*
 *
 * Member functions.
 *
 */

static xrt_result_t
push_event(struct xrt_session_event_sink *xses, const union xrt_session_event *xse)
{
	struct u_system *usys = container_of(xses, struct u_system, broadcast);

	u_system_broadcast_event(usys, xse);

	return XRT_SUCCESS;
}

static xrt_result_t
create_session(struct xrt_system *xsys,
               const struct xrt_session_info *xsi,
               struct xrt_session **out_xs,
               struct xrt_compositor_native **out_xcn)
{
	struct u_system *usys = u_system(xsys);
	xrt_result_t xret = XRT_SUCCESS;

	if (out_xcn != NULL && usys->xsysc == NULL) {
		U_LOG_E("No system compositor in system, can't create native compositor.");
		return XRT_ERROR_COMPOSITOR_NOT_SUPPORTED;
	}

	struct u_session *us = u_session_create(usys);

	if (out_xcn != NULL) {
		// Normal path: create compositor for the caller.
		xret = xrt_syscomp_create_native_compositor( //
		    usys->xsysc,                             //
		    xsi,                                     //
		    &us->sink,                               //
		    out_xcn);                                //
		if (xret != XRT_SUCCESS) {
			goto err;
		}
	} else if (usys->xsysc != NULL) {
		// Headless path: no compositor returned to caller, but still
		// register with the system compositor so event broadcasts
		// (RENDERING_MODE_CHANGED, etc.) reach this session via IPC.
		// Without this, u_system_broadcast_event on the server skips
		// headless sessions because they have no server-side entry.
		xret = xrt_syscomp_create_native_compositor( //
		    usys->xsysc,                             //
		    xsi,                                     //
		    &us->sink,                               //
		    &us->headless_xcn);                      //
		if (xret != XRT_SUCCESS) {
			goto err;
		}
	}

	*out_xs = &us->base;

	return XRT_SUCCESS;

err:
	return xret;
}

static void
destroy(struct xrt_system *xsys)
{
	struct u_system *usys = u_system(xsys);

	// Use shared fini function.
	u_system_fini(usys);

	free(usys);
}


/*
 *
 * 'Exported' functions.
 *
 */

struct u_system *
u_system_create(void)
{
	struct u_system *usys = U_TYPED_CALLOC(struct u_system);

	// Use init function, then add the common destroy function.
	if (!u_system_init(usys, destroy)) {
		free(usys);
		return NULL;
	}

	return usys;
}

bool
u_system_init(struct u_system *usys, void (*destroy_fn)(struct xrt_system *))
{
	// xrt_system fields.
	usys->base.create_session = create_session;
	usys->base.destroy = destroy_fn;

	// xrt_session_event_sink fields.
	usys->broadcast.push_event = push_event;

	usys->sessions.capacity = 2;
	usys->sessions.pairs = U_TYPED_ARRAY_CALLOC(struct u_system_session_pair, usys->sessions.capacity);
	if (usys->sessions.pairs == NULL) {
		U_LOG_E("Failed to allocate session array");
		return false;
	}

	// u_system fields.
	XRT_MAYBE_UNUSED int ret = os_mutex_init(&usys->sessions.mutex);
	assert(ret == 0);

	return true;
}

void
u_system_fini(struct u_system *usys)
{
	// Just in case, should never happen.
	if (usys->sessions.count > 0) {
		U_LOG_E("Number of sessions not zero, things will crash!");
	}

	free(usys->sessions.pairs);
	usys->sessions.count = 0;

	// Mutex needs to be destroyed.
	os_mutex_destroy(&usys->sessions.mutex);
}

void
u_system_add_session(struct u_system *usys, struct xrt_session *xs, struct xrt_session_event_sink *xses)
{
	assert(xs != NULL);
	assert(xses != NULL);

	os_mutex_lock(&usys->sessions.mutex);

	const uint32_t new_count = usys->sessions.count + 1;

	if (new_count > usys->sessions.capacity) {
		usys->sessions.capacity *= 2;
		const size_t size = usys->sessions.capacity * sizeof(*usys->sessions.pairs);
		struct u_system_session_pair *tmp = realloc(usys->sessions.pairs, size);
		if (tmp == NULL) {
			U_LOG_E("Failed to reallocate session array");
			goto add_unlock;
		}
		usys->sessions.pairs = tmp;
	}

	usys->sessions.pairs[usys->sessions.count++] = (struct u_system_session_pair){xs, xses};

add_unlock:
	os_mutex_unlock(&usys->sessions.mutex);
}

void
u_system_remove_session(struct u_system *usys, struct xrt_session *xs, struct xrt_session_event_sink *xses)
{
	os_mutex_lock(&usys->sessions.mutex);

	uint32_t count = usys->sessions.count;
	uint32_t dst = 0;

	// Find where the session we are removing is.
	while (dst < count) {
		if (usys->sessions.pairs[dst].xs == xs) {
			break;
		} else {
			dst++;
		}
	}

	// Guards against empty array as well as not finding the session.
	if (dst >= count) {
		U_LOG_E("Could not find session to remove!");
		goto remove_unlock;
	}

	// Should not be true with above check.
	assert(count > 0);

	/*
	 * Do copies if we are not removing the last session,
	 * this also guards against uint32_t::max.
	 */
	if (dst < count - 1) {
		// Copy from every follow down one.
		for (uint32_t src = dst + 1; src < count; src++, dst++) {
			usys->sessions.pairs[dst] = usys->sessions.pairs[src];
		}
	}

	count--;
	// This ensures that the memory returned in add is always zero initialized.
	U_ZERO(&usys->sessions.pairs[count]);

	usys->sessions.count = count;

remove_unlock:
	os_mutex_unlock(&usys->sessions.mutex);
}

void
u_system_broadcast_event(struct u_system *usys, const union xrt_session_event *xse)
{
	xrt_result_t xret;

	os_mutex_lock(&usys->sessions.mutex);

	for (uint32_t i = 0; i < usys->sessions.count; i++) {
		xret = xrt_session_event_sink_push(usys->sessions.pairs[i].xses, xse);
		if (xret != XRT_SUCCESS) {
			U_LOG_W("Failed to push event to session, dropping.");
		}
	}

	os_mutex_unlock(&usys->sessions.mutex);
}

void
u_system_set_system_compositor(struct u_system *usys, struct xrt_system_compositor *xsysc)
{
	assert(usys->xsysc == NULL);

	usys->xsysc = xsysc;
}

void
u_system_fill_properties(struct u_system *usys, const char *name)
{
	usys->base.properties.vendor_id = 42;
	// The magical 247 number, is to silence warnings.
	snprintf(usys->base.properties.name, XRT_MAX_SYSTEM_NAME_SIZE, "DisplayXR: %.*s", 247, name);
	usys->base.properties.form_factor = XRT_FORM_FACTOR_HMD;
}
