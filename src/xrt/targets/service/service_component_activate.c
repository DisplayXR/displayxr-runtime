// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The service's live-activation slot (see service_component_activate.h).
 * @ingroup targets_service
 */

#include "service_component_activate.h"

#include "xrt/xrt_component_license.h"

#include "os/os_threading.h"
#include "os/os_time.h"
#include "util/u_activation_url.h"
#include "util/u_logging.h"

#include <stdio.h>
#include <string.h>

//! A staging nobody touched for this long may be taken over by another connection.
#define STAGING_STALE_NS (10ull * 1000ull * 1000ull * 1000ull)
//! An answer nobody collected for this long may be dropped for another connection.
#define DONE_STALE_NS (5ull * 1000ull * 1000ull * 1000ull)

enum slot_state
{
	SLOT_IDLE = 0,
	SLOT_STAGING,
	SLOT_RUNNING,
	SLOT_DONE,
};

static struct
{
	bool lock_ready;
	struct os_mutex lock;

	bool armed;
	service_component_activate_deliver_fn deliver;
	void *userdata;

	enum slot_state state;
	uint64_t owner;
	bool owner_gone; //!< the owner closed while RUNNING: its answer is dropped
	uint64_t touched_ns;

	uint32_t staged;
	char token[XRT_COMPONENT_ACTIVATION_TOKEN_MAX];
	uint32_t class_id;
	char id[U_ACTIVATION_ID_MAX];

	bool thread_live; //!< a worker was started and not yet joined
	struct os_thread thread;

	uint32_t delivery; //!< enum service_component_activate_delivery
	uint32_t result;   //!< enum target_component_activate_result
	struct xrt_component_license_state lic;
	char err[XRT_COMPONENT_ACTIVATION_ERROR_MAX];
} g;

static void
wipe_token_locked(void)
{
	volatile char *p = (volatile char *)g.token;
	for (size_t i = 0; i < sizeof(g.token); i++) {
		p[i] = 0;
	}
	g.staged = 0;
}

static void
idle_locked(void)
{
	wipe_token_locked();
	g.state = SLOT_IDLE;
	g.owner = 0;
	g.owner_gone = false;
	g.id[0] = '\0';
	g.class_id = 0;
}

static void *
worker(void *ptr)
{
	(void)ptr;
	// g.token / g.id / g.class_id are frozen while RUNNING: only this thread
	// reads them, and nothing writes them until it has moved the state on.
	uint32_t result = 0;
	struct xrt_component_license_state lic;
	char err[XRT_COMPONENT_ACTIVATION_ERROR_MAX] = {0};
	xrt_component_license_state_init(&lic);
	const enum service_component_activate_delivery d =
	    g.deliver(g.userdata, g.class_id, g.id, g.token, g.staged, &result, &lic, err, sizeof(err));
	err[sizeof(err) - 1] = '\0';
	xrt_component_license_state_sanitize(&lic);

	os_mutex_lock(&g.lock);
	// Lifecycle line, once per activation. Never the token.
	U_LOG_W("component activation: class %u '%s': %s, licence %s", (unsigned)g.class_id, g.id,
	        d == SERVICE_COMPONENT_ACTIVATE_DELIVERY_NOT_HOSTED ? "no live instance here (not delivered)"
	        : result == 0                                       ? "accepted"
	                                                            : "not accepted",
	        xrt_component_license_state_str(lic.state));
	wipe_token_locked();
	if (g.owner_gone || !g.armed) {
		idle_locked();
	} else {
		g.delivery = (uint32_t)d;
		g.result = result;
		g.lic = lic;
		memcpy(g.err, err, sizeof(g.err));
		g.state = SLOT_DONE;
		g.touched_ns = os_monotonic_get_ns();
	}
	os_mutex_unlock(&g.lock);
	return NULL;
}

bool
service_component_activate_start(service_component_activate_deliver_fn deliver, void *userdata)
{
	if (deliver == NULL) {
		return false;
	}
	if (!g.lock_ready) {
		if (os_mutex_init(&g.lock) != 0) {
			U_LOG_W(
			    "component activation: no lock; live activation is off (the handler delivers in its "
			    "own process).");
			return false;
		}
		g.lock_ready = true;
	}
	os_mutex_lock(&g.lock);
	g.deliver = deliver;
	g.userdata = userdata;
	g.armed = true;
	os_mutex_unlock(&g.lock);
	return true;
}

void
service_component_activate_stop(uint32_t grace_ms)
{
	if (!g.lock_ready) {
		return;
	}
	os_mutex_lock(&g.lock);
	g.armed = false;
	uint32_t waited = 0;
	while (g.state == SLOT_RUNNING && waited < grace_ms) {
		os_mutex_unlock(&g.lock);
		os_nanosleep(10ull * 1000ull * 1000ull);
		waited += 10;
		os_mutex_lock(&g.lock);
	}
	const bool finished = g.state != SLOT_RUNNING;
	const bool join = g.thread_live && finished;
	if (finished) {
		idle_locked();
	} else {
		U_LOG_W("component activation: a component is still inside activate() after %u ms; abandoning it.",
		        (unsigned)grace_ms);
	}
	os_mutex_unlock(&g.lock);
	if (join) {
		os_thread_join(&g.thread);
		os_thread_destroy(&g.thread);
		g.thread_live = false;
	}
}

uint32_t
service_component_activate_put(uint64_t owner, uint32_t offset, const uint8_t *bytes, uint32_t size)
{
	if (!g.lock_ready) {
		return SERVICE_COMPONENT_ACTIVATE_UNSUPPORTED;
	}
	uint32_t status = SERVICE_COMPONENT_ACTIVATE_PENDING;
	os_mutex_lock(&g.lock);
	const uint64_t now = os_monotonic_get_ns();
	if (!g.armed) {
		status = SERVICE_COMPONENT_ACTIVATE_UNSUPPORTED;
	} else if (bytes == NULL || size == 0 || owner == 0) {
		status = SERVICE_COMPONENT_ACTIVATE_BAD_REQUEST;
	} else if (offset == 0) {
		const bool mine = g.owner == owner;
		if (g.state == SLOT_RUNNING ||
		    (g.state == SLOT_STAGING && !mine && now - g.touched_ns < STAGING_STALE_NS) ||
		    (g.state == SLOT_DONE && !mine && now - g.touched_ns < DONE_STALE_NS)) {
			status = SERVICE_COMPONENT_ACTIVATE_BUSY;
		} else {
			idle_locked();
			g.state = SLOT_STAGING;
			g.owner = owner;
		}
	} else if (g.state != SLOT_STAGING || g.owner != owner) {
		status = SERVICE_COMPONENT_ACTIVATE_NONE; // nothing of this connection's is staged
	} else if (offset != g.staged) {
		idle_locked(); // a gap or an overlap: start over
		status = SERVICE_COMPONENT_ACTIVATE_BAD_REQUEST;
	}
	if (status == SERVICE_COMPONENT_ACTIVATE_PENDING) {
		if (size > sizeof(g.token) - 1 - g.staged) {
			idle_locked();
			status = SERVICE_COMPONENT_ACTIVATE_BAD_REQUEST;
		} else {
			memcpy(g.token + g.staged, bytes, size);
			g.staged += size;
			g.touched_ns = now;
		}
	}
	os_mutex_unlock(&g.lock);
	return status;
}

uint32_t
service_component_activate_commit(uint64_t owner, uint32_t class_id, const char *id, uint32_t token_size)
{
	if (!g.lock_ready) {
		return SERVICE_COMPONENT_ACTIVATE_UNSUPPORTED;
	}
	uint32_t status = SERVICE_COMPONENT_ACTIVATE_PENDING;
	bool join = false;
	os_mutex_lock(&g.lock);
	if (!g.armed) {
		status = SERVICE_COMPONENT_ACTIVATE_UNSUPPORTED;
	} else if (g.state != SLOT_STAGING || g.owner != owner) {
		status = SERVICE_COMPONENT_ACTIVATE_NONE;
	} else if (class_id == 0 || !u_activation_id_valid(id) || token_size == 0 || token_size != g.staged ||
	           memchr(g.token, '\0', g.staged) != NULL) {
		idle_locked();
		status = SERVICE_COMPONENT_ACTIVATE_BAD_REQUEST;
	} else {
		g.token[g.staged] = '\0';
		g.class_id = class_id;
		(void)snprintf(g.id, sizeof(g.id), "%s", id);
		g.owner_gone = false;
		g.state = SLOT_RUNNING;
		join = g.thread_live;
	}
	os_mutex_unlock(&g.lock);
	if (status != SERVICE_COMPONENT_ACTIVATE_PENDING) {
		return status;
	}
	// The slot is RUNNING, so nobody else touches g.thread: the previous
	// worker (if any) has moved the state on and is at most returning.
	if (join) {
		os_thread_join(&g.thread);
		os_thread_destroy(&g.thread);
		g.thread_live = false;
	}
	if (os_thread_init(&g.thread) != 0 || os_thread_start(&g.thread, worker, NULL) != 0) {
		os_mutex_lock(&g.lock);
		idle_locked();
		os_mutex_unlock(&g.lock);
		U_LOG_W("component activation: could not start the delivery thread; nothing was delivered.");
		return SERVICE_COMPONENT_ACTIVATE_UNSUPPORTED;
	}
	g.thread_live = true;
	return SERVICE_COMPONENT_ACTIVATE_PENDING;
}

uint32_t
service_component_activate_poll(
    uint64_t owner, uint32_t *out_result, struct xrt_component_license_state *out_state, char *err, size_t err_cap)
{
	if (out_result != NULL) {
		*out_result = 0;
	}
	if (out_state != NULL) {
		xrt_component_license_state_init(out_state);
	}
	if (err != NULL && err_cap > 0) {
		err[0] = '\0';
	}
	if (!g.lock_ready) {
		return SERVICE_COMPONENT_ACTIVATE_UNSUPPORTED;
	}
	uint32_t status = SERVICE_COMPONENT_ACTIVATE_NONE;
	os_mutex_lock(&g.lock);
	if (g.owner == owner && owner != 0) {
		if (g.state == SLOT_RUNNING) {
			status = SERVICE_COMPONENT_ACTIVATE_PENDING;
		} else if (g.state == SLOT_DONE) {
			if (g.delivery == SERVICE_COMPONENT_ACTIVATE_DELIVERY_NOT_HOSTED) {
				status = SERVICE_COMPONENT_ACTIVATE_NOT_HOSTED;
			} else {
				status = SERVICE_COMPONENT_ACTIVATE_DONE;
				if (out_result != NULL) {
					*out_result = g.result;
				}
				if (out_state != NULL) {
					*out_state = g.lic;
				}
				if (err != NULL && err_cap > 0) {
					(void)snprintf(err, err_cap, "%s", g.err);
				}
			}
			idle_locked(); // handed out once
		}
	}
	os_mutex_unlock(&g.lock);
	return status;
}

void
service_component_activate_client_gone(uint64_t owner)
{
	if (!g.lock_ready || owner == 0) {
		return;
	}
	os_mutex_lock(&g.lock);
	if (g.owner == owner) {
		if (g.state == SLOT_RUNNING) {
			g.owner_gone = true; // the delivery finishes; its answer is dropped
		} else {
			idle_locked();
		}
	}
	os_mutex_unlock(&g.lock);
}
